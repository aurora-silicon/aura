#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Copy a make_bundle.py staging tree onto the EFI system partition, and nothing else.
#
#   install_esp.sh [--esp <mountpoint>] --partuuid <uuid> [--dry-run] <staging dir>
#
# It only ever writes aurora/macos/* and m1n1/stage2-macos.bin. It never touches
# m1n1/hv_boot.bin (the Linux Stage 2), aurora-linux/, the Linux kernels, the boot loader, the aura-try marker or NVRAM, so how
# Linux boots does not change. Safe to rerun: a file already correct is skipped, a new one goes
# to a temporary name, is checked, and only then replaces the old one. Selecting this mode is a
# separate step (arm_trial.sh).
#
# The ESP must be mounted already, read-write (/boot on the M5's SSD Linux); this script
# finds it by PARTUUID and never mounts, formats or repartitions anything.
set -eu

PARTUUID=
ESP=
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --esp) ESP=$2; shift 2 ;;
        --partuuid) PARTUUID=$2; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || { echo "usage: $0 [--esp <mountpoint>] --partuuid <uuid> [--dry-run] <staging dir>" >&2; exit 2; }
SRC=$1
BUNDLE=aurora/macos
STAGE2=m1n1/stage2-macos.bin

die() { echo "install_esp: $*" >&2; exit 1; }
[ -n "$PARTUUID" ] || die "--partuuid is required"

if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | cut -d' ' -f1; }
else
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

[ -f "$SRC/$STAGE2" ] && [ -f "$SRC/$BUNDLE/SHA256SUMS" ] && [ -f "$SRC/$BUNDLE/manifest.txt" ] ||
    die "$SRC is not a make_bundle.py tree"

# The tree must be what make_bundle.py wrote.
while read -r want name; do
    [ -n "$name" ] || continue
    case "$name" in
        ../../m1n1/stage2-macos.bin) f=$SRC/$STAGE2 ;;
        */*|..*) die "unexpected name in SHA256SUMS: $name" ;;
        *) f=$SRC/$BUNDLE/$name ;;
    esac
    [ -f "$f" ] || die "$f is missing"
    [ "$(sha "$f")" = "$want" ] || die "$f fails its SHA-256 in the staging tree"
done < "$SRC/$BUNDLE/SHA256SUMS"

# The config line in the image must name this manifest and this partition.
MSHA=$(sha "$SRC/$BUNDLE/manifest.txt")
# (it follows the image's last bytes directly, with no newline between them)
LINE=$(tail -c 256 "$SRC/$STAGE2" | LC_ALL=C tr -d '\000' | LC_ALL=C grep -ao 'macos=[^[:cntrl:]]*' | tail -n 1 || true)
[ "$LINE" = "macos=$PARTUUID;$BUNDLE;$MSHA" ] ||
    die "the Stage 2's config line is not macos=$PARTUUID;$BUNDLE;<this manifest> (got: $LINE)"

if [ -z "$ESP" ]; then
    if command -v findmnt >/dev/null 2>&1; then
        DEV=$(readlink -f "/dev/disk/by-partuuid/$PARTUUID" 2>/dev/null || true)
        [ -n "$DEV" ] && [ -b "$DEV" ] || die "no partition with PARTUUID $PARTUUID"
        ESP=$(findmnt -n -o TARGET -S "$DEV" | head -n 1 || true)
    fi
    [ -n "$ESP" ] || die "the ESP is not mounted: mount it, or give --esp <mountpoint>"
elif command -v findmnt >/dev/null 2>&1 && [ -e "/dev/disk/by-partuuid/$PARTUUID" ]; then
    DEV=$(readlink -f "/dev/disk/by-partuuid/$PARTUUID")
    [ "$(findmnt -n -o SOURCE --target "$ESP" || true)" = "$DEV" ] ||
        die "$ESP is not the partition with PARTUUID $PARTUUID"
fi
[ -d "$ESP" ] || die "$ESP is not a directory"
if command -v diskutil >/dev/null 2>&1; then
    got=$(diskutil info "$ESP" 2>/dev/null | awk -F': *' '/Partition UUID/ { print tolower($2) }')
    [ "$got" = "$PARTUUID" ] || die "$ESP is partition '$got', not $PARTUUID"
fi
[ -w "$ESP" ] || [ "$DRY" = 1 ] || die "$ESP is not writable (root? mounted read-only?)"
if command -v findmnt >/dev/null 2>&1; then
    case "$(findmnt -n -o FSTYPE --target "$ESP" || true)" in
        vfat|msdos|"") ;;
        *) die "$ESP is not a FAT file system" ;;
    esac
fi
echo "ESP: $ESP (PARTUUID $PARTUUID)"

# Room for what has to change. A replaced file exists twice for a moment, so count it in full.
need=0
for f in "$SRC/$BUNDLE"/* "$SRC/$STAGE2"; do
    case "$f" in
        "$SRC/$STAGE2") d=$ESP/$STAGE2 ;;
        *) d=$ESP/$BUNDLE/$(basename "$f") ;;
    esac
    if [ ! -f "$d" ] || [ "$(sha "$d")" != "$(sha "$f")" ]; then
        need=$((need + $(wc -c < "$f")))
    fi
done
free=$(df -k "$ESP" | awk 'NR==2 { print $4 }')
[ "$need" -eq 0 ] || [ $((need / 1024 + 1024)) -le "$free" ] ||
    die "the ESP has $free KiB free and this needs $((need / 1024 + 1024)) KiB"

copy_one() { # source, destination
    if [ -f "$2" ] && [ "$(sha "$2")" = "$(sha "$1")" ]; then
        echo "  same     ${2#"$ESP"/}"
        return
    fi
    if [ "$DRY" = 1 ]; then
        echo "  would copy ${2#"$ESP"/}"
        return
    fi
    tmp=$2.new
    rm -f "$tmp"
    cp "$1" "$tmp"
    sync
    [ "$(sha "$tmp")" = "$(sha "$1")" ] || { rm -f "$tmp"; die "$tmp did not read back correctly"; }
    mv -f "$tmp" "$2"
    echo "  copied   ${2#"$ESP"/}"
}

[ "$DRY" = 1 ] || mkdir -p "$ESP/$BUNDLE"
for f in "$SRC/$BUNDLE"/*; do
    n=$(basename "$f")
    # The manifest and the image last: until they land, an older complete bundle stays valid only
    # if none of its files changed, and aura refuses a mismatch either way.
    [ "$n" = manifest.txt ] && continue
    copy_one "$f" "$ESP/$BUNDLE/$n"
done
copy_one "$SRC/$BUNDLE/manifest.txt" "$ESP/$BUNDLE/manifest.txt"
copy_one "$SRC/$STAGE2" "$ESP/$STAGE2"

if [ "$DRY" = 0 ]; then
    sync
    # What is on the disk now, read back through the file system.
    while read -r want name; do
        [ -n "$name" ] || continue
        case "$name" in
            ../../m1n1/stage2-macos.bin) f=$ESP/$STAGE2 ;;
            *) f=$ESP/$BUNDLE/$name ;;
        esac
        [ "$(sha "$f")" = "$want" ] || die "$f fails its SHA-256 on the ESP"
    done < "$SRC/$BUNDLE/SHA256SUMS"
    echo "installed and verified. Linux's boot is unchanged."
    echo "To boot macOS once: tools/esp-macos/arm_trial.sh --esp $ESP"
fi
