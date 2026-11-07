# Explicit J813 N1 acquisition

`n1: "j813-acquire-v1"` requests a separate whole-controller acquisition. It is
experimental and is not a fallback for `j813-v1`: the latter retains its strict
cold admission. Both use one native attempt and one host attempt per resident.
A failed call quarantines the Windows personality. Use a fresh resident to retry.

The launcher requires both N1 handoff capability `0x20` and acquisition capability
`0x1000` before preparation. Native opcode `0xc84` is the acquisition operation;
`0xc82` remains cold preparation and `0xc83` remains read-only heap high-water.
Both successful producers publish the same 128-byte NW1H v1 descriptor and
existing guarded Mu census. The host receipt records the acquisition profile.
No Mu wire-format change, firmware download, DMA allocation, or BME enable is
part of this operation.

## Ownership scope

This first profile requires T8142/J813 identity and the existing exact register,
SMC reset-provider and GPIO PERST bindings. Native code validates bounded ADT
bytes before walking properties, rejects duplicate properties, and validates
the complete PMGR table and all clock/power consumers before hardware writes.

Only these seven gates are activated, in order, through nonrecursive PMGR writes:

| ID | Gate | Parents |
|---|---|---|
| 74 | APCIE_GP | 60 |
| 109 | APCIE_SYS_GP | 74 |
| 110 | APCIE_ST | 76 |
| 144 | APCIE_SYS_ST | 76, 110 |
| 152 | APCIE_PHY_SW | 144, 109 |
| 155 | ST0_AUS5_AON | 152 |
| 156 | ST0_AUS5 | 155 |

The complete admitted descendant closure also contains SYS_GP_OCLA (143),
SYS_ST_OCLA (153), and the four virtual gates 338–341. Unexpected descendants,
consumers, duplicate IDs or register aliases reject acquisition. `/arm-io/apcie`
is the only clock/power consumer intersecting this closure. ANS and both OCLA
registers are observed and preserved at each phase boundary; they are never
power-transitioned by acquisition.

The N1 DART has exactly four mapper children and four consumers: Centauri
Control, Alpha, Beta and APCIE0 PIODMA. All phandles are resolved dynamically and
must be unique; the bridge's four DART function tuples must resolve to this
controller. PIODMA is SID17, with reserved SID metadata 16–18 and 19 total
streams. The public Linux J813 DTS maps Control/Alpha/Beta RIDs to SID0. These
are sanitized hardware relationships, not copied ADT bytes or device identifiers.

## Initialization and failure behavior

After scope proof, the existing GPIO clamp and unchanged Linux PHY/root and
port/reset initialization run. Full link and endpoint BME-clear checks precede
any DART access. No intermediate hold/drain is inserted before DART.

This profile admits only the observed DART geometry: 16-KiB pages, PA42/VA42,
version 2.4 and 19 streams. Protection and command busy reject admission. Every
initial TCR must be exactly 0 or 1 and every TTBR must have its valid bit clear.
Nonzero invalid roots are allowed as observed initial state; this does not
prove firmware ownership or establish a reset default. Ownership comes from
the explicit complete-controller scope admission. The loader never accesses
or frees memory referenced by those roots.

The whole-controller sequence follows public Linux `apple-dart.c` at
[3a0da31bcc027d4b0a28b0501e649522c7a232f6](https://github.com/aurora-silicon/linux/blob/3a0da31bcc027d4b0a28b0501e649522c7a232f6/drivers/iommu/apple-dart.c),
SHA-256 `e9e4af75967b857d72e0bb7cb43a52a5b223af8b24fc6909e6aa890d0941afcd`:
all 19 TCR 0, all 19 TTBR 0, global ENABLE write-one-set, stale exception capture
and exact observed-bitmap W1C, ordered readback, conditional `0x400` unlock,
ERROR_MASK 0, and `0x8100 | SID` invalidation for all 19 streams. Commands have
pre/post 2-ms deadlines and 1-us polling delays. ENABLE readback is observational;
DIAG_LOCK bit1 configures lock-on-error and need not clear. Reserved exception
bits are recorded and acknowledged only because this path owns the controller.
No BLOCK_ERROR clear write is invented.

Readback failure, timeout, guarded exception, persistent exception bitmap or
final `BLOCK_ERROR & 0x777`, changed protected power state or existing BAR/ROM/
BME check failure prevents publication. Final checks require all 19 TCR/TTBR
words zero. Existing phase-aware clamp/drain cleanup applies; failed cleanup
remains quarantined rather than asserting that drainage succeeded.

## Repeatable offline evidence

Write the failure model and scenarios before implementation. Replay with:

```
python3 tools/windows_n1_e2e.py --output NEW_PRODUCER_DIRECTORY
python3 proxyclient/tools/windows_launcher_e2e.py --out NEW_LAUNCHER_DIRECTORY
python3 proxyclient/tools/windows_hv_e2e.py --out NEW_HV_DIRECTORY
```

The first command snapshots production sources, compiles the producer model
with ASan/UBSan, writes every transcript and successful descriptor, and compares
the unchanged Linux hardware-action transcript. Launcher/HV scenarios exercise
actual profile/capability admission and host lifecycle over modeled transport,
including the new opcode and absence of cold fallback. Native Windows and
ordinary Aura builds remain required. These artifacts do not prove hardware
reset/drain, Windows driver installation, DMA, interrupts or working Wi-Fi.

Capability allocation is coordinated with public PR93 (`8758b165`), PR94
(`1bd3e172`) and PR95 (`defe0f37`): `0x0800` remains CPUFREQ_OWNERSHIP, and N1
acquisition reserves `0x1000`. Those branches allocate no `0xc84` opcode.
CPU-only capability `0x0800` cannot authorize N1 acquisition. Standalone N1
revision `8b5264f2` advertised `0x4157494e000117ff`; the combined CPU/N1 tree
advertises `0x4157494e00011fff`. See [CPU/N1 integration](windows-cpu-n1-integration.md)
for the combined profile and its separate qualification boundaries.
