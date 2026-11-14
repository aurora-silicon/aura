# macOS under the aura hypervisor — developer install guide

This is the developer-facing guide for `tools/macos-hv-install/`, a public tool
that installs **macOS under the aura hypervisor**, with the in-guest `auractl`
control tool, on an Apple Silicon Mac that already has Aurora Linux installed.
It stays as close to the Aurora Linux install flow as the hardware allows: you
run it from macOS, it builds and stages everything from this Mac's own installed
firmware, and it leaves you with one step to do in recoveryOS (1TR).

Read this before you run it. Several things here are known gaps or are not yet
proven on hardware, and the guide says so plainly where that is the case. See
also `tools/macos-hv-install/README.md` for the tool reference and
`docs/m5-macos-esp.md` for the T6050 ESP loader this builds on.

## What it does, and the end state

The intended end state is on-device and self-contained: you pick the macOS
volume in the boot picker (or bless it from Linux with
`asahi-bless --set-boot-macos`) and macOS boots under the aura hypervisor with
no host, no chainload and no tether. From inside that macOS, the `auractl` tool
drives aura's MMIO and trace over the kext-free DCC transport (aura PR #238), so
there is no kernel patch and no kext.

What it writes is deliberately small, and it does not touch your Aurora Linux
install:

- A work directory you choose.
- The macOS bundle files on the ESP — `aurora/macos/*` and the macOS Stage 2 —
  written by the public `tools/esp-macos/install_esp.sh`, which never touches
  the Linux loader, kernels or NVRAM. Linux boots exactly as before.
- The `auractl` tool, copied into the running macOS.
- Only in the enrol step, and only from 1TR, the macOS volume's Stage 1 boot
  object.

One command uninstalls it: `tools/macos-hv-install/macos-hv-uninstall`.

No Apple binary is downloaded or redistributed. The ESP bundle is built on this
Mac from the macOS already installed on it (SPTM, TXM, the kernelcache, the
trust cache and the exclaves), the same way the Aurora Linux installer takes
firmware from the installed macOS.

## Current status — read this first

**On-device self-boot has not been proven on any SoC yet.** "Pick the macOS
volume and it boots under the hypervisor with no host" is the target, not a
shipped capability. The installer stages everything and enrols the boot object
regardless, but until a SoC is qualified the first boot may still need a host,
and the installer is explicit about this in `preflight`.

The per-SoC situation, from `tools/macos-hv-install/lib/supported.json`:

| SoC | Marketing | Self-boot status | What is real today |
| --- | --- | --- | --- |
| T6041 | M4 Pro (J616s) | qualifying | First SoC targeted for on-device self-boot (ESPBOOT-3 acceptance). Its `j616sap` BuildIdentity carries no `Ap,ExclaveOS`/`Ap,cL4`, so the guest is SPTM+TXM+XNU only — no exclaves. Not yet proven. |
| T6050 | M5 Pro | no | Files load from the ESP, but the validated entry is still a **host chainload** of `stage2-macos.bin`, and its Stage 1 ignored the `aura-try` selector (see `docs/m5-macos-esp.md`). No unattended self-boot. |
| T8140 | A18 Pro class (Neo / J700) | no | Entry is a one-shot bless of the macOS volume → aura proxy → host chainload. No unattended self-boot. |

Only the Macs listed under `models` in `supported.json` are recognised; today
that is the T6041 family (`Mac16,5`–`Mac16,11`). A Mac or macOS build that is
not on the list is refused unless you pass `--allow-unqualified`, which only
stages the files and does the enrol without claiming self-boot works. Nothing is
promised in that mode.

## What it changes, exactly

1. **A work directory you choose** (`--work`, default `./macos-hv-work`). The
   fetched firmware and the staging tree live here.

2. **The macOS bundle files on the ESP**, written by the public
   `tools/esp-macos/install_esp.sh`:

   ```text
   m1n1/stage2-macos.bin       aura + macos=<PARTUUID>;aurora/macos;<manifest SHA-256>
   aurora/macos/manifest.txt   sizes, hashes, segments, patches, settings
   aurora/macos/              stock SPTM, TXM, kernelcache, trustcache and exclave files
   ```

   `install_esp.sh` only ever writes those paths. It never touches the Linux
   loader, kernels or NVRAM, so Linux boots exactly as before.

3. **The `auractl` tool**, copied into macOS (default `/usr/local/bin/auractl`).

