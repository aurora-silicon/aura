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
or any raw boot object) runs it and boots as it always did, and aura never reads a
`CTXR` register there. There is no boot option or build option.

WFI wipes the general registers, the vector registers and NZCV on these
cores, from EL2, EL1 and EL0, on E and P cores. FPCR, `SP_EL0` and the 33
system registers we tracked survive, including the pointer authentication
keys. WFE wipes nothing. Linux has to save registers around idle, and the
Aurora kernel does.

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

An object can arm more than the window, as one padded for 256 MiB does. The OS
still gets 128 MiB at the same address, so a Mac enrolled at a larger size runs
the same memory layout as a release. Everything else armed stays reserved:
`CTXR_D` outside the window and the windows `CTXR_A` to `CTXR_C`, which T8140's
firmware RO range already covers. It can't go back to the allocator, because EL0
stalls executing there on a confined core and the OS pools only its window.

### No usable window

There is no fallback. If a check fails, kboot refuses at `kboot_prepare_dt()` and
again at `kboot_boot()`, and prints the reason on the console and the panel with
the way out, which is to enroll Stage 1 again through the installer's Recovery
step. The boot then takes the existing failure path into the proxy. The checks
read the registers on each chip:

- `CTXR_D` is armed, and aura lies inside it and in its slot.
- `CTXR_D` reaches the end of the 128 MiB window. An object that wasn't padded
  arms about 5 MiB.
- No read-only firmware range is in the window. T8140 uses its firmware RO range.
  Other chips use `CTXR_A` to `CTXR_C` and the `CTRR_C` and `CTRR_D` ranges.
- `boot_args` is not in the window, and the heap starts above it.

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
(aurora-silicon/u-boot#32): when `/chosen/aurora,ctxr-exec-window` is present,
U-Boot loads an arm64 `Image` at the lowest free 2 MiB boundary inside it, on
both the EFI path (GRUB's `LoadImage`) and `booti`, and tells the EFI stub to
run it in place. Without that U-Boot change the image lands outside the window
and an enforcing kernel panics in `ctxr_exec_reserve()`. A compressed (zboot)
kernel cannot be pinned and is refused. Aura has no CPU off for spin-table, so
taking a CPU offline and back is up to the kernel.

## Status

kmutil from macOS 27.2 and from macOS 26.6.2 (25G83) parses the boot
object and derives `kcbz : 0`. On a MacBook Neo (T8140, J700) a proxy Stage 1
padded for 256 MiB cold-boots, with `CTXR_D` ending where `kcrz` predicts. With an
earlier version of this code that had the window at 256 MiB, Linux reached a login
at EL2 on all six CPUs from NVMe through U-Boot's `booti`, on one boot.

Not tested: this code on any hardware, a 128 MiB window on hardware, the refusal
path, and any chip other than T8140.
