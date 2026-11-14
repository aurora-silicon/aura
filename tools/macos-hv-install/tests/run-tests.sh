#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Pure-host test harness for the macOS-under-aura-HV installer.
#
# NO hardware, NO real Mac, NO network. Everything the installer would run on a Mac
# (uname/sysctl/diskutil/csrutil/findmnt/kmutil/bputil/md5/script) is replaced by a stub
# in tests/stubs/, prepended to PATH. Stubs log their invocations to $STUBLOG so a test
# can assert whether a *mutating* call (kmutil configure-boot) happened.
#
# Run from anywhere:  bash tests/run-tests.sh
# Exits non-zero if any test fails.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)                 # tools/macos-hv-install
TOOLS=$(cd "$ROOT/.." && pwd)                # tools/
STUBS="$HERE/stubs"
LIB="$ROOT/lib"

INSTALL="$ROOT/macos-hv-install"
UNINSTALL="$ROOT/macos-hv-uninstall"
INSTALL_ESP="$TOOLS/esp-macos/install_esp.sh"
ENROLL="$TOOLS/recovery/enroll-stage1.sh"
FETCH="$LIB/fetch_firmware.py"
SUPPORTED_PY="$LIB/supported.py"
SUPPORTED_JSON="$LIB/supported.json"

# PATH with the macOS stubs in front (uname -> Darwin, etc.).
MACPATH="$STUBS:$PATH"

PASS=0
FAIL=0

# mktmp writes the new dir to the global MKTMP (not stdout), because callers use it in a
# command substitution subshell where an appended array would be lost. Pattern:
#   base=$(mktmp)   # still works for the path
# plus we record it in TMPS from the subshell via a temp registry file.
MKTMP_REG=$(mktemp)
mktmp() { d=$(mktemp -d); printf '%s\n' "$d" >> "$MKTMP_REG"; printf '%s' "$d"; }
cleanup() {
    if [ -f "$MKTMP_REG" ]; then
        while read -r d; do [ -n "$d" ] && rm -rf "$d"; done < "$MKTMP_REG"
        rm -f "$MKTMP_REG"
    fi
}
trap cleanup EXIT

# report NAME  (reads $ok, $why)
report() {
    if [ "${ok:-0}" = 1 ]; then
        printf 'PASS  %s\n' "$1"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %s\n' "$1"
        [ -n "${why:-}" ] && printf '        %s\n' "$why"
        FAIL=$((FAIL + 1))
    fi
}

sha256() { sha256sum "$1" | cut -d' ' -f1; }

# -------------------------------------------------------------------------------------
# Fixture builders
# -------------------------------------------------------------------------------------

# A fetch_firmware fixture root: $1=dir, $2="exclave"|"noexclave" controls whether the
# exclave pieces are present. Returns via the created tree.
make_fw_fixture() {
    root=$1
    group="11111111-2222-3333-4444-555555555555"
    printf '%s\n' "$group" > "$root/.hvi-group"
    printf 'ADT-FIXTURE-BYTES' > "$root/.hvi-adt"
    env="$root/System/Volumes/Preboot/$group/boot/ABCDEF0123"
    mkdir -p "$env/usr/standalone/firmware/t6041"
    mkdir -p "$env/System/Library/Caches/com.apple.kernelcaches"
    printf 'SPTM-IMG4-BYTES' > "$env/usr/standalone/firmware/t6041/sptm.img4"
    printf 'TXM-IMG4-BYTES'  > "$env/usr/standalone/firmware/t6041/txm.img4"
    printf 'TRUSTCACHE-BYTES' > "$env/StaticTrustCache.img4"
    # kernelcache must be a Mach-O (magic cf fa ed fe) so decompress_kernelcache copies it.
    printf '\xcf\xfa\xed\xfe\x00\x00\x00\x00KERNELCACHEBODY' \
        > "$env/System/Library/Caches/com.apple.kernelcaches/kernelcache.release.mac"
    if [ "$2" = exclave ]; then
        printf 'EXCLAVE-BUNDLE'  > "$env/exclavecore_bundle.im4p"
        printf 'EXCLAVE-INTEG'   > "$env/foo.integrity_catalog"
        printf 'EXCLAVE-TC'      > "$env/foo.aea.trustcache"
    fi
}

