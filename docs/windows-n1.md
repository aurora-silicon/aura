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
The exported six resources are subranges of those checked ADT apertures. All
PHY, PHY-IP, AXI, port and root DBI tunable widths/alignment/bounds are validated
before reset or power changes.

Cold admission uses seven exact APCIE gate names and their captured parent
relationships: APCIE_GP, APCIE_SYS_GP, APCIE_ST, APCIE_SYS_ST, APCIE_PHY_SW,
ST0_AUS5_AON and ST0_AUS5. Every gate must have target and actual power state 0,
with reset and automatic power control clear. State 4 is clock-gated and may
retain a live owner, so it is rejected. Shared ANS and ancestor gates are not
required to be off. Every observed state is logged; an active or unknown gate
rejects preparation before GPIO or power changes.

After admission, GPIO PERST is asserted and read back. The producer executes
the public Linux PHY/PLL/pclk/AXI and root-controller prefix, then completes the
unchanged SMC reset, port/refclk/PERST and root DBI sequence. It requires link
establishment and verifies every present endpoint's identity and bus-master bit
before the first DART access. No endpoint bus-master enable or firmware load is
added. Active or unknown ownership is rejected by cold-gate admission before
this reset sequence.

The [physical held-reset prefix probe](validation/j813-n1-held-reset-dart-20261106.json)
completed, but its first DART read at
`0x488000000` stalled with SError. Thus neither PMGR activation nor PHY/root
setup alone proves DART accessibility. The public Linux
[J813 N1 device tree](https://github.com/aurora-silicon/linux/blob/3a0da31bcc027d4b0a28b0501e649522c7a232f6/arch/arm64/boot/dts/apple/t8142-j813-n1.dtsi)
requires loader-trained PCIe before Linux takes over the ordinary T8110 DART.
The revised ordering follows that boundary; hardware qualification of this
candidate remains pending.

Both clean-DART checks remain: before and after Control BAR assignment. Each
address is logged before its guarded read and on completion. SID0 must be
disabled, unprotected and without a page-table root; translation geometry and
idle command state must match. An active or busy DART is rejected, never cleared.
Every present endpoint must still have bus mastering disabled. Control BAR0 is assigned and read back, ROM must be at cold
stage 0, and DART/endpoint state is rechecked before producing the record.
The ordinary Linux entry still runs the same complete ordered sequence.

Failure before port/root readiness uses only the validated GPIO clamp and
returns `-2`; it does not probe an unready port to claim drain. After readiness,
the producer attempts the existing PERST and full port drain. Return `-1`
means admission failed without device changes or full cleanup completed.
Return `-2` means drain remains unproved; the caller must quarantine the Windows
session and prevent guest entry. Every failure clears the output record.
Neither the offline model nor GPIO readback establishes electrical/DMA drain.

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
snapshot, compiles that snapshot with ASan/UBSan, and runs the producer/census scenarios in
separate processes. The tests exercise the complete preparation entry point
against modelled ADT, MMIO and endpoint behavior. They cover successful handoff,
identity/resource/reset-function rejection, locked/active/busy DART, cold owner rejection and bus mastering on any of the three functions, first-DART faults,
translation state changed at BAR assignment, training failure, wrong endpoint identity, BAR
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

A second fixture compiles the actual production Linux/Windows PHY sequence.
Its Linux hardware-action transcript must match the retained pre-split public
source baseline `c30bec09`; the Windows replay rejects port/ECAM/SMC access in
the prefix and checks stage ordering and repeated-entry rejection. The two
sequence logs and hashes are included in the receipt. MMIO, tunable application
and SMC transport remain modeled boundaries.

The host preparation gate is stubbed enabled. The tests do not execute the HVC
instruction/exception dispatcher, hypervisor lifecycle gating, physical PHY/SMC
operations, Windows PnP, interrupt delivery or DMA. A physical Aura → Mu →
Windows handoff and reset/drain receipt is required before merge or a hardware
support claim.


## Physical admission attempt: 2026-11-06

The candidate at `3a86afbb5184b25810b7d6f53ff4618d4ca79054` was RAM-chainloaded
and exercised through the production N1 census-microguest launcher. It rejected
the seven inherited APCIE gate states before any N1 reset/power write, DART read,
descriptor publication or guest entry. The new post-link DART boundary was not
reached, so it remains unqualified on hardware.

The [admission and recovery receipt](validation/j813-n1-dart-order-20261106.json)
retains all seven raw words and the source/image/log hashes. They exactly match
the gate values captured before any HV/guest preparation in the earlier
held-reset diagnostic. This is an ownership-handoff prerequisite, not evidence
that the reordered DART read succeeded. The earlier GPIO snapshot is separately
labelled; this attempt did not read GPIO after rejecting the gates. No admission
condition was relaxed or unknown owner state cleared to proceed.

The attempt was followed by a separate external reset and restoration of the
qualified CPU Aura/Mu images. The complete 1 MiB CDC readback and zero-exception
guard passed; the Windows agent responded, and the installed CPU driver again
reported Running/Code0 with its exact signed binary hash and E2/P10 state.
No frequency SET, N1 firmware load, endpoint bus-master enable or DART mapping
was performed in the restored Windows session.
