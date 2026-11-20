#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Link every Stage 1 flavour with one toolchain and check how much room each has left before
# the DCP log buffer (aura-raw.ld, tools/stage1_room.py). CI runs it with each distribution's
# toolchain (.github/workflows/stage1-size.yml):
#
#   tools/stage1_size.sh <label> [make arguments...]
#
# MAKE, NM and PYTHON pick the tools. Each build starts from make clean. A flavour fails when it
# does not link or has less headroom than its floor, and gets a warning below WARN bytes. The
# lab-only dev flavour with SECBOOT is reported but not required to fit.
set -u

label=$1
shift
MAKE=${MAKE:-make}
PYTHON=${PYTHON:-python3}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
WARN=${WARN:-8192}
status=0

check() {
	name=$1 floor=$2 required=$3
	shift 3
	$MAKE clean >/dev/null 2>&1
	log=stage1-size-$name.log
	if ! $MAKE -j"$JOBS" RELEASE=1 CHAINLOADING=1 "$@" >"$log" 2>&1; then
		echo "$label $name: does not link:"
		grep -E 'error|Error' "$log" | grep -v '^  CC' | head -5
		[ "$required" = yes ] && status=1
		return
	fi
	if ! $PYTHON tools/stage1_room.py build/aura-raw.elf --label "$label $name" --min "$floor" \
		${NM:+--nm "$NM"}; then
		[ "$required" = yes ] && status=1
	elif ! $PYTHON tools/stage1_room.py build/aura-raw.elf --min "$WARN" ${NM:+--nm "$NM"} \
		>/dev/null 2>&1; then
		# A GitHub Actions annotation; plain text elsewhere.
		echo "::warning::$label $name: Stage 1 headroom is below $WARN B"
	fi
}

# The Stage 1 that ships (dev flavour, no signature checks): fails below 4 KiB of headroom, and
# warns below WARN.
check stage1 4096 yes "$@"
# The secure flavour that aurora-secboot fills with the owner's key.
check stage1-secure-audit 8192 yes "$@" SECBOOT=audit SECBOOT_FLAVOUR=secure
check stage1-secure-enforce 8192 yes "$@" SECBOOT=enforce SECBOOT_FLAVOUR=secure
# Lab builds: signature checks with the USB host windows kept.
check stage1-dev-audit 0 no "$@" SECBOOT=audit

$MAKE clean >/dev/null 2>&1
exit $status