# A valid install_esp.sh staging tree at $1/staging for PARTUUID $2.
# Hand-crafted (make_bundle.py needs real firmware). Verified below by test 5.
make_staging() {
    base=$1
    partuuid=$2
    stg="$base/staging"
    mkdir -p "$stg/aurora/macos" "$stg/m1n1"
    # A few small bundle files.
    printf 'SPTM-PAYLOAD'       > "$stg/aurora/macos/sptm.bin"
    printf 'TXM-PAYLOAD'        > "$stg/aurora/macos/txm.bin"
    printf 'TRUSTCACHE-PAYLOAD' > "$stg/aurora/macos/trustcache.bin"
    printf 'KERNELCACHE-PAYLOAD' > "$stg/aurora/macos/kernelcache"
    printf 'the bundle manifest\n' > "$stg/aurora/macos/manifest.txt"

    msha=$(sha256 "$stg/aurora/macos/manifest.txt")

    # stage2-macos.bin: payload + config line appended with NO trailing newline, within
    # the last 256 bytes. install_esp.sh reads last 256 bytes, strips NULs, greps macos=.
    {
        printf 'STAGE2-M1N1-PAYLOAD-BYTES'
        printf 'macos=%s;aurora/macos;%s' "$partuuid" "$msha"
    } > "$stg/m1n1/stage2-macos.bin"
    s2sha=$(sha256 "$stg/m1n1/stage2-macos.bin")

    # SHA256SUMS: every file in aurora/macos/ (two spaces), then the stage2 line.
    : > "$stg/aurora/macos/SHA256SUMS"
    for f in sptm.bin txm.bin trustcache.bin kernelcache manifest.txt; do
        printf '%s  %s\n' "$(sha256 "$stg/aurora/macos/$f")" "$f" >> "$stg/aurora/macos/SHA256SUMS"
    done
    printf '%s  %s\n' "$s2sha" "../../m1n1/stage2-macos.bin" >> "$stg/aurora/macos/SHA256SUMS"
}

# Plant decoy Linux boot files in an ESP dir $1. These must NEVER be touched.
plant_linux_decoys() {
    esp=$1
    mkdir -p "$esp/m1n1" "$esp/aurora-linux" "$esp/loader/entries" "$esp/EFI/BOOT"
    printf 'LINUX-HV-BOOT-STAGE2' > "$esp/m1n1/hv_boot.bin"
    printf 'LINUX-KERNEL-IMAGE'   > "$esp/aurora-linux/vmlinuz"
    printf 'title Aurora Linux'   > "$esp/loader/entries/aurora.conf"
    printf 'EFI-BOOT-STUB'        > "$esp/EFI/BOOT/BOOTAA64.EFI"
}

# Snapshot decoy shas into globals DECOY_SHAS (newline list of "path\tsha").
decoy_shas() {
    esp=$1
    for rel in m1n1/hv_boot.bin aurora-linux/vmlinuz loader/entries/aurora.conf EFI/BOOT/BOOTAA64.EFI; do
        printf '%s\t%s\n' "$rel" "$(sha256 "$esp/$rel")"
    done
}

# Assert the decoys in $1 still match the snapshot $2 (file). Sets $ok/$why.
assert_decoys_intact() {
    esp=$1; snap=$2
    ok=1; why=
    while IFS=$'\t' read -r rel want; do
        [ -n "$rel" ] || continue
        if [ ! -f "$esp/$rel" ]; then ok=0; why="decoy $rel was deleted"; return; fi
        got=$(sha256 "$esp/$rel")
        if [ "$got" != "$want" ]; then ok=0; why="decoy $rel changed"; return; fi
    done < "$snap"
}

# -------------------------------------------------------------------------------------
# Test 1: supported.py mapping
# -------------------------------------------------------------------------------------
test_supported() {
    ok=1; why=
    out=$(python3 "$SUPPORTED_PY" "$SUPPORTED_JSON" "Mac16,7") || { ok=0; why="exit nonzero"; }
    echo "$out" | grep -q '^HVI_SUPPORTED=1$'   || { ok=0; why="Mac16,7 not SUPPORTED=1: $out"; }
    echo "$out" | grep -q "^HVI_SOC=T6041$"     || { ok=0; why="Mac16,7 SOC not T6041: $out"; }
    echo "$out" | grep -q '^HVI_EXCLAVES=0$'    || { ok=0; why="Mac16,7 EXCLAVES not 0: $out"; }
    out2=$(python3 "$SUPPORTED_PY" "$SUPPORTED_JSON" "Mac99,99") || { ok=0; why="unknown exit nonzero"; }
    echo "$out2" | grep -q '^HVI_SUPPORTED=0$'  || { ok=0; why="unknown model not SUPPORTED=0: $out2"; }
    report "supported.py: Mac16,7 -> T6041/exclaves=0/supported; unknown -> unsupported"
}

