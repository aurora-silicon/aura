# G17 preservation failure model

Written before this implementation. This slice admits a reservation-only
handoff; it does not start either ASC or create a Windows GPU device.

- A wrong chip, board, model, unknown firmware build, swapped role, malformed
  property width, missing segment, or disagreeing DATA span is accepted.
- A TEXT read uses a wrapping, unbounded, non-DRAM address or faults. Validate
  the full range first and require checksummed, exception-guarded proxy reads.
  A session that disabled transfer checksums must not bypass the native probe.
- A failed checksum negotiation or TEXT transfer leaves a partial reply, but
  unconditional feature restoration sends another NOP. That can consume stale
  bytes, retry an ambiguous channel, or hide the first timeout/framing/device
  failure. After any failed transport call, issue no further command. Restore
  features after completed transport and normal semantic admission rejection.
  Exercise the actual UART encoder/parser with truncated replies/data, command
  mismatch, checksum corruption and device loss; retain TX/RX bytes and verify
  no command follows the fault and the original exception reaches the caller.
- Only the six named data regions survive; either loaded TEXT image is reused.
  Preserve all eight complete 16-KiB-aligned lifetimes.
- A region crosses a guest-RAM boundary, overlaps another firmware region,
  profile reservation, resident/heap, source metadata, framebuffer or relocated
  Mu image. Refuse before allocating or copying anything.
- Actual DRAM is confused with usable guest RAM. Classify every span as wholly
  inside guest RAM, below it, or above it; reject ambiguous partial overlap.
- A host descriptor is mistaken for an EFI reservation. Mu must compare its
  current ADT/BootArgs and carve guest spans before exposing RAM to DXE, then
  check the EFI map. Outside-guest spans must remain absent from allocatable RAM.
- ADT data, heap high water, layout or marker bytes change after admission.
  Revalidate source facts when the HV reloads ADT and bind the final exact layout.
- ADT growth fits the reserved capacity but BootArgs still advertises the source
  byte count, so Mu copies an incomplete marker/tree. Publish the exact final
  serialized byte count, and reject later tree-size drift before guest entry.
- The marker is inherited when the option is disabled, a partial descriptor is
  accepted, or unknown version/flags/fields are ignored. Disabled removes it;
  enabled consumers must reject every invalid record, with no GPU publication.
- A checksum is treated as authorization. CRC32 detects corruption only; exact
  current source identity, spans and firmware layout are independent admission.
- A replay parser rejects a valid fixed-width 32-byte ADT key that Mu accepts,
  hiding the real consumer result. Keep parser semantics aligned with production;
  use a shorter new marker name to avoid strict-tool interoperability ambiguity.
- A failure frees firmware-owned memory or issues a guessed reset. This slice
  never frees, clears, writes, starts or resets firmware and exposes no GPU IRQs.
- Offline replay is reported as physical Windows or accelerated graphics proof.
  Preserve source hashes, command logs and serialized artifacts and explicitly
  report the absence of hardware access and native GPU startup.

The end-to-end campaign must serialize the actual host ADT/record, run the Mu
consumer over that artifact and inspect resulting exclusions. Exercise default,
enabled, tampered/truncated/stale and boundary/overlap/fault cases, with source
hashes and repeatable command receipts. Native builds qualify compilation only.
