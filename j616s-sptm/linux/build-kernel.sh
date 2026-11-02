#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# build-kernel.sh SRC BUILD : the guest kernel. SRC is a checkout of aurora-silicon/linux at
# 50ba71c552f1 (integration/aurora) with linux/00*.patch applied in order (git am, then git apply
# for the WIP diff). Rust 1.99.0 and bindgen 0.73 were used for the image that ran on hardware.
set -euo pipefail
S=$(readlink -f "$1"); B=$(readlink -f "$2"); D=$(dirname "$(readlink -f "$0")")
mkdir -p "$B"
[ -f "$B/.config" ] || cp "$D/config-7.3.0-m4.macho" "$B/.config"
export LOCALVERSION=.macho
cd "$S"
make O="$B" olddefconfig
make O="$B" -j"$(nproc)" Image modules
ls -l "$B/arch/arm64/boot/Image"
