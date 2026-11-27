#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Boot macOS from the ESP on the next boot only: write the aura-try marker Stage 1 reads.
#
#   arm_trial.sh --esp <mountpoint> [--disarm]
#
# Stage 1 rewrites the marker's first line to "spent" on the disk and reads that back before it
# loads the image, so every later boot takes the configured Stage 2 (Linux) again, whatever macOS
# does. Separate from install_esp.sh on purpose: installing never selects anything.
set -eu

ESP=
DISARM=0
while [ $# -gt 0 ]; do
    case "$1" in
        --esp) ESP=$2; shift 2 ;;
        --disarm) DISARM=1; shift ;;
        *) echo "usage: $0 --esp <mountpoint> [--disarm]" >&2; exit 2 ;;
    esac
done
[ -n "$ESP" ] && [ -d "$ESP" ] || { echo "usage: $0 --esp <mountpoint> [--disarm]" >&2; exit 2; }
if [ "$DISARM" = 1 ]; then
    rm -f "$ESP/aura-try"
    sync
    echo "marker removed: the next boot takes the configured Stage 2"
    exit 0
fi
[ -f "$ESP/m1n1/stage2-macos.bin" ] && [ -f "$ESP/aurora/macos/manifest.txt" ] || {
    echo "no macOS bundle on $ESP: run install_esp.sh first" >&2
    exit 1
}
printf 'armed\nm1n1/stage2-macos.bin\n' > "$ESP/aura-try"
sync
echo "armed: the next boot, and only that one, starts macOS under aura"
