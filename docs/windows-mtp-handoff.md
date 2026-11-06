# Experimental Windows MTP handoff

## Failure model (recorded before implementation)

- A requested mode is not proof of a prepared device. Keep the guest MTP
  disabled marker set until the producer has prepared both DART and stage-2
  mappings; refuse guest entry after any preparation or final-map failure.
- Wrong-board addresses, mapper stream IDs, DART generations, malformed ADT
  segments or inherited active DMA must fail before destructive setup. The
  first supported profile is explicitly J813 / Mac17,3 / T8142 only.
- The fixed staging and grant pool can overlap Aura, its heaps, the Mu image,
  boot arguments, ADT or firmware allocations. Require host memory preflight
  for the entire physical range 0x10020000000..0x10020200000 before target
  allocation or writes; recheck resident ownership in the C producer.
- Mapping only the faulting telemetry page would hide the missing handoff.
  Prepare the complete 1 MiB DART staging window at DVA 0x1800000, backed by
  physical 0x10020000000, and the matching guest stage-2 alias. Reserve the
  following 1 MiB for all AP-owned RTKit grants, with no heap fallback.
- IOP-owned shared buffers must stay inside the validated fixed SRAM window.
  Reject arithmetic overflow, zero/oversized requests and pool exhaustion.
  Preserve existing Aura RTKit buffer ownership and display adoption behavior.
- Reading DockChannel RX data would steal Windows' INIT packets. During
  preparation inspect only RX_COUNT; never consume FIFO data, acknowledge
  DockChannel interrupts or change its thresholds/masks.
- The IOP needs early AP power and continuing RTKit acknowledgements. Bound
  startup and service work; guest-time service must not block on a full ASC
  mailbox, allocate outside the pool, flood logs or race another servicing CPU.
- A failed or retired handoff must not free page tables/buffers while DMA can
  still reach them. Stop the IOP, block and flush its DART stream, and retain
  all allocations for the lifetime of this resident. A failed stop must
  quarantine this Aura session. Never retry the handoff in the same resident.
- Disabled profiles must not expose a working MTP device to Windows. A matching
  Mu build must honor the explicit disabled property; absence of this firmware
  behavior is not implied support for older images.

Validation must exercise the production allocation, resource admission,
failure/quarantine and property-publication paths, retain source/input hashes
and a repeatable artifact, and then qualify the affected J813 hardware before
merge. Source and offline replay results are not keyboard/trackpad proof.

### DART constructor failure model (before its allocation-order repair)

The existing constructor enables a stream before all root allocations succeed,
and its failure branch frees the embedded root-pointer array. Inject failure at
every root allocation before changing the constructor: no register may change,
only successfully allocated roots may be freed, inherited roots must survive,
and a locked DART must never allocate or write registers. A successful cold
constructor must publish initialized roots before enabling DMA. Replay the
actual constructor body with independent allocator/MMIO models and retain the
source, executable, failure logs and receipt; this isolation cannot qualify
physical DART ordering by itself. Cache cleans must complete with a DSB before
the first TTBR write, including the keep-existing-tables path.

## Opt-in profile and admission

`mtp` defaults to `"disabled"`. An experimental J813 profile may explicitly use
`"mtp": "j813-v1"` with `chip_id: "0x8142"` and `model: "Mac17,3"`. The host
automatically reserves physical `0x10020000000` for `0x200000` bytes as
`aura-mtp-handoff`; this name cannot be supplied by the profile. Normal memory
preflight checks the reservation against Aura's maximum heap, guest payload,
metadata, firmware and other declared reservations before allocating or writing.
The image path and SHA-256 binding remain required. This is not support for
other Mac models or older Mu images.

The resident advertises capability bit 10 (`0x400`); configuration bit 4 (`0x10`)
requests the handoff. Memory-preflight capability bit 7 is also required.
All Windows guest trees contain `aura,windows-mtp-disabled` under `/chosen` as
an exact little-endian u32. It remains 1 until `hv_init` reports successful MTP
preparation and initial stage-2 mapping. Only then does the host serialize 0.
Mu must implement the matching ACPI admission gate. A final stage-2 remap is
checked immediately before guest entry; a failure aborts the launch.

