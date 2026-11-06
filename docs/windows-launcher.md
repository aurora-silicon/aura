# Explicit Aura → Mu → Windows launcher

This is an experimental, profile-driven Windows guest path. It does not claim
that every Apple Silicon Mac is supported or tested. Normal Aura guest loading
keeps its existing behavior. Windows requires an explicitly built Windows
personality and a matching Mu image.

## Failure model, before implementation

- A normal, old or incompatible Aura resident must be rejected before changing
  baud, heap limits, registers, USB usage or guest mappings. Probe the read-only
  Windows capability opcode first; a failed capability probe must not fall
  through to the legacy allocator ABI.
- A malformed profile, wrong image hash, path, entry point, chip or model must
  fail before target mutation. Read identity from boot arguments and ADT using
  bounded reads without constructing the allocating ProxyUtils object.
- Replacing a firmware file between validation and upload must not change the
  bytes launched. Retain the hash-verified image in host memory.
- Mu runtime firmware must start on a 64-KiB boundary and fit the guest memory
  range; all later addresses must be calculated from that aligned base. Keep
  the normal guest loader's alignment behavior unchanged.
- Windows C code owns HCR, timer/PMU routing and guest system-register setup.
  The generic host loader must not overwrite that state after `hv_init` or
  enable the default SPRR emulation/instruction-patching path.
- Unsupported RVBAR writes remain forbidden by the existing CPU feature
  checks. Windows uses PSCI instead of the generic guest CPU-start hooks.
- Changing the advertised framebuffer depth without a real conversion backend
  is not permitted. A profile that needs BGRA conversion requires its matching
  capability bit.
- An unexpected exception, transport loss or guest return must not be reported
  as Windows readiness or successful shutdown. Retain the exact profile,
  source fingerprints and console log with an explicitly incomplete result.
- ADT growth must be rejected before upload; final ADT capacity is checked
  before start changes device usage or page tables. A failed watchdog start
  must prevent guest entry. Return/error paths must restore host signal
  handlers and serial timeout. Exceptions forwarded to Python must abort the
  Windows guest instead of entering generic emulation or an interactive shell.
- An appended RAM disk must land at its firmware-defined offset, even when the
  Mu FD file is shorter than its reserved firmware region. Explicit append
  offsets may introduce zero padding but must not overlap earlier parts,
  exceed the 1-GiB payload limit, or change the hash-checked file bytes. The
  composed image must still fit live guest RAM before upload.
- Large, already compressed WinPE images must not require a second image-sized
  target allocation. Windows uploads must use bounded compression chunks that
  fit the proxy heap, retain exact image offsets and stop on any chunk failure.
- USB has no validated ownership handoff in this path. The default profile
  disables guest USB and must hide unhanded USB controllers, their DARTs and
  dependent aliases before Mu's early DART driver can touch them. A static
  ACPI table that still exposes removed USB hardware must also be suppressed
  by Mu; ADT pruning alone is not proof of safe Windows USB isolation.
- N1 defaults to disabled. Only an explicit J813 profile may request the
  producer after successful HV initialization; missing capability, wrong
  chip/model, failed production/readback, or any changed NW1H header/resource
  field must prevent image upload and guest entry. Reserve the 128-byte ADT
  property before any target mutation. A failed attempt must not be retried or
  bypassed by switching the same host session back to disabled.
- Boot-arguments pointers use unsigned 64-bit modular translation. A wrapped
  virtual ADT address must resolve correctly, while the resulting complete
  physical span must remain inside the reported RAM window. Reject invalid
  spans before reading ADT memory and perform no target mutation in identity.

## Capability contract

`P_HV_WINDOWS_CAPS = 0xc80` is read-only. Its result is a 64-bit value:
bits 63:32 `0x4157494e` (AWIN), bits 31:16 ABI version 1, and low 16 feature bits.
Bits 0–3 are required: Windows personality, native AIC, PSCI, and raw Mu loader
contract. Bit 4 advertises a BGRA framebuffer shadow/converter. Bit 5 is the
independently gated N1 handoff producer. Bit 6 advertises explicit configuration.
Normal builds return zero. The launcher does not treat a capability bit as
hardware qualification.

`P_HV_WINDOWS_CONFIG = 0xc81` sets Windows options before `hv_init`: argument bit
0 opts into the experimental T8142 instruction/vector rewrites and guessed SP
repair; bit 1 requests BGRA shadow conversion. It returns 0 on acceptance and
-1 for invalid flags, unsupported target or an invalid lifecycle state. The
host requires capability bit 6 for either option. The C backend verifies the
scanout format, creates the shadow and changes guest boot arguments only after
successful setup; the host never spoofs depth. An unsuccessful requested
conversion prevents entry to the guest.

The dedicated `0xc80` namespace deliberately avoids legacy m1n1 PSCI opcodes
`0xc11..0xc18`, which can mutate CPU state. In particular, never probe `0xc12`
or `0xc13` for capabilities. `0xc82` is the separate N1 preparation
operation. Unknown capability commands are rejected without a legacy fallback.

The raw loader contract is the existing public boot-arguments ABI: the guest
receives its boot-arguments pointer in x0, the image starts at a 64-KiB-aligned
physical address, entry is `image_base + entry_offset`, and ADT virtual/physical
translation follows the boot arguments. SEPFW, TrustCache and optional preoslog
are relocated with the image. The Windows C personality supplies the Mu ADT
alias and interrupt/PSCI environment.

## Profile and launch

