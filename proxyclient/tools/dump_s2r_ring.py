#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the S2R trace ring from a parked aura session and print every record, oldest first.

The ring (src/sptm_virt_s2r.c, struct s2r_rec) holds the guest's accesses to the hooked
coprocessor, PMGR and DART pages. Park the guest first, for example with
AURA_GPU_STOP_BOOT_SECS=<n> on run_guest_sptm.py, then run this from the aura tree whose
build/ holds the matching aura-raw.elf:

    python3 proxyclient/tools/dump_s2r_ring.py --out ring.txt [--pa 0x290600000:0x8000]
        [--val 0x28de94100] [--cpu 0] [--t0 TICKS]

Time is printed as raw CNTPCT ticks and as seconds from --t0 (default: the first record).
Every record is tagged with the ADT node and reg index that own its page, as <node[reg]+offset>
(the narrowest window when several overlap), and a per-node count follows the records.
"""
import argparse
import collections
import pathlib
import struct
import sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.append(str(pathlib.Path(__file__).resolve().parent))

REC = struct.Struct("<4Q")
CHUNK = 1 << 20
PKT_TAG = 0x544B5043  # "CPKT", the pc field of a DCP packet record
PKT_KINDS = {0: "AP-call", 1: "DCP-callback", 2: "AP-call-out", 3: "callback-out", 7: "resolve"}
MARKER = 1 << 55
WRITE = 1 << 50


def decode(rec):
    """One record: (t, pa, bits, write, cpu, marker, val, pc). cpu bits 5:4 are in meta[57:56]."""
    t, meta, val, pc = REC.unpack(rec)
    cpu = ((meta >> 51) & 15) | (((meta >> 56) & 3) << 4)
    return t, meta & ((1 << 48) - 1), 8 << ((meta >> 48) & 3), bool(meta & WRITE), cpu, bool(meta & MARKER), val, pc


def val_matches(val, wanted):
    """A value, or an address as its page number (>> 12) or its low 32 bits."""
    return any(val in (w, w >> 12, w & 0xFFFFFFFF) for w in wanted)


def owner(lookup, pa):
    """(<node[reg]+offset>, node[reg]) of the narrowest ADT window holding pa."""
    hits = lookup.lookup_all(pa) if lookup else []
    if not hits:
        return "<unmapped>", "unmapped"
    name, zone = min(hits, key=lambda h: h[1].stop - h[1].start)
    return f"<{name}+0x{pa - zone.start:x}>", name


def fmt(i, rec, t0, freq, lookup=None):
    t, pa, bits, write, cpu, marker, val, pc = decode(rec)
    secs = (t - t0) / freq
    if marker:
        return f"S2R {i} t={t:x} +{secs:.6f}s c{cpu} MARK {pc.to_bytes(8, 'little').rstrip(bytes(1)).decode('latin1')} 0x{val:x}"
    if pc & 0xFFFFFFFF == PKT_TAG:  # s2r_dcp_pkt.c: a DCP call's packet word, or a resolve attempt
        idx, kind, word = (pc >> 32) & 15, (pc >> 36) & 15, pc >> 40
        what = PKT_KINDS.get(kind, f"kind{kind}")
        extra = ""
        if kind < 4 and word == 0:  # packet head: 4 tag characters, then the input size
            tag4 = bytes((val >> (8 * k)) & 255 for k in range(4))
            extra = f" tag={tag4.decode('ascii', 'replace')!r} in={val >> 32}"
        if kind == 7:
            extra = f" found={val >> 32} refused={val & 0xFFFFFFFF} stream={word}"
        return f"S2R {i} t={t:x} +{secs:.6f}s c{cpu} PKT dcp{idx} {what} w{word} 0x{pa:x} = 0x{val:x}{extra}"
    tag = owner(lookup, pa)[0] if lookup else ""
    return f"S2R {i} t={t:x} +{secs:.6f}s c{cpu} {'W' if write else 'R'}{bits} 0x{pa:x} {tag} = 0x{val:x} pc=0x{pc:x}"


def keep(rec, args):
    t, pa, bits, write, cpu, marker, val, pc = decode(rec)
    if marker:
        return True
    if args.pa and not any(lo <= pa < lo + size for lo, size in args.pa):
        return False
    if args.cpu is not None and cpu != args.cpu:
        return False
    if args.val and not val_matches(val, args.val):
        return False
    return True


def parse_pa(text):
    lo, _, size = text.partition(":")
    return int(lo, 0), int(size, 0) if size else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=pathlib.Path, default=pathlib.Path("build/aura-raw.elf"))
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--out", type=pathlib.Path, help="write here instead of stdout")
    ap.add_argument("--pa", type=parse_pa, action="append", help="base[:size]; records inside only (repeatable)")
    ap.add_argument("--val", type=lambda v: int(v, 0), action="append", help="records with this value (repeatable)")
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--t0", type=lambda v: int(v, 0), help="CNTPCT ticks for +0 s (default: first record)")
    ap.add_argument("--freq", type=int, default=24_000_000, help="CNTFRQ_EL0 (default 24 MHz)")
    ap.add_argument("--no-names", action="store_true", help="skip the ADT node lookup")
    ap.add_argument("--last", type=int, help="only the last N records in the ring")
    args = ap.parse_args()

    from run_guest_sptm import Symbols
    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    sym = Symbols(args.nm, args.elf, p.get_base())
    lookup = None if args.no_names else ProxyUtils(p).adt.build_addr_lookup()

    ring = p.read64(sym["sptm_virt_s2r_ring"])
    nrec = p.read64(sym["sptm_virt_s2r_nrec"])
    head = p.read64(sym["sptm_virt_s2r_head"])
    if not ring or not nrec:
        raise SystemExit("no S2R ring: aura was not run with AURA_S2R_TRACE=1")
    have = min(head, nrec)
    if args.last:
        have = min(have, args.last)
    first = head - have
    print(f"ring 0x{ring:x}, {nrec} records, head {head}: reading {have} (records {first}..{head - 1})",
          file=sys.stderr)

    out = open(args.out, "w") if args.out else sys.stdout
    t0, freq = args.t0, args.freq
    per = CHUNK // REC.size
    done = 0
    per_node = collections.Counter()
    while done < have:
        n = min(per, have - done)
        i0 = (first + done) % nrec
        n = min(n, nrec - i0)  # one contiguous run; the ring wraps at nrec
        data = iface.readmem(ring + i0 * REC.size, n * REC.size)
        for k in range(n):
            rec = data[k * REC.size:(k + 1) * REC.size]
            if t0 is None:
                t0 = decode(rec)[0]
            if keep(rec, args):
                print(fmt(first + done + k, rec, t0, freq, lookup), file=out)
                if lookup and not decode(rec)[5]:
                    per_node[owner(lookup, decode(rec)[1])[1]] += 1
        done += n
    for name, count in per_node.most_common():
        print(f"NODE {name} {count}", file=out)
    if args.out:
        out.close()
        print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
