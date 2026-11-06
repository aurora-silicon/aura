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

## Running the OS

With `kcbz` zero, iBoot leaves four CTXR windows armed and locked on the
secondary CPUs. The boot CPU reports the same bounds with enforcement off.
On T8140 the windows change where a secondary can execute:

- At EL2 it executes only inside them. `CTXR_D` is the large one. It covers
  the boot object and starts 0xd8000 below its load base.
- At EL1 the rule inverts. Executing inside `CTXR_D` takes an instruction
  abort straight to EL2 (ESR 0x8200000c), and execution elsewhere works.

An OS outside the windows therefore cannot run at EL2 in this mode. Aura stays
at EL2 as a monitor and enters the OS at EL1 on every CPU (`src/monitor.c`). At kboot,
`monitor_required()` asks a secondary to run two instructions from the heap
and uses EL1 only if that faults. The probe runs on every chip, not on a list
of chips: a Mac without the windows (M1 to M3, or any raw boot object) runs it
and keeps the OS at EL2. There is no build option. When it does
use EL1, kboot reserves `CTXR_D` so Linux never allocates from it.
`monitor_enter_os()` leaves EL2 the way the kernel's `init_el2_state` and
`finalise_el2_state` would, for the features this chip has, so the OS takes
no traps. Linux reports `CPU: All CPU(s) started at EL1`, and KVM is not
available yet.

WFI wipes the general registers, the vector registers and NZCV on these
cores, from EL2, EL1 and EL0, on E and P cores. FPCR, `SP_EL0` and the 33
system registers we tracked survive, including the pointer authentication
keys. WFE wipes nothing. Linux has to save registers around idle, and the
Aurora kernel does.

## Running the OS at EL2 inside the window

A kernel whose executable pages all sit inside `CTXR_D` may run at EL2.
`aura-ctxr-exec`, or a `chosen.aura,ctxr-exec=1` line in the ESP Stage 2, does
that when iBoot armed a large enough window. It takes a
boot object padded by aurora-installer (Utilities, Expanded Code Window) and a
kernel built with `CONFIG_ARM64_CTXR_EXECMEM`. The padding gives a 128 MiB window,
which holds the kernel image and the code of every module with room to spare. RAM
in the window that the kernel doesn't use is lost to the rest of the system.

`ctxr_exec_window()` in `src/monitor.c` finds the window. It starts at the first
2 MiB boundary past aura's 16 MiB slot and ends where `CTXR_D` ends. The device
tree, the trust cache and aura are in `CTXR_D` too, below that start, so the
window holds only memory the kernel owns. The kernel builds its executable pool
from the memory in the window that nothing has reserved.

In this mode aura moves the kernel from the heap to the start of the window and
publishes the window as `/chosen/aurora,ctxr-exec-window`. It keeps spin-table
with no `/psci` node, so the kernel releases each secondary into its own text. It
reserves the rest of `CTXR_D` and the firmware RO range but not the window, then
enters the kernel directly, as on M1 to M3. The monitor is never installed, so
NV2 and PV-KVM are off.

Aura keeps the monitor and prints why if the window fails a check: an object
that was not padded, a chip other than T8140, or firmware RO, `boot_args` or the
heap inside it. The kernel then gets a base-0 window, which confines it in
software only, and runs at EL1.

A kernel aura loads itself, with `boot=` or an inline payload, can use this
directly. Booting through U-Boot and GRUB needs U-Boot support for the window
(aurora-silicon/u-boot#32): when `/chosen/aurora,ctxr-exec-window` is present,
U-Boot loads an arm64 `Image` at the lowest free 2 MiB boundary inside it, on
both the EFI path (GRUB's `LoadImage`) and `booti`, and tells the EFI stub to
run it in place. Without that U-Boot change the image lands outside the window
and an enforcing kernel panics in `ctxr_exec_reserve()`. A compressed (zboot)
kernel cannot be pinned and is refused while enforcing. Spin-table has no CPU
off, so CPU hotplug does not work in this mode.

## Status

kmutil from macOS 27.2 and from macOS 26.6.2 (25G83) parses the boot
object and derives `kcbz : 0`. On a MacBook Neo (T8140, J700) a proxy Stage 1
built this way cold-boots, and the installed Fedora kernel boots to a login
at EL1 with pointer authentication on and all six CPUs up. A proxy Stage 1
padded for 256 MiB also cold-boots, with `CTXR_D` ending where `kcrz` predicts,
and from it Linux reaches a login at EL2 on all six CPUs through U-Boot's `booti`.

Not tested: a raw boot with `src/monitor.c` in the tree, a filled Stage 1 that
loads Stage 2 from the ESP, any chip other than T8140, a 128 MiB window on
hardware, and the EL2 path through GRUB.
