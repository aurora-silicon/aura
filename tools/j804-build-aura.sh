#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# j804-build-aura.sh: build J804's aura boot objects from one committed revision.
#
#   J804_PRIVATE_DATA=<folder> tools/j804-build-aura.sh <output-dir> [<rev>]
#
# Builds, each in its own clean tree exported with `git archive <rev>` (default HEAD) under
# <output-dir>, so nothing in this checkout or its build/ folder is used or changed:
#
#   Stage 1   gmake USE_CLANG=1 CHAINLOADING=1 FILE_LIMIT=0x200000
#             stage1-aura.bin          raw image, Stage 1 configuration not filled in
#             stage1-aura-boot.macho   the same image wrapped by tools/macho_boot_object.py
#                                      (build/aura-boot.macho), for kmutil without --raw
#   Stage 2   gmake USE_CLANG=1 CHAINLOADING=1 ESP_STAGE2=1 FILE_LIMIT=0x200000
#             stage2-aura.bin          raw image, the start of m1n1/boot-ctxr.bin
#
# plus the matching *-raw.elf files, the build logs and manifest.json (sha256 and size of
# every output, the AURALOAD footprint of each stage, the toolchain and the source revision).
#
# Only committed source is built: uncommitted changes in this checkout are not part of the
# output (the script says so when there are any). The output is byte-for-byte reproducible
# for the same revision, BUILD_TAG and toolchain; the ELFs embed the build path, the raw
# images and the Mach-O do not.
#
# Environment:
#   J804_PRIVATE_DATA  required: the folder holding aura/g18g-j804-gpu-profile.h (Makefile).
#                      It is read by the build; its contents are not copied to the output.
#   AURA_VERSION_TAG   BUILD_TAG to embed. Default: what version.sh prints for <rev> in a
#                      git checkout (VERSION, plus -g<commit> unless <rev> is that release).
#   JOBS               parallel make jobs (default 8).
#   KEEP_SOURCE=1      keep the exported build trees (src-stage1, src-stage2) afterwards.
#   GMAKE              GNU make to run (default gmake, else make).
#   J804_KEEP_BUILD_PATHS=1  do not remap $HOME (below): only to reproduce an image built
#                      before the remap.
#
# Rust panic locations name their source files by absolute path, and for the cargo registry,
# git checkouts and -Z build-std's rust-src those are under the builder's home. The Rust build
# runs with --remap-path-prefix=$HOME=/home/builder (CARGO_ENCODED_RUSTFLAGS), so the images
# carry neither the build host's login nor its folder layout.
#
# Checks (the script fails, and writes no manifest, if one does not hold):
#   - the Mach-O is MH_FILESET (filetype 12), carries stage1-aura.bin byte for byte at file
#     offset 0x10000 and has no __TEXT_BOOT_EXEC segment;
#   - each raw image has its AURALOAD load header, and Stage 2's footprint (code, data, .bss
#     and stacks) is at most 16 MiB, the slot below the CTXR execute window, and equal to the
#     size of stage2-aura.bin (payloads appended to Stage 2 are found at _end, the footprint);
#   - both images carry the T8152 no-trial guard of rust_load_image().
set -eu

die() { echo "j804-build-aura.sh: $*" >&2; exit 1; }

