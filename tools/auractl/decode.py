#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Decode an `auractl dump` file into readable trace rows.
#
# The dump is a flat array of 32-byte records, four little-endian u64 each, in the order auractl
# popped them from the device FIFO (oldest first):
#
#   t     CNTPCT_EL0 at the access
#   meta  pa[47:0] | width[50:48] | write[51] | cpu[55:52]
#   val   value written, or value read
#   pc    guest PC (ELR) at the access
#
# width is log2(bytes): 2 = 4-byte, 3 = 8-byte. A 128-bit (LDP/STP) access is recorded as two
# 8-byte rows at consecutive addresses. This is the same record layout the SPTM-virt GPU tracer
# writes, so this decoder reads either.

import argparse
import struct
import sys

REC = struct.Struct("<QQQQ")  # t, meta, val, pc


def width_bytes(w):
    # log2(bytes) -> bytes; the device never records width > 3 (128-bit is split into two 8-byte rows)
    return 1 << w


def main():
    ap = argparse.ArgumentParser(description="decode an auractl trace dump")
    ap.add_argument("file", help="dump file written by `auractl dump -o <file>`")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="subtract this base and print offsets relative to it")
    ap.add_argument("--writes-only", action="store_true", help="show only writes")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        blob = f.read()

    if len(blob) % REC.size:
        sys.stderr.write(
            f"warning: {len(blob)} bytes is not a whole number of {REC.size}-byte records; "
            f"decoding {len(blob) // REC.size}\n"
        )

    n = len(blob) // REC.size
    first_t = None
    shown = 0
    for i in range(n):
        t, meta, val, pc = REC.unpack_from(blob, i * REC.size)
        pa = meta & ((1 << 48) - 1)
        width = (meta >> 48) & 7
        write = (meta >> 51) & 1
        cpu = (meta >> 52) & 15
        if args.writes_only and not write:
            continue
        if first_t is None:
            first_t = t
        where = f"0x{pa:x}"
        if args.base is not None:
            where = f"+0x{pa - args.base:x}"
        print(
            f"{t - first_t:>12} c{cpu} {'W' if write else 'R'}{8 * width_bytes(width):<3} "
            f"{where:>16} 0x{val:016x} pc 0x{pc:x}"
        )
        shown += 1

    sys.stderr.write(f"{shown} rows ({n} records in the file)\n")


if __name__ == "__main__":
    main()
