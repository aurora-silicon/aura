# Mach-O boot object without SPTM

`make` also builds `build/aura-boot.macho`: `build/aura.bin` wrapped as a
Mach-O boot object for `kmutil configure-boot` without `--raw`.

## Why

`kmutil configure-boot` records properties of a Mach-O boot object in the
boot policy. One of them, `kcbz`, is the size of the object's
`__TEXT_BOOT_EXEC` segment. macOS 27.2 iBoot loads TXM, ExclaveOS and SPTM
and enters SPTM only when `kcbz` is nonzero. With `kcbz` zero it skips that
loader and jumps to the object's entry, as it does for a raw boot object.
In this mode the GXF registers are writable, as on M1 to M3.

`aura-boot.macho` has no `__TEXT_BOOT_EXEC`, so it is meant for Macs with
M4 or later, including the A18 Pro, that would otherwise start SPTM.

## Layout

`tools/macho_boot_object.py` writes an `MH_FILESET` around the raw image:

| Segment          | Contents                                          |
| ---------------- | ------------------------------------------------- |
| `__TEXT`         | the fileset header, at file offset 0              |
| `__PRELINK_TEXT` | the primary member's header (`com.apple.kernel`)  |
| `__TEXT_EXEC`    | the raw image, byte for byte                      |
| `__PRELINK_INFO` | prelink information with no extensions            |
| `__DATA`         | one empty page                                    |
| `__LINKEDIT`     | a chained-fixup table with no fixups              |

kmutil requires `__TEXT` at offset 0, `__TEXT_EXEC`, `__PRELINK_INFO` and
`__LINKEDIT`. `__TEXT_BOOT_EXEC`, `__DATA_SPTM` and `__RECEIPT_INFO` are
optional. Every segment is file-backed at its virtual offset, so the raw
image is loaded exactly as in raw mode, 64 KiB after the load base. The
entry point is the raw image's own entry (`--entry-point 2048` in raw mode),
and Aura relocates itself there as usual. `x16` in `LC_UNIXTHREAD`, which
kmutil 27.2 records as `kcev`, points at the vector table at the start of
the raw image.

Because the raw image is unchanged, a Stage 1 whose configuration block was
filled in (`tools/fill_stage1_config.py`) can be wrapped afterwards, or its
block can be found by its magic inside the boot object.

## Use

Wrap a raw image, then enroll it from the target volume's paired Recovery:

```sh
python3 tools/macho_boot_object.py stage1.bin aura-boot.macho
kmutil configure-boot -c aura-boot.macho -v /Volumes/<volume>
```

Check what kmutil will record before enrolling. This also runs in normal
macOS:

```sh
kmutil inspect -z -B "$PWD/aura-boot.macho" --show-mach-boot-properties
```

It must report `kcbz : 0`.

