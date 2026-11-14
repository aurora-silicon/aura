#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Bootstrap for "macOS under the aura hypervisor". Run from macOS:
#
#   curl -fsSL https://<release-host>/macos-hv/install.sh | sh
#
# It downloads the aura macos-hv toolkit for this Mac's SoC (the installer scripts, this
# aura release's ESP Stage 2, the self-boot Stage 1 object, and the auractl DCC tool),
# then runs tools/macos-hv-install/macos-hv-install. It downloads only OUR code and this
# Mac's matching aura build; the macOS firmware that goes into the bundle is read from
# THIS Mac, never downloaded. Nothing Apple-signed is redistributed.
#
# Configure with environment variables:
#   HVI_BASE_URL   release host base (default below)
#   HVI_CHANNEL    release channel: stable (default) | testing
#   HVI_PARTUUID   the ESP PARTUUID to write the bundle to (else you are prompted)
#   HVI_ESP        the ESP mount point (optional)
#   HVI_WORK       work directory (default a temp dir)
#   HVI_DRY_RUN=1  pass --dry-run to the installer
set -eu

BASE_URL=${HVI_BASE_URL:-https://releases.aurorasilicon.org}
CHANNEL=${HVI_CHANNEL:-stable}

err() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || err "run this from macOS"
[ "$(uname -m)" = arm64 ] || err "this is for Apple Silicon Macs"
command -v curl >/dev/null 2>&1 || err "curl is required"

model=$(sysctl -n hw.product 2>/dev/null || sysctl -n hw.model 2>/dev/null || echo "")
[ -n "$model" ] || err "could not read the Mac model"

work=${HVI_WORK:-$(mktemp -d /tmp/macos-hv.XXXXXX)}
mkdir -p "$work"
echo "==> downloading the macos-hv toolkit for $model ($CHANNEL) into $work" >&2

url="$BASE_URL/macos-hv/$CHANNEL/macos-hv-$model.tar.gz"
if ! curl -fsSL "$url" -o "$work/toolkit.tar.gz"; then
    err "no toolkit for $model at $url
This Mac may not be supported yet. See docs/macos-hv-install.md for the supported list,
or clone aurora-silicon/aura and run tools/macos-hv-install/macos-hv-install directly."
fi
tar xzf "$work/toolkit.tar.gz" -C "$work" || err "could not unpack the toolkit"

# The toolkit unpacks an aura tree with the installer, this SoC's ESP Stage 2, the
# self-boot object, the auractl tool, and a manifest naming them.
root="$work"
[ -d "$work/tools/macos-hv-install" ] || root=$(find "$work" -maxdepth 2 -type d -name macos-hv-install | head -n1 | xargs -I{} dirname {} 2>/dev/null | xargs -I{} dirname {} 2>/dev/null)
[ -n "$root" ] && [ -x "$root/tools/macos-hv-install/macos-hv-install" ] || err "the toolkit is missing the installer"

# release.env (shipped in the toolkit) sets STAGE2 / SELF_BOOT_OBJECT / AURACTL_TOOL paths.
# shellcheck disable=SC1091
[ -f "$root/release.env" ] && . "$root/release.env"

set -- all --work "$work/run"
[ "${HVI_DRY_RUN:-0}" = 1 ] && set -- --dry-run "$@"
[ -n "${HVI_PARTUUID:-}" ] && set -- --partuuid "$HVI_PARTUUID" "$@"
[ -n "${HVI_ESP:-}" ] && set -- --esp "$HVI_ESP" "$@"
[ -n "${STAGE2:-}" ] && set -- --stage2 "$root/$STAGE2" "$@"
[ -n "${SELF_BOOT_OBJECT:-}" ] && set -- --self-boot-object "$root/$SELF_BOOT_OBJECT" "$@"
[ -n "${AURACTL_TOOL:-}" ] && set -- --auractl-tool "$root/$AURACTL_TOOL" "$@"

echo "==> running: macos-hv-install $*" >&2
exec "$root/tools/macos-hv-install/macos-hv-install" "$@"
