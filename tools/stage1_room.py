#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""How much a Stage 1 image can still grow before it reaches the DCP log buffer.

aura-raw.ld puts the code (.init and .text), .rodata + .rela.dyn and .data on 64 KiB steps, and
everything up to _file_vital_end must end below _file_limit (0x120000, see aura-raw.ld). The link
fails once it does not. This reports how close a build is, so a change or a toolchain that eats
the last few KiB fails in CI before a distribution's toolchain trips over it:

  data   bytes .data can grow before _file_vital_end reaches the limit
  text/rodata
         bytes each can grow before it crosses its next 64 KiB step, which moves .data up 64 KiB
  init   the same for .init, only in a link that starts .text on its own 64 KiB step (older
         trees, without _text_content_start)

The headroom is the data room while that is 64 KiB or more, else the smallest of all four.

  tools/stage1_room.py build/aura-raw.elf [--min BYTES] [--nm NM]
"""

import argparse
import os
import shutil
import subprocess
import sys

FILE_LIMIT = 0x120000
STEP = 0x10000
SYMBOLS = ("_init_content_end", "_text_content_end", "_rodata_content_end", "_data_start",
           "_file_vital_end")
# Start of .text, in a link where .text follows .init directly and the two share their room.
OPTIONAL = ("_text_content_start",)


def find_nm(arg):
    for nm in (arg, os.environ.get("NM"), "llvm-nm", "aarch64-linux-gnu-nm", "nm"):
        if nm and shutil.which(nm.split()[0]):
            return nm
    sys.exit("stage1_room: no nm found (pass --nm)")


def read_symbols(elf, nm):
    out = subprocess.run(nm.split() + [elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and f[-1] in SYMBOLS + OPTIONAL:
            syms[f[-1]] = int(f[0], 16)
    missing = [s for s in SYMBOLS if s not in syms]
    if missing:
        sys.exit(f"stage1_room: {elf} has no {', '.join(missing)}: not an aura-raw.ld link")
    return syms


def to_step(addr):
    return (addr + STEP - 1) & ~(STEP - 1)


def room(syms):
    r = {}
    if "_text_content_start" not in syms:
        # .text starts on its own 64 KiB step (older aura-raw.ld).
        r["init"] = to_step(syms["_init_content_end"]) - syms["_init_content_end"]
    r |= {
        "text": to_step(syms["_text_content_end"]) - syms["_text_content_end"],
        "rodata": to_step(syms["_rodata_content_end"]) - syms["_rodata_content_end"],
        "data": FILE_LIMIT - syms["_file_vital_end"],
    }
    headroom = r["data"] if r["data"] >= STEP else min(r.values())
    return r, headroom


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf")
    ap.add_argument("--min", type=int, default=0, help="fail when the headroom is below this")
    ap.add_argument("--nm", help="nm to use (default: $NM, llvm-nm, aarch64-linux-gnu-nm, nm)")
    ap.add_argument("--label", default="", help="name printed with the result")
    args = ap.parse_args()

    syms = read_symbols(args.elf, find_nm(args.nm))
    r, headroom = room(syms)
    label = f"{args.label}: " if args.label else ""
    print(f"{label}_file_vital_end {syms['_file_vital_end']:#x} of {FILE_LIMIT:#x}; room " +
          ", ".join(f"{k} {v}" for k, v in r.items()) + f"; headroom {headroom} B")
    if headroom < args.min:
        print(f"{label}headroom {headroom} B is below {args.min} B: this Stage 1 is about to "
              "reach the DCP log buffer (aura-raw.ld)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
