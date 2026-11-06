#!/bin/sh
# SPDX-License-Identifier: MIT
# Exercise the actual kboot fixups with a synthetic ADT and real libfdt.
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
python3 - "$test_dir/pmp.c" "${1:-}" <<'PY'
import subprocess
import sys
from pathlib import Path
source = (subprocess.check_output(['git', 'show', sys.argv[2] + ':src/kboot.c'], text=True)
          if sys.argv[2] else Path('src/kboot.c').read_text())
start = source.index('static const char *excluded_pmp_props[]')
end = source.index('\n/*\n * T6030 and T8122:', start)
blocks = source[start:end]
Path(sys.argv[1]).write_text(Path('tests/pmp_boot_data.c').read_text().replace('/* SOURCE FUNCTIONS */', blocks))
PY
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc "$test_dir/pmp.c" src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c \
    src/libfdt/fdt_wip.c src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c \
    src/libfdt/fdt_addresses.c -o "$test_dir/pmp"
"$test_dir/pmp"
