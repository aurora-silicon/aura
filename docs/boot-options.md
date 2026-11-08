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

- `aura-console` keeps the debug UART as the OS console (see below) without
  the rest of `aura-debug`. `aura-debug` keeps it too.

The options may be combined. `aura-debug` alone does not request a proxy pause.
Normal payload failure still follows the existing recovery fallback.

The `tso=` payload setting controls the Mach-O handoff's TSO setup.

## CTXR execute window

There is no option for this. On a machine whose secondaries are confined, aura
runs the OS inside the armed window and emits `/chosen/aurora,ctxr-exec-window`.
With no usable window it does not start the OS. Elsewhere nothing changes.
[The boot object notes](macho-boot-object.md#running-the-os) say what it needs.

## Sleep and CPU power-off for an OS at EL2

There is no option for this. An OS that aura starts at EL2 has no monitor below
it, so it powers cores off and suspends the system itself. From M4 on, the Neo's
A18 Pro included, iBoot locks every core's reset vector to the boot object, so each
core then restarts inside aura. On those chips kboot publishes
`/chosen/aurora,s2r-mailbox`, the physical address of a 32-byte mailbox in aura's
image:

- a secondary comes back through its spin-table release address, which still holds
  the OS's;
- the boot core jumps to the resume address the OS left in the mailbox, at EL2 with
  the MMU and caches off and interrupts masked, or reboots if the OS left none.

The CPUs keep `spin-table`. The property says they can power off and come back:
Linux with the T8140 sleep series gives them its parking spin table then, and an OS
that doesn't know the property ignores it. Whether to deep-sleep is the OS's choice.
Once the mailbox is published, reset paths don't print, since the OS owns the UART
and the display. The mailbox sits in aura's image, which kboot reserves anyway, so it
takes no memory of its own. On A7 to M3 RVBAR isn't locked, so nothing is published,
and a guest at EL1 gets nothing.

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

## The debug UART is the OS console on debug boots only

The Apple device trees set `/chosen/stdout-path = "serial0"`: the Samsung UART
on M1 and M2 laptops, the DockChannel UART on the MacBook Neo. Both are
reachable only from a debug host over a VDM `debugusb` link. Linux makes the
`stdout-path` UART a console when its command line has no `console=`, and
Plymouth, finding a serial console, drops to text mode (ttyDC0 on the Neo,
ttySAC0 on M1/M2).

kboot therefore removes `stdout-path` and `linux,stdout-path` when they name an
`apple,s5l-uart` or `apple,dockchannel-uart` node, unless this is a debug boot:

- `aura-debug` or `aura-console` in the boot arguments;
- a Stage 1 built with `DEBUGUSB_PROXY=1` saw a debugusb host reading the
  DockChannel console (see below). It passes `aura-console` to the next stage
  and logs `chainload: debugusb host, the OS keeps the debug console`;
- `chosen.aura,serial-console=1` in the aura configuration, or a config line
  that sets `chosen.stdout-path` itself.

`chosen.aura,serial-console=0` removes it on every boot. The UART node stays,
so `console=ttyDC0` (or `console=ttySAC0`) on the kernel command line still
works. kboot logs `FDT: removed chosen.stdout-path (debug UART, not a debug
boot)` or `FDT: keeping chosen.stdout-path on the debug UART (<reason>)`. A
`stdout-path` to any other node is left alone. The J616s and J614s device trees
set none.

## External displays on the M4 MacBook Air (J713)

On the J713, iBoot starts both external display coprocessors, dcpext0 and
dcpext1, and leaves them quiesced with their DARTs set up. A kernel that drives
external displays adopts them as they are. With `chosen.asahi,j713-dcpext=1`
in the aura configuration, kboot hands each one over: it reserves its firmware
memory (the OS log segment by physical address, as for the internal DCP),
publishes the firmware UUID and marks the node
`apple,t8132-dcpext-firmware-ready`. The node stays disabled.

A controller is handed over only when:

- the internal display was handed over too, so an external display never costs
  the panel;
- its device-tree node is a disabled `apple,t8132-dcpext`;
- iBoot left it powered: DISPEXTn SYS and FE on, and its CPU domain on or
  targeted on with hardware gating it while idle.

PMGR is only read. The CPU domain and the controller are left to the kernel.
The value must be exactly `1`. Without it, or when any check fails, nothing is
reserved for that controller and a marker already in the tree is removed.
kboot logs `FDT: 'dcpext0' firmware reserved, left to the kernel`, or the
reason followed by `kernel gate remains closed`.

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
