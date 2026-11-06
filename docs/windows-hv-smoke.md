# Windows hypervisor smoke guest

This test exercises the public Windows Aura personality before loading Mu. It
builds a freestanding AArch64 guest, checks PSCI calls through SMC and HVC, then
uses PSCI `SYSTEM_OFF` to return to the resident proxy. It does not boot Windows
or qualify a Mac as Windows compatible.

## Failure model, before implementation

- A normal/old resident, incompatible capability ABI, wrong chip or wrong model
  must be rejected before guest allocation, mapping or execution. A device path
  and exact chip/model must be explicit; importing the test must not open one.
- A mislinked guest or changed file must not execute. Build a raw position
  independent image with an explicit entry point and fixed result block; retain
  its source, ELF, disassembly, binary, hashes and hash-bound launch profile.
- Incorrect exception return PCs can skip the first instruction after SMC/HVC.
  Store each result immediately after its call and check both conduits.
- Wrong PSCI dispatch, clobbered guest registers, wrong initial exception level,
  stale memory, partial execution or a false host return must not pass. Require
  an input nonce copied by the guest, completion markers, expected results and
  host status 1 only after `SYSTEM_OFF` drains the guest CPUs.
- An unsupported PSCI function must return `NOT_SUPPORTED`; it must not start a
  CPU, shut down the physical machine, reset it or modify unrelated devices.
- `SYSTEM_OFF` must not return to its guest caller. Mark any such return as
  failure. A host timeout, transport loss or exception must save a failed receipt
  and must never become a success based only on printed guest output.
- The test launches only the boot CPU. No advertised capability currently
  distinguishes tested secondary-CPU startup/off support, so this test makes no
  SMP qualification claim.
- On failure, close the host transport without automatically resetting,
  reinstalling, recovering, retrying or starting another guest. A timed-out
  guest may remain running; the receipt must say its final state is unknown.

## Usage

Build only, without opening a device:

```sh
python3 tools/windows_hv_smoke.py --build-only --chip-id 0x8142 \
  --model Mac17,3 --output build/windows-hv-smoke-build
```

For a supervised run against an already booted public Windows Aura resident:

```sh
python3 tools/windows_hv_smoke.py --device /dev/cu.EXPLICIT_DEVICE \
  --chip-id 0x8142 --model Mac17,3 --output build/windows-hv-smoke-run
```

The output directory must be new. The tool uses Clang, LLD and LLVM objcopy/
objdump; override their locations with the matching command-line options if
they are not available on `PATH`. Hardware runs use `aura.windows.HardwareSession`
and `HV(windows=True)` with native framebuffer policy and no experimental guest
compatibility patches. The deadline defaults to 30 seconds.

`receipt.json` records build-only, passed or failed status, whether target
mutation began, the exact target identity/capabilities, guest result bytes,
source/artifact hashes, command lines and logs. The built launch profile is an
input to the normal immutable-image validator. The guest result block is part
of the uploaded image at offset `0x1000`; it never uses an arbitrary host or
firmware-owned scratch address.

A pass establishes entry at EL1, the tested PSCI SMC/HVC dispatch and return
paths, guest-memory visibility, and return to the proxy after the boot CPU exits.
It does not establish Mu boot, Windows boot, N1, ACPI, DMA, interrupt delivery,
framebuffer conversion, suspend/resume, secondary-CPU startup or reset handling.
