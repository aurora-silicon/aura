#!/bin/sh
# Host test of the T6030 display ADT facts (src/kboot_display_facts.c) and of the atc-phy3 lane
# tunable rule (src/kboot_atc.c).
#
#   sh tests/run-t6030-display.sh [BASE_REF]
#
# Runs the checks of tests/t6030_display_host.c against this tree. With BASE_REF (a git ref of
# this repository, e.g. aurora8.4), the ATC scenarios are also built against that ref's
# kboot_atc.c: every PHY except T6030's atc-phy3 must come out identical, and so must atc-phy3
# whenever it has its lane tunables or the SoC is not T6030.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
base=${1:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

build() { # build <kboot_atc.c> <out>
    atc=$1 out=$2
    mkdir -p "$out/src"
    cp "$atc" "$out/src/kboot_atc.c"
    cp "$root/src/kboot_display_facts.c" "$root/tests/t6030_display_host.c" \
        "$root"/tests/t6030_host/*.h "$out/src/"
    ${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-function \
        -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation \
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
        -I"$out/src" -I"$root/src" -I"$root" \
        "$out/src/t6030_display_host.c" "$out/src/kboot_atc.c" "$out/src/kboot_display_facts.c" \
        "$root"/src/libfdt/fdt.c "$root"/src/libfdt/fdt_ro.c "$root"/src/libfdt/fdt_rw.c \
        "$root"/src/libfdt/fdt_wip.c "$root"/src/libfdt/fdt_sw.c "$root"/src/libfdt/fdt_addresses.c \
        "$root"/src/libfdt/fdt_empty_tree.c "$root"/src/libfdt/fdt_strerror.c \
        -Wl,--wrap=malloc -o "$out/t6030-display-host"
}

build "$root/src/kboot_atc.c" "$tmp/cur"
"$tmp/cur/t6030-display-host" checks

[ -n "$base" ] || exit 0

git show "$base:src/kboot_atc.c" > "$tmp/base-kboot_atc.c"
build "$tmp/base-kboot_atc.c" "$tmp/base"
fail=0
for s in t6030-lanes t6030-nolanes t6030-lane1 t6030-cio0 t6030-nocommon t8122-nolanes \
    t6020-nolanes; do
    "$tmp/cur/t6030-display-host" atc "$s" > "$tmp/cur-$s"
    "$tmp/base/t6030-display-host" atc "$s" > "$tmp/base-$s"
    case $s in
    t6030-lanes | t8122-nolanes | t6020-nolanes) same="phy[0-3] |log" ;;
    t6030-nocommon) same="phy[0-3] " ;;
    *) same="phy[0-2] " ;;
    esac
    grep -E "$same" "$tmp/base-$s" > "$tmp/base-$s.cmp" || true
    grep -E "$same" "$tmp/cur-$s" > "$tmp/cur-$s.cmp" || true
    if ! cmp -s "$tmp/base-$s.cmp" "$tmp/cur-$s.cmp"; then
        echo "FAIL: $s differs from $base"
        diff "$tmp/base-$s" "$tmp/cur-$s" || true
        fail=1
    fi
done
# The intended change: T6030 atc-phy3 without lane tunables now keeps its common tunables
if [ "$(grep 'phy3 ' "$tmp/base-t6030-nolanes")" = "$(grep 'phy3 ' "$tmp/cur-t6030-nolanes")" ]; then
    echo "FAIL: t6030-nolanes atc-phy3 unchanged from $base"
    fail=1
fi
[ $fail = 0 ] && echo "ATC scenarios against $base: unchanged except T6030 atc-phy3 without lanes"
exit $fail
