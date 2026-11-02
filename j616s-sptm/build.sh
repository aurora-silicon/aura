#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# build.sh KC IMAGE KBUILD OUT : monitor + Linux Image + guest DT + initrd, written into the user's own BootKC.
#   KC      kernelcache.release.mac16j (IM4P) or its decompressed Mach-O, macOS 26.7.1 25G241, from your Mac or Apple's IPSW
#   IMAGE   arch/arm64/boot/Image of the guest kernel (linux/build-kernel.sh)
#   KBUILD  that kernel's build directory (modules for the initrd)
#   OUT     output directory; OUT/rk.macho is the boot object for kmutil. It contains Apple's code: never share it.
set -euo pipefail
D=$(dirname "$(readlink -f "$0")"); KC=$1; IMG=$2; KB=$3; OUT=$(mkdir -p "$4" && readlink -f "$4")
[ -f "$D/guest/guest-platform.dtsi" ] || { echo "run tools/gen-guest-platform.py guest/guest-platform.dtsi on your J616s first (raw-boot Linux)" >&2; exit 1; }
python3 -I "$D/tools/check-io.py" "$D/monitor/platform-io.h"
dtc -q -i "$D/guest" -I dts -O dtb -o "$OUT/guest.dtb" "$D/guest/guest-native.dts"
find "$OUT" -maxdepth 1 -name "*.o" -delete
make -s -C "$D/monitor" B="$OUT" EXTRA="-DP_APIF -DP_FB -DP_WDT -DP_PLATFORM -DP_IOMMU -DP_GPU -DP_DCP ${EXTRA:-}" -j"$(nproc)"
"$D/initrd/mk-initrd.sh" "$KB" "$OUT/initrd.cpio"
python3 -I "$D/tools/kc_patch.py" "$KC" "$OUT/mon.elf" "$OUT/rk.macho" "$IMG" "$OUT/guest.dtb" "$OUT/initrd.cpio"
