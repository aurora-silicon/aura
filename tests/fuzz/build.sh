#!/bin/sh
# Build aura's host fuzz targets (libFuzzer, ASan, UBSan) into <out>.
#
#   sh tests/fuzz/build.sh <out>
#
# Needs clang with libFuzzer and a Rust toolchain whose LLVM matches clang's (both 22 today).
# The ADT reader (rust/src/adt.rs) is built as a host staticlib with ASan, the sanitizer
# coverage libFuzzer uses, overflow checks and panic=abort, so a Rust panic is a crash too.
# Nothing here is part of the aura image.
set -eu
[ $# = 1 ] || {
    echo "usage: build.sh <out>" >&2
    exit 2
}
cd "$(dirname "$0")/../.."
root=$(pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
CC=${CC:-clang}
RUSTC=${RUSTC:-rustc}

# rustc's sanitizer flags are unstable; RUSTC_BOOTSTRAP lets a stable toolchain take them
RUSTC_BOOTSTRAP=1 "$RUSTC" --edition 2021 --crate-type staticlib --crate-name adt_fuzz \
    -C opt-level=1 -C debuginfo=1 -C debug-assertions=on -C overflow-checks=on -C panic=abort \
    -C force-frame-pointers=yes -Zsanitizer=address -C passes=sancov-module \
    -C llvm-args=-sanitizer-coverage-level=4 \
    -C llvm-args=-sanitizer-coverage-inline-8bit-counters \
    -C llvm-args=-sanitizer-coverage-pc-table \
    -C llvm-args=-sanitizer-coverage-trace-compares \
    -o "$out/libadt_fuzz.a" tests/fuzz/adt_host.rs

# aura runs on arm64 with unaligned loads allowed, and libfdt reads a tree's words byte by byte,
# so UBSan's alignment check is off: it fires on libfdt's reserve map entries at any offset.
cflags="-std=gnu11 -D_GNU_SOURCE -O1 -g -fno-omit-frame-pointer -Wall -Wno-unused-function
    -fsanitize=fuzzer,address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined"
incs="-I$root/src -I$root"

libfdt="src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c
    src/libfdt/fdt_sw.c src/libfdt/fdt_strerror.c src/libfdt/fdt_check.c src/libfdt/fdt_empty_tree.c
    src/libfdt/fdt_addresses.c"
# The C files under test go next to nothing but themselves, so that their #include "utils.h"
# finds the host stand-in in tests/fuzz/host instead of src/utils.h
python3 -I tests/fuzz/src_copy.py "$out/src_copy.c"
cp src/kboot_audio.c src/devicetree.c "$out/"

# shellcheck disable=SC2086
$CC $cflags $incs tests/fuzz/adt_fuzz.c "$out/libadt_fuzz.a" -lpthread -ldl -lm -o "$out/adt-fuzz"
# shellcheck disable=SC2086
$CC $cflags -I"$root/tests/fuzz/host" $incs tests/fuzz/dt_fuzz.c "$out/src_copy.c" "$out/devicetree.c" \
    "$out/kboot_audio.c" $libfdt "$out/libadt_fuzz.a" -lpthread -ldl -lm -o "$out/dt-fuzz"
# pmgr.c includes ../build/build_cfg.h, which only a firmware build generates
mkdir -p "$out/src" "$out/build"
: >"$out/build/build_cfg.h"
cp src/pmgr.c src/sptm_virt_s2r_soc.c "$out/src/"
# shellcheck disable=SC2086
$CC $cflags -I"$root/tests/fuzz/host" $incs tests/fuzz/pmgr_fuzz.c "$out/src/pmgr.c" \
    "$out/src/sptm_virt_s2r_soc.c" "$out/libadt_fuzz.a" -lpthread -ldl -lm -o "$out/pmgr-fuzz"
echo "built $out/adt-fuzz $out/dt-fuzz $out/pmgr-fuzz"