# -------------------------------------------------------------------------------------
# Test 2: preflight refuses on a real Linux host (no uname stub)
# -------------------------------------------------------------------------------------
test_preflight_linux() {
    ok=1; why=
    # Deliberately use the real PATH (no stubs), so uname -> Linux.
    out=$(PATH="$PATH" "$INSTALL" preflight 2>&1); rc=$?
    [ "$rc" -ne 0 ] || { ok=0; why="expected nonzero exit on Linux, got 0"; }
    echo "$out" | grep -q 'run the installer from macOS' ||
        { ok=0; why="missing 'run the installer from macOS': $out"; }
    report "preflight refuses on a real Linux host"
}

# -------------------------------------------------------------------------------------
# Test 3: preflight with macOS stubs reaches the supported-model line for Mac16,7.
#
# LIMITATION: preflight calls `macos_build /`, which reads
#   /System/Library/CoreServices/SystemVersion.plist
# at the literal path "/". On a Linux buildbox that file does not exist, so preflight
# will die at the build-read step. There is no override for that path, so we assert only
# up to the supported-model line ("Mac16,7 is M4 Pro (T6041)"), which proves host_os=macos,
# is_apple_silicon, mac_model and supported.py all passed under the stubs.
# -------------------------------------------------------------------------------------
test_preflight_macos() {
    ok=1; why=
    out=$(PATH="$MACPATH" HVI_TEST_MODEL="Mac16,7" "$INSTALL" preflight 2>&1); rc=$?
    echo "$out" | grep -q 'running on macOS'   || { ok=0; why="did not pass host_os=macos: $out"; }
    echo "$out" | grep -q 'Apple Silicon'      || { ok=0; why="did not pass is_apple_silicon: $out"; }
    echo "$out" | grep -q 'model: Mac16,7'     || { ok=0; why="model line missing: $out"; }
    echo "$out" | grep -q 'Mac16,7 is M4 Pro (T6041)' ||
        { ok=0; why="supported-model line missing: $out"; }
    report "preflight (macOS stubs) accepts Mac16,7 up to the supported-model line"
}

# -------------------------------------------------------------------------------------
# Test 4: fetch_firmware.py
# -------------------------------------------------------------------------------------
test_fetch_dryrun() {
    ok=1; why=
    base=$(mktmp); root="$base/root"; mkdir -p "$root"
    make_fw_fixture "$root" noexclave
    work="$base/work"
    out=$(HVI_FAKE_ROOT="$root" python3 "$FETCH" --work "$work" --no-exclave --dry-run 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="dry-run exit $rc: $out"; }
    echo "$out" | grep -q 'sptm:'        || { ok=0; why="no sptm provenance: $out"; }
    echo "$out" | grep -q 'txm:'         || { ok=0; why="no txm provenance: $out"; }
    echo "$out" | grep -q 'trustcache:'  || { ok=0; why="no trustcache provenance: $out"; }
    echo "$out" | grep -q 'xnu'          || { ok=0; why="no xnu provenance: $out"; }
    echo "$out" | grep -qi 'adt'         || { ok=0; why="no adt provenance: $out"; }
    [ ! -d "$work" ] || { ok=0; why="dry-run created the work dir $work"; }
    report "fetch_firmware.py --dry-run finds all pieces, creates no output dir"
}

test_fetch_real() {
    ok=1; why=
    base=$(mktmp); root="$base/root"; mkdir -p "$root"
    make_fw_fixture "$root" noexclave
    work="$base/work"
    out=$(HVI_FAKE_ROOT="$root" python3 "$FETCH" --work "$work" --no-exclave 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="real exit $rc: $out"; }
    for f in sptm.bin txm.bin trustcache.bin kernelcache.macho live.adt; do
        [ -f "$work/$f" ] || { ok=0; why="missing $work/$f: $out"; }
    done
    # kernelcache.macho must be a Mach-O copy.
    if [ -f "$work/kernelcache.macho" ]; then
        magic=$(head -c4 "$work/kernelcache.macho" | od -An -tx1 | tr -d ' \n')
        [ "$magic" = "cffaedfe" ] || { ok=0; why="kernelcache.macho not Mach-O (magic $magic)"; }
    fi
    report "fetch_firmware.py (real) copies pieces; kernelcache.macho is a Mach-O"
}

