#!/bin/sh
# SPDX-License-Identifier: MIT
# Host check of locked-DART AOP OS-RAM reservation and failure handling (src/kboot.c +
# src/fdt_disable.c) with real libfdt.
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
python3 - "$tmp/aop_os_ram.c" <<'PY'
from pathlib import Path
src = Path("src/kboot.c").read_text()
start = src.index("static int dt_find_aop(")
end = src.index("\n/*\n * The AOP's accelerometer", start)
blocks = src[start:end]
Path(__import__("sys").argv[1]).write_text(
    Path("tests/aop_os_ram_host.c").read_text().replace("/* SOURCE FUNCTIONS */", blocks)
)
PY
cflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
    -fno-omit-frame-pointer -fno-sanitize-recover=undefined -Isrc"
${CC:-cc} $cflags "$tmp/aop_os_ram.c" src/fdt_disable.c src/libfdt/fdt.c src/libfdt/fdt_ro.c \
    src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c src/libfdt/fdt_sw.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_check.c -o "$tmp/aop-os-ram-host"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} "$tmp/aop-os-ram-host"
