#!/bin/sh
# Host test of the speaker amp configuration and the speaker assembly ID from the ADT
# (src/kboot_audio.c).
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
R0=c0c1c2c3
R1=d0d1d2d3
R2=e0e1e2e3
STALE_ROW=eeeeeeee
STALE_ZD=eeeeeeeeeeeeeeee
Z0=1011121314151617
Z1=18191a1b1c1d1e1f
Z2=2021222324252627
amp() { # amp <addr> <bop> <dcb> <row> <zd row>
    printf 'amp 0x%s apple,bop-config=%s apple,amp-dcblocker-config=%s apple,speaker-config=%s apple,amp-tx-zd-config=%s\n' "$@"
}
six() { # six <addr> <group index>: an amp of the six-amps group
    i=$2
    amp $1 $BOP $DCB "$(printf '%02x%02x%02x%02x' $((0x60 + i)) $((0x70 + i)) $((0x80 + i)) $((0x90 + i)))" \
        "$(printf '%02x' $(seq $((0x30 + 8 * i)) $((0x37 + 8 * i))) | tr -d '\n')"
}
expect() { # expect <scenario>: the amp lines that scenario must give (tree order 0x40, 0x42, 0x41)
    case $1 in
    group)
        amp 40 $BOP $DCB $R0 $Z0
        amp 42 $BOP $DCB $R2 $Z2
        amp 41 $BOP $DCB $R1 $Z1 ;;
    no-dcblocker) # as on the 14"/16" M1 Pro/Max, whose ADT has no DC blocker setting
        amp 40 $BOP - $R0 $Z0
        amp 42 $BOP - $R2 $Z2
        amp 41 $BOP - $R1 $Z1 ;;
    six-amps) # tree order 0x38..0x3d; group order 0x38 0x3b 0x39 0x3c 0x3a 0x3d
        six 38 0
        six 39 2
        six 3a 4
        six 3b 1
        six 3c 3
        six 3d 5 ;;
    short-speaker-config)
        amp 40 $BOP $DCB - -
        amp 42 $BOP $DCB - -
        amp 41 $BOP $DCB - - ;;
    single-amp)
        amp 40 $BOP - - - ;;
    amp-missing-in-dt)
        amp 40 - - - -
        amp 42 $STALE_BOP $STALE_DCB $STALE_ROW $STALE_ZD ;;
    amp-not-unique-in-dt)
        amp 40 - - - -
        amp 42 - - - -
        amp 41 - - - -
        amp 42 - - - - ;;
    dangling-phandle|no-amps)
        amp 40 - - - -
        amp 42 - - - -
        amp 41 $STALE_BOP $STALE_DCB $STALE_ROW $STALE_ZD ;;
    speaker-id-*) expect group ;;
    *) fail "no expected result for $1" ;;
    esac
}

