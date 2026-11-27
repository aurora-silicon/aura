#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Decode a file written by `auractl read` (docs/auractl.md). It is the same AURATRC1 file
# `auractl print` reads, so this is a standalone, scriptable decoder that runs anywhere Python 3
# does: a dump copied off the Mac decodes on another host.
#
# Layout (src/auractl_abi.h, tools/auractl/auractl.c):
#   header  struct trc_header: magic "AURATRC1", version, rec_size, cntfrq, first, count, head,
#           lost, build[48].
#   records struct auractl_rec x count: t, meta, val, pc (four little-endian u64).
#
#   meta = pa[47:0] | width[49:48] | write[50] | cpu[54:51] | marker[55] | cpu[57:56]
#   width is log2(bytes). A marker record carries its tag in pc (8 chars) and its value in val.

import argparse
import struct
import sys

HDR = struct.Struct("<8sIIQQQQQ48s")  # magic, version, rec_size, cntfrq, first, count, head, lost, build
REC = struct.Struct("<QQQQ")  # t, meta, val, pc
MAGIC = b"AURATRC1"


def rec_pa(meta):
    return meta & ((1 << 48) - 1)


def rec_width(meta):  # log2 of bytes
    return (meta >> 48) & 3


def rec_write(meta):
    return (meta >> 50) & 1


def rec_cpu(meta):
    return ((meta >> 51) & 0xF) | (((meta >> 56) & 3) << 4)


def rec_marker(meta):
    return (meta >> 55) & 1


def main():
    ap = argparse.ArgumentParser(description="decode an auractl read file (AURATRC1)")
    ap.add_argument("file", help="file written by `auractl read <file>`")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="show each address as +offset from this base")
    ap.add_argument("--pa", type=lambda s: int(s, 0), default=None,
                    help="keep only records on this 16 KiB page (or in [--pa, --pa+--span))")
    ap.add_argument("--span", type=lambda s: int(s, 0), default=0x4000,
                    help="width in bytes for --pa (default one 16 KiB page)")
    ap.add_argument("--writes-only", action="store_true", help="show only writes")
    ap.add_argument("--csv", action="store_true", help="emit CSV (seq,t_s,cpu,op,width,pa,val,pc)")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        blob = f.read()

    if len(blob) < HDR.size:
        sys.exit(f"{args.file}: too short to hold a header")
    magic, version, rec_size, cntfrq, first, count, head, lost, build = HDR.unpack_from(blob, 0)
    if magic != MAGIC or version != 1 or rec_size != REC.size or not cntfrq:
        sys.exit(f"{args.file}: not an auractl read file")

    build = build.split(b"\0", 1)[0].decode("ascii", "replace")
    body = blob[HDR.size:]
    n = len(body) // REC.size
    if len(body) % REC.size:
        sys.stderr.write(f"warning: trailing {len(body) % REC.size} bytes are not a whole record\n")

    if args.csv:
        print("seq,t_s,cpu,op,width_bits,pa,val,pc")
    else:
        print(f"# aura {build}: {count} records from sequence {first}, {cntfrq} Hz, {lost} older gone")

    t0 = None
    shown = 0
    for i in range(n):
        t, meta, val, pc = REC.unpack_from(body, i * REC.size)
        seq = first + i
        marker = rec_marker(meta)
        pa = rec_pa(meta)
        write = rec_write(meta)
        cpu = rec_cpu(meta)
        width_bits = 8 << rec_width(meta)
        if t0 is None:
            t0 = t
        secs = (t - t0) / cntfrq
        if not marker:
            if args.writes_only and not write:
                continue
            if args.pa is not None and not (args.pa <= pa < args.pa + args.span):
                continue
        if args.csv:
            if marker:
                tag = struct.pack("<Q", pc).split(b"\0", 1)[0].decode("ascii", "replace")
                print(f"{seq},{secs:.6f},{cpu},MARK,,{tag},0x{val:x},")
            else:
                print(f"{seq},{secs:.6f},{cpu},{'W' if write else 'R'},{width_bits},"
                      f"0x{pa:x},0x{val:x},0x{pc:x}")
        elif marker:
            tag = struct.pack("<Q", pc).split(b"\0", 1)[0].decode("ascii", "replace")
            print(f"{seq} {secs:.6f} c{cpu} MARK {tag:<8} 0x{val:x}")
        else:
            where = f"+0x{pa - args.base:x}" if args.base is not None else f"0x{pa:x}"
            print(f"{seq} {secs:.6f} c{cpu} {'W' if write else 'R'}{width_bits:<3} "
                  f"{where:>16} 0x{val:016x} pc 0x{pc:x}")
        shown += 1

    sys.stderr.write(f"{shown} rows ({n} records in the file)\n")
    if n != count:
        sys.stderr.write(f"warning: header says {count} records, file holds {n}\n")


if __name__ == "__main__":
    main()
