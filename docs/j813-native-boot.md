# Initial native Linux boot on J813 (M5)

This supports RAM loading on the M5 MacBook Air (J813, Mac17,3, T8142),
using the boot CPU, firmware framebuffer and DockChannel console. It builds
on the CPU identification and PMGR power-group parsing in `aurora-wip`.

The T8142 early UART is at `0x3a5200000`. Native handoff prepares DAPF only
for enabled AOP, MTP, PMP and ISP aliases in the supplied FDT. The initial
J813 device tree contains none of those consumers, so their firmware-owned
protection state is preserved. Other SoCs retain the existing DAPF setup.
Native boot passes the prepared FDT to U-Boot; no custom root property or
legacy EL2 ADT remapping is required.

## Build and RAM chainload

Build with `make RELEASE=1 -j8` (`gmake` on macOS). The normal build requires
the documented LLVM/Rust toolchains and initialized Git submodules.

Connect the J813 USB-C port farther from MagSafe. With the resident aura
proxy available through DebugUSB, select its current KIS device:

```sh
export AURADEVICE=/dev/cu.kis-00100000-ch-0
python3 proxyclient/tools/chainload.py build/aura.macho
```

The device path is host-dependent. Confirm chip `0x8142` and model `Mac17,3`
before loading payloads. Chainload preserves cold T8142 secondary RVBARs;
writing those registers from the host can wedge CPU0. Keep secondary cores
stopped for this initial native boot. Concatenated payload boot, SMP and
other M5 models are outside this qualification.

Always use a proxy client matching the running resident's ABI for the first
RAM reload. The older local Aurora Windows development fork assigns opcode
`0x604` to a top-of-memory allocation, whereas this public branch assigns it
to the heap-limit setter. Mixing those clients can corrupt the boot memory
bounds. Once this public build is running, use its own proxy client for
subsequent chainloads and native Linux handoff.

The proxy client bounds KIS memory requests to 1 KiB writes and 256 KiB
reads, with the normal per-request checksum/reply protocol. This prevents
large transfers from exhausting the bridge's receive window.

## Linux handoff and repeatable validation

Use the J813 DTB, Image and config from
<https://github.com/aurora-silicon/linux/pull/159>, a RAM-only BusyBox
initramfs, and U-Boot built with the T8142 map and DockChannel serial driver.
Keep a manifest of source revisions and SHA-256 hashes for every binary.

Using the proxy, allocate separate buffers for Image, initramfs, DTB and
`u-boot-nodtb.bin`. Reserve at least 64 MiB for the Image's runtime footprint.
Upload and read back every byte, comparing SHA-256 hashes before handoff.
Set `/chosen/bootargs` to:

```
earlycon=dockchannel,0x38812c000 console=tty0 console=ttyDC0 keep_bootcon maxcpus=1 nokaslr rdinit=/init panic=0
```

Set the initrd range, `/chosen/stdout-path=serial0:1500000n8`, and the U-Boot
configuration `bootcmd=booti <Image-address> <initrd-address>:<size> $fdtcontroladdr`,
`bootdelay=0`, `baudrate=1500000`. Call `kboot_prepare_dt` with the J813 DTB,
check its return value, then `kboot_boot` with the U-Boot address.

Capture the entire serial session. In Linux, record `uname -a`, the model and
compatible properties, `/sys/devices/system/cpu/online`, `/proc/fb`,
`/proc/mounts`, and the SHA-256 of the decompressed `/proc/config.gz`. Capture
`/proc/uptime` and `/proc/interrupts` before and after `sleep 5`. Require CPU0,
the expected kernel/config, roughly five seconds of uptime progress, advancing
architectural timer interrupts, zero IRQ errors and only RAM-backed mounts.
Repeat after a fresh VDM reboot. Save both captures and their hashes with the
binary manifest; a build or proxy reconnect alone is not a boot pass.

This milestone does not qualify SMP, KVM, cpufreq, suspend, native DCP/GPU,
input, storage, audio, networking or USB host devices. Leave the installed
bootloader and internal disks unchanged during RAM testing.

## Guarded direct Image uploads

`proxyclient/tools/boot_j813.py` provides separate prepare, audit and boot
phases for the current public J813 kernel/firmware profile, plus optional
verified CDC uploads. It checks the matching Mach-O/ELF pair, physical PMU
receipts, full upload readbacks and the prepared device tree. See
[CDC boot uploads](cdc-boot.md) for the guarded procedure and transport limits.
