#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dump the memory the devices of a parked SPTM-virt guest reach through their DARTs and the GPU UAT.

Park the guest first (AURA_GPU_STOP_SECS / AURA_GPU_STOP_BOOT_SECS on run_guest_sptm.py, or any other stop in the
proxy), then, from the aura tree:

    python3 proxyclient/tools/dump_guest_mem.py --out DIR --dart dart-ane [--dart ...] [--gpu] [--max-mib 160]

The MMIO trace rings show the registers macOS writes; the work itself (command buffers, firmware rings, InitData,
DCP RPC buffers, ANE task descriptors) lives in memory the devices reach through their DARTs (and the GPU through its
UAT). With the guest frozen (the deadline park), this walks those page tables and copies what they map:

  DART targets  every DART instance of the ADT node (its "instance" entries of type DART), every stream whose TCR
                translates; 3- or 4-level tables (T8110 format). Streams listed in "exclave-sid" are secure-world
                memory: recorded, never walked or read.
  --gpu         the GPU context table at sgx gpu-region-base (TTBR0/TTBR1 per context) and both firmware kernel roots
                in gfx-shared-region-base (+0, +0x40000: one per GPU role). Context 0 and the kernel roots are
                dumped; the other contexts (apps) get their maps only, unless --gpu-user.

Safety: the guest is frozen, nothing is written to the device or to memory. Only DRAM (ADT /chosen dram-base/size)
is read; MMIO that a DART maps is listed, never read. Every access runs under the proxy's exception guard (skip +
count): a DART that is powered off, or a page the CPU may not read, faults harmlessly; a faulting range is marked
and reading moves on. The budget (--max-mib) is spent on small mappings first (descriptors, rings, structures); a
mapping larger than --big-mib (frames, framebuffers, weights) gets only its first --head-kib.

