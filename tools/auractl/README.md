<!-- SPDX-License-Identifier: MIT -->
# auractl

In-guest control for aura's SPTM-HV MMIO tracing. `auractl` runs inside the macOS guest under
aura's hypervisor and arms, starts, stops, and dumps hypervisor MMIO traces itself -- no debug
cable, and without taking a USB port from macOS.

The hypervisor exposes one 16 KiB synthetic MMIO page (trapped with `hv_map_hook` / `SPTE_HOOK`).
The page is not backed by RAM or any Apple block; aura implements every register from its own state.
The register file and opcodes are defined in `sptm_virt_auractl_abi.h`, which aura, this kext, and
this tool all share.

## Contents

- `sptm_virt_auractl_abi.h` -- verbatim copy of aura's `src/sptm_virt_auractl_abi.h`. Keep in sync.
- `AuractlDevice/` -- the kext: an `IOService` (`AuractlDevice`) that maps the control page as
  device memory, and an `IOUserClient` (`AuractlUserClient`) that the tool talks to.
- `auractl.c` -- the userland tool.
- `Makefile` -- `tool`, `kext`, `host-check`, `clean`.

## How it maps the page

MVP: the kext maps the fixed guest-physical page `AURACTL_MMIO_BASE` (from the ABI header) as device
memory, uncached (`kIOMapInhibitCache`) so MMIO ordering holds. SIP is disabled on the J616s macOS,
so a dev-signed kext can map a fixed physical page directly. There is no ADT dependency.

Discoverable alternative (not the MVP): add an `aura,auractl` node to the guest ADT and match it as
an `IOService`, taking the `reg` from the node. The stage-2 hook fires on the access either way.

## Transport

The tool reaches the kext through `IOServiceOpen`. It prefers a direct mapping of the control page
(`IOConnectMapMemory`, user client memory type 0) and drives the registers itself. If the
environment refuses to map device memory into userland, the tool falls back to two scalar methods
the kext exposes: `doCommand(cmd, arg0, arg1) -> status` and `readReg(off) -> value` (plus
`writeReg`). The kext validates every offset: within the register file and 8-byte aligned.

## Build (on the Mac)

```
make tool        # builds ./auractl, links IOKit + CoreFoundation
make kext        # builds ./AuractlDevice.kext (unsigned)
```

`make kext` uses `clang` against `Kernel.framework` for `arm64e`. Override `SDKROOT` or `KARCH` on
the command line if your SDK path or target differs.

## Dev-sign and load (SIP off)

The J616s macOS runs with SIP disabled. Sign the kext with your development identity, then load it:

```
codesign --force --sign "<Your Developer ID>" AuractlDevice.kext
kmutil load -p AuractlDevice.kext        # or, legacy: kextload AuractlDevice.kext
kmutil showloaded | grep AuractlDevice   # confirm it loaded
```

On load, the kext probes `AURACTL_REG_MAGIC`. If the magic does not read back as `AURACTL_MAGIC`,
it logs a line (`no device at 0x...`) but stays up so you can run `auractl status` and diagnose.

## Run

```
auractl status                      # probe + decode the registers
auractl reset                       # clear ranges, ring, counters (-> IDLE)
auractl arm <base>:<size> [...]     # add trace range(s); base/size in hex (0x..) or decimal
auractl start                       # ARMED -> RUNNING
auractl stop                        # RUNNING -> STOPPED
auractl dump [-o file]              # latch + pop the ring to a raw record file
```

`arm` writes `ARG0=base`, `ARG1=size`, then the `ARM` doorbell, and reads `STATUS` back. If the
error field is not `NONE` it prints which `AURACTL_ERR_*` and exits non-zero -- for example
`BADRANGE` if the range is not in guest device-MMIO space, is misaligned, or is zero.

`dump` output is binary, so it goes to a file (default `./auractl-dump.bin`, override with `-o`).
Each record is four little-endian `u64` -- `t`, `meta`, `val`, `pc`, 32 bytes -- the same layout the
hypervisor's ring uses. The tool reports the record count.

## Decode the dump

The dump file uses the record layout `ring-dump.py` reads (`AURACTL_REC_BYTES = 32`; `meta` =
`pa[47:0] | width[50:48] | write[51] | cpu[55:52]`, decoded by the `AURACTL_META_*` macros). Run the
decoder against the dump:

```
ring-dump.py kbd.bin       # or whatever input flag your copy of ring-dump.py takes
```

## End-to-end hardware proof (J616s, tetherless)

The goal is to capture the USB controller's MMIO traffic when a Magic Keyboard is plugged and
unplugged, with no debug cable.

1. Boot macOS under the hypervisor by the qualified entry path. No tether is needed for arm, start,
   stop, or dump.

2. Read the USB controller's MMIO base and size live from the guest -- do not hardcode an Apple
   address. From the guest, read the controller's `reg` property out of the IORegistry, for example:

   ```
   ioreg -l -p IODeviceTree | grep -A4 -i usb        # find the USB controller node
   ```

   The node carries a `reg` property (base/size pairs, parented through `ranges`); take the base and
   size of the controller window from it. Pass that base and size to `arm`.

3. Arm, start, exercise the device, stop, dump:

   ```
   auractl arm <usb-controller-mmio-base>:<usb-controller-mmio-size>
   auractl start
   # plug in the Magic Keyboard, then unplug it
   auractl stop
   auractl dump -o kbd.bin
   ```

4. Decode `kbd.bin` with `ring-dump.py` and read the captured controller accesses.

## Notes

- `arm` adds ranges one at a time; the hypervisor caps the count and total size, and rejects ranges
  outside guest device-MMIO space (RAM, the aura carve-out, SPTM, out-of-ADT), misaligned ranges,
  and zero-size ranges. The tool only observes the guest's own accesses -- it never injects writes.
- `status` reads `CAPS` for the maximum range count, record size, and whether the dump FIFO is
  present, and decodes `STATUS` into a state and a last-error string.
