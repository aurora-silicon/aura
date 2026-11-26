#!/bin/sh
# SPDX-License-Identifier: MIT
# Host run of the guest MMIO tracer (src/sptm_virt_mmio.c) through its own stage-2 hook
# (tests/mmio_host/harness.c), then its dump through tools/auractl/mmio_decode.py (joined with
# dcp_afk_decode.py's timeline) and sep_decode.py. The hook's LDP/STP need aarch64: a static
# aarch64-linux-gnu build, run natively on an arm64 host or under qemu-aarch64.
# Usage: tests/run-mmio-trace.sh [<outdir>]
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
if [ $# -ge 1 ]; then
    tmp=$1
    mkdir -p "$tmp"
else
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fi
cc=${MMT_CC:-aarch64-linux-gnu-gcc}
run=""
if [ "$(uname -m)" != aarch64 ]; then
    run=${MMT_RUN:-qemu-aarch64}
fi
command -v "$cc" >/dev/null || { echo "SKIP: no $cc"; exit 77; }
[ -z "$run" ] || command -v "$run" >/dev/null || { echo "SKIP: no $run"; exit 77; }

# The tracer's quoted includes resolve next to it first: compile a copy beside stub headers
# that all reduce to the shim.
mkdir -p "$tmp/src" "$tmp/build"
cp src/sptm_virt_mmio.c "$tmp/src/"
: >"$tmp/build/build_cfg.h"
for h in adt exception hv malloc memory smp sptm_virt sptm_virt_priv string utils; do
    printf '#include "%s/tests/mmio_host/shim.h"\n' "$root" >"$tmp/src/$h.h"
done
$cc -static -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -DAURACTL \
    -DMMT_MAX_PAGES=131 -I"$tmp" -o "$tmp/mmt-host" tests/mmio_host/harness.c
$run "$tmp/mmt-host" "$tmp/mmt.trc"
python3 -I tools/auractl/mmio_decode.py "$tmp/mmt.trc" "$tmp/out" --afk
python3 -I tools/auractl/sep_decode.py "$tmp/mmt.trc" "$tmp/sep"
cp -r "$tmp/out/afk" "$tmp/afk"
echo "timeline: $tmp/out/timeline.txt"