An object wrapped this way arms only about 5 MiB of `CTXR_D`, which suits a
proxy-only Stage 1. To boot an OS it has to be padded, as the installer does when
it enrolls one (see [Running the OS](#running-the-os)). A Stage 2 from this tree
refuses to start an OS under an unpadded Stage 1 on a confined machine.

## The CTXR windows

With `kcbz` zero, iBoot leaves four CTXR windows armed and locked on the
secondary CPUs. The boot CPU reports the same bounds with enforcement off.
On T8140 the windows change where a secondary can execute:

- At EL2 it executes only inside them. `CTXR_D` is the large one. It covers
  the boot object and starts 0xd8000 below its load base.
- At EL1 the rule inverts. Executing inside `CTXR_D` takes an instruction
  abort straight to EL2 (ESR 0x8200000c), and execution elsewhere works.

At kboot `ctxr_exec_confined()` (`src/ctxr_exec.c`) asks a secondary to run two
instructions from the heap, and the machine is confined if that faults. The probe
runs on every chip, not on a list of chips: a Mac without the windows (M1 to M3,
or any raw boot object) runs it and boots as it always did. Aura reads a `CTXR`
register there only when it has no secondary to ask, and then under an exception
guard. There is no boot option or build option.

With no secondary running there is nobody to ask, which happens where
`smp_start_secondaries()` fails. Aura then keeps no answer, reads `CTXR_D` under an
exception guard, and warns if it is armed. The OS still starts, outside the window,
on the boot CPU alone, because `dt_set_cpus()` drops every CPU that isn't running.
A probe that couldn't be sent counts as not confined too.

A secondary that doesn't take the probe within 1 second, or takes it and doesn't
answer within 500 ms, stops the boot. Either way the call stays in its slot, and
kboot never hands off with a call pending, so aura says so on the panel and
doesn't start the OS. The probe isn't sent again.

WFI wipes the general registers, the vector registers and NZCV on these
cores, from EL2, EL1 and EL0, on E and P cores. FPCR, `SP_EL0` and the 33
system registers we tracked survive, including the pointer authentication
keys. WFE wipes nothing. Linux has to save registers around idle, and the
Aurora kernel does.

### Reset vector and system registers

iBoot locks each secondary's RVBAR. When it doesn't point at aura's vectors, aura
writes a branch where it does point, on any chip, if that is the boot object's
Mach-O header 64 KiB below the raw image (where iBoot locks it on T6040, which
doesn't take the vector from the object's `kcev`) or an earlier aura image's
vector table. Anything else is refused.

At EL2 aura can set `HACR_EL2`, the Apple `ACTLR` bits and the per-core deep sleep
enable, but `CYC_OVRD` is undefined from M4 on and aura never touches it there. It
never sets the deep sleep enable for the OS: a secondary in deep WFI with it set
stopped a Neo within 2 seconds. Linux finds the `ACTLR` features it uses by
itself, so the device tree says nothing about them.

## Running the OS

A kernel whose executable pages all sit inside `CTXR_D` can run at EL2, so on a
confined machine that is where it runs, on every CPU, with nothing of aura's left
under it. This takes a boot object padded by aurora-installer and a kernel built
with `CONFIG_ARM64_CTXR_EXECMEM`.

`ctxr_exec_window()` in `src/ctxr_exec.c` finds the window. It starts at the first
2 MiB boundary past aura's 16 MiB slot and is 128 MiB long, which holds the kernel
image and the code of every module with room to spare. The device tree, the trust
cache and aura are in `CTXR_D` too, below that start, so the window holds only
memory the kernel owns. The kernel builds its executable pool from the memory in
the window that nothing has reserved. RAM in the window that it doesn't use is lost
to the rest of the system.

Aura moves the kernel from the heap to the start of the window and publishes the
window as `/chosen/aurora,ctxr-exec-window`. It uses spin-table, so the kernel
releases each secondary into its own text. It then enters the kernel directly, as
on M1 to M3.

Aura can't tell whether a kernel was built with `CONFIG_ARM64_CTXR_EXECMEM`, and
moves one without it all the same. Its secondaries then fault on the first code
they run outside the window. On T8140, T6041, T8152 and the J713, which keep this
stage's heap reserved for firmware buffers, the copy the kernel leaves in the heap
stays reserved too. That is the size of the image: about 1 MiB for U-Boot, and about
40 MiB for a kernel booted directly with `boot=`.

An object can arm more than the window, as one padded for 256 MiB does. The OS
still gets 128 MiB at the same address, so a Mac enrolled at a larger size runs
the same memory layout as a release. Everything else armed stays reserved:
`CTXR_D` outside the window and the windows `CTXR_A` to `CTXR_C`. A window inside
T8140's firmware RO range is left to that range's reservation. The monitor's Neo
boots of 2026-10-31 relied on the range covering all three, so the J700 tree gains
no entry for them. None of it can go back to the allocator, because EL0 stalls
executing there on a confined core and the OS pools only its window.

### No usable window

There is no fallback. If a check fails, kboot refuses at `kboot_prepare_dt()` and
again at `kboot_boot()`, and prints the reason on the console and the panel with
the way out, which is to enroll Stage 1 again through the installer's Recovery
step. The boot then takes the existing failure path into the proxy. The checks
read the registers on each chip:

- `CTXR_D` is armed, and aura lies inside it and in its slot.
- `CTXR_D` reaches the end of the 128 MiB window. An object that wasn't padded
  arms about 5 MiB.
- Nothing kboot keeps from the OS is in the window: `CTXR_A` to `CTXR_C`, the
  `CTRR_C` and `CTRR_D` ranges the secondaries can't write, and on T8140 its
  firmware RO range too. Every chip is checked against all of them, except that
  one with unlocked Apple system registers (M1 to M3) skips CTRR, as kboot does.
- `boot_args` is not in the window, and the heap starts above it.

Later steps refuse too, and take the same failure path. `kboot_boot()` won't place
an image without the `ARM\x64` magic, or one that doesn't fit the window. It also
won't place one when the initrd or the device tree overlaps the window. A described
bootloader handoff that overlaps the window fails `kboot_prepare_dt()`.

### Which Stage 2 a Stage 1 loads

A Stage 2 from this tree can only run under a padded Stage 1, so on Mach-O chips a
padded Stage 1 is filled with `m1n1/boot-ctxr.bin` and an older one keeps
`m1n1/boot.bin`, which package updates stop touching there. An old Stage 1 then
can't load a Stage 2 that would refuse to boot. The installer fills the path with
the same `tools/fill_stage1_config.py` format. Aura doesn't name either path: a
Stage 1 reads its target from its configuration block, whose path field is 192
bytes, and fatfs is built with long file names.

### U-Boot and GRUB

A kernel aura loads itself, with `boot=` or an inline payload, can use the window
directly. Booting through U-Boot and GRUB needs U-Boot support for it
(aurora-silicon/u-boot#32). When `/chosen/aurora,ctxr-exec-window` is present:

- U-Boot relocates its image, which holds its EFI runtime code, to the top of the
  window. Other EFI runtime code it allocates goes in the window too. Its malloc
  area and EFI stack stay outside, since no confined core runs them.
- It loads an arm64 `Image` at the lowest free 2 MiB boundary in the window, on both
  the EFI path (GRUB's `LoadImage`) and `booti`, and tells the EFI stub to run it in
  place.

Without that U-Boot change the image lands outside the window. The kernel then logs
why and boots on the boot CPU alone, leaving the secondaries in aura. A kernel that
finds EFI runtime code outside the window, or misaligned, warns and turns runtime
services off for that boot. A compressed (zboot) kernel can't be placed in the
window, so U-Boot warns and it boots on the boot CPU the same way.

### What a confined Mac loses

These worked under the monitor, or were reachable through it, except power-off:

- **CPU hotplug.** Spin-table has no CPU off, and aura no longer parks a core for
  the OS. The kernel in aurora-silicon/linux#985 has no `cpu_die` for spin-table, so
  taking a CPU offline fails with `EOPNOTSUPP`. Suspend to RAM and hibernation take
  the secondaries offline the same way, so they fail there too. aura publishes
  `/chosen/aurora,s2r-mailbox` for an OS at EL2 on these chips, though, and a kernel
  that reads it powers cores off and suspends itself
  ([boot options](boot-options.md#sleep-and-cpu-power-off-for-an-os-at-el2)).
- **PSCI `SYSTEM_RESET`.** Restart now needs `macsmc-reboot` or the Apple watchdog.
  The monitor answered `SYSTEM_OFF` with a reset as well, so power-off never worked
  through it, and now needs `macsmc-reboot`. The J700 tree describes both, but
  `macsmc-reboot` is a module in the asahi config, so a kernel that hasn't loaded it
  can only restart through the watchdog and can't power off.
- **kexec and kdump.** aurora-silicon/linux#985 refuses both with `EOPNOTSUPP` while
  a window is active, because the relocation code runs outside it.
- **`cpuidle-psci`.** This loses less than it sounds. No Apple tree in the kernel has
  `cpu-idle-states`, so `cpuidle-psci` had no states to register under the monitor
  either. `cpuidle-apple` binds on T8140 through `apple,t8140-cpuidle`, which the J700
  tree enables. Both of its states are plain WFI there, because the deep state's
  `CYC_OVRD` write is undefined on that chip. This comes from reading the driver and
  trees, and no boot has confirmed it.
- **NV2 and PV-KVM.** Both are gone, and with them the only `/dev/kvm` measured on a
  confined Mac. KVM at EL2 in the window hasn't been tried.

Idle power in this mode hasn't been measured, and the J700 is fanless, so it needs
measuring on a Neo.

### Device trees

Every running secondary needs `cpu-release-addr` in the kernel's tree, or
`dt_set_cpus()` fails the boot. The monitor used to replace it with PSCI. In
`integration/aurora` of aurora-silicon/linux:

| Chip  | Tree                                   | Result                            |
| ----- | -------------------------------------- | --------------------------------- |
| T8140 | `t8140.dtsi`                           | spin-table on all 6 CPUs          |
| T8132 | `t8132.dtsi`                           | spin-table on all 10 CPUs         |
| T8142 | `t8142-j813.dts`                       | spin-table on all 10 CPUs         |
| T6041 | `t604x.dtsi`, `t6041.dtsi`             | spin-table on all 16 CPUs         |
| T6040 | `t604x.dtsi`, `t6040.dtsi`             | all but `cpu@10100`, the boot CPU |
| T605x | none                                   | no tree, so no Linux to boot      |

T6040 only works if iBoot really boots it on `cpu@10100`, which the tree's comment
says but nothing here has checked. None of this says those chips boot. It only
means the tree doesn't rule them out.

## Status

kmutil from macOS 27.2 and from macOS 26.6.2 (25G83) parses the boot
object and derives `kcbz : 0`. On a MacBook Neo (T8140, J700) a proxy Stage 1
padded for 256 MiB cold-boots, with `CTXR_D` ending where `kcrz` predicts. With an
earlier version of this code that had the window at 256 MiB, Linux reached a login
at EL2 on all six CPUs from NVMe through U-Boot's `booti`, on one boot. A J616s
(T6040) started its secondaries through the Mach-O header branch in
aurora-silicon/aura#66, with Linux under the monitor this tree no longer has.

Not tested: this code on any hardware, a 128 MiB window on hardware, the refusal
path, idle power in this mode, restart and power-off without PSCI, and any chip
other than T8140.
