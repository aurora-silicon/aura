# SPDX-License-Identifier: MIT
# shellcheck shell=sh
#
# Shared helpers for the macOS-under-aura-HV installer. POSIX sh only (macOS /bin/sh
# and 1TR recoveryOS both have a limited shell). Source this, do not run it.
#
# Every helper here either prints, computes or checks: nothing here writes to a disk,
# an ESP or NVRAM. The steps that change the machine live in the orchestrator and the
# tools it wraps (install_esp.sh, enroll-stage1.sh), each with its own dry run.

# ---- output -------------------------------------------------------------------------

# Colour only on a terminal; never in logs or pipes.
if [ -t 2 ] && [ -z "${NO_COLOR:-}" ]; then
    _c_bold=$(printf '\033[1m'); _c_red=$(printf '\033[31m')
    _c_grn=$(printf '\033[32m'); _c_yel=$(printf '\033[33m'); _c_rst=$(printf '\033[0m')
else
    _c_bold=; _c_red=; _c_grn=; _c_yel=; _c_rst=
fi

say()  { printf '%s\n' "$*"; }
step() { printf '%s==> %s%s\n' "$_c_bold" "$*" "$_c_rst" >&2; }
info() { printf '    %s\n' "$*" >&2; }
ok()   { printf '    %sok%s  %s\n' "$_c_grn" "$_c_rst" "$*" >&2; }
warn() { printf '    %swarning%s %s\n' "$_c_yel" "$_c_rst" "$*" >&2; }
die()  { printf '%smacos-hv-install: %s%s\n' "$_c_red" "$*" "$_c_rst" >&2; exit 1; }

# A line the user is meant to copy. Indented so it stands out, never coloured.
cmd() { printf '      %s\n' "$*" >&2; }

# ---- tool detection -----------------------------------------------------------------

have() { command -v "$1" >/dev/null 2>&1; }

require_tools() {
    missing=
    for t in "$@"; do have "$t" || missing="$missing $t"; done
    [ -z "$missing" ] || die "missing required tool(s):$missing"
}

# sha256sum (Linux) or shasum -a 256 (macOS); both print "<hex>  <name>".
sha256() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# md5 as enroll-stage1.sh wants it (lowercase 32 hex): macOS `md5 -q`, Linux `md5sum`.
md5hex() {
    if have md5; then md5 -q "$1"
    elif have md5sum; then md5sum "$1" | cut -d' ' -f1
    else die "no md5 or md5sum available"; fi
}

# ---- platform ----------------------------------------------------------------------

# "macos", "linux" or "unknown". recoveryOS reports as macos.
host_os() {
    case "$(uname -s 2>/dev/null)" in
        Darwin) echo macos ;;
        Linux)  echo linux ;;
        *)      echo unknown ;;
    esac
}

# True on Apple Silicon. On macOS, arm64 + an Apple CPU brand.
is_apple_silicon() {
    [ "$(uname -m 2>/dev/null)" = arm64 ] || [ "$(uname -m 2>/dev/null)" = aarch64 ] || return 1
    if have sysctl; then
        case "$(sysctl -n machdep.cpu.brand_string 2>/dev/null)" in
            *Apple*) return 0 ;;
            "") return 0 ;;   # recoveryOS may not expose it; arm64 Mac is enough
            *) return 1 ;;
        esac
    fi
    return 0
}

# The Mac model identifier, e.g. "Mac16,7". macOS only.
mac_model() {
    if have sysctl; then sysctl -n hw.product 2>/dev/null || sysctl -n hw.model 2>/dev/null
    else echo ""; fi
}

# The SoC marketing/board hint, e.g. "T6041". Best effort from the model DB.
# (kept out of here; preflight.sh maps model -> soc from supported.json)

# The installed macOS build, e.g. "26A428", read from a System volume's plist.
# Arg: the System volume mount point (default /).
macos_build() {
    plist=${1:-/}/System/Library/CoreServices/SystemVersion.plist
    [ -f "$plist" ] || { echo ""; return; }
    # No PlistBuddy in 1TR; pull the string that follows ProductBuildVersion.
    sed -n '/<key>ProductBuildVersion<\/key>/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$plist"
}

# ---- dry run helper -----------------------------------------------------------------
# run "<description>" command args...   — prints under --dry-run, runs otherwise.
# DRY is set by the orchestrator (1 = dry run).
run() {
    desc=$1; shift
    if [ "${DRY:-0}" = 1 ]; then
        printf '    %swould%s %s\n' "$_c_yel" "$_c_rst" "$desc" >&2
        printf '          %s\n' "$*" >&2
        return 0
    fi
    "$@"
}

# The directory this file lives in, so the orchestrator can find its siblings and the
# wrapped tools. Works when sourced from the orchestrator.
self_dir() {
    d=$(dirname "$0")
    case "$d" in /*) echo "$d" ;; *) echo "$(pwd)/$d" ;; esac
}
