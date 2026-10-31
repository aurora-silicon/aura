# Startup and recovery options

Configured Stage 1 images boot their selected Stage 2 immediately by default.
`tools/fill_stage1_config.py` sets the proxy window to zero unless
`--window-ms` explicitly requests a development interception window.
Changing this default does not modify an already enrolled boot object:
configure and enroll a new Stage 1 to replace an older 15-second window.

The boot-argument tokens are matched exactly:

- `aura-debug` selects DebugUSB early and prevents CDC from taking its carrier.
- `aura-proxy` makes Stage 1 wait up to 15 seconds for a host. A host that
  connects gets the proxy; otherwise Stage 1 boots its Stage 2 as normal. An
  ESP-loaded Stage 2 ignores it.
- An `aura-proxy` file at the root of the ESP Stage 1 boots from does the same,
  so a development Mac can be switched from Linux without touching Stage 1 or
  NVRAM: `touch <esp>/aura-proxy` to turn it on, `rm <esp>/aura-proxy` to turn
  it off. Without either, Stage 1 boots straight through.

The options may be combined. `aura-debug` alone does not request a proxy pause.
Normal payload failure still follows the existing recovery fallback.

## Resident KVM modes (development)

`aura-nv2` (or `aura-nv2=1`) selects the NV2 host mode, which lets Linux run
at virtual EL2 for nested KVM. `aura-nv2=0` explicitly disables that mode,
including in a build made with `NV2_BOOT=1`. An ESP `chosen.aura,nv2=1` or
`chosen.aura,nv2=0` configuration line selects the policy for the next reboot;
it cannot change the EL2 regime of a running kernel.

`aura-pv` selects the separate physical-EL1 PV-KVM prototype. It is rejected
when NV2 is selected. The current PV ABI is a bounded discovery/RUN prototype,
not a complete `/dev/kvm` backend; Linux integration and guest state switching
remain required before enabling it on a production boot.

The `tso=` payload setting is independent of both KVM modes. It controls the
Mach-O handoff's TSO setup and does not imply NV2 or PV-KVM.

## DebugUSB interception (development builds)

A build made with `DEBUGUSB_PROXY=1` also opens the 15 second `aura-proxy`
window when the boot was started by a VDM `macvdmtool reboot debugusb` and a
host is reading the DockChannel console (kisd, for example). That gives a way
back into a Mac whose Stage 2 or kernel hangs, without the ESP file or NVRAM.
It logs `aura: debugusb boot, opening proxy window`.

The signal is the DockChannel TX FIFO. Nothing on the SoC drains it: without a
reader on the dock side it only fills, so its free space plus every byte this
stage has written stays at the free space it had when the stage started. A
debugusb host reads the console out, which breaks that sum by the size of the
log. A byte waiting on the RX side (a catcher sending NOPs) counts as well. The
check is two register reads, so a normal boot does not wait for it. A Mac
without a DockChannel UART never sees it.

The window opens after the payload scan and before NVMe is touched, in Stage 1
and in a Stage 2 built with the flag. It runs once per chain: a Stage 1 that
offered any window passes `aura.host-window=done` to the next stage, and a
Stage 2 started with a live ANS session from Stage 1 skips it too. A debugusb
boot with a reader attached therefore takes 15 seconds longer when no host
claims the window. Without the flag nothing changes.

## Common build profiles

Stage 1: `make RELEASE=1 CHAINLOADING=1 BUILTIN_LOGO=aurora`.
Stage 2: add `ESP_STAGE2=1` to that command. Preserve each stage before rebuilding.
These profiles are shared across boards. The embedded ESP configuration is
consumed on every supported chip, and normal boot attempts its payload before
initializing the USB proxy. An explicit proxy request or missing payload starts
the platform's USB transport; `aura-debug` retains DebugUSB instead.

The former Neo CDC/KIS make switches and Neo Stage 2 alias have been removed.
The raw image links both USB protocol engines; the PHY chooses the engine at
runtime. The native Aura image retains its native USB/DART implementation.
Physical register and firmware handoff differences remain runtime driver logic.

Cross-platform hardware qualification is required before replacing installed
boot objects; host tests and successful links do not establish that qualification.
