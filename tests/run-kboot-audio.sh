#!/bin/sh
# Host test of the speaker amp configuration from the ADT (src/kboot_audio.c).
#
#   sh tests/run-kboot-audio.sh                          (the synthetic scenarios)
#   sh tests/run-kboot-audio.sh <IODeviceTree.plist> <kernel.dtb>
#
# Without arguments, every scenario of tests/kboot_audio_adt.py runs against its expected result
# below. With a Mac's `ioreg -a -p IODeviceTree` plist and a kernel DTB for that Mac, the plist
# is turned into a binary ADT (outside the tree) and the result for that Mac is printed, so that
# what aura hands the kernel can be compared with what the kernel used before. Neither file is
# copied into the tree.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
fail() {
    echo "FAIL: $*" >&2
    exit 1
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# aura's ADT reader, built for the host as tests/run-t6031-handoff.sh does
mkdir -p "$tmp/rs" "$tmp/src"
if grep -q 'CStr::from_bytes_until_nul(&self.name)' rust/src/adt.rs; then
    sed 's|CStr::from_bytes_until_nul(&self.name)|CStr::from_bytes_until_nul(unsafe { \&*(\&self.name as *const [c_char; 32] as *const [u8; 32]) })|' \
        rust/src/adt.rs > "$tmp/rs/adt.rs"
elif grep -q 'from_raw_parts(self.name.as_ptr() as \*const u8, 32)' rust/src/adt.rs; then
    cp rust/src/adt.rs "$tmp/rs/adt.rs"
else
    fail "rust/src/adt.rs changed; update the host build in $0"
fi
printf '#![allow(non_camel_case_types, dead_code, unused_imports)]\ntype c_size_t = usize;\nmod adt;\n' \
    > "$tmp/rs/lib.rs"
RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-1.89.0} rustc --edition 2021 --crate-type staticlib -O \
    -o "$tmp/rs/libadt_host.a" "$tmp/rs/lib.rs" || fail "host build of the ADT reader"

# The code under test and its headers next to the host stand-ins, so that their #include "types.h"
# and "utils.h" find them
cp src/kboot_audio.c src/kboot_audio.h src/adt.h tests/kboot_audio_host/*.h tests/kboot_audio_host.c "$tmp/src/"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -I"$tmp/src" -I"$root/src" -I"$root" \
    "$tmp"/src/*.c src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_check.c "$tmp/rs/libadt_host.a" -lm -lpthread -ldl -o "$tmp/kboot-audio-host" ||
    fail "host build"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}

if [ "$#" = 2 ]; then
    python3 -I tests/kboot_audio_adt.py ioreg "$1" "$tmp/mac.adt"
    "$tmp/kboot-audio-host" "$tmp/mac.adt" "$2"
    exit
fi
[ "$#" = 0 ] || fail "usage: $0 [<IODeviceTree.plist> <kernel.dtb>]"

BOP=a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7
DCB=5a010000
STALE_DCB=eeee
STALE_BOP=ee
expect() { # expect <scenario>: the amp lines that scenario must give
    case $1 in
    group)
        printf 'amp 0x40 apple,bop-config=%s apple,amp-dcblocker-config=%s\n' $BOP $DCB
        printf 'amp 0x42 apple,bop-config=%s apple,amp-dcblocker-config=%s\n' $BOP $DCB
        printf 'amp 0x41 apple,bop-config=%s apple,amp-dcblocker-config=%s\n' $BOP $DCB ;;
    no-dcblocker)
        printf 'amp 0x40 apple,bop-config=%s apple,amp-dcblocker-config=-\n' $BOP
        printf 'amp 0x42 apple,bop-config=%s apple,amp-dcblocker-config=-\n' $BOP
        printf 'amp 0x41 apple,bop-config=%s apple,amp-dcblocker-config=-\n' $BOP ;;
    single-amp)
        printf 'amp 0x40 apple,bop-config=%s apple,amp-dcblocker-config=-\n' $BOP ;;
    amp-missing-in-dt)
        printf 'amp 0x40 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x42 apple,bop-config=%s apple,amp-dcblocker-config=%s\n' $STALE_BOP $STALE_DCB ;;
    amp-not-unique-in-dt)
        printf 'amp 0x40 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x42 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x41 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x42 apple,bop-config=- apple,amp-dcblocker-config=-\n' ;;
    dangling-phandle|no-amps)
        printf 'amp 0x40 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x42 apple,bop-config=- apple,amp-dcblocker-config=-\n'
        printf 'amp 0x41 apple,bop-config=%s apple,amp-dcblocker-config=%s\n' $STALE_BOP $STALE_DCB ;;
    *) fail "no expected result for $1" ;;
    esac
}

n=0
for s in $(python3 -I tests/kboot_audio_adt.py scenarios); do
    python3 -I tests/kboot_audio_adt.py scenario "$s" "$tmp/$s.adt" "$tmp/$s.dts"
    ${DTC:-dtc} -q -I dts -O dtb -o "$tmp/$s.dtb" "$tmp/$s.dts"
    "$tmp/kboot-audio-host" "$tmp/$s.adt" "$tmp/$s.dtb" > "$tmp/$s.out" 2>&1 ||
        { cat "$tmp/$s.out"; fail "$s: host run"; }
    grep '^amp ' "$tmp/$s.out" > "$tmp/$s.got" || true
    expect "$s" > "$tmp/$s.want"
    if ! cmp -s "$tmp/$s.got" "$tmp/$s.want"; then
        cat "$tmp/$s.out"
        diff -u "$tmp/$s.want" "$tmp/$s.got" || true
        fail "$s"
    fi
    echo "ok $s: $(grep -v '^amp ' "$tmp/$s.out" | tail -n 1)"
    n=$((n + 1))
done
[ "$n" -gt 0 ] || fail "no scenarios ran"
echo "kboot audio: $n scenarios pass"
