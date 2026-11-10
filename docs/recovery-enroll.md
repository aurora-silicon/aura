# Enrolling a Stage 1 by hand from Recovery

`tools/recovery/enroll-stage1.sh` enrolls a Stage 1 that you built yourself,
from the target volume's paired One True recoveryOS (1TR). It runs every check
first, then a single `kmutil configure-boot`. It does nothing that
[macho-boot-object.md](macho-boot-object.md#use) and
[j700-install.md](j700-install.md#test-and-install) don't already describe; it
adds the checks and a log.

An installation made by aurora-installer updates its own Stage 1 from its
Recovery app. Use this script for a hand-built Stage 1, such as a development
build or one with a local patch.

## Before Recovery

Put the script, the new Stage 1 and the Stage 1 you would go back to on a FAT
volume that Recovery can mount, such as the ESP or a USB stick. Write down their
MD5s, from `md5 -q` on macOS or `md5sum` on Linux.

For a Mach-O object, check it while still in macOS. It must report `kcbz : 0`:

```sh
kmutil inspect -z -B "$PWD/aura-boot.macho" --show-mach-boot-properties
```

This script doesn't pad an object. On T8140 and later an OS only starts from a
padded object, which aurora-installer makes (see
[Running the OS](macho-boot-object.md#running-the-os)). An unpadded object suits a
proxy-only Stage 1 there, and M1 to M3.

## In Recovery

Shut down. Hold the power button until "Loading startup options", choose
Options, and authenticate. Recovery must be paired with the target volume: if
it isn't, make that volume the startup disk and enter 1TR again. Then open
Utilities → Terminal:

```sh
diskutil list               # find the ESP, or the USB stick
diskutil mount diskXsY
cd /Volumes/EFI/aurora      # wherever the files are
bash enroll-stage1.sh --check --keep old-stage1.bin <old MD5> Linux aura-boot.macho <MD5>
bash enroll-stage1.sh --keep old-stage1.bin <old MD5> Linux aura-boot.macho <MD5>
```

That example enrolls a Mach-O object for the volume `/Volumes/Linux`, with a raw
image kept for rollback. The first run only checks. The arguments are the target
System volume (a name under `/Volumes`, or a path), the object and its MD5.
Options:

| Option | Meaning |
| --- | --- |
| `--raw` | The object is a raw image. It is enrolled with `--raw --entry-point 2048 --lowest-virtual-address 0`. |
| `--keep FILE MD5` | A Stage 1 to go back to, which must be present with that MD5. Repeat it for several. |
| `--no-rollback` | Enroll without a `--keep` image. |
| `--compress` | Pass `--compress` to kmutil, for a padded object. |
| `--group UUID` | The volume's APFS volume group. By default it comes from `diskutil info`. |
| `--log FILE` | Where to append the session. By default `enroll-stage1.log` next to the object. |
| `--check` | Run every check, then stop. |

The checks, in order, all before anything changes:

1. The volume is mounted, and has a volume group UUID.
2. The object, and each `--keep` image, has the MD5 given. Without a `--keep`
   image the script stops, unless `--no-rollback` is given.
3. `bputil -d -v <group>` says this is the one true recoveryOS and that it is
   paired with the volume.
4. For a Mach-O object, `kmutil inspect` reports `kcbz` 0, so iBoot won't start
   SPTM.
5. For a raw image, Kernel CTRR is not enabled. Enrolling a Mach-O object turns it
   back on, and a raw Stage 1 then gets no secondary CPUs
   ([macho-boot-object.md](macho-boot-object.md#use)). If it is enabled, the script
   stops and prints the `bputil -nc -v <group>` to run first.

Without `--check`, the script then prints the kmutil command and asks you to type
`yes`. kmutil asks for the volume owner's password. bputil's output, kmutil's
output and the result all go to the log, which stays on the FAT volume for later.

To go back, run the script again with the kept image, adding `--raw` if it is
raw.

## What recoveryOS has

Recovery has few command-line tools, and its bash is version 3.2. The script uses
bash builtins and `md5`, `sed`, `tee`, `date`, `sync`, `script`, `diskutil`,
`bputil` and `kmutil`, and nothing else: there is no `grep`, `cat`, `dirname`,
`shasum` or `cmp`, which is why the checksums are MD5. `script` records kmutil's
prompts in the log. If it is missing, kmutil runs without it.

## Status

The script has not been run in Recovery. It is a generalization of scripts that
enrolled a Mach-O Stage 1 on a MacBook Air (M3, J613) in its 1TR on 2026-11-07
and 2026-11-08. Those scripts used the same commands and checks, apart
from the Kernel CTRR check, the volume group lookup through `diskutil info` and the
confirmation prompt, which are new here. This script was run on macOS 27.2 with
`PATH` limited to the tools above, stubs for `bputil`, `diskutil` and
`kmutil configure-boot`, and the real `kmutil inspect`.