# The made-up AP GPIO controller of tests/kboot_audio_adt.py (pin registers at 0x201000000) and
# a pin register with the input stage on (mode 7: input, interrupt off; bit 9: input enable)
GPIO=0x201000000
IN=0x0000020e
pin() { # pin <pin> <register> <pad level>: one KBOOT_AUDIO_GPIO entry
    printf '0x%x:%s:%s' $((GPIO + 4 * $1)) "$2" "$3"
}
gpio_env() { # gpio_env <scenario>: the pins that scenario models
    case $1 in
    speaker-id-3|speaker-id-not-gpio|speaker-id-bad-phandle|speaker-id-pin-out-of-range|\
    speaker-id-no-valid-ids|speaker-id-find-off|speaker-id-find-2|speaker-id-upper-control-bytes|\
    speaker-id-no-strap-resolves)
        echo "$(pin 0x34 $IN 1),$(pin 0x35 $IN 1)" ;;
    speaker-id-0) echo "$(pin 0x34 $IN 0),$(pin 0x35 $IN 0)" ;;
    speaker-id-not-valid) echo "$(pin 0x34 $IN 1),$(pin 0x35 $IN 0)" ;;
    speaker-id-no-input-stage) echo "$(pin 0x34 0 1),$(pin 0x35 0x00000180 1)" ;;
    speaker-id-output-pin) echo "$(pin 0x34 0x2 1),$(pin 0x35 $IN 1)" ;;
    speaker-id-peripheral-pin) echo "$(pin 0x34 0x0000022e 1),$(pin 0x35 $IN 1)" ;;
    speaker-id-inverted) echo "$(pin 0x34 $IN 0),$(pin 0x35 $IN 1)" ;;
    speaker-id-one-strap) echo "$(pin 0x34 $IN 1),$(pin 0x35 $IN 1)" ;;
    speaker-id-third-strap-ignored) echo "$(pin 0x30 $IN 1),$(pin 0x31 $IN 0),$(pin 0x32 $IN 1)" ;;
    *) echo "" ;;
    esac
}
left() { # left <pin> <register>: a modelled pin as the code must leave it
    printf 'gpio 0x%x 0x%08x\n' $((GPIO + 4 * $1)) $(($2))
}
expect_id() { # expect_id <scenario>: the speaker-id line, then every modelled pin as left
    case $1 in
    speaker-id-3|speaker-id-inverted|speaker-id-upper-control-bytes) echo "speaker-id 3" ;;
    speaker-id-0) echo "speaker-id 0" ;;
    # Read as plain GPIO inputs, like macOS, and left that way
    speaker-id-no-input-stage|speaker-id-output-pin|speaker-id-peripheral-pin) echo "speaker-id 3" ;;
    speaker-id-one-strap) echo "speaker-id 2" ;; # the missing strap reads as 0
    speaker-id-third-strap-ignored) echo "speaker-id 1" ;; # only straps 0 and 1 count
    # Unlisted ID, or no valid-speaker-ids: macOS publishes 65535
    speaker-id-not-valid|speaker-id-no-valid-ids|speaker-id-not-gpio|speaker-id-bad-phandle|\
    speaker-id-pin-out-of-range) echo "speaker-id 65535" ;;
    *) echo "speaker-id -" ;;
    esac
    case $1 in
    speaker-id-no-input-stage)
        left 0x34 $IN
        left 0x35 0x0000038e ;; # pull kept
    speaker-id-output-pin)
        left 0x34 $IN
        left 0x35 $IN ;;
    speaker-id-peripheral-pin) # the data bit it read back is written with the input setup
        left 0x34 0x0000020f
        left 0x35 $IN ;;
    *) # pins already inputs, or never read, stay as they were
        for e in $(gpio_env "$1" | tr ',' ' '); do
            r=${e#*:}
            printf 'gpio %s 0x%08x\n' "${e%%:*}" $((${r%%:*}))
        done ;;
    esac
}

n=0
for s in $(python3 -I tests/kboot_audio_adt.py scenarios); do
    python3 -I tests/kboot_audio_adt.py scenario "$s" "$tmp/$s.adt" "$tmp/$s.dts"
    ${DTC:-dtc} -q -I dts -O dtb -o "$tmp/$s.dtb" "$tmp/$s.dts"
    KBOOT_AUDIO_GPIO=$(gpio_env "$s") "$tmp/kboot-audio-host" "$tmp/$s.adt" "$tmp/$s.dtb" \
        > "$tmp/$s.out" 2>&1 || { cat "$tmp/$s.out"; fail "$s: host run"; }
    grep -E '^(amp|speaker-id|gpio) ' "$tmp/$s.out" > "$tmp/$s.got" || true
    { expect "$s"; expect_id "$s"; } > "$tmp/$s.want"
    if ! cmp -s "$tmp/$s.got" "$tmp/$s.want"; then
        cat "$tmp/$s.out"
        diff -u "$tmp/$s.want" "$tmp/$s.got" || true
        fail "$s"
    fi
    echo "ok $s: $(grep -v -E '^(amp|speaker-id|gpio) ' "$tmp/$s.out" | tail -n 1)"
    n=$((n + 1))
done
[ "$n" -gt 0 ] || fail "no scenarios ran"
echo "kboot audio: $n scenarios pass"