4. **Only in the enrol step, and only from 1TR**: the macOS volume's Stage 1
   boot object, via `kmutil configure-boot`, with the volume owner's password.
   The enrol wrapper (`tools/recovery/enroll-stage1.sh`) runs every check first.
   It enrols with no rollback image (the uninstaller restores the stock boot with
   `kmutil configure-boot -v`), and records the pre-enrol boot policy under the
   work dir for reference.

No Apple binary is downloaded or redistributed at any step. The bundle is built
from **this** Mac's own installed macOS firmware — SPTM, TXM, the kernelcache,
the trust cache and the exclaves — the way the Aurora Linux installer takes
firmware from the installed macOS.

## Requirements

- An Apple Silicon Mac on the supported list (`supported.json`; T6041 family
  today). Other Macs need `--allow-unqualified`, with nothing promised.
- Aurora Linux already installed on this Mac.
- The macOS whose firmware you are bundling installed and bootable (its firmware
  is read live, from this machine).
- Xcode command-line tools, for `python3`, used by the build and fetch steps
  (`fetch_firmware.py`, `make_bundle.py`).
- The admin / volume-owner password, for the 1TR enrol step.

## Install walkthrough

You can bootstrap over curl, or run from an aura checkout. Either way the flow is
the same: `preflight → fetch-firmware → build-bundle → stage-esp → install-tool`,
then the printed 1TR enrol step.

### Bootstrap over curl

```sh
curl -fsSL https://<release-host>/macos-hv/install.sh | sh
```

`install.sh` downloads the macos-hv toolkit for this Mac's SoC (the installer
scripts, this aura release's ESP Stage 2, the self-boot Stage 1 object and the
`auractl` DCC tool) and runs the installer. It downloads only our code plus this
Mac's matching aura build; the macOS firmware that goes into the bundle is read
from this Mac, never downloaded. Set `HVI_PARTUUID`, `HVI_ESP`, `HVI_CHANNEL`
(`stable`/`testing`), `HVI_WORK` or `HVI_DRY_RUN=1` in the environment to
configure it.

### From an aura checkout

Do a dry run first — it checks and shows, and writes nothing:

```sh
tools/macos-hv-install/macos-hv-install --dry-run \
    --partuuid <ESP-PARTUUID> --stage2 <esp-stage2.bin> \
    --self-boot-object <stage1.macho> --auractl-tool <auractl> all
```

Then run it for real:

```sh
tools/macos-hv-install/macos-hv-install \
    --partuuid <ESP-PARTUUID> --stage2 <esp-stage2.bin> \
    --self-boot-object <stage1.macho> --auractl-tool <auractl> all
```

You need the ESP PARTUUID (`diskutil list` to find the ESP, `diskutil info` for
its PARTUUID) and this SoC's released artifacts for `--stage2`,
`--self-boot-object` and `--auractl-tool`. The curl bootstrap toolkit ships
those three; from a checkout you supply them yourself (build the ESP Stage 2
with this SoC's `stage2_build_flags` from `supported.json`, and `auractl` with
`make -C tools/auractl ENTRY=dcc`).

### What the steps do

- **preflight** — checks this is a supported Apple Silicon Mac on macOS, reads
  the installed build, and prints the honest self-boot status for this SoC.
  Changes nothing.
- **fetch-firmware** — copies this Mac's firmware into `<work>/fw` with
  `fetch_firmware.py`. It prints where it found each piece.
- **build-bundle** — assembles the ESP staging tree with `make_bundle.py`. This
  step has no dry run of its own; under `--dry-run` it prints the command it
  would run and skips the build.
- **stage-esp** — writes the bundle to the ESP with `install_esp.sh`. It always
  does `install_esp.sh --dry-run` first so you see exactly what will change, then
  asks you to confirm (unless `-y`).
- **install-tool** — copies `auractl` into macOS. Skipped if you pass no
  `--auractl-tool`.

The `all` command runs all of the above (as `stage`) and then prints the 1TR
enrol step:

1. Shut down. Hold the power button until "Loading startup options".
2. Choose Options, then your macOS volume, and unlock with your password.
3. Open Terminal (Utilities → Terminal) and run the printed line:

   ```sh
   macos-hv-install --volume / --self-boot-object <stage1.macho> enroll
   ```

4. Enter the volume owner's password when `kmutil` asks.
5. Restart and pick the macOS volume: it boots under the aura hypervisor.

Then, inside macOS:

```sh
sudo auractl status
```

Each subcommand can also be run on its own (`preflight`, `fetch-firmware`,
`build-bundle`, `stage-esp`, `install-tool`, `enroll`). `-h` lists the global
options.

## auractl in the guest

