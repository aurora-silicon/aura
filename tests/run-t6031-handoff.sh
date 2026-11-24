#!/bin/sh
# Host test of the T6031 (M3 Max J514C/J516C) handoff on one Mac's ADT.
#
#   sh tests/run-t6031-handoff.sh <adt-allowlist.txt> [scenario...]
#   T6031_DTB=<kernel build>/arch/arm64/boot/dts/apple/t6031-j516c.dtb sh tests/run-t6031-handoff.sh ...
#   T6031_OUT=<dir> sh tests/run-t6031-handoff.sh ...     (keeps each scenario's output there)
#
# <adt-allowlist.txt> is the ADT allowlist of an aurora-sep --m3-report from that Mac. It is read
# here and never copied into the tree. tests/t6031_adt.py turns it into a binary ADT, with
# synthetic values for what an allowlist leaves out (listed in its output). tests/t6031_host.c
# then runs aura's display handoff, GPU setup, firmware facts, PMP values (kboot.c's dt_pass_pmp(),
# copied out of src/kboot.c below) and firmware image UUIDs on it, with aura's ADT reader
# (rust/src/adt.rs, built for the host), pmgr.c, firmware.c and libfdt, against
# tests/t6031-j516c-handoff.dts or, with T6031_DTB, a kernel's J516C device tree.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
allow=${1:?usage: run-t6031-handoff.sh <adt-allowlist.txt> [scenario...]}
shift
fail() {
    echo "FAIL: $*" >&2
    exit 1
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# aura's ADT reader. A reader that still converts the node name through c_char gets one
# expression changed to take char as unsigned, as it is on arm64 but not on x86-64; one that
# reads the name as bytes already is used unchanged.
mkdir -p "$tmp/rs" "$tmp/src"
if grep -q 'CStr::from_bytes_until_nul(&self.name)' rust/src/adt.rs; then
    sed 's|CStr::from_bytes_until_nul(&self.name)|CStr::from_bytes_until_nul(unsafe { \&*(\&self.name as *const [c_char; 32] as *const [u8; 32]) })|' \
        rust/src/adt.rs > "$tmp/rs/adt.rs"
    if [ "$(diff rust/src/adt.rs "$tmp/rs/adt.rs" | grep -c '^>')" != 1 ]; then
        echo "FAIL: rust/src/adt.rs changed; update the host build in $0"
        exit 1
    fi
elif grep -q 'from_raw_parts(self.name.as_ptr() as \*const u8, 32)' rust/src/adt.rs; then
    cp rust/src/adt.rs "$tmp/rs/adt.rs"
else
    echo "FAIL: rust/src/adt.rs changed; update the host build in $0"
    exit 1
fi
printf '#![allow(non_camel_case_types, dead_code, unused_imports)]\ntype c_size_t = usize;\nmod adt;\n' \
    > "$tmp/rs/lib.rs"
RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-1.89.0} rustc --edition 2021 --crate-type staticlib -O \
    -o "$tmp/rs/libadt_host.a" "$tmp/rs/lib.rs"

# kboot.c's PMP pass for T6031, copied out as it is
pick() { # pick <first line regex> <last line regex>
    awk -v a="$1" -v b="$2" '$0 ~ a { on = 1 } on { print } on && $0 ~ b { exit }' src/kboot.c
}
{
    printf '#include <string.h>\n#include "adt.h"\n#include "kboot_t6031.h"\n#include "soc.h"\n'
    printf '#include "types.h"\n#include "utils.h"\n#include "libfdt/libfdt.h"\n'
    printf '#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)\n'
    printf 'static void *dt;\n'
    pick '^static const char [*]excluded_pmp_props' '^};'
    pick '^static bool skip_pmp_prop[(]' '^}'
    pick '^static int adt_get_u32_exact[(]' '^}'
    pick '^static int dt_fill_pmp_from[(]' '^}'
    pick '^static int dt_fill_pmp[(]' '^}'
    pick '^struct pmp_values_soc [{]' '^};'
    pick '^static const struct pmp_values_soc pmp_values_t6031 = [{]' '^};'
    pick '^static void dt_pass_pmp[(]' '^}'
    pick '^static int dt_set_pmp_t6031[(]' '^}'
    printf 'void host_pmp_t6031(void *d)\n{\n    dt = d;\n    dt_set_pmp_t6031();\n}\n'
} > "$tmp/src/kboot_pmp_t6031_copy.c"
for f in dt_fill_pmp_from dt_fill_pmp dt_pass_pmp dt_set_pmp_t6031 pmp_values_t6031; do
    grep -q "$f" "$tmp/src/kboot_pmp_t6031_copy.c" || {
        echo "FAIL: $f not found in src/kboot.c"
        exit 1
    }
done

# The code under test next to the host stand-ins, so that its #include "utils.h" finds them
for f in kboot_display_handoff.c kboot_gpu.c kboot_gpu26.c kboot_gpu_t6031.c \
    kboot_gpu_t8122_power.c kboot_t6031.c firmware.c pmgr.c; do
    cp "src/$f" "$tmp/src/"
done
cp tests/t6031_host/*.h tests/t6031_host.c "$tmp/src/"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-function -Wno-unused-variable \
    -Wno-format-truncation -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -I"$tmp/src" -I"$root/src" -I"$root" \
    "$tmp"/src/*.c src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_addresses.c "$tmp/rs/libadt_host.a" -lm -lpthread -ldl -o "$tmp/t6031-host"

python3 -I tests/t6031_adt.py "$allow" "$tmp/mac.adt"
dtb=${T6031_DTB:-}
if [ -z "$dtb" ]; then
    ${DTC:-dtc} -q -I dts -O dtb -o "$tmp/fixture.dtb" tests/t6031-j516c-handoff.dts
    dtb="$tmp/fixture.dtb"
fi
echo "### $(basename "$dtb")"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
out=${T6031_OUT:-$tmp/out}
mkdir -p "$out"
fail=0
if [ "$#" -gt 0 ]; then
    scenarios=$*
else
    scenarios=$("$tmp/t6031-host" "$tmp/mac.adt" "$dtb" list) ||
        fail "T6031 scenario discovery"
fi
executed=0
for s in $scenarios; do
    if "$tmp/t6031-host" "$tmp/mac.adt" "$dtb" "$s" > "$out/$s.txt" 2>&1; then
        grep -E '^(ok|FAIL) ' "$out/$s.txt" | sed "s/^/$s: /"
    else
        fail=1
        cat "$out/$s.txt"
    fi
    tail -n 1 "$out/$s.txt"
    executed=$((executed + 1))
done
[ "$executed" -gt 0 ] || fail "no T6031 scenarios executed"
[ $fail = 0 ] && echo "T6031 handoff: $executed scenarios; every scenario passes"
exit $fail