Use an already resident Windows Aura build. This tool does not enter DFU,
install Aura, discover a target, or take over another process's device session.
Load a fresh Aura image into RAM for each guest run: the Windows personality
admits one guest per resident session and rejects retry after exit. Capability
admission also requires a Mac, the native AIC backend and fast-IPI support.
Use the same Python environment as the other proxyclient tools (dependencies
are pinned in `requirements.txt`). Prepare a profile for the exact physical
target and the exact Mu build; paths are relative to the profile file:

```json
{
  "schema": "aura.windows.boot.v1",
  "name": "local-j813-experiment",
  "target": {"chip_id": "0x8142", "model": "Mac17,3"},
  "mu": {
    "path": "Mu.fd",
    "sha256": "REPLACE_WITH_64_LOWERCASE_HEX_SHA256",
    "entry_offset": "0x0"
  },
  "framebuffer": "native",
  "guest_compatibility": "none",
  "usb": "disabled",
  "n1": "disabled"
}
```

The example is a template, not a qualified profile. Determine the entry offset
from the specific Mu build; the current raw Mu FD starts at offset zero,
whereas a separately wrapped image may use another offset. Optional `append`
is an ordered array of additional images. Each record binds `path` and `sha256`
and may set an absolute `offset` relative to Mu's image base. An omitted offset
places the image directly after the preceding image; an explicit offset must
not overlap it. Gaps are filled with zeros only after every source hash passes.
The complete image, including padding, is limited to 1 GiB and must fit live
guest RAM. Upload uses 16-MiB compressed chunks to fit the proxy staging heap.
For the current Mu build, `PcdFdSize` is `0x1e00000`, larger than the
`0x1d88000` FD file: a RAM disk that Mu probes at `base + PcdFdSize` needs:

```json
"append": [{"path": "winpe-ramdisk.img", "sha256": "REPLACE_WITH_SHA256", "offset": "0x1e00000"}]
```

Use the reserved size from the exact Mu build; it is not a universal constant.
The launch receipt records each offset and both source and composed hashes.
Optional `boot_args` is at most 255 ASCII bytes. Select `bgra-shadow`
only when conversion is needed and supported. Select `t8142-experimental` only
for a deliberately tested T8142 experiment; it is never inferred from the
model and is rejected for other chips. The normal/default profile requests no
experimental guest rewrites.

`usb` defaults to `disabled`, currently the only supported value. The host
removes all `usb-drdN` and `dart-usbN` nodes, aliases to those nodes and stale
console paths, and emits `/chosen/aura,windows-usb-disabled` as exactly the
little-endian u32 value 1. This prevents Mu's early USB DART enumeration. A
matching Mu build must also consume that property and set USB ACPI device
status to zero; older Mu builds with static present USB devices are unsuitable
for this profile. This policy preserves unrelated display nodes.

`n1` defaults to `disabled`, which removes any stale N1 handoff property and
never invokes the producer. `j813-v1` is an explicit opt-in restricted to
chip `0x8142`, model `Mac17,3`, and resident capability bit 5. After successful
`hv_init`, the host calls `P_HV_WINDOWS_N1_PREPARE = 0xc82` with a 128-byte output
buffer, then verifies every header field and all six resource ranges against
the [J813 N1 contract](windows-n1.md). Only then is the raw descriptor published
as `/chosen/aura,windows-n1-v1`. The ADT reserves room for it during read-only
preflight. Any producer, readback or validation failure prevents guest loading
and entry for that session, including an attempt to switch back to disabled.
It requires the matching opt-in Mu profile; it does not establish Windows
driver operation or permit a second attempt without a fresh Aura load.

```sh
python3 proxyclient/tools/boot_windows.py validate --profile local-profile.json
python3 proxyclient/tools/boot_windows.py boot --profile local-profile.json \
  --device /dev/cu.EXPLICIT_RESIDENT_DEVICE --out /tmp/windows-run-UNIQUE
```

Validation is offline and opens no device. Boot checks capabilities and live
chip/model before any target allocation, configuration or loading. The result
directory retains the exact profile, image hashes, host source fingerprints,
console log and `launch.json`. A guest exit is reported as unqualified; status
1 means guest poweroff and status 2 means guest reset requested. Both return to
the resident proxy after C drains guest CPUs, without asserting a physical
machine shutdown/reboot or that Windows reached readiness. A complete hardware
acceptance receipt must additionally identify the resident build, Mu build,
physical target and observed Windows milestones.

An exception forwarded to Python requests guest exit and reports an error;
the Windows path never opens the generic interactive exception shell. Guest
return and error restore the prior host SIGINT/SIGQUIT handlers and serial
timeout. An actual panic or broken transport may still require the established
device recovery path before loading a fresh resident image.

## Repeatable host E2E artifacts

```sh
python3 proxyclient/tools/windows_launcher_e2e.py --out /tmp/windows-admission-UNIQUE
python3 proxyclient/tools/windows_hv_e2e.py --out /tmp/windows-host-hv-UNIQUE
```

The first exercises the real profile loader, CLI and launch orchestration with
an injected transport boundary, including wrong-target/hash/capability cases.
The second exercises real `HV.init`, `load_raw` and `start` with serialized ADT
and boot arguments, using a modeled proxy. It rejects any host system-register
write or locked-target RVBAR write and verifies configuration ordering, aligned
payload placement, unchanged depth and out-of-RAM rejection. Its artifact
includes uploaded image/ADT/boot-arguments blobs, hashes and a host log. Both
produce `result.json` with replay commands and source hashes. Neither executes
the C backend or guest, so neither replaces hardware testing before merge.