test_fetch_missing_sptm() {
    ok=1; why=
    base=$(mktmp); root="$base/root"; mkdir -p "$root"
    make_fw_fixture "$root" noexclave
    group="11111111-2222-3333-4444-555555555555"
    # Remove the sptm candidate so the search fails.
    rm -f "$root/System/Volumes/Preboot/$group/boot/ABCDEF0123/usr/standalone/firmware/t6041/sptm.img4"
    work="$base/work"
    out=$(HVI_FAKE_ROOT="$root" python3 "$FETCH" --work "$work" --no-exclave 2>&1); rc=$?
    [ "$rc" -ne 0 ] || { ok=0; why="expected nonzero when sptm missing, got 0: $out"; }
    echo "$out" | grep -q -- '--sptm' || { ok=0; why="error does not mention --sptm: $out"; }
    report "fetch_firmware.py errors (mentioning --sptm) when sptm is missing"
}

test_fetch_exclave_needs_bi() {
    ok=1; why=
    base=$(mktmp); root="$base/root"; mkdir -p "$root"
    make_fw_fixture "$root" exclave
    work="$base/work"
    # Exclave mode (no --no-exclave) with no --build-identity -> error.
    out=$(HVI_FAKE_ROOT="$root" python3 "$FETCH" --work "$work" 2>&1); rc=$?
    [ "$rc" -ne 0 ] || { ok=0; why="expected nonzero in exclave mode w/o build-identity: $out"; }
    echo "$out" | grep -Eqi 'build-identity|--no-exclave' ||
        { ok=0; why="error does not mention build-identity / --no-exclave: $out"; }
    report "fetch_firmware.py (exclave mode) errors without --build-identity"
}

# -------------------------------------------------------------------------------------
# Test 5: install_esp.sh via a crafted staging tree; Linux decoys untouched.
# -------------------------------------------------------------------------------------
test_install_esp() {
    ok=1; why=
    partuuid="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    base=$(mktmp)
    make_staging "$base" "$partuuid"
    esp="$base/esp"; mkdir -p "$esp"
    plant_linux_decoys "$esp"
    snap="$base/decoys.snap"; decoy_shas "$esp" > "$snap"

    # Dry-run: writes nothing to the ESP bundle.
    out=$(PATH="$MACPATH" HVI_TEST_PARTUUID="$partuuid" \
        sh "$INSTALL_ESP" --esp "$esp" --partuuid "$partuuid" --dry-run "$base/staging" 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="dry-run exit $rc: $out"; }
    echo "$out" | grep -q 'would copy' || { ok=0; why="dry-run did not report 'would copy': $out"; }
    [ ! -e "$esp/aurora/macos/sptm.bin" ] || { ok=0; why="dry-run wrote bundle files"; }
    [ ! -e "$esp/m1n1/stage2-macos.bin" ] || { ok=0; why="dry-run wrote the Stage 2"; }
    assert_decoys_intact "$esp" "$snap"; [ "$ok" = 1 ] || { report "install_esp.sh: dry-run writes nothing + decoys intact; real install copies+verifies"; return; }

    # Real install: copies + verifies.
    out=$(PATH="$MACPATH" HVI_TEST_PARTUUID="$partuuid" \
        sh "$INSTALL_ESP" --esp "$esp" --partuuid "$partuuid" "$base/staging" 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="real exit $rc: $out"; }
    echo "$out" | grep -q 'installed and verified' || { ok=0; why="no 'installed and verified': $out"; }
    for f in sptm.bin txm.bin trustcache.bin kernelcache manifest.txt SHA256SUMS; do
        [ -f "$esp/aurora/macos/$f" ] || { ok=0; why="missing $esp/aurora/macos/$f after install"; }
    done
    [ -f "$esp/m1n1/stage2-macos.bin" ] || { ok=0; why="missing stage2 after install"; }
    assert_decoys_intact "$esp" "$snap"
    report "install_esp.sh: dry-run writes nothing + decoys intact; real install copies+verifies"
}

