# Booting macOS under aura from the ESP (M5 Pro, J714sAP)

An `SPTM_VIRT_ESP=1` Stage 2 boots macOS under SPTM-virt from files on the EFI system partition,
with no USB host. It does what `proxyclient/tools/run_guest_sptm.py` does over the debug link for
the M5 trace runs (`/path/to/capture`, `cp.env`), with a C port
of `plan_boot` and a manifest written ahead of time. Ported from the Neo's version (aura
`auractl-hw`, b6d05747), without auractl and ATC0-to-macOS.

Linux's boot does not change: the mode is its own image, `m1n1/stage2-macos.bin`, started for one
boot by Stage 1's `aura-try` marker.

## Build

    make clean
    make USE_CLANG=1 BUILDSTD=1 CHAINLOADING=1 ESP_STAGE2=1 SPTM_VIRT=1 SPTM_VIRT_ESP=1 DEBUGUSB_PROXY=1

## Files on the ESP

    m1n1/stage2-macos.bin          aura, then the line macos=<PARTUUID>;aurora/macos;<manifest SHA-256>
    aurora/macos/manifest.txt      sizes, SHA-256s, segments, patches, hidden nodes, S2R settings
    aurora/macos/sptm.bin txm.bin kernelcache trustcache.bin
    aurora/macos/cl4-bundle.bin exclave-integrity.bin exclave-trustcache.bin
    aurora/macos/SHA256SUMS        for the install script; aura does not read it

About 155 MiB for 26A428. These are Apple's binaries, stock, for this Mac's owner; nothing here
puts them in git. aura applies its patches in RAM after it has checked the hashes.

## Make the bundle

`tools/esp-macos/make_bundle.py` (its docstring has the full command for the trace lane's inputs)
takes the images, a live device tree of this Mac (for the debug port's nodes and PHYs) and
BuildIdentity.plist, and writes a staging tree. It uses the runner's own code for what the runner
decides from files: hidden nodes, XNU quirks, SPTM patch sites and MTE sequence, cL4 regions,
boot-args. `--flag`, `--s2r NAME=VALUE` and `--ans-drop` change the trace settings and the ANS
nub; the defaults are cp.env's (TRACE, GO, FULLTRACE, COPROC; dcp_pkt 1, wake_s 30, go_count 1).

`tests/python/test_esp_macos.py` checks the C planner against plan_boot on real inputs
(`AURA_ESP_FW`, `AURA_ESP_TC`, `AURA_ESP_ADTS`).

## Install and select

On the M5's SSD Linux, as root, with the ESP at /boot:

    sh install_esp.sh --esp /boot --dry-run <staging>
    sh install_esp.sh --esp /boot <staging>
    sh arm_trial.sh --esp /boot            # the next boot only; --disarm to undo

The install writes only `aurora/macos/*` and `m1n1/stage2-macos.bin`, checks the partition's
PARTUUID, and verifies what it wrote. Stage 1 spends the marker (rewrites it to `spent`) before it
loads the image, so every later boot is Linux again.

## What happens at boot

Stage 1 chainloads `m1n1/stage2-macos.bin` with its ANS session live. The image opens its debugusb
window if a host reads the console, adopts the ANS session, reads the manifest, then every file
straight into the top of the managed window (the kernelcache alone nearly fills aura's heap),
checking each size and SHA-256. It lays the images out, edits the device tree, patches SPTM, hands
ANS over quiesced (controller off, AP and IOP QUIESCED, as Stage 1's own proxy leaves it, the only
state XNU was seen to take), disarms the trial watchdog and enters SPTM with the S2R tracer
configured.

Any refusal before entry prints the reason on the console and the panel and returns; the trial
watchdog (178 s) then restarts the Mac into Linux.

## Reading the trace

At the S2R stop (go_count wakes, then the next commit withheld) aura waits in its proxy on the
DockChannel console. A host re-arms DebugUSB, restarts kisd and reads the ring with the trace
lane's `tools/dump.py <rundir>` (`M5_AURA_ELF` = this image's aura-raw.elf).