The host reservation is a placement guard; it does not create an EFI memory
descriptor. Before DXE can allocate memory, Mu must exclude the complete
`[0x10020000000, 0x10020200000)` staging and RTKit pool range from system RAM
whenever it admits MTP, including its legacy marker-absent path. The range must
remain `EfiReservedMemoryType` in the map supplied to Windows. Publishing MTP0
without this exclusion lets Windows allocate boot-driver pages in the firmware
buffer, which the MTP firmware copy can then overwrite.

Resource admission precedes MMIO. Power is enabled and read back for declared
clock gates; missing or empty gate lists mean the ADT declares no gate to enable.
Cold ASC/DART ownership is required. Phase logs identify resource admission,
power, inherited state, DAPF/DART setup, staging, RTKit boot and non-consuming
DockChannel readiness. RTKit boot and readiness each have a three-second bound;
the host temporarily uses at least a twelve-second proxy timeout for `hv_init`.
Bounds cannot recover a fabric read that never completes, so physical admission
still requires a controlled hardware test and recovery path.

The phase 7 refinement follows the failure model in
[`windows-rtkit.md`](windows-rtkit.md#phase-7-compatibility-failure-model-before-implementation).
It accepts public-compatible app/debug and opaque non-buffer OSLog notifications
while continuing to read DockChannel RX_COUNT without consuming INIT. Successful
preparation logs their separate counts. Failure logs distinguish
an RTKit MMIO fault, a rejected mailbox message (first reason and both raw words),
invalid service state, a DockChannel MMIO fault and a readiness timeout. Each
failure retains the existing allocation quarantine and prevents guest entry.
The original phase 7 hardware log lacked this distinction. The next controlled
run identified a rejected OSLog type-3 message, followed by type 4 on a later
attempt. The exact replays and public non-buffer OSLog contract are documented
in `windows-rtkit.md`. A subsequent J813 run with the corrected handler passed
all preparation phases, consumed six OSLog notifications, preserved four
DockChannel RX bytes, and let Mu publish MTP0. This proves the firmware handoff;
Windows subsequently started `AppleMtpHid`, the HID keyboard and mouse/trackpad
stacks in D0 without PnP problems. Physical key/pointer events and interactive
login remain separate qualification gates.

## Repeatable offline evidence

Choose unused output directories and run:

```sh
python3 tools/windows_mtp_e2e.py --output build/windows-mtp-replay
python3 tools/windows_dart_init_e2e.py --output build/windows-dart-replay
python3 tools/windows_rtkit_e2e.py --output build/windows-rtkit-replay
python3 proxyclient/tools/windows_hv_e2e.py --out build/windows-mtp-host-replay
python3 proxyclient/tools/windows_launcher_e2e.py --out build/windows-launcher-replay
```

The C replays snapshot production sources and model only hardware/allocator
boundaries. They retain compiler logs, source hashes, executables, individual
failure logs and a JSON receipt. MTP replay covers resource rejection before
MMIO, power and allocation failures, full translation verification, failed
RTKit startup/readiness, final-map failure, runtime service failure, stop failure
and one-shot lifecycle. It asserts that DockChannel access is RX_COUNT only.
The DART replay covers cold, locked and inherited-root paths with every root
allocation failure and checks clean/barrier/publication ordering. Host replay
verifies that failed preparation never publishes the enabling marker or enters
the guest. None of these artifacts proves physical input delivery.

## Public reference

The port is based on public aurora-silicon/m1n1 commit
`69534d4899e20108d2c7a085565a7097509fa17f`, specifically
`src/mtp_handoff.c`, `src/mtp_handoff.h` and the required RTKit additions.
Linux `e8efe09d4f378992c890d181d65e2ed8d8cb1194` documents the split between
RTKit helper ownership and DockChannel HID ownership, and constrains fixed
shared buffers to the declared SRAM resource. Its T602x SRAM addresses are
not evidence for a different SoC's addresses.
