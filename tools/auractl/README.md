# auractl guest entries

`auractl` (docs/auractl.md) controls aura's stage-2 MMIO trace from inside the macOS guest. A
request reaches aura through an *entry* (`entry.h`). This directory holds two, both driving the one
ABI (`src/auractl_abi.h`) and the one handler:

- `entry_dcc.c` — the `ENTRY=dcc` entry. An unprivileged process writes its request's address to
  the EL0 debug register `DBGDTR_EL0`, which aura traps to EL2 (`MDCR_EL2.TDCC`). **No kext, no
  root, no SIP change.** Use this where the kext cannot load (below) — the J616s, for one.
- `AuractlDevice/` + `entry_iokit.c` — the `ENTRY=iokit` entry. `AuractlDevice.kext`, a minimal
  IOKit driver, runs one `hvc #0xac71` on the calling thread. It maps no hardware and keeps no
  state. It needs root, SIP off, and a kext that will load.

Neither adds a second ABI: both carry the same `struct auractl_req`.

## Build

aura, with the trace control compiled in (never in Stage 1):

    make CHAINLOADING=1 SPTM_VIRT=1 AURACTL=1

The tool, in the guest (macOS, Apple silicon):

    make -C tools/auractl ENTRY=dcc       # kext-free; no root, no SIP change, no kext to load
    make -C tools/auractl ENTRY=iokit     # through the kext; links against IOKit
    make -C tools/auractl kext            # AuractlDevice.kext (needs Xcode's macOS SDK)

`make ENTRY=none` (the default) builds a tool that can only `auractl print` a saved file. The
`dcc` tool is plain C with two inline-asm instructions: any clang in the guest builds it, and it
needs no frameworks and no SDK beyond libc.

The kext is arm64e (the kernel's ABI); the tool is arm64. If the guest macOS has no Xcode,
cross-build the kext on a Mac that does (its SDK must match the guest macOS kernel version), then
copy the bundle over.

## Using the DCC entry (no kext)

`ENTRY=dcc` needs nothing loaded. Build the tool and run it:

    make -C tools/auractl ENTRY=dcc
    ./auractl status        # reports `entry: dcc`, aura's version, and whether the ring is present

`status` works for any user. The aura the guest runs under must be an **AURACTL** build booted with
the DCC transport armed (`AURA_AURACTL_DCC=1`, or `--flag AURA_AURACTL_DCC` for an ESP bundle); that
is what sets `MDCR_EL2.TDCC` so the `DBGDTR_EL0` access traps to aura. On an aura without it, or bare
hardware, the access does not trap and the tool reports `ENODEV` (it checks the tag aura puts in the
read-back), so it never acts on stray debug-channel state.

Everything past `status` — `arm`, `start`/`stop`, `read`, `mark`, `peek` — needs **root**, because
it needs the per-boot cookie aura put in the guest boot-args, which only root can read (`sysctl
kern.bootargs`). The tool reads it and presents it automatically; a non-root caller gets `EPERM`.

## Load the kext (SIP off)

Loading a dev-signed kext needs SIP off and root; ad-hoc signing is enough:

    codesign --force --sign - AuractlDevice.kext      # ad-hoc; a real cert also works
    sudo kextload AuractlDevice.kext                  # see the kmutil note below
    kmutil showloaded | grep -i auractl               # confirm it is resident

`sudo auractl status` then reports `entry: iokit` and aura's version.

`make kext` stamps the bundle's `kmod_info` and an `LC_BUILD_VERSION`, so the Mach-O carries the
bundle id, version and start/stop entry points `kextload`/`kmutil` need.

Note on `kmutil load`: it does not work under SPTM-virt. `kmutil` builds an AuxKC and splices it
into the boot kext collection, but here the BootKC is fed by the host loader, not written by the
booted macOS, so there is no writable collection for it to extend (measured on the J616s). Use the
legacy `kextload`, which loads the kext into the running kernel directly, instead.

Hazard: load this only in the macOS guest under an **AURACTL** build of aura. On any other aura the
HVC is reflected to the guest, which panics.

## Booting macOS under an AURACTL aura

To trace tetherless, macOS must boot as the guest of an aura built with `AURACTL=1` and with the
trace ring enabled in its manifest. Building that Stage 2 object and enrolling it as a boot object
is the normal aura HV enrol flow (see the aura HV docs); it is not auractl-specific, so it is not
repeated here. Once macOS boots under such an aura, `auractl status` reports the ring is present.

## End-to-end: trace a USB controller, tetherless

With macOS booted under an AURACTL build of aura (no debug cable), in the guest (the `dcc` entry
needs no root; the `iokit` entry does):

    auractl status                                  # entry; aura version; ring present
    auractl list                                    # the catalogue aura built from this ADT
    # find the USB controller's MMIO base LIVE from the guest, do not hardcode it:
    ioreg -l -w0 | grep -i -A3 'usb.*reg'           # or the ATC/dwc3 node's `reg`
    auractl arm 0x<usb-reg-base>+0x4000             # a physical range in a split block
    auractl mark kbd-plug 1
    auractl start
    # plug / unplug the Magic Keyboard
    auractl stop
    auractl read /tmp/kbd.trc                       # drains the ring to a file
    auractl print /tmp/kbd.trc                      # or:
    ./decode.py --base 0x<usb-reg-base> /tmp/kbd.trc # offsets relative to the controller

The plug/unplug shows as writes and reads to the controller's registers in the rows.

## decode.py

`decode.py` reads the same `AURATRC1` file `auractl read` writes and prints the records, like
`auractl print`, with a few extras for scripting: `--base` to show offsets relative to a register
block, `--writes-only`, `--pa` to keep only one page/range, and `--csv`. It runs anywhere Python 3
does, so a dump copied off the Mac decodes on the buildbox.

    ./decode.py dump.trc
    ./decode.py --base 0x230000000 --writes-only dump.trc
    ./decode.py --csv dump.trc > dump.csv

## PEEK and the chip's never-read list

`auractl peek` reads a few plain PMGR registers, and only on a chip whose never-read page list aura
carries, because some register reads fault the SoC. T8140's list ships in `src/sptm_virt_ctl.c`.
PEEK is not enabled yet on any other chip, the J616s (T6040/T6041) included: with no confirmed
list aura returns `EACCES` there (fail-safe). ARM, START/STOP, READ, MARK and CONSOLE do not
depend on it and work everywhere.