# -------------------------------------------------------------------------------------
# Test 6: orchestrator stage-esp --dry-run writes nothing to the ESP.
# -------------------------------------------------------------------------------------
test_stage_esp_dryrun() {
    ok=1; why=
    partuuid="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    base=$(mktmp)
    make_staging "$base" "$partuuid"
    esp="$base/esp"; mkdir -p "$esp"
    plant_linux_decoys "$esp"
    snap="$base/decoys.snap"; decoy_shas "$esp" > "$snap"
    out=$(PATH="$MACPATH" HVI_TEST_PARTUUID="$partuuid" \
        "$INSTALL" --work "$base" --esp "$esp" --partuuid "$partuuid" --dry-run stage-esp 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="exit $rc: $out"; }
    echo "$out" | grep -q 'would copy' || { ok=0; why="no install_esp dry-run 'would copy': $out"; }
    [ ! -e "$esp/aurora/macos/sptm.bin" ] || { ok=0; why="dry-run wrote bundle files"; }
    [ ! -e "$esp/m1n1/stage2-macos.bin" ] || { ok=0; why="dry-run wrote the Stage 2"; }
    assert_decoys_intact "$esp" "$snap"
    report "macos-hv-install stage-esp --dry-run runs install_esp dry-run, writes nothing"
}

# -------------------------------------------------------------------------------------
# Test 7: the enroll path with the macOS+bputil+kmutil+md5 stubs.
#   --check  -> every check runs, kmutil inspects the object, NO configure-boot.
#   real     -> kmutil configure-boot IS logged (the mutating call).
# The orchestrator computes the md5 itself via the md5 stub (asserted separately).
#
# test_enroll drives the wrapped tool enroll-stage1.sh directly (options before the
# VOLUME OBJECT MD5 positionals); test_enroll_orchestrator drives the full orchestrator
# `enroll` subcommand to prove its do_enroll builds those args correctly.
# -------------------------------------------------------------------------------------
test_enroll() {
    ok=1; why=
    base=$(mktmp)
    obj="$base/stage1.macho"
    printf 'SELF-BOOT-STAGE1-OBJECT-BYTES' > "$obj"
    md5=$(PATH="$MACPATH" md5 -q "$obj")
    log="$base/stub.log"
    vol="$base/MacintoshHD"; mkdir -p "$vol"

    # --check (what --dry-run maps to): no configure-boot.
    : > "$log"
    out=$(PATH="$MACPATH" STUBLOG="$log" \
        bash "$ENROLL" --no-rollback --compress --check "$vol" "$obj" "$md5" 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="--check exit $rc: $out"; }
    grep -q 'kmutil inspect' "$log" || { ok=0; why="--check did not inspect the object: $(cat "$log")"; }
    if grep -q 'kmutil configure-boot' "$log"; then
        ok=0; why="--check ran configure-boot (should not): $(grep configure-boot "$log")"
    fi
    [ "$ok" = 1 ] || { report "enroll (--check) inspects, no configure-boot; real enroll runs configure-boot"; return; }

    # Real enroll; enroll-stage1.sh still prompts "type yes" -> feed on stdin.
    : > "$log"
    out=$(printf 'yes\n' | PATH="$MACPATH" STUBLOG="$log" \
        bash "$ENROLL" --no-rollback --compress "$vol" "$obj" "$md5" 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="real enroll exit $rc: $out"; }
    grep -q 'kmutil configure-boot' "$log" ||
        { ok=0; why="real enroll did NOT run configure-boot: $(cat "$log")"; }
    report "enroll (--check) inspects, no configure-boot; real enroll runs configure-boot"
}

# The orchestrator's `enroll` subcommand must build enroll-stage1.sh's args correctly
# (options --compress --no-rollback [--check] BEFORE the VOLUME OBJECT MD5 positionals) and
# compute the object's md5 itself. --dry-run -> --check (no configure-boot); real -> it.
test_enroll_orchestrator() {
    ok=1; why=
    base=$(mktmp)
    obj="$base/stage1.macho"
    printf 'SELF-BOOT-STAGE1-OBJECT-BYTES' > "$obj"
    log="$base/stub.log"
    vol="$base/MacintoshHD"; mkdir -p "$vol"   # absolute, so do_enroll uses it verbatim

    : > "$log"
    out=$(PATH="$MACPATH" STUBLOG="$log" \
        "$INSTALL" --volume "$vol" --self-boot-object "$obj" --work "$base/w" --dry-run enroll 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="orchestrator enroll --dry-run exit $rc: $out"; }
    if grep -q 'kmutil configure-boot' "$log"; then
        ok=0; why="orchestrator enroll --dry-run ran configure-boot (should not)"
    fi
    [ "$ok" = 1 ] || { report "orchestrator enroll: --dry-run checks only; real runs configure-boot"; return; }

    : > "$log"
    out=$(printf 'yes\n' | PATH="$MACPATH" STUBLOG="$log" \
        "$INSTALL" --volume "$vol" --self-boot-object "$obj" --work "$base/w" enroll 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="orchestrator enroll exit $rc: $out"; }
    grep -q 'kmutil configure-boot' "$log" ||
        { ok=0; why="orchestrator enroll did NOT run configure-boot: $(cat "$log")"; }
    report "orchestrator enroll: --dry-run checks only; real runs configure-boot"
}

