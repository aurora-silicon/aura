#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode an auractl ring dump taken with AURA_SEP_TRACE (src/sptm_virt_sep.c).

usage: sep_decode.py <dump.trc> <outdir>
Writes <outdir>/timeline.txt (every SEP message, markers, buffer copies) and
<outdir>/buf/<seq>-ep<xx>-<in|out>-<send|reply>.bin (each OOL buffer copy, trailing zeros cut).
"""
import os
import struct
import sys

HDR = struct.Struct("<8sII5Q48s")
REC = struct.Struct("<4Q")
SEPH, SEPD = 0x48504553, 0x44504553


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "buf"), exist_ok=True)
    data = open(src, "rb").read()
    magic, ver, rsz, frq, first, count, head, lost, build = HDR.unpack_from(data, 0)
    assert magic == b"AURATRC1" and rsz == 32, (magic, rsz)
    recs = [REC.unpack_from(data, HDR.size + i * 32) for i in range(count)]
    tl = open(os.path.join(out, "timeline.txt"), "w")
    tl.write(f"# {src}: {count} records from seq {first}, lost {lost}, cntfrq {frq}, "
             f"build {build.rstrip(bytes(1)).decode(errors='replace')}\n")
    t0 = recs[0][0] if recs else 0
    names = {}
    cur = None  # (seq, fname, words, chunks)

    def flush():
        nonlocal cur
        if cur:
            seq, fn, words, chunks = cur
            open(os.path.join(out, "buf", fn), "wb").write(b"".join(chunks))
            if len(chunks) != words:
                tl.write(f"#   seq {seq}: {len(chunks)} of {words} words in the ring\n")
        cur = None

    for t, meta, val, pc in recs:
        ts = (t - t0) / frq
        cpu = ((meta >> 51) & 15) | (((meta >> 56) & 3) << 4)
        pa = meta & 0xFFFFFFFFFFFF
        wr = (meta >> 50) & 1
        if (meta >> 55) & 1:
            flush()
            tag = struct.pack("<Q", pc).rstrip(b"\0").decode(errors="replace")
            tl.write(f"{ts:12.6f} c{cpu} MARK {tag} 0x{val:x}\n")
            continue
        lo = pc & 0xFFFFFFFF
        if lo == SEPH:
            flush()
            seq = pc >> 32
            ep, buf, reply, words = val & 0xFF, (val >> 8) & 1, (val >> 9) & 1, val >> 16
            fn = f"{seq:06d}-ep{ep:02x}-{'out' if buf else 'in'}-{'reply' if reply else 'send'}.bin"
            tl.write(f"{ts:12.6f} c{cpu}   BUF seq {seq} ep 0x{ep:02x} {names.get(ep, '')} "
                     f"{'out' if buf else 'in'} @{'reply' if reply else 'send'} iova 0x{pa:x} "
                     f"{words * 8} bytes -> {fn}\n")
            cur = (seq, fn, words, [])
            continue
        if lo == SEPD:
            if cur and (pc >> 32) == cur[0]:
                cur[3].append(struct.pack("<Q", val))
            continue
        flush()
        off = pa & 0xFFF
        if off not in (0x800, 0x830):
            continue
        ep, tag, ty, param, d = val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, \
            (val >> 24) & 0xFF, val >> 32
        if ep == 0xFD and ty == 0:
            names[param] = struct.pack("<I", d).decode(errors="replace")[::-1]
        dirn = "A2I" if off == 0x800 else "I2A"
        tl.write(f"{ts:12.6f} c{cpu} {dirn} ep 0x{ep:02x} {names.get(ep, ''):4} tag 0x{tag:02x} "
                 f"type 0x{ty:02x} param 0x{param:02x} data 0x{d:08x}  (msg 0x{val:016x})\n")
    flush()
    tl.write("# endpoints: " + ", ".join(f"0x{k:02x}={v}" for k, v in sorted(names.items())) + "\n")


if __name__ == "__main__":
    main()
