# Booting macOS under aura from the ESP

An `SPTM_VIRT_ESP=1` Stage 2 boots macOS under SPTM-virt from files on the Linux install's EFI system
partition, with no USB host. It does what `proxyclient/tools/run_guest_sptm.py` does over the debug
link for `boot_desktop.sh`, using a C port of `plan_boot` and a manifest written ahead of time.

Linux's boot does not change. The mode lives in its own image, `aurora/stage2-macos.bin`, and only
a one-shot `aura-try` marker or a Stage 1 configured for that path can start it.

## Build

    make clean
    make CHAINLOADING=1 ESP_STAGE2=1 SPTM_VIRT=1 SPTM_VIRT_GUARDED=1 SPTM_VIRT_ESP=1 AURACTL=1

Add `ATC0_TO_MACOS=1` to give macOS the USB controller (below). `AURACTL=1` is optional, and without
it XNU's `acct()` stays stock.

## Files on the ESP

    aurora/stage2-macos.bin              aura, then the line macos=<PARTUUID>;aurora/macos;<manifest SHA-256>
    aurora/macos/manifest.txt            sizes, SHA-256s, segments, patches, hidden nodes, boot-args
    aurora/macos/sptm.bin
    aurora/macos/txm.bin
    aurora/macos/kernelcache             or kernelcache.gz with --gzip-kernelcache
    aurora/macos/trustcache.bin
    aurora/macos/cl4-bundle.bin          the bundle's payload, without its IM4P wrapper
    aurora/macos/exclave-integrity.bin
    aurora/macos/exclave-trustcache.bin
    aurora/macos/SHA256SUMS              for the install script; aura does not read it

These are Apple's binaries, stock. They are for this Mac's owner and are not for redistribution, so
nothing here puts them in git. aura applies its patches in RAM after it has checked the hashes.

## Make the bundle and install it

On the build machine:

    python3 tools/esp-macos/make_bundle.py --stage2 build/aura.bin \
        --sptm sptm.bin --txm txm.bin --xnu kernelcache.release.mac17p.decompressed \
        --trustcache iboot-trustcache-25G83.tc --exclave-fw exclave-fw-25G83 --out /tmp/neo-macos-esp

Copy `/tmp/neo-macos-esp` and `tools/esp-macos/` to the Neo. Then, as root, with the ESP mounted:

    tools/esp-macos/install_esp.sh --dry-run /tmp/neo-macos-esp
    tools/esp-macos/install_esp.sh /tmp/neo-macos-esp

The script writes only `aurora/macos/*` and `aurora/stage2-macos.bin`. It finds the ESP by PARTUUID
(default `3ce364b1-1de6-4418-a00d-5b2d95884ad7`), refuses another partition, skips files that are
already correct, and verifies everything it wrote. Running it twice changes nothing.

## Select the mode

    tools/esp-macos/arm_trial.sh --esp <mountpoint>     # the next boot only
    tools/esp-macos/arm_trial.sh --esp <mountpoint> --disarm

Stage 1 spends the marker on the disk before it loads the image, so every later boot takes the
configured Stage 2 again. The installed Stage 1 must be new enough to read `aura-try`.

To make the mode permanent, fill a Stage 1 with `--stage2-path aurora/stage2-macos.bin` and enroll it
in 1TR ([j700-install.md](j700-install.md)). Nothing here does that.

## What Stage 2 checks

The `macos=` line carries the manifest's SHA-256, and the manifest carries a size and a SHA-256 for
each file (and for each file's unpacked contents, if gzipped). aura reads and checks them all before
it writes to the RAM SPTM will own. It then refuses a manifest whose `atc0` line disagrees with the
build, a blob that overlaps aura or the device tree, a DCP log buffer inside aura's image, a load that
would overwrite the source of an iBoot data copy, and a patch whose old word is not in RAM.

Any refusal prints the reason on the debug console and the panel, and aura falls back to its usual
no-payload path. A trial boot's 178 s watchdog then restarts the Mac into its configured Stage 2.

aura disarms that watchdog just before it enters SPTM, because macOS takes longer than 178 s to come
up. A hang after that needs the power button. The marker is spent by then, so the next boot is Linux.

## ATC0 to macOS

`ATC0_TO_MACOS=1` with `make_bundle.py --atc0-to-macos`. It is off by default, and aura refuses a
manifest and a build that disagree.

- XNU sees the nodes `run_guest_sptm.py` calls `KIS_PORT_NODES`: `hpm0`, `usb-drd`, `dart-usb`,
  `atc-phy0`, `atc0-dpxbar`, `atc0-dpphy`, the USB repeater and the hub's I2C node. The display
  crossbar keeps its `atc0` endpoint.
- aura keeps none of their power domains up, hooks none of their power registers, does not power
  them down at the sleep commit, and has no USB retake. They are XNU's.
- aura's console stays on DockChannel, which stays hidden from XNU. Once XNU drives `hpm0` the port
  leaves debug mode, so expect that console to have no reader. `auractl console` reads aura's
  console ring from macOS instead, and `auractl` is how the trace gets armed and read.
