#!/usr/bin/env python3
# Report the vital-region size of a linked aura image and check it against _file_limit.
#
# The ESP macOS stage 2 (SPTM_VIRT_ESP) is chainloaded to the base of stage 1; on T6030/T8122/T6041
# iBoot keeps the panel DCP's __OS_LOG buffer _file_limit bytes past that base and the DCP overwrites
# anything mapped there. Everything up to _file_vital_end (code and non-pixel data) must therefore end
# below _file_limit. aura-raw.ld already ASSERTs this at link time; this script makes the number
# visible in CI (and on a GitHub step summary) so a regression in the vital size is caught in review.
#
# Usage: esp_vital_size.py <image.elf> [--nm llvm-nm]
import argparse, os, subprocess, sys


def syms(elf, nm):
    out = subprocess.check_output([nm, elf], text=True)
    vals = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("A", "D", "d", "T", "t", "R", "r", "B", "b"):
            vals.setdefault(parts[2], int(parts[0], 16))
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", default=os.environ.get("NM", "llvm-nm"))
    args = ap.parse_args()

    v = syms(args.elf, args.nm)
    for name in ("_base", "_file_vital_end", "_file_limit"):
        if name not in v:
            sys.exit(f"esp_vital_size: {name} not found in {args.elf}")

    vital = v["_file_vital_end"] - v["_base"]
    limit = v["_file_limit"]
    margin = limit - vital
    over = margin < 0
    line = (f"ESP vital region: {vital} B (0x{vital:x}); "
            f"limit {limit} B (0x{limit:x}); "
            f"margin {margin} B {'OVER' if over else 'free'}")
    print(line)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(f"### ESP stage 2 vital size\n\n")
            f.write(f"| vital (`_file_vital_end - _base`) | limit (`_file_limit`) | margin |\n")
            f.write(f"|---|---|---|\n")
            f.write(f"| {vital} B (0x{vital:x}) | {limit} B (0x{limit:x}) | "
                    f"{margin} B {'**OVER**' if over else 'free'} |\n")

    sys.exit(1 if over else 0)


if __name__ == "__main__":
    main()
