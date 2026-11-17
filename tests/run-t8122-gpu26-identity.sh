#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
# Compile the actual 25G83 calibration path; only hardware and unchanged
# ADT/calibration helpers are supplied by the host fixture.
python3 - "$test_dir/production.inc" <<'PY'
import re, sys
from pathlib import Path
source = Path('src/kboot_gpu.c').read_text()
def definition(pattern):
    match = re.search(pattern, source, re.M)
    assert match, pattern
    start = match.start()
    brace = source.index('{', match.end() - 1)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if source[end:end+1] == ';':
        end += 1
    return source[start:end] + '\n'
structs = ['perf_state', 'g15_soc']
prelude = ''.join(definition(r'^struct ' + name + r' \{') for name in structs)
functions = ['g15_dt_reg_by_name', 't8122_identity_read', 'g15_read_hw',
             'dt_gpu_t8122_26_calibrate']
bodies = ''.join(definition(r'^static (?:int|bool|void) ' + name + r'\([^;]+?\)\n\{')
                 if name != 'dt_gpu_t8122_26_calibrate' else
                 definition(r'^int ' + name + r'\([^;]+?\)\n\{')
                 for name in functions)
gpu26 = Path('src/kboot_gpu26.c').read_text()
match = re.search(r'^static int t8122_fdt_transaction\(', gpu26, re.M)
assert match
start = match.start()
brace = gpu26.index('{', match.end() - 1)
depth = 1
end = brace + 1
while depth:
    depth += (gpu26[end] == '{') - (gpu26[end] == '}')
    end += 1
bodies += gpu26[start:end] + '\n'
Path(sys.argv[1]).write_text(prelude + '\n/* FIXTURE HELPERS */\n' + bodies)
PY
python3 - "$test_dir/production.inc" "$test_dir/gpu26-identity.c" <<'PY'
from pathlib import Path
import sys
production = Path(sys.argv[1]).read_text()
fixture = Path('tests/t8122_gpu26_identity.c').read_text()
prelude, bodies = production.split('/* FIXTURE HELPERS */')
Path(sys.argv[2]).write_text(fixture.replace('/* SOURCE STRUCTS */', prelude)
                          .replace('/* SOURCE FUNCTIONS */', bodies))
PY
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-sign-compare -Wno-format \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc "$test_dir/gpu26-identity.c" src/kboot_gpu_t8122_power.c \
    src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c \
    src/libfdt/fdt_wip.c src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c \
    src/libfdt/fdt_addresses.c -o "$test_dir/gpu26-identity"
"$test_dir/gpu26-identity"
