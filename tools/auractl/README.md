# auractl guest entry + J616s runbook

`auractl` (docs/auractl.md) controls aura's stage-2 MMIO trace from inside the macOS guest through
one HVC (`src/auractl_abi.h`). Userland cannot issue that HVC, so the guest supplies an *entry*
(`entry.h`) that carries each request to the guest's kernel. This directory holds the IOKit entry:

- `AuractlDevice/` — `AuractlDevice.kext`, a minimal IOKit driver. Its user client runs one HVC on
  the calling thread, so aura reads and writes the request through the caller's own EL0
  translation. It maps no hardware and keeps no state.
- `entry_iokit.c` — the `ENTRY=iokit` entry: it opens the kext and calls that one method.

There is no second ABI: the kext issues Dj's `hvc #0xac71` and nothing else.

## Build

aura, with the trace control compiled in (never in Stage 1):

    make CHAINLOADING=1 SPTM_VIRT=1 AURACTL=1

The tool and the kext, in the guest (macOS, Apple silicon):

    make -C tools/auractl ENTRY=iokit     # the tool, linked against IOKit
    make -C tools/auractl kext            # AuractlDevice.kext (needs Xcode's macOS SDK)

`make ENTRY=none` (the default) builds a tool that can only `auractl print` a saved file.

The kext is arm64e (the kernel's ABI); the tool is arm64. If the J616s macOS has no Xcode,
cross-build the kext on a Mac that does (its SDK must match the J616s macOS kernel version), then
copy the bundle over.

## Load the kext (SIP off)

The J616s macOS runs with SIP disabled, so a dev-signed kext loads:

    codesign --force --sign - AuractlDevice.kext      # ad-hoc; a real cert also works
    sudo kmutil load -p AuractlDevice.kext            # or: sudo kextload AuractlDevice.kext
    kmutil showloaded | grep -i auractl               # confirm it is resident

`sudo auractl status` then reports `entry: iokit` and aura's version.

Hazard: load this only in the macOS guest under an **AURACTL** build of aura. On any other aura the
HVC is reflected to the guest, which panics. The build enrolled for the proof carries `AURACTL=1`.

## The AURACTL HV object for the J616s

The enrolled Macintosh HD object must be built from this branch with the trace control in it and
the manifest flag set, using the same build + padding recipe HV70OBJ used (the untouched
`origin/feat/sptm-hv` tip does not link the ESP Stage 2 without the init-slot room fix, aura #210,
under it — see the lane note):

1. Build the macOS-HV ESP Stage 2 from this branch (rebased onto a `feat/sptm-hv` that carries
   #210) with `AURACTL=1` added to the HV70OBJ flags
   (`kits/hv/20261111-j616s-macos-trace/build.sh`), in the CI-matching rustup toolchain.
2. `make_bundle.py --flag AURA_AURACTL ...` so the manifest enables the trace ring, plus the
   session's keep-USB flags.
3. Pad and enrol it as the Macintosh HD HV object exactly as HV70OBJ/AURA70 enrol a padded object
   (1TR, Ryan present).

## End-to-end: trace the Magic Keyboard's USB controller, tetherless

With macOS booted under the AURACTL HV (no debug cable), as root in the guest:

    auractl status                                  # entry: iokit; aura version; ring present
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
carries, because some register reads fault the SoC. T8140's list ships in `src/sptm_virt_ctl.c`;
the J616s (T6040/T6041) list is not confirmed yet, so PEEK returns `EACCES` there (fail-safe). ARM,
START/STOP, READ, MARK and CONSOLE do not depend on it and work on the J616s.
