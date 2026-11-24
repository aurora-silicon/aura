# macos-hv-install

Install **macOS under the aura hypervisor**, with the in-guest `auractl` tool, on an
Apple Silicon Mac — as close to the Aurora Linux install flow as the hardware allows.
You run it from macOS, it builds and stages everything from this Mac's own firmware, and
it leaves you with one step to do in recoveryOS (1TR): enrolling the boot object.

The developer guide is [`docs/macos-hv-install.md`](../../docs/macos-hv-install.md). This
README is the tool reference.

## What it is

- **Public code only.** No Apple binary is downloaded or redistributed. The ESP bundle is
  built on your Mac from the macOS already installed on it (SPTM, TXM, the kernelcache,
  the trust cache, and so on), the same way the Aurora Linux installer takes firmware from
  the installed macOS.
- **Your Linux install is not touched.** The bundle files go on the ESP through the public
  `tools/esp-macos/install_esp.sh`, which only ever writes `aurora/macos/*` and the macOS
  Stage 2, never the Linux loader, kernels or NVRAM.
- **One command to undo it.** `macos-hv-uninstall` restores the plain macOS boot and
  removes the bundle, and it works even if the hypervisor never booted.
- **auractl over DCC.** The in-guest tracing control uses the kext-free DCC transport
  (aura PR #238), with a boot-time gate and a root-only per-boot cookie. No kernel patch.

## Files

| File | Role |
|------|------|
| `install.sh` | curl-able bootstrap: download the toolkit for this SoC, run the installer |
| `macos-hv-install` | the installer (subcommands below) |
| `macos-hv-uninstall` | undo: `un-enrol`, `clean-esp`, `remove-tool`, `all` |
| `lib/common.sh` | shared sh helpers (no side effects) |
| `lib/preflight.sh` | model / SoC / macOS-build supported checks |
| `lib/supported.json` | the supported-Mac list, per SoC |
| `lib/supported.py` | reads the list (keeps JSON parsing out of sh) |
| `lib/fetch_firmware.py` | locate this Mac's firmware for the bundle |
| `tests/` | host tests (dry-run on a fake disk layout; no hardware) |

It reuses, and does not duplicate, the public tools: `tools/esp-macos/make_bundle.py`,
`install_esp.sh`, `arm_trial.sh` and `tools/recovery/enroll-stage1.sh`.

## Use

From macOS, with the release toolkit unpacked (or from an aura checkout):

```sh
# See what it would do, change nothing:
tools/macos-hv-install/macos-hv-install --dry-run \
    --partuuid <ESP-PARTUUID> --stage2 <esp-stage2.bin> \
    --self-boot-object <stage1.macho> --auractl-tool <auractl> all

# Do it (stages everything, then prints the 1TR enrol step):
tools/macos-hv-install/macos-hv-install \
    --partuuid <ESP-PARTUUID> --stage2 <esp-stage2.bin> \
    --self-boot-object <stage1.macho> --auractl-tool <auractl> all
```

Then reboot to 1TR and run the printed `… enroll` line. Restart, pick the macOS volume,
and it boots under the hypervisor; `sudo auractl status` confirms it.

Each subcommand can also be run on its own (`preflight`, `fetch-firmware`, `build-bundle`,
`stage-esp`, `install-tool`, `enroll`). `-h` lists the global options.

## Status

On-device self-boot ("pick the macOS volume and it boots under the HV with no host") has
not yet been proven on any SoC — see the per-SoC table in the developer guide. The
installer stages everything and enrols the object regardless, but it is honest in
`preflight` about what is qualified. The installer runs everywhere; the acceptance run is
scheduled on hardware separately.

## Tests

```sh
tools/macos-hv-install/tests/run-tests.sh
```

Pure host tests: they stub `diskutil`, `kmutil`, `bputil`, `findmnt`, `md5` and `uname`,
build a fake disk layout, and check that dry runs write nothing, that only the bundle
files on the ESP are ever touched (never Linux's boot), that the enrol wrapper never
mutates boot under `--check`, and that the uninstaller restores things without the HV
having booted.
