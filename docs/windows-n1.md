# Optional N1 handoff to Windows

The public Windows build contains an explicit J813 N1 preparation operation.
It is not called by the normal Linux or Windows boot path. It requires an
initialized Windows hypervisor before guest entry and permits one attempt per
Aura session. The operation does not load N1 firmware or install DART tables.

The producer checks the live J813 identity (`T8142`, board `0x24`, `Mac17,3`),
ADT register apertures, the PCI memory window and the GPIO/SMC reset functions.
The SMC reset provider must be `smc-pmu` below
`/arm-io/smc/iop-smc-nub`, with the expected controller, nub and provider types.
The bridge's reset phandle names that PMU child, not the SMC controller itself.
The exported six resources are subranges of those checked ADT apertures. Before
training, SID0 must be disabled, unprotected and without a page-table root; the
DART translation geometry and idle command state must also match. An existing
endpoint bus master causes rejection without resetting it.

Preparation reuses the public Linux `j813_n1_link()` sequence through the
`j813_n1_train_windows()` wrapper. It checks the trained link and endpoint
identities, assigns and reads back Control BAR0, verifies bus mastering remains
disabled, and requires cold ROM stage 0. DART state is checked again before a
handoff record is produced. If the initial DART register access is unavailable
before the link's power-enable sequence, preparation rejects the handoff; it
does not infer that the controller is safe to reset.

On a failure after training starts, the producer attempts the known PERST and
port drain sequence. Return `-1` means either no device writes occurred or the
failure cleanup completed. Return `-2` means cleanup could not prove drain;
the caller must quarantine the Windows session and prevent guest entry. A
failure clears the output record. The offline model does not prove that these
register observations establish an electrical or DMA drain on physical J813.

## Handoff record

Success returns the 128-byte little-endian `NW1H` version 1 record for the Mu
ADT property `/chosen/aura,windows-n1-v1`. Mu independently validates both the
record and current hardware state before exposing `ACPI\AURA8130`. The firmware
profile tag `0x32354776` names the 25G76-derived bring-up contract; it is not
firmware authentication or permission to load an arbitrary firmware image.

| Offset | Field |
| --- | --- |
| 0 | u32 magic `0x4831574e` |
| 4 | u16 version 1; u16 size 128 |
| 8 | u32 flags 7: exclusive controller, trained link, cold ROM/clean DART |
| 12 | u32 chip `0x8142` |
| 16 | u32 board `0x24` |
| 20 | u32 firmware profile `0x32354776` |
| 24 | u16 RIDs `0x100`, `0x101`, `0x102`; u16 SID 0 |
| 32 | Six u64 base/u64 length pairs below |

| Resource | Base | Length |
| --- | --- | --- |
| ECAM, buses 0–1 | `0x1cb0000000` | `0x200000` |
| APCIE DART | `0x488000000` | `0x20000` |
| Port | `0x488028000` | `0x4000` |
| PHY | `0x48f010000` | `0x1000` |
| PERST GPIO | `0x39a000300` | 4 |
| CPU BAR window | `0xb80000000` | `0x600000` |

## Guarded firmware census

Mu reads the live census through HVC `#0`, not direct guest MMIO. The service is
available only after this Aura session completed N1 preparation successfully,
and only on the boot CPU. It permits exactly the 17 aligned 32-bit reads used by
the version 1 census: port link state, root identity, endpoint identities and
commands, Control BAR0, ROM stage, and the DART geometry/protection/SID0 state.
The resource ranges in the record do not authorize other reads or any writes.

| Register | Request | Response |
| --- | --- | --- |
| x0 | `0x4e3152443332` (`N1RD32`) | `0x4e3141434b31` (`N1ACK1`) |
| x1 | Physical address | Signed status |
| x2 | Version 1 | Zero-extended u32 value; zero on failure |
| x3 | Reserved, must be zero | Version 1 |

Status 0 is success; `-1` is unavailable, `-2` is an invalid request/address,
and `-3` is a guarded bus fault. Unknown magic is left for the other HVC
handlers. Mu must check the exact acknowledgement, version, status and value
width, and must not fall back to raw MMIO when the service is absent or fails.
The service refuses reads while another exception guard is active or SError is
masked/pending. It uses Aura's exception guard and completion barriers to turn
reported faults into an error response. Physical fault delivery and recovery
remain subject to hardware qualification.

## Repeatable offline evidence

With Clang and its address/undefined-behavior sanitizers available:

```sh
python3 tools/windows_n1_e2e.py --output build/windows-n1-e2e
```

Use a new output directory for every run. The runner copies the production
producer, its dependencies and the existing failure-first model into a source
snapshot, compiles that snapshot with ASan/UBSan, and runs the 40 scenarios in
separate processes. The tests exercise the complete preparation entry point
against modelled ADT, MMIO and endpoint behavior. They cover successful handoff,
identity/resource/reset-function rejection, locked/active/busy DART, pre-existing
or newly enabled bus mastering, training failure, wrong endpoint identity, BAR
probe/readback faults, active ROM, and failed cleanup quarantine.
The reset-provider fixture retains only paths, compatible strings, phandles and
the reset tuple from a captured J813 ADT. It reproduces the controller-versus-PMU
phandle mismatch and checks that missing, moved or mistyped providers and changed
reset targets/operations are rejected before MMIO. The rest of the ADT service
remains modeled; this does not test the production ADT parser.
The census scenarios additionally exercise preparation gating, nonzero boot
CPU identity, protocol/address rejection, all 17 allowed reads, nested guard
and SError-state rejection, and load/barrier fault reporting without writes.

`result.json` records source and binary hashes, compiler version, exact commands,
sanitizer settings, per-scenario transcripts and the emitted descriptor hash.
The descriptor is retained at `cases/success/descriptor.bin`. Preparation and
census assertions were each written before their corresponding implementation.

The host preparation gate is stubbed enabled. The tests do not exercise
the HVC instruction/exception dispatcher, hypervisor lifecycle gating,
one-shot reentry, physical PHY/SMC operations,
Windows PnP, interrupt delivery or DMA. The Linux training call is modelled;
the production Linux function and its default caller are unchanged by the
Windows wrapper. A physical Aura → Mu → Windows handoff and reset/drain receipt
is still required before merge or a hardware-support claim.
