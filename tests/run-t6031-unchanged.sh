#!/bin/sh
# Shows that the T6031 handoff leaves the other SoCs' handoff results as they were.
#
#   sh tests/run-t6031-unchanged.sh <base-ref>     (e.g. the integration/aurora tip it is built on)
#
# Each host test below is built twice with this tree's test files: once with <base-ref>'s src
# and once with this tree's. Every scenario must give the same output both ways.
#  1. tests/kboot_gpu_g15_host.c, the G15 GPU setup on T6030 and T8122: the device tree dump, the
#     bus and PMGR traces and the log. A scenario that fails its own checks must fail the same way
#     with both (some T8122 stand-in scenarios fail on <base-ref> already; they are listed).
#  2. tests/t8122_display_host.c, the internal display handoff: every J613 scenario, its 25G83
#     ones, and the same scenarios in its T6030 world: the output device tree, byte for byte, and
#     the log, allowing the T6030 ADT segment summaries.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
base=${1:?usage: run-t6031-unchanged.sh <base-ref>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/base-tree"
git archive "$base" | tar -x -C "$tmp/base-tree"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
fail=0
fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# The output of a run, with the build directory's name taken out of assertion messages
run() { # run <out> <cmd...>
    out=$1
    shift
    rc=0
    { "$@" > "$out" 2>&1; } 2>/dev/null || rc=$?
    sed "s|$tmp/[a-z0-9-]*/|BUILD/|g" "$out" > "$out.normalized"
    mv "$out.normalized" "$out"
    echo "exit $rc" >> "$out"
}

# ---- 1. the G15 GPU setup ----
build_g15() { # build_g15 <tree> <out>
    tree=$1 out=$2
    mkdir -p "$out/src"
    cp "$tree/src/kboot_gpu.c" "$tree/src/firmware.c" "$tree/src/kboot_gpu26.c" "$out/src/"
    cp tests/g15_host/*.h tests/kboot_gpu_g15_host.c "$out/src/"
    extra="$tree/src/kboot_gpu_t8122_power.c"
    for f in kboot_gpu_t6031.c kboot_t6031.c; do
        if [ -f "$tree/src/$f" ]; then
            cp "$tree/src/$f" "$out/src/"
            extra="$extra $out/src/$f"
        fi
    done
    ${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-function \
        -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-sanitize-recover=undefined -DG15_NEW -DG15_FW26 \
        -I"$out/src" -I"$tree/src" -I"$tree" \
        "$out/src/kboot_gpu_g15_host.c" "$out/src/firmware.c" $extra \
        "$tree"/src/libfdt/fdt.c "$tree"/src/libfdt/fdt_ro.c "$tree"/src/libfdt/fdt_rw.c \
        "$tree"/src/libfdt/fdt_wip.c "$tree"/src/libfdt/fdt_sw.c \
        "$tree"/src/libfdt/fdt_empty_tree.c "$tree"/src/libfdt/fdt_addresses.c \
        -lm -o "$out/kboot-gpu-g15"
}
build_g15 "$tmp/base-tree" "$tmp/g15-base"
build_g15 "$root" "$tmp/g15-new"
same=0 failing=""
scenarios=$("$tmp/g15-base/kboot-gpu-g15" list) || fail "baseline G15 scenario discovery"
executed=0
for s in $scenarios; do
    executed=$((executed + 1))
    run "$tmp/g15-base/$s.out" "$tmp/g15-base/kboot-gpu-g15" "$s"
    run "$tmp/g15-new/$s.out" "$tmp/g15-new/kboot-gpu-g15" "$s"
    if ! cmp -s "$tmp/g15-base/$s.out" "$tmp/g15-new/$s.out"; then
        diff -u "$tmp/g15-base/$s.out" "$tmp/g15-new/$s.out" | head -40
        echo "FAIL: G15 scenario $s differs from $base"
        fail=1
        continue
    fi
    same=$((same + 1))
    tail -n 1 "$tmp/g15-new/$s.out" | grep -qx 'exit 0' || failing="$failing $s"
done
[ "$executed" -gt 0 ] || fail "no baseline G15 scenarios executed"
echo "G15 GPU setup: $same scenarios identical to $base (DT, bus and PMGR traces, log)"
[ -z "$failing" ] || echo "  of which failing their own checks on $base and here alike:$failing"

# ---- 2. the internal display handoff ----
build_disp() { # build_disp <tree> <out>
    tree=$1 out=$2
    mkdir -p "$out/src"
    cp "$tree/src/kboot_display_handoff.c" tests/t8122_display_host.c \
        tests/t8122_display_host/*.h "$out/src/"
    ${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-function \
        -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation \
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
        -I"$out/src" -I"$tree/src" \
        "$out/src/t8122_display_host.c" "$out/src/kboot_display_handoff.c" \
        "$tree"/src/libfdt/fdt.c "$tree"/src/libfdt/fdt_ro.c "$tree"/src/libfdt/fdt_rw.c \
        "$tree"/src/libfdt/fdt_wip.c "$tree"/src/libfdt/fdt_sw.c \
        "$tree"/src/libfdt/fdt_empty_tree.c "$tree"/src/libfdt/fdt_strerror.c \
        "$tree"/src/libfdt/fdt_addresses.c -o "$out/display-host"
}
build_disp "$tmp/base-tree" "$tmp/disp-base"
build_disp "$root" "$tmp/disp-new"
${DTC:-dtc} -q -I dts -O dtb -o "$tmp/j613.dtb" tests/t8122-j613-display.dts
sed -e 's/t8122/t6030/g' -e 's/j613/j516s/g' tests/t8122-j613-display.dts |
    ${DTC:-dtc} -q -I dts -O dtb -o "$tmp/j516s.dtb" -
plain="unarmed publish repeat wrong-image not-running outside-segment power-off"
same=0
for mode in j613 j613-25g83 t6030; do
    case $mode in
    j613) dtb=$tmp/j613.dtb list="$plain contract-selftest" pre= env= ;;
    j613-25g83) dtb=$tmp/j613.dtb list="$plain missing-clock wrong-os missing-profile" pre= \
        env=J613_25G83=1 ;;
    t6030) dtb=$tmp/j516s.dtb list=$plain pre=t6030- env= ;;
    esac
    for s in $list; do
        for b in base new; do
            out="$tmp/disp-$b/$mode-$s.out"
            run "$out" env $env "$tmp/disp-$b/display-host" "$dtb" "$pre$s" \
                "$tmp/disp-$b/$mode-$s.dtb"
            if [ "$mode" = t6030 ]; then
                sed -E '/^DCP: T6030: dcp segment [0-9]+: .+, (region-id-[0-9]+|physical buffer)$/d' \
                    "$out" > "$out.filtered"
                mv "$out.filtered" "$out"
            fi
        done
        if ! cmp -s "$tmp/disp-base/$mode-$s.out" "$tmp/disp-new/$mode-$s.out" ||
            ! cmp -s "$tmp/disp-base/$mode-$s.dtb" "$tmp/disp-new/$mode-$s.dtb"; then
            diff -u "$tmp/disp-base/$mode-$s.out" "$tmp/disp-new/$mode-$s.out" | head -40
            echo "FAIL: display scenario $mode $s differs from $base"
            fail=1
            continue
        fi
        same=$((same + 1))
    done
done
grep -h 'handoff published' "$tmp"/disp-new/t6030-*.out | sort -u | sed 's/^/  T6030: /'
echo "Display handoff: $same J613, J613 25G83 and T6030 scenarios identical to $base (DT bytes, log)"

[ $fail = 0 ] && echo "PASS: every other SoC's handoff results are as on $base"
exit $fail
