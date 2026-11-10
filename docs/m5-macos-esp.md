# macOS under SPTM virtualization from the ESP

`SPTM_VIRT_ESP=1` adds an ESP loader for the M5 Pro (T6050). It reads stock
firmware files supplied by the machine's owner, checks the manifest's sizes and
SHA-256 hashes, lays them out using the same plan as `run_guest_sptm.py`, and
enters the virtualized SPTM. No firmware binaries belong in this repository.

This series depends on the public T6050 Linux/storage series for the shared
ANS, RTKit, SART, PCIe and boot-window infrastructure. Apply that dependency
when building the combined ESP path; do not replay duplicate shared helpers.

## Build

```sh
make USE_CLANG=1 BUILDSTD=1 CHAINLOADING=1 ESP_STAGE2=1 DEBUGUSB_PROXY=1 SPTM_VIRT=1 SPTM_VIRT_ESP=1
```

For a plain boot using an existing trace manifest, set these build overrides:
`EXTRA_CFLAGS="-DSPTM_VIRT_ESP_TRACE=0 -DSPTM_VIRT_ESP_WAKE_S=0 -DSPTM_VIRT_ESP_GO_COUNT=0"`.
This suppresses instrumentation and aura's timed-wake/stop settings. It does not
establish that full macOS sleep or wake works.

## Bundle

`tools/esp-macos/make_bundle.py` takes the stock images, their BuildIdentity,
and a live ADT for the debug port's nodes and PHYs. Supply the target ESP's
PARTUUID explicitly with `--esp-partuuid`; there is no machine-specific default.
The tool writes a staging directory, leaving the disk untouched:

```text
m1n1/stage2-macos.bin       aura + macos=<PARTUUID>;aurora/macos;<manifest SHA-256>
aurora/macos/manifest.txt   sizes, hashes, segments, patches, hidden nodes, S2R settings
aurora/macos/              stock SPTM, TXM, kernelcache, trustcache and exclave files
```

The manifest's trace and S2R flags are explicit capture settings. Instrumentation
can affect the guest; successful plain boots do not qualify every hook set.
`tests/python/test_esp_macos.py` contains the C/Python planner comparison.

## Select and boot

Installation is separate from selection. On the target Linux system, the
installer takes `--esp <mountpoint> --partuuid <uuid> [--dry-run] <staging>`.
It checks the partition, manifest and copied files. It never writes APFS.

The enrolled M5 Stage 1 used during validation did **not** consume `aura-try`.
Do not rely on `arm_trial.sh` for an unattended M5 boot. The validated entry
route is a host chainload caught in the existing Stage 2's DebugUSB window,
with `nvme.adopt=live-rtkit-v1` and `aura.host-window=done`. It leaves the ESP
unchanged and returns to the recorded default Linux boot after the session.
A different enrolled Stage 1 needs its own selection-path qualification.

The loader stages files below MTE tag storage, preserves iBoot's end of RAM
from ADT region-id-24, and reads that value without assuming 8-byte alignment.
After using NVMe it disables the controller and quiesces both sides of RTKit
for XNU's ANS handoff. Failure before entry returns to the proxy. The tested
T6050 watchdog layout does not support the trial watchdog, so a host-controlled
reset is required for a stalled session.

macOS reaches loginwindow and SSH on qualified ESP images. A timer commit/wake
capture is distinct from full macOS wake: sleep-entry ACC rail-gating timeouts
and full wake remain under investigation. No full-wake claim is made here.
