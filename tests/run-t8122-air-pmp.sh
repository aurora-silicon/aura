#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
python3 - "$test_dir/pmp.c" <<'PY'
import re, sys
from pathlib import Path

def definition(source, pattern):
    match = re.search(pattern, source, re.M)
    assert match, pattern
    brace = source.index('{', match.end() - 1)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if source[end:end+1] == ';': end += 1
    return source[match.start():end] + '\n'
source = Path('src/kboot.c').read_text()
blocks = definition(source, r'^struct pmp_values_soc \{')
blocks += definition(source, r'^static const struct pmp_values_soc pmp_values_t8122 = \{')
for name in ['dt_pass_pmp', 'dt_set_pmp_t8122']:
    blocks += definition(source, r'^static (?:void|int) ' + name + r'\([^;]+?\)\n\{')
Path(sys.argv[1]).write_text(Path('tests/t8122_air_pmp.c').read_text().replace('/* SOURCE FUNCTIONS */', blocks))
PY
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-variable -Wno-format -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc "$test_dir/pmp.c" src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c \
    src/libfdt/fdt_wip.c src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c \
    src/libfdt/fdt_addresses.c -o "$test_dir/pmp"
"$test_dir/pmp"
