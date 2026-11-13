#!/bin/sh
# Host test of the sleep trace's DCP packet copier on a fake T6050 DART. Needs an Armv8 host
# (aura's utils.h uses its register accessors). dart_walk_locked_t8110() is copied out of
# src/dart.c with its register definitions, so a change to the real walk is under test.
#
#   sh tests/run-s2r-dcp-pkt.sh
set -eu
cd "$(dirname "$0")/.."
# smp.h includes build/build_cfg.h, which a fresh clone does not have yet; one that exists is a
# configured build's and is left as it is
[ -f build/build_cfg.h ] || make build-cfg > /dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
{
    printf '#include "types.h"\n#include "utils.h"\n#include "dart.h"\n'
    sed -n '/^#define DART_T8020_ENABLED_STREAMS/,/^#define DART_TCR(/p' src/dart.c
    sed -n '/^int dart_walk_locked_t8110(/,/^}/p' src/dart.c
} > "$tmp/dartwalk.c"
grep -q 'dart_walk_locked_t8110' "$tmp/dartwalk.c" || { echo "FAIL: walk not found in src/dart.c"; exit 1; }
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-function -Isrc \
    tests/s2r_dcp_pkt_host.c src/s2r_dcp_pkt.c "$tmp/dartwalk.c" -o "$tmp/test"
"$tmp/test"
