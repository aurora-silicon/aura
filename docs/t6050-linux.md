# T6050 Linux handoff

The M5 Pro Linux path prepares the firmware-owned display engines, ATC PHYs,
GPU, cameras and PCIe links before entering Linux. It reserves the display logs,
ANS SART ranges and retained firmware buffers, and publishes the S2R mailbox.
GPU leakage calibration is read from the part's fuses rather than stored in a
board device tree. A matching Linux device tree and drivers are still required.

Build an ESP Stage 2 with:

```sh
make USE_CLANG=1 BUILDSTD=1 CHAINLOADING=1 ESP_STAGE2=1 DEBUGUSB_PROXY=1
```

`boot=` selects the kernel, initrd and device tree on the ESP; see
[boot-options.md](boot-options.md). `nvme.adopt=live-rtkit-v1` identifies an
inherited ANS session. An inherited T6050 controller that never becomes ready
gets one recovery attempt using the same suspend, SART preservation and ANS
power-cycle implementation used after normal storage access.

The optional GP ports are discovered from the board's ADT. Port 0 carries N1
Wi-Fi/Bluetooth where present; absent ports are left alone. The PCIe sequence
checks storage state while bringing up the GP side. An already trained link is
adopted, and the root-port bus numbers and tunables handed to Linux agree.

For a Stage 1 build omit `ESP_STAGE2=1 DEBUGUSB_PROXY=1`; the default build
without `CHAINLOADING=1` remains a separate configuration. Preserve artifacts
and their configuration and hashes for each build. Successful links do not
qualify other chips or replace hardware validation.

The macOS SPTM virtualization series depends on this shared storage and boot
infrastructure. Its ESP loader and ANS-to-XNU handoff are separate from this
Linux series. Full macOS sleep and wake remains under investigation.
