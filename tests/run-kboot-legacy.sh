#!/bin/sh
# LEGACY-ASAHI(2027-02-28): host test of src/kboot_legacy.c against the bundled libfdt.
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Isrc tests/kboot_legacy_host.c src/kboot_legacy.c \
    src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c -o "$test_dir/legacy"
"$test_dir/legacy"
