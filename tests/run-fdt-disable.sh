#!/bin/sh
# Host check of fdt_disable_node() (src/fdt_disable.c): keeps a node from being probed, with room in
# the tree and with no free space at all.
#
#   sh tests/run-fdt-disable.sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cflags="-std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined -Isrc"
${CC:-cc} $cflags -Dfdt_setprop=fdt_setprop_test -c src/fdt_disable.c -o "$tmp/fdt_disable.o"
${CC:-cc} $cflags tests/fdt_disable_host.c "$tmp/fdt_disable.o" src/libfdt/fdt.c src/libfdt/fdt_ro.c \
    src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c src/libfdt/fdt_sw.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_check.c -o "$tmp/fdt-disable-host"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} "$tmp/fdt-disable-host"