`auractl` drives aura over the kext-free DCC transport. The transport is gated
two ways: a boot flag decides it exists at all, and a per-boot cookie decides who
may act (Dj's conditions). The installer turns on the boot flag by building the
bundle with `make_bundle.py --flag AURA_AURACTL_DCC`; `esp_macos` then bakes a
random per-boot cookie into the guest's boot-args as `aura-actl-ck=<hex>`. In the
guest only root can read the boot-args (`sysctl kern.bootargs`), so **run
`auractl` with `sudo`**. Without the cookie, every command past the read-only
`status` returns an error.

If the running aura build predates the gate (PR #238 not yet on this tip), it has
no `AURA_AURACTL_DCC` flag. The installer detects that at build-bundle time and
warns that the bundle will boot macOS under the hypervisor but that
auractl-over-DCC stays off until the gate lands; everything else installs
normally.

Commands: `status`, `list`, `arm`, `start`, `stop`, `read`, `print`, `mark`,
`clear`, `disarm`. `peek` is available per-SoC.

## Uninstall / rollback

`tools/macos-hv-install/macos-hv-uninstall` undoes the install in three
independent parts, each safe on its own. None of them needs the hypervisor to
have ever booted, so you can always recover — from 1TR or from Linux.

- **un-enrol** (from 1TR) — restores the macOS volume's stock Stage 1 with
  `kmutil configure-boot -v <vol>` and no custom object, so the volume boots
  macOS the normal way again.
- **clean-esp** (from Linux, macOS or 1TR) — removes the bundle files from the
  ESP (`aurora/macos/*`, the macOS Stage 2 and the `aura-try` marker). It
  guards against a bad `--esp` and never touches Linux's own boot files or
  NVRAM.
- **remove-tool** — removes `auractl` from macOS.
- **all** — does un-enrol (only if you are in 1TR), clean-esp and remove-tool as
  the current environment allows, and prints what it could not do.

If the hypervisor never booted, recover by running `un-enrol` from 1TR to put
the stock Stage 1 back, and `clean-esp` from Linux or 1TR.

## Troubleshooting

- **Unsupported Mac, or untested macOS build.** `preflight` refuses it. Pass
  `--allow-unqualified` to proceed; this only stages the files and does the
  enrol, and nothing about self-boot is promised. The bundle is built from this
  Mac's own firmware, so a newer build may still work, but it is unverified; you
  can always restore the stock boot with the uninstaller's `un-enrol`.
- **`fetch_firmware could not find X`.** The tool stops with the exact override
  to pass rather than guessing. Supply the piece explicitly with
  `--fw-arg --<piece> --fw-arg <path>` (for example
  `--fw-arg --sptm --fw-arg <path>`).
- **Kernelcache must be decompressed.** `fetch_firmware.py` will not guess at a
  wrapped or compressed kernelcache. Supply a decompressed Mach-O kernelcache
  with `--fw-arg --xnu --fw-arg <path>` (e.g. from pyimg4/img4 + lzfse).
- **The live ADT.** There is no supported kext-free macOS userspace way to read
  the raw ADT yet, so on most Macs you must pass an explicit
  `--fw-arg --adt --fw-arg <path>` (for example captured over the proxy with
  `proxyclient/tools/boot_j873.py`, or from the acceptance kit). This is a known
  gap being closed on hardware.
- **ESP not found or not mounted.** Pass `--esp <mountpoint>` and/or
  `--partuuid <uuid>`.
- **enroll-stage1 says "not 1TR" or "not paired".** The enrol must run from the
  volume's own one true recoveryOS. Shut down, hold the power button until
  "Loading startup options", choose Options, and pick the macOS volume. If the
  recoveryOS is not paired with that volume, make that volume the startup disk
  and enter 1TR again.
- **First boot stalls on a SoC without a trial watchdog.** On T6050 the tested
  watchdog layout does not support the trial watchdog, so a stalled first boot
  needs a host-controlled reset (see `docs/m5-macos-esp.md`).

## How it fits together

The installer **reuses, and does not fork**, the public tools it calls:
`tools/esp-macos/make_bundle.py`, `tools/esp-macos/install_esp.sh`,
`tools/esp-macos/arm_trial.sh`, `tools/recovery/enroll-stage1.sh` and the
procedure in `docs/recovery-enroll.md`. It adds the preflight checks, the
firmware fetch from this Mac, and the orchestration that ties the steps together
and prints the 1TR step.

The per-SoC ESP document for J700 (`j700-macos-esp.md`) is not yet public; this
guide fills that gap for the installer's audience until it is.
