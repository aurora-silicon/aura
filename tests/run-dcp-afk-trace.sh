#!/bin/sh
# SPDX-License-Identifier: MIT
# Host run of the DCP AFK/EPIC ring tracer (src/sptm_virt_dcp_afk.c) through its own mailbox hook
# (tests/dcp_afk_host/harness.c), then its dump through tools/auractl/dcp_afk_decode.py.
# The tracer's inline ldp/stp need aarch64: a static aarch64-linux-gnu build, run natively on an
# arm64 host or under qemu-aarch64. Usage: tests/run-dcp-afk-trace.sh [<outdir>]
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
cc=${DAFK_CC:-aarch64-linux-gnu-gcc}
run=""
if [ "$(uname -m)" != aarch64 ]; then
    run=${DAFK_RUN:-qemu-aarch64}
fi
command -v "$cc" >/dev/null || { echo "SKIP: no $cc"; exit 77; }
[ -z "$run" ] || command -v "$run" >/dev/null || { echo "SKIP: no $run"; exit 77; }

# The tracer's quoted includes resolve next to it first: compile a copy beside stub headers
# that all reduce to the shim.
mkdir -p "$tmp/src" "$tmp/build"
cp src/sptm_virt_dcp_afk.c "$tmp/src/"
: >"$tmp/build/build_cfg.h"
for h in adt exception hv malloc memory sptm_virt sptm_virt_priv string utils xnuboot; do
    printf '#include "%s/tests/dcp_afk_host/shim.h"\n' "$root" >"$tmp/src/$h.h"
done
$cc -static -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -DAURACTL \
    -I"$tmp" -o "$tmp/dafk-host" tests/dcp_afk_host/harness.c
$run "$tmp/dafk-host" "$tmp/dafk.trc"
python3 -I tools/auractl/dcp_afk_decode.py "$tmp/dafk.trc" "$tmp/out"
echo "timeline: $tmp/out/timeline.txt"
