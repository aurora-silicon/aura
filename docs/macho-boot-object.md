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

An OS therefore cannot run at EL2 in this mode. Aura stays at EL2 as a
monitor and enters the OS at EL1 on every CPU (`src/monitor.c`). At kboot,
`monitor_required()` asks a secondary to run two instructions from the heap
and uses EL1 only if that faults. There is no build option. When it does
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

## Status

kmutil from macOS 27.2 and from macOS 26.6.2 (25G83) parses the boot
object and derives `kcbz : 0`. On a MacBook Neo (T8140, J700) a proxy Stage 1
built this way cold-boots, and the installed Fedora kernel boots to a login
at EL1 with pointer authentication on and all six CPUs up.

On a MacBook Air M5 (T8142, J813) the same holds. A single-stage boot object
(a `CHAINLOADING=1 ESP_STAGE2=1` build with its `boot=` and `chosen.bootargs`
variables appended, then wrapped) cold-boots, loads the kernel, DTB and
initramfs from the ESP, and Linux reports `CPU: All CPU(s) started at EL1`
with all ten CPUs, NVMe, Wi-Fi and the GPU working. The kernel then detects
Apple TSO (the ACTLR_EL1 bit toggles) and `PR_SET_MEM_MODEL` switches a
process to TSO.

A filled Stage 1 that chainloads a Stage 2 from the ESP does not work in this
mode. The CTXR windows cover the enrolled Stage 1, not the chainloaded Stage 2,
so the secondaries Stage 2 starts run outside them at EL2. On J813 every
secondary took an exception as soon as Stage 2 handed over to Linux, and
the machine rebooted in a loop. A Stage 2 that leaves the secondaries parked
in Stage 1 boots Linux on the boot CPU, at EL2 or, with the monitor, at EL1.
Boot single-stage instead; kernel updates stay on the ESP.

Not tested: a raw boot with `src/monitor.c` in the tree, and chips other than
T8140 and T8142.
