#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode the AURA_MMIO_TRACE records (src/sptm_virt_mmio.c) of an auractl ring dump.

usage: mmio_decode.py <dump.trc> <outdir> [--afk] [--sep-mbox <page>] [--join <timeline.txt>]...
Writes
  <outdir>/timeline.txt  one time-ordered line per access: time (s), cpu, node[reg index]+offset
                         (or 'outside its ranges @pa'), R/W and bits, value, guest PC; folded
                         polls ('repeated n more times', at the last repeat's time), drops, and
                         '# MMIO' lines for the setup (ranges hooked, pages refused) and a summary
  <outdir>/mmio.csv      the accesses: time,cpu,node,reg,offset,dir,bits,value,pc,repeat
  <outdir>/joined.txt    with --afk and/or --join: this timeline merged by time with
                         dcp_afk_decode.py's (run on the same dump into <outdir>/afk; it has the
                         SEP trace too) and/or other timelines of the same dump.
All the auractl decoders count time from the dump's first record, so their timelines share one
time base and join by their first column.

Record format (meta bit 63 set, node index in meta [62:58]): see the top of src/sptm_virt_mmio.c.
"""
import os
import struct
import subprocess
import sys

HDR = struct.Struct("<8sII5Q48s")
REC = struct.Struct("<4Q")
TAG = {int.from_bytes(k.encode(), "little"): k
       for k in ("DMMN", "DMMG", "DMMS", "DMMR", "DMMD", "DMMC")}
WHY = {1: "already hooked or not mapped", 2: "coprocessor or DART registers",
       3: "page budget used up"}
PG = 0x4000


def cpu_of(meta):
    return ((meta >> 51) & 15) | (((meta >> 56) & 3) << 4)


def load(path):
    data = open(path, "rb").read()
    magic, ver, rsz, frq, first, count, head, lost, build = HDR.unpack_from(data, 0)
    assert magic == b"AURATRC1" and rsz == 32, (magic, rsz)
    count = min(count, (len(data) - HDR.size) // 32)
    recs = [REC.unpack_from(data, HDR.size + i * 32) for i in range(count)]
    info = (f"{count} records from seq {first}, lost {lost}, cntfrq {frq}, "
            f"build {build.rstrip(bytes(1)).decode(errors='replace')}")
    return recs, frq, info


class Nodes:
    def __init__(self, recs):
        self.parts = {}   # node -> {part: bytes}
        self.ranges = {}  # node -> [(idx, base, size)]
        self.setup = []   # (node, text)
        self.config = None
        for t, meta, val, pc in recs:
            if not meta >> 63 or not (meta >> 55) & 1:
                continue
            n, lo, hi = (meta >> 58) & 31, pc & 0xFFFFFFFF, pc >> 32
            pa, tag = meta & 0xFFFFFFFFFFFF, TAG.get(pc & 0xFFFFFFFF)
            if tag == "DMMN":
                self.parts.setdefault(n, {})[hi & 0xFF] = struct.pack("<Q", val)
            elif tag == "DMMG":
                self.ranges.setdefault(n, []).append((hi & 0xFF, pa, val))
                self.setup.append((n, f"reg[{hi & 0xFF}] 0x{pa:x} +0x{val:x}: "
                                      f"{(hi >> 8) & 0xFFFFFF} pages hooked"))
            elif tag == "DMMS":
                why, idx = hi & 0xFF, (hi >> 8) & 0xFF
                if why == 4:
                    pages = (-(-(pa + val) // PG) * PG - pa // PG * PG) // PG
                    self.setup.append((n, f"reg[{idx}] 0x{pa:x} +0x{val:x}: too big "
                                          f"({pages} pages), not hooked"))
                else:
                    self.setup.append((n, f"page 0x{pa:x} not hooked: {WHY.get(why, why)}"))
            elif tag == "DMMC":
                self.config = (hi & 0xFF, (hi >> 8) & 0xFFFFFF, val)

    def name(self, n):
        p = self.parts.get(n)
        if not p:
            return f"node{n}"
        return b"".join(p[k] for k in sorted(p)).rstrip(b"\0").decode(errors="replace")

    def label(self, n, pa):
        for idx, base, size in self.ranges.get(n, []):
            if base <= pa < base + size:
                return f"{self.name(n)}[{idx}]+0x{pa - base:x}", idx, pa - base
        if n in self.ranges:
            return f"{self.name(n)} outside its ranges @0x{pa:x}", "", pa
        return f"{self.name(n)} @0x{pa:x}", "", pa


def decode(recs, frq, nodes):
    t0 = recs[0][0] if recs else 0
    lines, rows = [], []
    last = {}  # node -> (row index, val, cpu)
    stats = {}  # node -> [records, folded, dropped]
    for t, meta, val, pc in recs:
        if not meta >> 63:
            continue
        n = (meta >> 58) & 31
        ts = (t - t0) / frq
        pa, width, wr = meta & 0xFFFFFFFFFFFF, (meta >> 48) & 3, (meta >> 50) & 1
        st = stats.setdefault(n, [0, 0, 0])
        if (meta >> 55) & 1:
            tag = TAG.get(pc & 0xFFFFFFFF)
            if tag == "DMMR":
                ri, lval, lcpu = last.get(n, (None, 0, cpu_of(meta)))
                lab = nodes.label(n, pa)[0]
                lines.append((ts, f"{ts:12.6f} c{lcpu} MMIO {lab} {'W' if wr else 'R'}"
                                  f"{8 << width} 0x{lval:x} repeated {val} more times"))
                st[1] += val
                if ri is not None:
                    rows[ri][9] += val
            elif tag == "DMMD":
                lines.append((ts, f"{ts:12.6f} c{cpu_of(meta)} MMIO {nodes.name(n)} DROPPED "
                                  f"{val} records so far (AURA_MMIO_MAX; counted live)"))
                st[2] = max(st[2], val)
            continue
        cpu = cpu_of(meta)
        lab, idx, off = nodes.label(n, pa)
        d = "W" if wr else "R"
        lines.append((ts, f"{ts:12.6f} c{cpu} MMIO {lab} {d}{8 << width} 0x{val:x} pc 0x{pc:x}"))
        rows.append([f"{ts:.6f}", cpu, nodes.name(n), idx, f"0x{off:x}", d, 8 << width,
                     f"0x{val:x}", f"0x{pc:x}", 0])
        last[n] = (len(rows) - 1, val, cpu)
        st[0] += 1
    return lines, rows, stats


def joined(own, others):
    """Merge timelines by their first column; '#' lines first, in input order."""
    head, body = [], []
    for src, text in enumerate([own] + others):
        for i, ln in enumerate(text):
            try:
                body.append((float(ln.split()[0]), src, i, ln))
            except (ValueError, IndexError):
                head.append(ln)
    return head + [b[3] for b in sorted(body)]


def main():
    args = sys.argv[1:]
    afk = "--afk" in args
    if afk:
        args.remove("--afk")
    sep_mbox, extra = None, []
    while "--sep-mbox" in args:
        i = args.index("--sep-mbox")
        sep_mbox = args[i + 1]
        del args[i:i + 2]
    while "--join" in args:
        i = args.index("--join")
        extra.append(args[i + 1])
        del args[i:i + 2]
    src, out = args
    os.makedirs(out, exist_ok=True)
    recs, frq, info = load(src)
    nodes = Nodes(recs)
    lines, rows, stats = decode(recs, frq, nodes)

    text = [f"# {src}: {info}",
            "# columns: time(s) cpu MMIO node[reg index]+offset R|W<bits> value pc <guest PC>"]
    if nodes.config:
        text.append(f"# MMIO setup: {nodes.config[0]} nodes, {nodes.config[1]} pages, at most "
                    f"{nodes.config[2]} records per node")
    for n, s in nodes.setup:
        text.append(f"# MMIO node {n} {nodes.name(n)} {s}")
    text += [ln for _, ln in sorted(lines, key=lambda x: x[0])]
    for n in sorted(stats):
        r, f, d = stats[n]
        text.append(f"# MMIO {nodes.name(n)}: {r} records, {f} repeats folded, {d} dropped")
    with open(os.path.join(out, "timeline.txt"), "w") as fp:
        fp.write("\n".join(text) + "\n")
    with open(os.path.join(out, "mmio.csv"), "w") as fp:
        fp.write("time,cpu,node,reg,offset,dir,bits,value,pc,repeat\n")
        for r in rows:
            fp.write(",".join(str(x) for x in r) + "\n")

    others = []
    if afk:
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dcp_afk_decode.py")
        cmd = [sys.executable, "-I", tool, src, os.path.join(out, "afk")]
        if sep_mbox:
            cmd += ["--sep-mbox", sep_mbox]
        subprocess.run(cmd, check=True)
        extra.insert(0, os.path.join(out, "afk", "timeline.txt"))
    for path in extra:
        others.append(open(path).read().splitlines())
    if others:
        with open(os.path.join(out, "joined.txt"), "w") as fp:
            fp.write("\n".join(joined(text, others)) + "\n")


if __name__ == "__main__":
    main()
