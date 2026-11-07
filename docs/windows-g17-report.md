# J813 G17 diagnostic resource report, version 1

This is a deliberately non-activating OEM ACPI handoff. It extends
`j813-preserve-v1` with the explicit profile policy `j813-resource-report-v1`.
The default remains disabled. Preservation-only policy does not emit the report.
No GPU device, `_CRS`, INF, interrupt alias, MMIO access or ASC startup is added.

The hardware facts below are from public Linux
`e50ba38b24140fc0751b811ea093c8f843bcec76`,
`arch/arm64/boot/dts/apple/t8142-j813-gpu.dtsi:63-127`, `asahi/m5/live_boot.rs`
and `drivers/soc/apple/mailbox.c`. They describe the fixed admitted J813 profile;
they are not readbacks of these MMIO registers. Identity, firmware and retained
RAM are bound to the actual boot by the existing preservation admission.

Aura publishes `/chosen/aura,windows-g17-report-v1`, exactly 688 bytes. Mu requires
the identical embedded preservation record and its existing validated GUID HOB,
then installs one OEM ACPI table. The table is 724 bytes: a normal 36-byte ACPI
header followed by the exact 688-byte record. Signature `AG17`, revision 1,
OEM ID `AURORA`, OEM table ID `G17RPTV1`, OEM revision 1, creator `AURA`, creator
revision 1; the normal ACPI checksum covers the whole table.

All payload integers are little-endian; no native structure layout is the ABI.

| Payload offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII `G17R` |
| 4 | 2 | Version 1 |
| 6 | 2 | Header size 32 |
| 8 | 4 | Total payload size 688 |
| 12 | 4 | Authority flags: must be zero |
| 16 | 4 | Preservation size: 304 |
| 20 | 4 | MMIO description count: 6 |
| 24 | 4 | IRQ description count: 4 |
| 28 | 4 | CRC32 of the payload with this field zeroed |
| 32 | 304 | Exact current preservation record, including its own CRC |
| 336 | 192 | Six 32-byte MMIO descriptions in the order below |
| 528 | 96 | Four 24-byte IRQ descriptions in the order below |
| 624 | 32 | UAT geometry description |
| 656 | 32 | Power dependency description |

A 32-byte MMIO description is `u16 tag, u16 role, u16 parent_tag, u16 authority,
u64 base, u64 size, u32 reserved0, u32 reserved1`. Authority and reserved fields
are zero. Roles are 0 common, 1 GFX, 2 GFX1. Every tuple must match exactly.

| Tag | Role | Parent | Base | Size | Meaning |
|---:|---:|---:|---|---|---|
| 1 | 0 | 0 | `0x580000000` | `0x4000000` | SGX aperture description |
| 2 | 1 | 1 | `0x582600000` | `0x88000` | GFX ASCWrap v6 |
| 3 | 1 | 1 | `0x582050000` | `8` | GFX IOP VBAR |
| 4 | 2 | 1 | `0x582e00000` | `0x88000` | GFX1 ASCWrap v6 |
| 5 | 2 | 1 | `0x582850000` | `8` | GFX1 IOP VBAR |
| 6 | 0 | 1 | `0x580e80000` | `0x30000` | GPU PMGR view |

These are descriptions, not independent exclusive resource claims. All five
children are contained by tag 1. The ASC mailbox begins at wrapper + `0x8000`.

A 24-byte IRQ description is `u16 role, u16 event, u32 physical_aic_irq,
u32 published_gsiv, u32 routing_authority, u32 electrical, u32 reserved`.
Events 1 and 2 mean send-empty and receive-not-empty; electrical 1 means
level-sensitive active-high. GSIV, routing authority and reserved are zero.
Ordered role/event/physical tuples are `(1,1,1123)`, `(1,2,1126)`, `(2,1,1127)`,
`(2,2,1130)`. Zero GSIV means explicitly unrouted, never physical IRQ passthrough.

The UAT description is `u16 ias=42, u16 oas=42, u16 page_shift=14,
u16 root_count=2, u32 pagetable_preserve_tag=3, u32 ttbs_preserve_tag=4,
u64 secondary_root_offset=0x40000, u32 ownership_authority=0, u32 reserved=0`.
It grants no root adoption, zeroing, mirroring, mapping or freeing authority.

The power description is eight `u32` values: authority 0, PMGR MMIO tag 6,
GPX offset 0, GFX offset `0x110`, boot-control offset `0x10000` within PMGR,
dependency model 1 (GFX depends on critical/always-on GPX), reserved 0, reserved 0.
It describes prerequisites only; it grants no reset, PMGR write or clock change.

## Windows discovery and scope

A read-only user-mode tool enumerates ACPI tables, requires exactly one `AG17`,
reads exactly 724 bytes, validates every field, and emits raw bytes plus a decoded
report in JSON (`table_hex` preserves all raw bytes). Its only platform calls are the documented `EnumSystemFirmwareTables`
and `GetSystemFirmwareTable` and ordinary output. No GPU driver is installed.
A future private CAPS_PROBE adapter can consume the same public ABI through
AuxKlib at PASSIVE_LEVEL after initialization; absence remains unsupported.
None of these readers may elevate the zero authority fields.

Build the ARM64 reader with `tools/windows_g17_report_build.py`, passing `--sdk`
and `--wdk` package/c directories, the Microsoft ARM64 SDK `--arm64-sdk` nupkg,
and explicit `--clang-cl`, `--linker`, `--readobj`, and new `--output` paths.
The build uses the actual SDK declarations, links only Kernel32, and verifies
the complete five-function import set. It emits a source/header/toolchain-bound
`receipt.json` alongside `aurora-g17-report.exe`. The no-CRT console PE uses
subsystem 6.2 and OS version 10.0. Running it on Windows prints a JSON report
to stdout and returns zero only for a validated diagnostic table. It makes
no installation or configuration changes.

Run the full offline chain with:

```text
python3 tools/windows_g17_report_e2e.py --mu /absolute/path/to/Mu \
  --mu-image /absolute/path/to/J813MACBOOKAIR2026_EFI.fd \
  --output /absolute/path/to/new-output
```

This exercises the real host lifecycle and serialized ADT/BootArgs, compiled
Mu early/PEI/DXE/ACPI code under ASan/UBSan, and the production Windows reader
through fixture firmware APIs. Raw tables, source hashes, commands and JSON
receipts are retained. The fixture API stage is not a Windows execution or
physical interrupt qualification. The native executable lets a later Windows
run report the actual installed table without loading WDDM.

## Deferred interrupt and startup contract

Public Aura/Mu currently route only 995→1277, 996→1155 and 997→1511 for J813.
The public Aura captured-ADT comment supports proposing 990/991 for GFX and
992/993 for GFX1, but this version does not install or advertise those routes.
A separate opt-in route version must recheck every original ADT `interrupts`
cell, existing published/physical aliases and the full combined set, then bind
identical pairs in Aura ownership/translation, Mu CSRT and GPU `_CRS`. Never
reuse J414S alias 46 or use Linux's physical lines as Windows GSIVs.

Linux starts GFX1 then GFX, after 42-bit UAT and crash views exist; adopts and
confirms primary firmware roots, builds the owned graph, mirrors the secondary
root, enables G17 gating, publishes/ACKs endpoint 0x20 in GFX1/GFX order, then
sends endpoint 0x21 control 0x89 in GFX/GFX1 order. RUN clearing is not proven
DMA quiescence; Linux retains all owners and callback code until reboot. That
native lifecycle is outside this diagnostic handoff version.
