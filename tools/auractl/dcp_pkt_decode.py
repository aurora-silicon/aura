#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Decode the DCP IOMFB packet records (AURA_S2R_DCP_PKT, src/s2r_dcp_pkt.c) of an `auractl read`
# dump (AURATRC1). Debug only.
#
# Each packet is a run of raw records whose pc is "CPKT" | idx << 32 | kind << 36 | word << 40
# (idx = the DCP in setup order, kind 0 AP call, 1 DCP callback, 2 AP call's output, 3 callback's
# output; word n = the packet's bytes 8n..8n+7 for a call, and for an output word 0/1 = the header
# again and word 2.. = the 8-aligned bytes from the output's start). The packet header is
# tag[4] (stored reversed: "A407" as '7' '0' '4' 'A'), in_len u32, out_len u32; the input follows
# at +12 and the output after it.
#
# Usage: dcp_pkt_decode.py <dump.trc> [<outdir>] [--tag A407,A408] [--hex N]
#   prints one line per packet (time, marker step, dcp, kind, tag, in/out sizes, bytes copied, the
#   first N bytes in hex); with <outdir>, writes <outdir>/<seq>-<tag>-<kind>.bin with the bytes
#   copied (input for a call, output for a reply) and <outdir>/packets.tsv.

import argparse
import struct
import sys
from pathlib import Path

HDR = struct.Struct("<8sIIQQQQQ48s")
REC = struct.Struct("<QQQQ")
MAGIC = b"AURATRC1"
CPKT = 0x544B5043
KINDS = ("call", "cb", "call-out", "cb-out")


def tag_name(h0):
    raw = (h0 & 0xFFFFFFFF).to_bytes(4, "little")[::-1]
    return raw.decode("latin1") if all(32 <= c < 127 for c in raw) else raw.hex()


class Packet:
    def __init__(self, t, idx, kind, a2i):
        self.t, self.idx, self.kind, self.a2i = t, idx, kind, a2i
        self.words = {}

    def add(self, word, val):
        self.words[word] = val

    @property
    def h0(self):
        return self.words.get(0, 0)

    @property
    def tag(self):
        return tag_name(self.h0)

    @property
    def in_len(self):
        return self.h0 >> 32

    @property
    def out_len(self):
        return self.words.get(1, 0) & 0xFFFFFFFF

    def payload(self):
        """The bytes copied: a call's input, or a reply's output (only the contiguous words)."""
        n = 0
        while n in self.words:
            n += 1
        if self.kind < 2:
            stream = b"".join(self.words[i].to_bytes(8, "little") for i in range(n))
            return stream[12 : 12 + self.in_len]
        if n < 2:
            return b""
        base = (12 + self.in_len) & ~7
        skip = 12 + self.in_len - base
        stream = b"".join(self.words[i].to_bytes(8, "little") for i in range(2, n))
        return stream[skip : skip + self.out_len]


def read_packets(path):
    blob = Path(path).read_bytes()
    if blob[:8] != MAGIC:
        raise SystemExit(f"{path}: not an AURATRC1 file")
    _, _, rec_size, cntfrq, first, count, _, lost, build = HDR.unpack_from(blob, 0)
    packets, cur, step = [], {}, ""
    for i in range(count):
        t, meta, val, pc = REC.unpack_from(blob, HDR.size + i * rec_size)
        if (meta >> 55) & 1:  # marker: its tag in pc
            step = pc.to_bytes(8, "little").rstrip(b"\0").decode("latin1", "replace")
            continue
        if pc & 0xFFFFFFFF != CPKT:
            continue
        idx, kind, word = (pc >> 32) & 0xF, (pc >> 36) & 0xF, pc >> 40
        key = (idx, kind)
        if word == 0:
            p = Packet(t / cntfrq if cntfrq else t, idx, kind, (meta >> 50) & 1)
            p.step = step
            packets.append(p)
            cur[key] = p
        if key in cur:
            cur[key].add(word, val)
    return packets, build.rstrip(b"\0").decode("latin1", "replace"), lost


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dump")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--tag", default="", help="only these tags (comma list)")
    ap.add_argument("--hex", type=int, default=32, help="bytes of hex per line")
    a = ap.parse_args(argv)
    want = {t for t in a.tag.split(",") if t}
    packets, build, lost = read_packets(a.dump)
    out = Path(a.outdir) if a.outdir else None
    if out:
        out.mkdir(parents=True, exist_ok=True)
        tsv = open(out / "packets.tsv", "w")
        tsv.write("seq\tt\tstep\tdcp\tkind\ttag\tin_len\tout_len\tcopied\n")
    print(f"# aura {build}: {len(packets)} packets, {lost} older records gone")
    for n, p in enumerate(packets):
        if want and p.tag not in want:
            continue
        data = p.payload()
        print(f"{p.t:14.6f} {p.step:8s} dcp{p.idx} {KINDS[p.kind] if p.kind < 4 else p.kind:8s} "
              f"{p.tag} in {p.in_len:#x} out {p.out_len:#x} copied {len(data):#x} "
              f"{data[: a.hex].hex()}")
        if out:
            (out / f"{n:06d}-{p.tag}-{KINDS[p.kind] if p.kind < 4 else p.kind}.bin").write_bytes(data)
            tsv.write(f"{n}\t{p.t:.6f}\t{p.step}\tdcp{p.idx}\t{p.kind}\t{p.tag}\t{p.in_len}\t"
                      f"{p.out_len}\t{len(data)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
