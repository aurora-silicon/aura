# Optional N1 preparation and census hardware test

This standalone test builds an EL1 microguest for the public Windows Aura
personality. A hardware run explicitly prepares J813 N1 through the production
producer, validates its descriptor, executes the bounded census HVC, then exits
through PSCI `SYSTEM_OFF`. Building is the default and never opens a device.

## Failure model, before implementation

- Missing or wrong device identity, an old capability ABI, or a resident without
  N1 support must be rejected before preparation. Hardware requires explicit
  device, chip and model arguments; the only supported N1 target is T8142,
  Mac17,3. The producer separately checks the board and live ADT resources.
- A failed preparation, malformed descriptor or resource mismatch must prevent
  guest entry. Never fabricate a descriptor, retry preparation, enable USB,
  load N1 firmware, install DART tables or use experimental guest patches.
- An incorrectly linked or modified image must not execute. Build twice, compare
  the raw templates, verify entry/result/request locations and request bytes,
  and bind the uploaded image (including its fresh nonce) to the launch profile.
- A skipped first instruction after HVC, corrupt/stale result, wrong ACK,
  unexpected status, non-32-bit value or incomplete run must fail. Store the
  HVC return immediately, then require all request and response fields, record
  ordinals, completion markers, nonce, entry EL, boot-arguments pointer and zero
  guard bytes on host readback.
- Each of the 17 whitelisted census reads must return success and the expected
  live N1 identity, BAR, ROM and DART invariants. Optional absent radios are
  represented by PCI all-ones identity; the test still requests all 17 addresses.
  If an absent function's command read faults, record that failure instead of
  substituting data or omitting the read.
- Wrong version, an unaligned address and a forbidden address must return the
  recognized ACK, `INVALID` status and zero data. Every request uses the correct
  magic; do not send unknown traps or fall back to direct guest MMIO.
- PSCI `SYSTEM_OFF` must stop the guest, return host status 1 and leave the
  resident proxy responsive. A returning `SYSTEM_OFF`, transport loss, timeout
  or forwarded guest exception must never pass based on console messages.
- A preparation failure can leave the controller quarantined. A timeout can
  leave the guest running. Record uncertain state and close the transport;
  never automatically reset, recover, retry or boot another guest.

## Usage

Build only (no device access):

```sh
python3 tools/windows_n1_smoke.py --output build/windows-n1-smoke-build
```

`--build-only` is also accepted explicitly. For a supervised run against a fresh
Windows Aura resident that includes the census HVC:

```sh
python3 tools/windows_n1_smoke.py --device /dev/cu.EXPLICIT_DEVICE \
  --chip-id 0x8142 --model Mac17,3 --output build/windows-n1-smoke-run
```

Use a new output directory each time. The hardware deadline defaults to 60
seconds. Compiler/linker/objcopy/objdump paths can be overridden with matching
options. The tool uses `aura.windows.HardwareSession` with `n1="j813-v1"`,
`usb="disabled"`, native framebuffer, and `guest_compatibility="none"`.

The result page is inside the image at offset `0x1000`. Its 128-byte header
contains magic `AN1SMK01`, ABI, phase, completed count, input/output nonce,
CurrentEL, entry x0 and completion marker. Twenty 64-byte records follow:
request address/version/reserved, response ACK/status/value/version, then the
record ordinal. Remaining bytes are zero guards. No firmware-owned scratch
address or guest device load/store is used.

`receipt.json` records source/artifact hashes, exact build commands, target
identity/capabilities, state transitions and observations. Raw descriptor and
before/after result pages, guest source/ELF/disassembly, logs and a hash-bound
profile remain in the output directory. A build-only receipt is not a hardware
pass, and local backend hashes do not attest the resident binary.

A pass qualifies this one preparation/census/PSCI-return path on the recorded
resident and target. It does not qualify Mu, Windows, N1 firmware execution,
DMA, radio traffic, interrupt delivery, reset recovery or subsequent ownership
transfer. The read-only guest does not reset N1 on exit; successful preparation
leaves it trained with bus mastering off and SID0 disabled.