Output in --out: maps.tsv (target, instance, stream/ctx, iova, pa, size, flags), streams.tsv (TCR/TTBR per stream),
mem.bin + mem.idx (pa, offset in mem.bin, size, status), summary.json. Read it offline with tools/guest_mem.py.
"""
import argparse, json, os, pathlib, re, struct, sys, time

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--out", type=pathlib.Path, required=True)
ap.add_argument("--dart", action="append", default=[], help="ADT node under /arm-io, e.g. dart-ane (repeatable)")
ap.add_argument("--gpu", action="store_true", help="the GPU's UAT: context table + firmware kernel roots")
ap.add_argument("--gpu-user", action="store_true", help="also read the content of GPU app contexts")
ap.add_argument("--max-mib", type=float, default=160, help="data budget (default 160 MiB, ~14 MiB/s over USB)")
ap.add_argument("--big-mib", type=float, default=16, help="mappings larger than this get only their head (default 16: "
                "reads run at ~14 MiB/s; framebuffers, 39-99 MiB, stay head-only)")
ap.add_argument("--head-kib", type=int, default=64)
ap.add_argument("--maps-only", action="store_true", help="walk and list, read no data pages")
ap.add_argument("--unknown-power", action="store_true",
                help="also touch a DART whose power state cannot be determined (default: skip it)")
ap.add_argument("--ring", type=pathlib.Path,
                help="tools/dump_gpu_ring.py's gpu.txt from the same park: a DART that is powered off at the freeze (JPEG, and AVD's auto "
                     "clock-gate, power down between frames) gets its TCR/TTBR from the last writes in the ring; "
                     "its tables are then walked in DRAM without touching the DART")
args = ap.parse_args()
if not os.environ.get("AURADEVICE"):
    sys.exit("set AURADEVICE first")

from aura.proxy import AuraProxy, UartInterface, GUARD  # noqa: E402
from aura.proxyutils import ProxyUtils, bootstrap_port  # noqa: E402

PAGE = 0x4000
iface = UartInterface()
p = AuraProxy(iface, debug=False)
bootstrap_port(iface, p)
u = ProxyUtils(p)
args.out.mkdir(parents=True, exist_ok=True)
log = open(args.out / "dump.log", "w")


def say(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    log.write(s + "\n")
    log.flush()


def prop_bytes(node, name):
    v = node.getprop(name)
    if v is None:
        return None
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    # The parser keeps each property's construct type: rebuild the exact raw
    # bytes (dart-isp's exclave-sid parses as two u64, dart-ane's as bytes).
    t = getattr(node, "_types", {}).get(name, (None,))[0]
    if t is not None:
        try:
            return bytes(t.build(v))
        except Exception:
            pass
    if isinstance(v, int):
        return struct.pack("<Q", v)
    if isinstance(v, (list, tuple)):
        if all(isinstance(x, int) for x in v):
            fmt = "<Q" if any(x > 0xffffffff or x < 0 for x in v) else "<I"
            return b"".join(struct.pack(fmt, x & 0xffffffffffffffff) for x in v)
    try:
        return bytes(v)
    except Exception:
        return None


def prop_u32s(node, name):
    b = prop_bytes(node, name)
    return list(struct.unpack(f"<{len(b) // 4}I", b[:len(b) // 4 * 4])) if b else []


def prop_u64(node, name):
    v = node.getprop(name)
    if isinstance(v, int):
        return v
    b = prop_bytes(node, name)
    return struct.unpack("<Q", b[:8])[0] if b and len(b) >= 8 else None


chosen = u.adt["/chosen"]
DRAM0 = prop_u64(chosen, "dram-base")
DRAM1 = DRAM0 + prop_u64(chosen, "dram-size")
say(f"DRAM {DRAM0:#x}..{DRAM1:#x}")


def in_dram(pa, size=PAGE):
    return DRAM0 <= pa and pa + size <= DRAM1


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
p.get_exc_count()  # reset


def faults():
    return p.get_exc_count()


def rd32(a):
    return p.read32(a)


table_cache = {}


def table(pa):
    """One 16 KiB page-table page as 2048 u64, or None if it cannot be read."""
    if pa in table_cache:
        return table_cache[pa]
    t = None
    if in_dram(pa):
        raw = iface.readmem(pa, PAGE)
        if not faults():
            t = struct.unpack("<2048Q", raw)
    table_cache[pa] = t
    return t


maps = []      # (target, inst, stream, iova, pa, size, flags, kind)  kind: ram / mmio / unread
streams = []   # rows for streams.tsv
problems = []

# --------------------------------------------------------------------------------------------------------- DARTs
DART2_PA_MASK = ((1 << 38) - 1) & ~((1 << 10) - 1)


def dart_leaves(root, four):
    """(iova, pa, pte) of every valid leaf: T8110 = 11 index bits per level, 16 KiB pages."""
    levels = [36, 25] if four else [25]
    out = []

    def walk(tpa, lvl, base):
        t = table(tpa)
        if t is None:
            problems.append(f"table {tpa:#x} unreadable")
            return
        shift = levels[lvl] if lvl < len(levels) else 14
        for i, pte in enumerate(t):
            if not pte & 1:
                continue
            va = base | (i << shift)
            pa = (pte & DART2_PA_MASK) << 4
            if lvl < len(levels):
                walk(pa, lvl + 1, va)
            else:
                out.append((va, pa, pte))

    walk(root, 0, 0)
    return out


# A powered-off block does not just fault once: the proxy's register read can fault in a loop (on a J815:
# dart-disp0-isoc with the panel on, 15 min of exception dumps). So a DART's registers are touched only when PMGR says its
# real power domain is active (aura/pmgr_power.py resolves virtual "-V" gates to their real ancestors). PMGR is
# always on.
from aura import pmgr_power  # noqa: E402


def dart_powered(name, node):
    """(True/False/None, why)."""
    doms, why = pmgr_power.domains(u.adt, name)
    if not doms:
        return None, why
    states = []
    for dname, addr in doms:
        v = rd32(addr)
        if faults():
            return None, f"{why}: {dname} PS register {addr:#x} unreadable"
        states.append((dname, (v >> 4) & 0xf, v))
    ok = all(st == 0xf for _, st, _ in states)
    return ok, ", ".join(f"{n} @ {a:#x} = {v:#x} (actual {st:#x})" for (n, a), (_, st, v) in zip(doms, states))


_ring = None


def ring_writes():
    """{physical address: last value} of the W32 records in --ring (oldest first), or {} without one."""
    global _ring
    if _ring is None:
        _ring = {}
        if args.ring and args.ring.is_file():
            pat = re.compile(r" W32 (0x[0-9a-f]+) = (0x[0-9a-f]+)")
            with open(args.ring, errors="replace") as f:
                for line in f:
                    m = pat.search(line)
                    if m:
                        _ring[int(m.group(1), 16)] = int(m.group(2), 16)
            say(f"ring {args.ring.name}: {len(_ring)} written registers")
    return _ring


def dart_instances(node):
    """[(reg index, name)] of the node's DART windows, from its "instance" table (16-byte entries)."""
    raw = prop_bytes(node, "instance") or b""
    res = []
    for i in range(len(raw) // 16):
        typ, idx, name = struct.unpack_from("<4sI8s", raw, i * 16)
        if typ[::-1] == b"DART":
            res.append((i, name.rstrip(b"\0").decode("ascii", "replace") or f"DART{idx}"))
    return res or [(0, "DART")]


for name in args.dart:
    path = f"/arm-io/{name}"
    try:
        node = u.adt[path]
    except Exception:
        say(f"{name}: not in the ADT, skipped"); continue
    pw, why = dart_powered(name, node)
    from_ring = False
    if pw is False or (pw is None and not args.unknown_power):
        if ring_writes() and pw is False:
            say(f"{name}: powered off ({why}): registers from the ring's last writes, tables read from DRAM only")
            problems.append(f"{name}: powered off at the freeze, TCR/TTBR taken from the ring")
            from_ring = True
        else:
            say(f"{name}: {'powered off' if pw is False else 'power unknown'} ({why}), not touched")
            problems.append(f"{name}: skipped, {why}")
            continue
    else:
        say(f"{name}: powered ({why})")
    nsid = (prop_u32s(node, "sid-count") or [16])[0]
    exclave = set(prop_u32s(node, "exclave-sid"))
    for reg_i, iname in dart_instances(node):
        base, _ = node.get_reg(reg_i)
        tag = f"{name}/{iname}"
        if from_ring:
            w = ring_writes()
            tcrs = [w.get(base + 0x1000 + 4 * s, 0) for s in range(nsid)]
            ttbrs = [w.get(base + 0x1400 + 4 * s, 0) for s in range(nsid)]
            if not any(t & 1 for t in tcrs):
                say(f"{tag} @ {base:#x}: no TCR write in the ring, skipped")
                problems.append(f"{tag}: powered off, nothing in the ring")
                continue
            streams.append((tag, base, "params", 0, 0, "from ring (DART off at the freeze)"))
        else:
            params = [rd32(base + o) for o in (0x0, 0x4, 0x8, 0xc)]  # PARAMS_0..C: revision, page size, sid count
            tcrs = [rd32(base + 0x1000 + 4 * s) for s in range(nsid)]
            ttbrs = [rd32(base + 0x1400 + 4 * s) for s in range(nsid)]
            if faults():
                say(f"{tag} @ {base:#x}: registers fault (powered off?), skipped")
                problems.append(f"{tag}: registers fault")
                continue
            streams.append((tag, base, "params", params[0], params[1], " ".join(f"{x:#x}" for x in params)))
            say(f"{tag} @ {base:#x}: PARAMS {' '.join(f'{x:#x}' for x in params)}")
        nmaps = 0
        for sid in range(nsid):
            tcr, ttbr = tcrs[sid], ttbrs[sid]
            streams.append((tag, base, sid, tcr, ttbr,
                            "exclave" if sid in exclave else ("ring" if from_ring else "")))
            if not (tcr & 1) or not (ttbr & 1):
                continue
            if sid in exclave:
                say(f"{tag} sid {sid}: exclave stream, not walked"); continue
            root = (ttbr >> 2) << 14
            leaves = dart_leaves(root, bool(tcr & 8))
            for va, pa, pte in leaves:
                maps.append((tag, reg_i, sid, va, pa, PAGE, pte & 0xf, "ram" if in_dram(pa) else "mmio"))
            nmaps += len(leaves)
            say(f"{tag} sid {sid}: tcr {tcr:#x} ttbr {ttbr:#x} -> {len(leaves)} pages "
                f"({len(leaves) * PAGE >> 10} KiB){' 4-level' if tcr & 8 else ''}")
        if not nmaps:
            say(f"{tag} @ {base:#x}: no translating stream with mappings")

# ----------------------------------------------------------------------------------------------------------- GPU
UAT_PA_MASK = ((1 << 48) - 1) & ~(PAGE - 1)
KVA = -(1 << 42) & ((1 << 64) - 1)  # T8142: 42-bit halves, upper half from 0xffff_fc00_0000_0000


def uat_leaves(root, upper):
    """(va, pa, pte) of every page of one UAT half: L1 64 entries (bits 41:36), L2/L3 2048 (35:25, 24:14)."""
    out = []
    t1 = table(root)
    if t1 is None:
        problems.append(f"uat root {root:#x} unreadable")
        return out
    base = KVA if upper else 0
    for i1 in range(64):
        e1 = t1[i1]
        if not e1 & 1:
            continue
        if not e1 & 2:
            out.append((base | (i1 << 36), e1 & UAT_PA_MASK, e1 | (1 << 62)))  # 64 GiB block: listed only
            continue
        t2 = table(e1 & UAT_PA_MASK)
        if t2 is None:
            continue
        for i2, e2 in enumerate(t2):
            if not e2 & 1:
                continue
            va2 = base | (i1 << 36) | (i2 << 25)
            if not e2 & 2:
                out.append((va2, e2 & UAT_PA_MASK, e2 | (1 << 62)))  # 32 MiB block
                continue
            t3 = table(e2 & UAT_PA_MASK)
            if t3 is None:
                continue
            for i3, e3 in enumerate(t3):
                if e3 & 3 == 3:
                    out.append((va2 | (i3 << 14), e3 & UAT_PA_MASK, e3))
    return out


if args.gpu:
    sgx = u.adt["/arm-io/sgx"]
    ctxt = prop_u64(sgx, "gpu-region-base")
    shared = prop_u64(sgx, "gfx-shared-region-base")
    say(f"gpu: context table {ctxt:#x}, kernel roots region {shared:#x}")
    raw = iface.readmem(ctxt, 64 * 16)
    if faults():
        say("gpu: context table unreadable"); raw = bytes(64 * 16)
    roots = {}
    for ctx in range(64):
        for half in (0, 1):
            v = struct.unpack_from("<Q", raw, (ctx * 2 + half) * 8)[0]
            if v & 1:
                streams.append(("gpu", ctxt, ctx * 2 + half, half, v, f"ctx{ctx} ttbr{half} asid {v >> 48}"))
                roots.setdefault((v & ((1 << 48) - 1) & ~1, half), []).append(ctx)
    for off, role in ((0, "gfx"), (0x40000, "gfx1")):
        roots.setdefault((shared + off, 1), []).append(f"kernel-{role}")
    for (root, half), who in sorted(roots.items()):
        content = args.gpu_user or any(w == 0 or str(w).startswith("kernel") for w in who)
        leaves = uat_leaves(root, half == 1)
        label = ",".join(str(w) for w in who)
        for va, pa, pte in leaves:
            big = pte >> 62 & 1
            kind = "block" if big else ("ram" if in_dram(pa) else "mmio")
            if kind == "ram" and not content:
                kind = "listed"
            maps.append((f"gpu/{label}", half, f"{root:#x}", va, pa, (1 << 25) if big else PAGE, pte & 0xfff, kind))
        say(f"gpu root {root:#x} ttbr{half} ({label}): {len(leaves)} pages{'' if content else ' (maps only)'}")

# ------------------------------------------------------------------------------------------------------ the data
# IOVA-contiguous runs per (target, stream); a run larger than --big-mib keeps only its head.
maps.sort(key=lambda m: (m[0], m[2], m[3]))
runs = []
for m in maps:
    if runs and runs[-1][0][0] == m[0] and runs[-1][0][2] == m[2] and runs[-1][-1][3] + runs[-1][-1][5] == m[3]:
        runs[-1].append(m)
    else:
        runs.append([m])
want = {}  # pa page -> priority (run size; small first)
big = int(args.big_mib * (1 << 20))
head = args.head_kib << 10
for r in runs:
    size = sum(m[5] for m in r)
    take = r if size <= big else []
    if size > big:
        acc = 0
        for m in r:
            if acc >= head:
                break
            take.append(m)
            acc += m[5]
    for m in take:
        if m[7] == "ram":
            want[m[4]] = min(want.get(m[4], size), size)

with open(args.out / "maps.tsv", "w") as f:
    f.write("# target\tinstance\tstream\tiova\tpa\tsize\tflags\tkind\n")
    for m in maps:
        f.write(f"{m[0]}\t{m[1]}\t{m[2]}\t{m[3]:#x}\t{m[4]:#x}\t{m[5]:#x}\t{m[6]:#x}\t{m[7]}\n")
with open(args.out / "streams.tsv", "w") as f:
    f.write("# target\tbase\tstream\ttcr\tttbr\tnote\n")
    for s in streams:
        f.write("\t".join(f"{x:#x}" if isinstance(x, int) else str(x) for x in s) + "\n")

budget = int(args.max_mib * (1 << 20))
pages = sorted(want, key=lambda pa: (want[pa], pa))
pages = sorted(pages[:budget // PAGE])
say(f"{len(maps)} mapped pages in {len(runs)} runs; reading {len(pages)} pages ({len(pages) * PAGE >> 20} MiB)"
    f"{' (maps only)' if args.maps_only else ''}, {len(want) - len(pages)} over budget")

idx = []
t0 = time.monotonic()
done = 0
next_report = 8 << 20
if not args.maps_only:
    with open(args.out / "mem.bin", "wb") as mf:
        i = 0
        while i < len(pages):
            j = i
            while j + 1 < len(pages) and pages[j + 1] == pages[j] + PAGE and j - i < 63:  # up to 1 MiB per read
                j += 1
            pa, n = pages[i], (j - i + 1) * PAGE
            data = iface.readmem(pa, n)
            if faults():  # retry page by page, so one bad page doesn't lose the rest
                for k in range(j - i + 1):
                    d = iface.readmem(pa + k * PAGE, PAGE)
                    bad = faults()
                    idx.append((pa + k * PAGE, mf.tell(), PAGE, "fault" if bad else "ok"))
                    mf.write(bytes(PAGE) if bad else d)
            else:
                idx.append((pa, mf.tell(), n, "ok"))
                mf.write(data)
            done += n
            i = j + 1
            if done >= next_report:
                el = time.monotonic() - t0
                rate = done / el / (1 << 20)
                left = (len(pages) * PAGE - done) / (1 << 20) / max(rate, 0.01)
                say(f"  {done >> 20}/{len(pages) * PAGE >> 20} MiB, {rate:.2f} MiB/s, ~{left:.0f} s left")
                next_report += 8 << 20
with open(args.out / "mem.idx", "w") as f:
    f.write("# pa\toffset\tsize\tstatus\n")
    for pa, off, n, st in idx:
        f.write(f"{pa:#x}\t{off:#x}\t{n:#x}\t{st}\n")
p.set_exc_guard(GUARD.OFF)

summary = dict(darts=args.dart, gpu=args.gpu, mapped_pages=len(maps), runs=len(runs), read_bytes=done,
               faulted=sum(1 for x in idx if x[3] == "fault"), over_budget=len(want) - len(pages),
               secs=round(time.monotonic() - t0, 1), problems=problems[:50],
               per_target={})
for m in maps:
    d = summary["per_target"].setdefault(m[0], dict(pages=0, mmio=0, listed=0))
    d["pages"] += 1
    if m[7] == "mmio":
        d["mmio"] += 1
    if m[7] in ("listed", "block"):
        d["listed"] += 1
(args.out / "summary.json").write_text(json.dumps(summary, indent=1))
say(f"dump: {done >> 20} MiB in {summary['secs']} s, {summary['faulted']} faulted pages -> {args.out}")
