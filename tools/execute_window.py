#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Which chips pad the Stage 1 boot object for the CTXR execute window.

The chip ID ranges live in data/execute-window.json, the one copy of them. Tools
that choose a Stage 1 form before aura runs (the installer, the payload
assembler, aurora-secboot) read that file or ask this tool; aura's build turns
it into build/execute_window.h, and each boot checks it against its CTXR probe
(src/ctxr_exec.c) and warns when the two disagree.

  execute_window.py check CHIP   exit 0 if CHIP pads (e.g. 0x8132), 1 if not
  execute_window.py json         print the ranges as JSON
  execute_window.py c-header     print the C header aura builds with
"""

import json
import sys
from pathlib import Path

DATA = Path(__file__).resolve().parents[1] / "data" / "execute-window.json"


def load(path=DATA):
    """The version 1 record: a list of inclusive (first, last) chip ID ranges."""
    record = json.loads(Path(path).read_text())
    if record.get("version") != 1:
        raise ValueError(f"{path}: unknown version {record.get('version')!r}")
    ranges = []
    for pair in record["chip_id_ranges"]:
        first, last = (int(v, 16) for v in pair)
        if not 0 < first <= last <= 0xffff:
            raise ValueError(f"{path}: bad chip ID range {pair!r}")
        ranges.append((first, last))
    if not ranges:
        raise ValueError(f"{path}: no chip ID ranges")
    return ranges


def uses_execute_window(chip, ranges=None):
    """Whether a Mac with this chip ID enrolls the padded boot object."""
    return any(first <= chip <= last for first, last in (ranges or load()))


def c_header(ranges):
    lines = [
        "/* Generated from data/execute-window.json by tools/execute_window.py. */",
        "#ifndef EXECUTE_WINDOW_H",
        "#define EXECUTE_WINDOW_H",
        "",
        "/* Whether the installer pads this chip's Stage 1 boot object for the execute window. */",
        "static inline bool execute_window_expected(u32 chip)",
        "{",
        "    return " + " ||\n           ".join(
            f"(chip >= 0x{first:x} && chip <= 0x{last:x})" for first, last in ranges) + ";",
        "}",
        "",
        "#endif",
    ]
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) == 3 and argv[1] == "check":
        return 0 if uses_execute_window(int(argv[2], 0)) else 1
    if len(argv) == 2 and argv[1] == "json":
        print(json.dumps([[f"0x{a:x}", f"0x{b:x}"] for a, b in load()]))
        return 0
    if len(argv) == 2 and argv[1] == "c-header":
        sys.stdout.write(c_header(load()))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
