#!/bin/sh
# SPDX-License-Identifier: MIT
# Host check of dart_translate_range() (src/dart.c) and rtkit_crashed() (src/rtkit.c).
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
python3 - "$tmp/rtkit_host.c" <<'PY'
import re, sys
from pathlib import Path

def func(src, name):
    m = re.search(r"^(?:static )?(?:void \*|void |bool |int )" + name + r"\(", src, re.M)
    assert m, name
    i = src.index("{", m.end())
    d = 1
    j = i + 1
    while d:
        d += (src[j] == "{") - (src[j] == "}")
        j += 1
    return src[m.start():j] + "\n"

dart = Path("src/dart.c").read_text()
rtkit = Path("src/rtkit.c").read_text()
structs = re.search(r"struct crashlog_hdr \{.*?\};\n\nstruct crashlog_entry \{.*?\};\n", rtkit, re.S).group(0)
blocks = (
    structs
    + "\n"
    + func(dart, "dart_translate_internal")
    + "\n"
    + func(dart, "dart_translate_silent")
    + "\n"
    + func(dart, "dart_translate_range")
    + "\n"
    + func(rtkit, "rtkit_crashed")
)
Path(sys.argv[1]).write_text(
    Path("tests/rtkit_placed_buffer_host.c").read_text().replace("/* SOURCE FUNCTIONS */", blocks)
)
PY
cflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-multichar -fsanitize=address,undefined
    -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
${CC:-cc} $cflags "$tmp/rtkit_host.c" -o "$tmp/rtkit-placed-buffer-host"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} "$tmp/rtkit-placed-buffer-host"
