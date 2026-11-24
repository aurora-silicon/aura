#!/bin/sh
# libFuzzer runs of aura's on-disk parsers, built for the host from rust/src as they are:
#   gpt        rust/src/gpt.rs on a disk image
#   apfs       rust_read_gigalocker(): GPT, then the APFS container and its xART volume
#   chainload  rust_load_image(), rust_take_boot_trial() and rust_read_into(): GPT, aura's NVMe
#              reads, the ESP's FAT file system (the fatfs crate aura builds) and aura-try
#   secboot    aura_secboot_verify() on arbitrary record bodies
# Host only; not part of the firmware build. Needs cargo-fuzz and a Rust toolchain that takes
# -Zsanitizer (nightly, or stable with RUSTC_BOOTSTRAP=1).
#
#   sh tests/fuzz/disk/run-disk-fuzz.sh <target> <out-dir> [seconds] [libFuzzer args...]
#
# <out-dir> keeps the build, the synthetic seed disks (src/bin/mkseeds.rs), the corpus and any
# crash-* inputs. A crash replays with <out-dir>/target/x86_64-unknown-linux-gnu/release/<target>
# <crash-file>; AURA_FUZZ_VERBOSE=1 prints aura's console output.
set -eu
[ $# -ge 2 ] || {
    echo "usage: run-disk-fuzz.sh <gpt|apfs|chainload|secboot> <out-dir> [seconds] [libFuzzer args...]" >&2
    exit 2
}
target=$1
out=$2
secs=${3:-60}
[ $# -ge 3 ] && shift 3 || shift 2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/corpus-$target" "$out/tmp"
out=$(cd "$out" && pwd)
(cd "$here" && cargo fuzz build --fuzz-dir . --target-dir "$out/target" "$target" && \
    cargo fuzz build --fuzz-dir . --target-dir "$out/target" mkseeds)
bin=$out/target/x86_64-unknown-linux-gnu/release
rm -rf "$out/seeds"
"$bin/mkseeds" "$out/seeds" > /dev/null
cd "$out"
# fork mode keeps its job files in TMPDIR
TMPDIR=$out/tmp exec "$bin/$target" -max_total_time="$secs" -timeout=10 -rss_limit_mb=1024 \
    -malloc_limit_mb=512 -max_len=262144 -artifact_prefix="$out/$target-" "$@" \
    "corpus-$target" "seeds/$target"
