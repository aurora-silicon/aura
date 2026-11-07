# J813 G17 firmware memory preservation

The optional profile field `"g17": "j813-preserve-v1"` admits the public
`M5-RTKit-3255.160.4.release` firmware layout on chip `0x8142`, board `0x24`,
model `Mac17,3`. It defaults to `disabled`. This contract preserves memory;
it does not create a GPU device, start either ASC, register an interrupt,
install a Windows graphics driver, or authorize firmware reset/free operations.

Use a companion Mu image containing this consumer. The normal profile binds
the exact Mu image SHA-256; the handoff does not discover support in arbitrary
older Mu images. The E2E command below binds a rebuilt companion image to the
actual producer and compiled firmware consumer artifacts.

The admission rules come from public Aura commit
`671c0cefdb39b58a19933c6fe254efb503d16212`, `src/kboot_gpu.c`, and public Linux
commit `e50ba38b24140fc0751b811ea093c8f843bcec76`, `drivers/gpu/drm/asahi/m5/`.
The two TEXT lengths are `0x4c000` and `0x48000`. Both must contain the build
string `RTKit-3255.160.4.release`; the GKTS/ECAP tag offsets are respectively
`0x45d09`/`0x45dec` and `0x45841`/`0x45924`, each with little-endian length 8.
Whole-image SHA-256 values are evidence, not firmware admission: iBoot patches
the TLV payload per boot.

Aura reads all addresses from the current ADT. It checks exact property widths,
two `__TEXT;__DATA` segment records per role, matching DATA physical spans,
16-KiB alignment, 42-bit physical bounds, installed DRAM, pairwise disjointness,
host allocation/copy collisions and guest-RAM classification before any TEXT
read. TEXT reads temporarily enable transfer checksums so the native proxy's
exception-guarded probe cannot be bypassed by a retained checksum-disabled
session. Short/faulting reads and unknown firmware fail admission. No firmware
contents are changed.

A failed transport operation stops all further commands, including checksum
feature restoration, and preserves the original failure. A semantic admission
rejection after completed transport still restores the prior feature settings.

Aura rechecks original ADT/BootArgs bytes before allocation, validates the
allocator's high-water mark, checks source metadata again when HV reloads ADT,
and checks the published marker before the final upload. The complete spans
remain borrowed firmware memory for this boot. There is no release API.

## Wire contract

`/chosen/aura,windows-g17-preserve-v1` is exactly 304 little-endian bytes.
Unknown versions, flags, lengths, padding, tags and classifications are rejected.

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 4 bytes | `AG17` |
| 4 | u16 | Version 1 |
| 6 | u16 | Length 304 |
| 8 | u32 | Chip `0x8142` |
| 12 | u32 | Board `0x24` |
| 16 | u32 | Region count 8 |
| 20 | u32 | Flags 1: reservation only |
| 24 | u64 | Guest RAM base from relocated BootArgs |
| 32 | u64 | Exclusive guest RAM end |
| 40 | u64 | Installed DRAM base: BootArgs.phys_base rounded down to 4 GiB |
| 48 | u64 | Exclusive installed DRAM end: base + mem_size_actual |
| 56 | 32 bytes | Fixed firmware profile, NUL padded |
| 88 | 16 bytes | `Mac17,3`, NUL padded |
| 104 | u32 | CRC32 over all 304 bytes with this field zeroed |
| 108 | u32 | Reserved zero |
| 112 | 8 × 24 bytes | `{u32 tag, u32 classification, u64 base, u64 size}` |

The ordered tags are 1 `gfx-handoff`, 2 `gfx-shared-l2-region`,
3 `gfx-shared-region`, 4 `gpu-region`, 5 `gfx-data`, 6 `gfx1-data`,
7 GFX TEXT and 8 GFX1 TEXT. The six SGX ranges use separate `*-base` and
`*-size` u64 properties. TEXT comes from each ASC's first 32-byte segment
record. DATA must agree with its second segment record. Segment IOVA, remap
and unknown fields do not grant UAT ownership in this revision.

Classification 1 is wholly within guest RAM and must become EFI reserved
memory before allocator exposure. Classification 2 is wholly below guest RAM;
3 is wholly above it. Both must remain absent from allocatable EFI memory.
Partial guest-boundary overlap is invalid. Every complete declared span is
retained, including the two TEXT images and unused page-table capacity.
`gfx-shared-region` must cover at least `0x44000`; DATA sizes are exactly
`0x134000` and `0x130000`.

Mu independently binds the record to its current source ADT and BootArgs,
rejects early-copy/stack/image conflicts, carves class-1 ranges from system
memory HOBs, and stores the complete record in a GUID HOB. DXE compares the
record/HOB and checks actual reserved/absent EFI coverage; ReadyToBoot repeats
that check. Invalid enabled admission stops boot. It never frees a prior
reservation to recover from partial failure. Missing property preserves the
existing default path. CRC32 detects corruption and is not authentication.

## Repeatable validation

```sh
python3 tools/windows_g17_e2e.py --mu /path/to/companion/mu \
  --mu-image /path/to/rebuilt/MACBOOKAIR2026_EFI.fd \
  --output build/g17-preservation-run
```

The runner uses real profile parsing, `HardwareSession.prepare`, `ProxyUtils`,
HV load/start and ADT/BootArgs serialization with modeled transport/hardware
calls. It compiles Mu's production ADT parser, admission, early gate, PEI HOB
reservation and DXE/ReadyToBoot checks under ASan/UBSan. It retains the exact
guest ADT, BootArgs, record, memory-map/HOB artifacts, tampered inputs, command
logs and source hashes. Without `--mu-image`, it uses a clearly labeled synthetic
payload; that mode is not a native firmware build result.

Native Aura builds remain `gmake WINDOWS=1 BUILDSTD=1` and
`gmake WINDOWS=0 BUILDSTD=1`. The firmware-size linker guards remain enabled.
These offline receipts do not establish a physical boot, firmware startup or
accelerated rendering. Hardware validation is required before merge under
the repository's assistant policy.