# -------------------------------------------------------------------------------------
# Test 8: uninstall clean-esp removes only bundle files, Linux decoys intact.
# -------------------------------------------------------------------------------------
test_clean_esp() {
    ok=1; why=
    base=$(mktmp)
    esp="$base/esp"; mkdir -p "$esp/aurora/macos" "$esp/m1n1"
    # Bundle files to be removed.
    printf 'x' > "$esp/aurora/macos/sptm.bin"
    printf 'x' > "$esp/aurora/macos/manifest.txt"
    printf 'x' > "$esp/m1n1/stage2-macos.bin"
    # Linux decoys (plant_linux_decoys also makes m1n1/, which must survive via hv_boot.bin).
    plant_linux_decoys "$esp"
    snap="$base/decoys.snap"; decoy_shas "$esp" > "$snap"

    # Dry-run removes nothing.
    out=$(PATH="$MACPATH" "$UNINSTALL" --esp "$esp" --dry-run clean-esp 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="dry clean-esp exit $rc: $out"; }
    [ -f "$esp/aurora/macos/sptm.bin" ] || { ok=0; why="dry clean-esp removed bundle file"; }
    [ -f "$esp/m1n1/stage2-macos.bin" ] || { ok=0; why="dry clean-esp removed stage2"; }
    assert_decoys_intact "$esp" "$snap"; [ "$ok" = 1 ] || { report "uninstall clean-esp: dry removes nothing; real removes only bundle, Linux intact"; return; }

    # Real clean-esp removes only the bundle; Linux decoys byte-identical.
    out=$(PATH="$MACPATH" "$UNINSTALL" --esp "$esp" clean-esp 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="real clean-esp exit $rc: $out"; }
    [ ! -e "$esp/aurora/macos" ] || { ok=0; why="aurora/macos survived clean-esp"; }
    [ ! -e "$esp/m1n1/stage2-macos.bin" ] || { ok=0; why="stage2 survived clean-esp"; }
    assert_decoys_intact "$esp" "$snap"
    report "uninstall clean-esp: dry removes nothing; real removes only bundle, Linux intact"
}

# -------------------------------------------------------------------------------------
# Test 9: uninstall un-enrol (dry -> no configure-boot; real -> configure-boot -v).
# -------------------------------------------------------------------------------------
test_un_enrol() {
    ok=1; why=
    base=$(mktmp)
    log="$base/stub.log"
    vol="$base/Volumes-System"; mkdir -p "$vol"   # SYS must be a directory

    # -y so the confirm() prompt (not gated by --dry-run) does not block on stdin.
    : > "$log"
    out=$(PATH="$MACPATH" STUBLOG="$log" \
        "$UNINSTALL" --volume "$vol" -y --dry-run un-enrol 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="dry un-enrol exit $rc: $out"; }
    if grep -q 'kmutil configure-boot' "$log"; then
        ok=0; why="dry un-enrol ran configure-boot (should not): $(grep configure-boot "$log")"
    fi
    [ "$ok" = 1 ] || { report "uninstall un-enrol: dry = no configure-boot; real = configure-boot -v (no HV needed)"; return; }

    : > "$log"
    out=$(printf 'yes\n' | PATH="$MACPATH" STUBLOG="$log" \
        "$UNINSTALL" --volume "$vol" -y un-enrol 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { ok=0; why="real un-enrol exit $rc: $out"; }
    grep -q "kmutil configure-boot -v $vol" "$log" ||
        { ok=0; why="real un-enrol did NOT run 'configure-boot -v $vol': $(cat "$log")"; }
    report "uninstall un-enrol: dry = no configure-boot; real = configure-boot -v (no HV needed)"
}

# -------------------------------------------------------------------------------------
echo "macos-hv-install host test harness"
echo "  installer: $INSTALL"
echo

test_supported
test_preflight_linux
test_preflight_macos
test_fetch_dryrun
test_fetch_real
test_fetch_missing_sptm
test_fetch_exclave_needs_bi
test_install_esp
test_stage_esp_dryrun
test_enroll
test_enroll_orchestrator
test_clean_esp
test_un_enrol

echo
echo "-----------------------------------------"
echo "  $PASS passed, $FAIL failed, $((PASS + FAIL)) total"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
