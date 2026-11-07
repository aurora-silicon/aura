# G17 diagnostic handoff failure model

Written before the diagnostic implementation or its E2E harness. This extends
the preservation failure model; that admission and all eight lifetimes remain
mandatory. This report grants no hardware access.

- Disabled or preservation-only boot inherits or emits a report; remove stale
  report markers and publish no ACPI report unless the explicit report policy
  is admitted. Preserve byte-for-byte existing IRQ aliases and GPU discovery.
- A report is accepted without its matching, independently validated 304-byte
  preservation record, or the two records bind different boots or identities.
- Truncated, oversized, trailing, misaligned, duplicate, reordered, unknown,
  wrapping or corrupt fields become trusted. Use an exact fixed length and
  explicit little-endian decoding, check every tag, constant and reserved field,
  both CRCs and the ACPI checksum. Reject duplicate OEM tables in Windows.
- Public fixed-profile MMIO/IRQ facts are mistaken for live MMIO observations,
  translated PnP resources, power ownership, active IRQ routing or start permission.
  Version 1 is diagnostic only; all authority and routing fields must remain zero.
- SGX contains the ASC, VBAR and PMGR views. Preserve this parent/subregion
  relationship; never convert these overlapping descriptions to independent
  exclusive `_CRS` claims or map them in a diagnostic reader.
- Physical AIC lines are passed directly to Windows, or a G14 alias is reused.
  Version 1 has GSIV zero (unrouted). Candidate future GSIVs require full original
  ADT collision checks and an atomic Aura/Mu/HAL route contract in another slice.
- Building or reading the report enables an alias, touches MMIO, starts RTKit,
  resets either ASC, creates a GPU devnode, changes an INF, or frees retained RAM.
  E2E must verify report-only traces and unchanged aliases/discovery.
- Mu publishes a table before preservation validation, or installs it repeatedly.
  Install exactly once only after the existing DXE lifetime check succeeds;
  ReadyToBoot must continue validating the same retained bytes and map coverage.
- A Windows read races a changing table length, trusts an unchecked short read,
  writes beyond its fixed buffer, or accepts the first of duplicate signatures.
  Enumerate/count, require exact fixed lengths on both reads, validate, then emit
  exact raw bytes and a deterministic decoded report. No driver installation.
- An offline artifact claims a Windows boot or interrupt qualification. Bind
  fixture, source and native binaries by SHA-256 and label simulated system
  firmware calls separately from an actual Windows execution receipt.
- Private WDDM implementation leaks into public firmware. Public implementation
  must be independently authored from public Aura/Mu/Linux facts; keep the private
  native lifecycle audit outside public repositories.

The E2E fixture is written first and must fail against the prior production
policy. Then run real Aura policy/admission/ADT serialization, Mu validation and
ACPI construction, and the same read-only Windows consumer decoder. Mutate both
host inputs and serialized records, recomputing checksums where necessary, so
semantic failures cannot pass merely by checking a CRC. Keep source-bound JSON
receipts and the exact ADT, BootArgs and ACPI table artifacts.
