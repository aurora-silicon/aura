#!/bin/sh
# Host test of the T8122 (J613) internal display handoff (src/kboot_display_handoff.c) against
# what the kernel's display enable gate and drm/apple's J613 board read from the device tree.
#
#   sh tests/run-t8122-display-handoff.sh
#   J613_DTB=<kernel build>/arch/arm64/boot/dts/apple/t8122-j613.dtb sh tests/run-t8122-display-handoff.sh
#
# Builds tests/t8122_display_host.c with the tree's handoff code and libfdt (ASan, UBSan), and runs
# every scenario on tests/t8122-j613-display.dts (compiled with dtc) and, if J613_DTB is set, on
# that kernel device tree too. See tests/t8122_display_host.c for the scenarios and the checks.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/src"
cp "$root/src/kboot_display_handoff.c" "$root/tests/t8122_display_host.c" \
    "$root"/tests/t8122_display_host/*.h "$tmp/src/"
if [ "$(uname -s)" = Darwin ]; then
    # This host fixture links as Mach-O, without aura's ELF linker script.
    sed 's/section("\.init\.t6050")/section("__TEXT,__t6050_init")/g' \
        "$root/src/kboot_display_handoff.c" > "$tmp/src/kboot_display_handoff.c"
fi
cc=${CC:-cc}
if [ -z "${CC:-}" ] && [ "${USE_CLANG:-0}" = 1 ]; then
    cc=clang
fi
$cc -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-function \
    -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -I"$tmp/src" -I"$root/src" \
    "$tmp/src/t8122_display_host.c" "$tmp/src/kboot_display_handoff.c" \
    "$root"/src/libfdt/fdt.c "$root"/src/libfdt/fdt_ro.c "$root"/src/libfdt/fdt_rw.c \
    "$root"/src/libfdt/fdt_wip.c "$root"/src/libfdt/fdt_sw.c \
    "$root"/src/libfdt/fdt_empty_tree.c "$root"/src/libfdt/fdt_strerror.c \
    "$root"/src/libfdt/fdt_addresses.c \
    -o "$tmp/t8122-display-host"

${DTC:-dtc} -q -I dts -O dtb -o "$tmp/fixture.dtb" "$root/tests/t8122-j613-display.dts"
dtbs="$tmp/fixture.dtb"
[ -n "${J613_DTB:-}" ] && dtbs="$dtbs $J613_DTB"

# The J504 (14" M3 MacBook Pro) runs the same DCP image: the same scenarios on the fixture with
# its root. The J433 (an M3 iMac, also T8122) is outside the board list: armed, nothing is published.
sed '14s/"apple,j613"/"apple,j504"/' "$root/tests/t8122-j613-display.dts" > "$tmp/j504.dts"
sed '14s/"apple,j613"/"apple,j433"/' "$root/tests/t8122-j613-display.dts" > "$tmp/j433.dts"
grep -q '"apple,j504", "apple,t8122"' "$tmp/j504.dts" && grep -q '"apple,j433", "apple,t8122"' "$tmp/j433.dts"
${DTC:-dtc} -q -I dts -O dtb -o "$tmp/j504.dtb" "$tmp/j504.dts"
${DTC:-dtc} -q -I dts -O dtb -o "$tmp/j433.dtb" "$tmp/j433.dts"

fail=0
extra=""
[ -n "${J613_25G83:-}" ] && extra="missing-clock wrong-os missing-profile"
for dtb in $dtbs; do
    echo "### $(basename "$dtb")"
    for s in unarmed publish repeat wrong-image not-running outside-segment power-off \
        contract-selftest $extra; do
        if ! "$tmp/t8122-display-host" "$dtb" "$s" > "$tmp/$s.log" 2>&1; then
            fail=1
            cat "$tmp/$s.log"
        else
            grep -E '^(ok|FAIL) ' "$tmp/$s.log" | sed "s/^/$s: /"
            tail -n 1 "$tmp/$s.log"
        fi
    done
done
if [ -z "${J613_25G83:-}" ]; then
    echo "### j504.dtb"
    for s in unarmed publish repeat wrong-image not-running outside-segment power-off \
        contract-selftest; do
        if ! T8122_BOARD=apple,j504 "$tmp/t8122-display-host" "$tmp/j504.dtb" "$s" \
            > "$tmp/j504-$s.log" 2>&1; then
            fail=1
            cat "$tmp/j504-$s.log"
        else
            tail -n 1 "$tmp/j504-$s.log" | sed "s/^/j504 $s: /"
        fi
    done
    echo "### j433.dtb"
    for s in unarmed other-board; do
        if ! T8122_BOARD=apple,j433 "$tmp/t8122-display-host" "$tmp/j433.dtb" "$s" \
            > "$tmp/j433-$s.log" 2>&1; then
            fail=1
            cat "$tmp/j433-$s.log"
        else
            tail -n 1 "$tmp/j433-$s.log" | sed "s/^/j433 $s: /"
        fi
    done
fi
[ $fail = 0 ] && echo "T8122 display handoff: every scenario passes"
exit $fail
