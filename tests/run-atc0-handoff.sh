#!/bin/sh
# Production ADT -> Linux DTB handoff, with no MMIO. Inputs stay outside the tree.
# Usage: sh tests/run-atc0-handoff.sh J714s.adt fixed-linux.dtb ARTIFACT_DIR
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
adt=$(realpath "${1:?retained J714s ADT}")
dtb=$(realpath "${2:?Linux DTB with disabled ATC0 handoff group}")
out=$(realpath -m "${3:?fresh artifact directory}")
test ! -e "$out"
mkdir -p "$out/build/src" "$out/cases"
cp src/kboot_atc.c tests/atc0_handoff_host.c tests/atc0_handoff_host/utils.h "$out/build/src/"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra \
    -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation \
    -Wno-attributes -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-sanitize-recover=undefined -I"$out/build/src" -I"$root/src" -I"$root" \
    "$out/build/src/atc0_handoff_host.c" "$out/build/src/kboot_atc.c" \
    src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_addresses.c -o "$out/build/handoff"
python3 tests/atc0_handoff_cases.py "$out/build/handoff" "$adt" "$dtb" "$out/cases"
sha256sum "$adt" "$dtb" src/kboot_atc.c src/kboot.c src/usb.c src/main.c > "$out/INPUTS.sha256"
find "$out/cases" -type f -exec sha256sum {} + > "$out/OUTPUTS.sha256"
printf 'PASS: production ATC0 handoff; artifacts %s\n' "$out"
