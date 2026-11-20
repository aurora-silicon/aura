# SPDX-License-Identifier: MIT
# shellcheck shell=sh
#
# Preflight checks for the macOS-under-aura-HV installer. Reads nothing it can change.
# Sourced by the orchestrator, which has already sourced common.sh and set:
#   LIBDIR          this directory
#   SUPPORTED_JSON  the supported list
#   ALLOW_UNQUAL    1 to proceed on an unqualified SoC
# Sets, for later steps: HVI_SOC HVI_EXCLAVES HVI_SELF_BOOT (and friends).

preflight() {
    step "Preflight"

    [ "$(host_os)" = macos ] || die "run the installer from macOS (it builds the bundle from this Mac's own firmware). Found $(host_os)."
    ok "running on macOS"

    is_apple_silicon || die "this is not an Apple Silicon Mac"
    ok "Apple Silicon"

    model=$(mac_model)
    [ -n "$model" ] || die "could not read the Mac model (hw.product)"
    info "model: $model"

    require_tools python3 diskutil
    eval "$(python3 "$LIBDIR/supported.py" "$SUPPORTED_JSON" "$model")" ||
        die "could not read the supported list"

    if [ "${HVI_SUPPORTED:-0}" != 1 ]; then
        warn "$model is not in the supported list ($SUPPORTED_JSON)."
        warn "Supported today: Apple Silicon Macs on the per-SoC table in docs/macos-hv-install.md."
        [ "${ALLOW_UNQUAL:-0}" = 1 ] || die "unsupported Mac. Pass --allow-unqualified to try anyway (nothing is promised)."
        warn "proceeding with --allow-unqualified; self-boot is not qualified on this Mac."
        HVI_SOC=${HVI_SOC:-unknown}; HVI_EXCLAVES=${HVI_EXCLAVES:-1}; HVI_SELF_BOOT=no
    else
        ok "$model is $HVI_MARKETING ($HVI_SOC)"
    fi

    build=$(macos_build /)
    [ -n "$build" ] || die "could not read this Mac's macOS build (SystemVersion.plist)"
    info "installed macOS build: $build"
    if [ -n "${HVI_BUILDS:-}" ]; then
        matched=0
        for b in $HVI_BUILDS; do [ "$b" = "$build" ] && matched=1; done
        if [ "$matched" = 1 ]; then
            ok "build $build is on the tested list for $HVI_SOC"
        else
            warn "build $build is not on the tested list for $HVI_SOC ($HVI_BUILDS)."
            warn "The bundle is built from THIS Mac's firmware, so a new build may still work,"
            warn "but it is unverified. The enrol step keeps a rollback of your current boot."
            [ "${ALLOW_UNQUAL:-0}" = 1 ] || die "untested macOS build. Pass --allow-unqualified to try anyway."
        fi
    else
        warn "no tested builds recorded for $HVI_SOC; this build is unverified"
    fi
    HVI_BUILD=$build

    # Self-boot status: be honest about what 'pick the macOS volume' does today.
    case "${HVI_SELF_BOOT:-no}" in
        yes)        ok "on-device self-boot is qualified on $HVI_SOC" ;;
        qualifying) warn "on-device self-boot is being qualified on $HVI_SOC and is not yet proven: ${HVI_SELF_BOOT_NOTE:-}" ;;
        *)          warn "on-device self-boot is NOT proven on $HVI_SOC: ${HVI_SELF_BOOT_NOTE:-}"
                    warn "The installer will stage everything and enrol the object, but until this"
                    warn "SoC is qualified you may still need a host for the first boot." ;;
    esac

    # SIP / boot-policy note: enrolling a custom Stage 1 needs a 1TR step and the volume
    # owner's password. We cannot do it from here; preflight only checks we can stage.
    if have csrutil; then info "SIP: $(csrutil status 2>/dev/null | sed 's/^System Integrity Protection status: //')"; fi

    # Rough free-space check on the current volume (the staging tree is ~130-200 MiB).
    free_kib=$(df -k / 2>/dev/null | awk 'NR==2 {print $4}')
    if [ -n "$free_kib" ] && [ "$free_kib" -lt 1048576 ]; then
        warn "less than 1 GiB free on /; the staging tree needs room (~200 MiB) plus working space"
    fi
    ok "preflight complete"
    export HVI_SOC HVI_EXCLAVES HVI_SELF_BOOT HVI_BUILD HVI_STAGE2_FLAGS
}
