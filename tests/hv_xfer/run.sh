#!/bin/sh
# SPDX-License-Identifier: MIT
#
# HvXfer offline end-to-end replay. No hardware is touched.
#
# Compiles the REAL src/hv/windows/hv_xfer.c and src/hv/hv_vm.c against the
# REAL Aura headers (from a byte-identical copy of src/, sysinc/ and config.h
# whose build/build_cfg.h says AURA_WINDOWS, so the host build cannot pick up
# a stale firmware build_cfg.h), links them with the real guest driver core
# (AppleHvXferCore.c) and the replay glue into one dylib, and drives it from
# the real Python host stack (replay.py). Writes $OUT/result.json.
#
# Environment:
#   PYTHON           python with construct + pyserial (proxyclient deps)
#   HVXFER_GUEST_DIR AuroraSilicon drivers/AppleSerial directory (required)
#   HVXFER_OUT       output directory (default: tests/hv_xfer/out)
#   CC               clang (default: Homebrew LLVM clang, else clang)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
guest=${HVXFER_GUEST_DIR:?set HVXFER_GUEST_DIR to the AuroraSilicon drivers/AppleSerial directory}
out=${HVXFER_OUT:-$here/out}
python=${PYTHON:-python3}
if [ -z "${CC:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then CC=/opt/homebrew/opt/llvm/bin/clang; else CC=clang; fi
fi

"$python" -c 'import construct, serial' 2>/dev/null || {
    echo "run.sh: $python lacks construct/pyserial; set PYTHON (e.g. a proxyclient venv)" >&2
    exit 2
}
[ -f "$guest/AppleHvXferCore.c" ] || { echo "run.sh: guest driver not found at $guest" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/hvxfer-replay.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$work/tree/build"
cp -R "$repo/src" "$repo/sysinc" "$repo/config.h" "$work/tree/"
printf '#define AURA_WINDOWS\n#define BUILTIN_LOGO aurora\n' > "$work/tree/build/build_cfg.h"
echo '#define BUILD_TAG "hvxfer-replay"' > "$work/tree/build/build_tag.h"

# Firmware-equivalent flags for an LP64 host: same includes, freestanding,
# with uint64_t spelled `unsigned long` as on aarch64-linux-gnu so the
# firmware's %lx formats type-check exactly as they do in the real build.
aura_flags="-std=gnu11 -O1 -g -Wall -Wundef -Werror=strict-prototypes -Wsign-compare \
 -Wunused-parameter -Wno-multichar -ffreestanding -fno-common -fno-stack-protector -fPIC \
 -nostdinc -isystem $($CC -print-file-name=include) -isystem $work/tree/sysinc \
 -I$work/tree/src -I$work/tree/src/hv"
compile_aura() { # src obj [extra flags...]
    src=$1 obj=$2
    shift 2
    # shellcheck disable=SC2086 # aura_flags is a word list
    $CC $aura_flags -U__UINT64_TYPE__ "-D__UINT64_TYPE__=long unsigned int" \
        -U__INT64_TYPE__ "-D__INT64_TYPE__=long int" "$@" -c "$src" -o "$obj" 2> "$obj.log" ||
        { cat "$obj.log" >&2; exit 1; }
}

compile_aura "$work/tree/src/hv/windows/hv_xfer.c" "$work/hv_xfer.o" -Werror
compile_aura "$work/tree/src/hv/hv_vm.c" "$work/hv_vm.o"
compile_aura "$here/replay_glue.c" "$work/replay_glue.o" -Werror "-I$guest"
$CC -std=gnu11 -O1 -g -Wall -Wextra -Werror -fPIC -c "$here/replay_host.c" -o "$work/replay_host.o"
$CC -std=c11 -O1 -g -Wall -Wextra -fPIC -I"$guest" -c "$guest/AppleHvXferCore.c" -o "$work/guest_core.o"
$CC -dynamiclib -o "$work/libhvxfer_replay.dylib" "$work/hv_xfer.o" "$work/hv_vm.o" \
    "$work/replay_glue.o" "$work/replay_host.o" "$work/guest_core.o"

# The copy must be what the repo holds, byte for byte.
for f in src/hv/windows/hv_xfer.c src/hv/windows/hv_xfer.h src/hv/hv_vm.c src/hv/windows/hv.h; do
    cmp -s "$repo/$f" "$work/tree/$f" || { echo "run.sh: copy of $f differs" >&2; exit 1; }
done
warnings=$(cat "$work"/*.o.log | grep -c 'warning:' || true)

mkdir -p "$out"
status=0
"$python" "$here/replay.py" --lib "$work/libhvxfer_replay.dylib" --repo "$repo" --out "$out" \
    --guest-dir "$guest" --build-json "$here/out-build/build.json" 2> "$work/replay.stderr" || status=$?
cat "$work/replay.stderr" >&2
cp "$work/replay.stderr" "$out/replay.stderr"
cat "$work"/*.o.log > "$out/compile.log"
if [ ! -f "$out/result.json" ]; then
    # The process died under us -- an EL2 assert() aborts it, exactly as it
    # would panic the hypervisor. Record that as the verdict.
    "$python" - "$out" "$status" <<'EOF'
import json, sys
from pathlib import Path
out, status = Path(sys.argv[1]), int(sys.argv[2])
tail = (out / "replay.stderr").read_text(errors="replace").splitlines()[-12:]
el2 = [line for line in tail if "EL2 ASSERTION FAILED" in line]
(out / "result.json").write_text(json.dumps({
    "schema": "aura.hvxfer.replay.v1", "verdict": "crash", "exit_status": status,
    "failed": ["replay.el2_abort" if el2 else "replay.crashed"], "el2_assertion": el2[:1],
    "stderr_tail": tail, "hardware_accessed": False}, indent=2) + "\n")
EOF
    [ "$status" -ne 0 ] || status=1
fi
echo "host-build warnings in EL2 sources: $warnings (hv_xfer.c and glue are -Werror)"
exit $status