[ $# -ge 1 ] && [ $# -le 2 ] || die "usage: J804_PRIVATE_DATA=<folder> $0 <output-dir> [<rev>]"
OUT=$1
REV=${2:-HEAD}
JOBS=${JOBS:-8}
if [ -z "${GMAKE:-}" ]; then
    if command -v gmake >/dev/null 2>&1; then GMAKE=gmake; else GMAKE=make; fi
fi

TOP=$(cd "$(dirname "$0")/.." && pwd)
command -v git >/dev/null 2>&1 || die "git is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"
git -C "$TOP" rev-parse --git-dir >/dev/null 2>&1 || die "$TOP is not a git checkout"
COMMIT=$(git -C "$TOP" rev-parse --verify "$REV^{commit}" 2>/dev/null) || die "no commit $REV"
TREE=$(git -C "$TOP" rev-parse "$COMMIT^{tree}")
SHORT=$(git -C "$TOP" rev-parse --short=8 "$COMMIT")

[ -n "${J804_PRIVATE_DATA:-}" ] || die "set J804_PRIVATE_DATA to the folder holding aura/g18g-j804-gpu-profile.h"
[ -f "$J804_PRIVATE_DATA/aura/g18g-j804-gpu-profile.h" ] ||
    die "$J804_PRIVATE_DATA/aura/g18g-j804-gpu-profile.h is missing"
PRIVATE=$(cd "$J804_PRIVATE_DATA" && pwd)

if [ -z "${AURA_VERSION_TAG:-}" ]; then
    release=$(git -C "$TOP" show "$COMMIT:VERSION" 2>/dev/null || true)
    release=${release:-unknown}
    if [ "$(git -C "$TOP" describe --tags --exact-match "$COMMIT" 2>/dev/null)" = "v$release" ]; then
        AURA_VERSION_TAG=$release
    else
        AURA_VERSION_TAG=$release-g$SHORT
    fi
fi
export AURA_VERSION_TAG

if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
    die "$OUT exists and is not empty"
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
case "$OUT/" in "$TOP"/*) die "the output directory must be outside this checkout" ;; esac

if [ "$REV" = HEAD ] && ! git -C "$TOP" diff --quiet HEAD -- 2>/dev/null; then
    echo "j804-build-aura.sh: note: uncommitted changes in $TOP are not built ($SHORT is)" >&2
fi

if [ "${J804_KEEP_BUILD_PATHS:-0}" != 1 ]; then
    [ -n "${HOME:-}" ] && [ "$HOME" != / ] || die "HOME is not set: needed to remap Rust source paths"
    CARGO_ENCODED_RUSTFLAGS="--remap-path-prefix=$HOME=/home/builder"
    export CARGO_ENCODED_RUSTFLAGS
fi

build() { # build <stage> <make variables...>
    stage=$1
    shift
    src="$OUT/src-$stage"
    mkdir "$src"
    git -C "$TOP" archive "$COMMIT" | tar -x -C "$src"
    echo "j804-build-aura.sh: $stage: $GMAKE USE_CLANG=1 $* (BUILD_TAG $AURA_VERSION_TAG)" >&2
    if ! "$GMAKE" -C "$src" -j"$JOBS" USE_CLANG=1 "$@" J804_PRIVATE_DATA="$PRIVATE" \
        >"$OUT/build-$stage.log" 2>&1; then
        tail -n 20 "$OUT/build-$stage.log" >&2
        die "$stage build failed, see $OUT/build-$stage.log"
    fi
    cp "$src/build/aura.bin" "$OUT/$stage-aura.bin"
    cp "$src/build/aura-raw.elf" "$OUT/$stage-aura-raw.elf"
}

build stage1 CHAINLOADING=1 FILE_LIMIT=0x200000
cp "$OUT/src-stage1/build/aura-boot.macho" "$OUT/stage1-aura-boot.macho"
build stage2 CHAINLOADING=1 ESP_STAGE2=1 FILE_LIMIT=0x200000

TOOLCHAIN=$(sed -n 's/^INFO: Toolchain path: //p' "$OUT/build-stage2.log" | head -n 1)
LLD=$(sed -n 's/^INFO: LLD path: //p' "$OUT/build-stage2.log" | head -n 1)
LLD=${LLD:-$TOOLCHAIN}
CLANG_VERSION=$("${TOOLCHAIN}clang" --version 2>/dev/null | head -n 1 || true)
LLD_VERSION=$("${LLD}ld.lld" --version 2>/dev/null | head -n 1 || true)
RUSTC_VERSION=$(rustc --version 2>/dev/null || true)
CARGO_VERSION=$(cargo --version 2>/dev/null || true)
DIRTY=false
git -C "$TOP" diff --quiet HEAD -- 2>/dev/null || DIRTY=true

python3 - "$OUT" "$COMMIT" "$TREE" "$AURA_VERSION_TAG" "$DIRTY" "$CLANG_VERSION" \
    "$LLD_VERSION" "$RUSTC_VERSION" "$CARGO_VERSION" <<'EOF'
import datetime
import hashlib
import json
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
commit, tree, tag, dirty, clang, lld, rustc, cargo = sys.argv[2:10]

LOAD_HEADER = 0x808          # start.S: "AURALOAD", version, flags, footprint, run_base
IMAGE_OFFSET = 0x10000       # tools/macho_boot_object.py: the raw image after two header pages
MH_MAGIC_64 = 0xFEEDFACF
MH_FILESET = 12
LC_SEGMENT_64 = 0x19
SLOT = 16 << 20              # aura must end below the CTXR execute window (ctxr_exec.c)
GUARD = b"aura-try: never taken on T8152"


def fail(msg):
    sys.exit(f"j804-build-aura.sh: {msg}")


def load_header(name, data):
    magic, version, flags, footprint, run_base = struct.unpack_from("<8sIIQQ", data, LOAD_HEADER)
    if magic != b"AURALOAD":
        fail(f"{name}: no AURALOAD load header at {LOAD_HEADER:#x}")
    if footprint < len(data):
        fail(f"{name}: footprint {footprint} is smaller than the file ({len(data)})")
    return {"version": version, "flags": flags, "footprint": footprint, "run_base": run_base}


def segments(data):
    magic, _, _, filetype, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64:
        fail("stage1-aura-boot.macho: not a 64-bit Mach-O")
    names, off = [], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            names.append(data[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace"))
        off += size
    return filetype, names


stage1 = (out / "stage1-aura.bin").read_bytes()
stage2 = (out / "stage2-aura.bin").read_bytes()
macho = (out / "stage1-aura-boot.macho").read_bytes()

filetype, segs = segments(macho)
if filetype != MH_FILESET:
    fail(f"stage1-aura-boot.macho: filetype {filetype}, not MH_FILESET ({MH_FILESET})")
if macho[IMAGE_OFFSET:IMAGE_OFFSET + len(stage1)] != stage1:
    fail(f"stage1-aura-boot.macho does not carry stage1-aura.bin at {IMAGE_OFFSET:#x}")
if b"__TEXT_BOOT_EXEC" in macho[:IMAGE_OFFSET]:
    fail("stage1-aura-boot.macho has a __TEXT_BOOT_EXEC segment")

s1 = load_header("stage1-aura.bin", stage1)
s2 = load_header("stage2-aura.bin", stage2)
if s2["footprint"] > SLOT:
    fail(f"stage2-aura.bin: footprint {s2['footprint']} is over 16 MiB")
if s2["footprint"] != len(stage2):
    # aura's payload walk starts at _end: a file shorter than its footprint puts the DTB inside
    # .bss, a longer one leaves bytes before the DTB.
    fail(f"stage2-aura.bin: footprint {s2['footprint']} differs from file size {len(stage2)}")
for name, data in (("stage1-aura.bin", stage1), ("stage2-aura.bin", stage2)):
    if GUARD not in data:
        fail(f"{name} lacks the T8152 no-trial guard")

files = {}
for path in sorted(out.iterdir()):
    if path.is_file() and path.name != "manifest.json":
        files[path.name] = {
            "size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }

manifest = {
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "source": {"commit": commit, "tree": tree, "build_tag": tag,
               "checkout_had_uncommitted_changes": dirty == "true",
               "note": "built from git archive of the commit; uncommitted changes are not built"},
    "toolchain": {"clang": clang, "lld": lld, "rustc": rustc, "cargo": cargo},
    "private_data": "J804_PRIVATE_DATA/aura/g18g-j804-gpu-profile.h, read by the build "
                    "(not recorded here)",
    "stage1": {
        "build": "gmake USE_CLANG=1 CHAINLOADING=1 FILE_LIMIT=0x200000",
        "raw": "stage1-aura.bin",
        "macho": "stage1-aura-boot.macho",
        "macho_check": {"filetype": filetype, "raw_offset": IMAGE_OFFSET,
                        "carries_raw": True, "segments": segs},
        "load_header": s1,
        "config": "AURORA-S1-CFG01 not filled in (the installer fills it)",
    },
    "stage2": {
        "build": "gmake USE_CLANG=1 CHAINLOADING=1 ESP_STAGE2=1 FILE_LIMIT=0x200000",
        "raw": "stage2-aura.bin",
        "load_header": s2,
        "footprint_limit": SLOT,
    },
    "t8152_no_trial_guard": "present in both images",
    "files": files,
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"stage1-aura.bin        {len(stage1):>10}  {files['stage1-aura.bin']['sha256']}")
print(f"stage1-aura-boot.macho {len(macho):>10}  {files['stage1-aura-boot.macho']['sha256']}")
print(f"stage2-aura.bin        {len(stage2):>10}  {files['stage2-aura.bin']['sha256']}")
print(f"stage2 footprint       {s2['footprint']:>10}  ({s2['footprint'] / (1 << 20):.2f} MiB)")
EOF

if [ "${KEEP_SOURCE:-0}" != 1 ]; then
    rm -rf "$OUT/src-stage1" "$OUT/src-stage2"
fi
echo "j804-build-aura.sh: wrote $OUT/manifest.json" >&2
