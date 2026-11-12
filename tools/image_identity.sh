#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build the default Stage 1 and the ESP Stage 2 from this tree and from another tree with one
# toolchain, and fail unless every image is byte-identical. A maintenance branch uses it to
# show that its boot images are the release's (.github/workflows/maint-identity.yml):
#
#   tools/image_identity.sh <label> <other tree> [make arguments...]
#
# Both builds get the same version label (AURA_VERSION_TAG, default "identity"), so a
# difference is code or data, not the label. MAKE picks make.
set -u

label=$1
other=$2
shift 2
MAKE=${MAKE:-make}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
export AURA_VERSION_TAG=${AURA_VERSION_TAG:-identity}
out=${OUT:-image-identity}
here=$(pwd)
status=0

sum() {
	if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
	else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

build() {
	tree=$1 dest=$2
	shift 2
	mkdir -p "$dest"
	(
		cd "$tree" || exit 1
		$MAKE clean >/dev/null 2>&1
		$MAKE -j"$JOBS" RELEASE=1 CHAINLOADING=1 "$@" >"$dest/stage1.log" 2>&1 || exit 1
		cp build/aura.bin "$dest/stage1-aura-raw.bin"
		cp build/aura.macho "$dest/stage1-aura.macho"
		cp build/aura-boot.macho "$dest/stage1-aura-boot.macho"
		$MAKE clean >/dev/null 2>&1
		$MAKE -j"$JOBS" RELEASE=1 CHAINLOADING=1 ESP_STAGE2=1 "$@" >"$dest/stage2.log" 2>&1 || exit 1
		cp build/aura.bin "$dest/esp-aura.bin"
		$MAKE clean >/dev/null 2>&1
	) || { echo "$label: build failed in $tree:"; tail -5 "$dest"/stage*.log; exit 1; }
}

mkdir -p "$out"
out=$(cd "$out" && pwd)
build "$here" "$out/this" "$@"
build "$other" "$out/other" "$@"
for f in stage1-aura-raw.bin stage1-aura.macho stage1-aura-boot.macho esp-aura.bin; do
	a=$(sum "$out/this/$f") b=$(sum "$out/other/$f")
	if [ "$a" = "$b" ]; then
		echo "$label $f: identical $a"
	else
		echo "$label $f: DIFFERS this $a other $b"
		status=1
	fi
done
exit $status
