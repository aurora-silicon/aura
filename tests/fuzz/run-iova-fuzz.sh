#!/bin/sh
# libFuzzer run of tests/fuzz/iova_fuzz.c, the IOVA allocator (src/iova.c) against a reference
# model. Host only (clang with libFuzzer); not part of the firmware build.
#
#   sh tests/fuzz/run-iova-fuzz.sh <out-dir> [seconds] [libFuzzer args...]
#
# <out-dir> keeps the binary, the corpus and any crash-* inputs. A crash input replays with
# <out-dir>/iova_fuzz <crash-file> (IOVA_FUZZ_VERBOSE=1 prints the allocator's messages).
set -eu
[ $# -ge 1 ] || {
    echo "usage: run-iova-fuzz.sh <out-dir> [seconds] [libFuzzer args...]" >&2
    exit 2
}
out=$1
secs=${2:-60}
[ $# -ge 2 ] && shift 2 || shift 1
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out/corpus"
${CLANG:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    -I"$root/src" "$root/tests/fuzz/iova_fuzz.c" -o "$out/iova_fuzz"
cd "$out"
exec ./iova_fuzz -max_total_time="$secs" -timeout=10 -rss_limit_mb=1024 -max_len=1024 \
    -artifact_prefix="$out/" "$@" corpus
