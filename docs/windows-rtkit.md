# Explicit Windows RTKit service mode

## Opaque OSLog notifications failure model (before implementation)

A later physical run sent endpoint `8`, message `0x04000020ce38cc21`, OSLog
type `4`, and hit the deliberately narrow type-3-only handler. Match the full
public Aura/Linux endpoint contract now: only type `1` is a buffer request;
every other OSLog type is an opaque, no-reply notification. This rule is confined
to OSLog, not unknown system endpoints. Payload semantics remain unspecified.

- Every non-type-1 value, including `0`, `2`, `3`, `4` and `0xff`, consumes the
  normal per-call budget without parsing a payload, accessing log memory,
  allocating, changing buffer ownership/mappings or sending a reply.
- Type `1` still takes the existing validated grant path. A malformed/duplicate
  grant, invalid endpoint, unknown system endpoint or crash still latches failure
  before any later notification is consumed.
- Rename the saturating counter to `oslog_notifications` so telemetry describes
  all non-buffer types. Full TX and latched failure leave its count unchanged.
- First replay the exact newly rejected type-4 wire against candidate17 bytes;
  then replay all 255 non-buffer type values after a complete production boot,
  checking the unchanged OSLog buffer/pool and zero additional replies. Keep
  malformed type-1 failure transcripts, source snapshots and repeatable receipts.

## OSLog type 3 failure model (before implementation)

This records the earlier restricted correction; the opaque OSLog contract above
supersedes its type-3-only admission rule.

The next physical run identified endpoint `8`, message `0x03ff006ea3b0e3f6`,
with rejection reason `3` (generic protocol rejection). OSLog uses bits 63:56
for its type: this is type `3`, not a type `1` buffer request. Its payload must
not be decoded as a buffer size or address. The public Aura implementation at
`69534d4899e20108d2c7a085565a7097509fa17f` and
[Linux at e8efe09d](https://github.com/AsahiLinux/linux/blob/e8efe09d4f378992c890d181d65e2ed8d8cb1194/drivers/soc/apple/rtkit.c#L477)
both ignore non-buffer OSLog messages without a reply. No more specific payload
semantics are established by those sources.

- Admit only the observed OSLog type `3` as a bounded no-op; do not generalize
  acceptance to all unknown system messages or OSLog types.
- Preserve existing type `1` grant validation, ownership and fatal failure
  behavior. Type `3` may not allocate, free, map, parse log memory or reply.
- Count consumed type `3` messages separately in saturating notification
  telemetry; full TX, exhausted budget and a latched failure must not count or
  consume them. Keep the first rejected wire diagnostic unchanged.
- Replay the exact physical wire value after a complete production handshake,
  followed by a valid log acknowledgement. Retain byte-exact pre-change sources,
  the failing replay and post-change sanitizer/build receipts. Exercise invalid
  OSLog types and malformed type `1` grants in fresh full-handshake transcripts.

## Phase 7 compatibility failure model (before implementation)

The first physical MTP attempt reached DockChannel readiness and failed within
milliseconds. Its log does not identify whether quiet service rejected a wire
message or the guarded readiness read failed; it does not prove a timeout.
The public implementation at `69534d4899e20108d2c7a085565a7097509fa17f`
(`src/rtkit.c` and `src/mtp_handoff.c`) bounds endpoint IDs below `0x100`,
returns application notifications (`0x20..0xff`) to a caller that explicitly
ignores them while waiting for DockChannel data, and ignores the known debug
endpoint (`3`). The Windows owner must reproduce those bounded no-op cases.

- App/debug notifications must consume the existing service budget, produce no
  reply, allocate nothing, and leave later log acknowledgements serviceable.
  TX backpressure must still prevent consumption. Never consume DockChannel data.
- Reject endpoint IDs outside the 8-bit wire contract and unknown system IDs;
  accepting app notifications must not mask malformed grants, invalid management
  messages, invalid log indices, failed replies, or a crash notification.
- Latch the first rejected wire message and a numeric reason without printing in
  quiet service. Repeated service on a failed session must not overwrite it.
  Diagnostic reads use the existing sole-owner contract and must not touch MMIO.
- Before adding notification telemetry, require separate saturating app/debug
  counters that advance only when a notification is consumed. TX backpressure
  must leave both unchanged. The phase 7 completion log must report these counts
  so a physical success proves whether the compatibility path was exercised.
- Phase 7 must distinguish service failure, guarded-read failure and exhausted
  readiness deadline. Every branch retains DMA allocations and withholds guest
  publication. Log the rejection snapshot only outside quiet service.

Replay complete production handshakes before app/debug traffic, at both endpoint
boundaries, interleaved with valid replies. Include rejected endpoint/system IDs,
malformed protocol/grants and crash cases, plus the MTP readiness failure branches.
Keep the failing pre-change replay, source hashes and post-change sanitizer logs.
Only a new physical run can identify the original rejected message.

## Failure model recorded before implementation

- Existing Aura DCP/SMC/NVMe boot, ASC-v8 handling, adopted buffers and narrow
  T8152 SRAM adoption must retain their behavior unless setters opt in.
- Configuration must reject zero/overflowing/misaligned ranges, incompatible
  SRAM/SART modes, overlap, repeated pool configuration and changes after boot
  or grants. A live DMA range must never be replaced by another setter.
- AP grants must come only from the fixed caller-owned pool. Map the whole
  pool through the supplied DART before boot; a runtime grant may not allocate
  page tables, call a failing/logging DART mapper, or fall back to the heap.
  Zero and clean each slice before publishing it to the IOP.
- Pool ownership differs from heap ownership and IOP-owned adoption. Pool
  slices count as owned DMA storage but are never individually freed/unmapped.
  Release the whole mapping only when the caller invokes free after proving
  the IOP stopped. MTP retains everything until physical reload instead.
- IOP-provided addresses must fit entirely inside the explicit physical SRAM
  window. Reject empty/duplicate/out-of-window grants, arithmetic overflow and
  values not representable in the reply fields; do not translate arbitrary
  IOP addresses through unrelated DART mappings.
- The timed early-AP boot must use one deadline for HELLO, all endpoint maps,
  endpoint starts, AP power and both power acknowledgements. Malformed or
  repeated maps may not restart the timeout. Start advertised system endpoints
  in addition to the known log endpoints, without exposing application IDs.
- Quiet service must consume at most the requested 1..64 messages, serialize
  per object without waiting, and check TX capacity before consuming RX.
  Every consumed supported message needs at most one reply. Do not print,
  parse crash-log memory, or hide malformed grants/unsupported system message
  types. The phase 7 refinement above permits bounded app/debug notifications;
  all other failures remain latched.
- A failed send or boot after granting storage must preserve ownership for
  quarantine; no allocator rollback may reclaim memory still reachable by DMA.

The production-code replay must cover a complete synthetic mailbox handshake,
fixed-pool and physical-buffer grants, logging acknowledgements, full TX,
budget limits, malformed grants and deadline failure. Record the exact
mailbox transcript, source hashes and sanitizer/build output. The mailbox,
cache operation and DART boundary are modeled; physical MTP qualification
remains a separate requirement.

## Caller contract

These APIs exist only in a Windows build. Before any boot attempt, configure
the physical SRAM window and a 16 KiB aligned fixed pool, enable early AP power,
then use `rtkit_boot_timed()` with a nonzero total deadline. The pool is mapped
through the supplied DART/IOVA domain during configuration. A failed mapping or
boot cannot be reset with setters. This mode does not adopt an inherited session.

The caller must be the sole ASC owner during boot and service. Runtime service
uses a non-waiting per-object guard and checks a TX slot before consuming RX;
other mailbox writers, ordinary `rtkit_recv()`, and concurrent boot/free are
outside this contract. A return of zero means no work or TX backpressure, and a
negative return requires the caller's quarantine path. Endpoint IDs `0x20..0xff`
and known debug endpoint `3` are unowned notifications, consumed without replies
within the same budget. OSLog endpoint `8` uses its own type byte: type `1` takes
the validated buffer-grant path, and every other type is consumed without a
reply or payload interpretation, matching the public handler. Unknown system
IDs and values at or above `0x100` are errors. No logging buffers are parsed by
this path.

After service returns, the sole owner can call `rtkit_get_first_rejection()` to
copy the first rejected `msg0` and `msg1`. Reason codes are 1 for an out-of-range
endpoint, 2 for an unknown system endpoint, 3 for a rejected protocol message or
failed reply, and 4 for a crash notification. A false result means no wire
rejection was recorded (for example, invalid service arguments or reentry).
The snapshot is immutable after the first rejection; callers must not race it
with service or free. No MMIO, allocation, or firmware-buffer parsing is involved.
`rtkit_get_notification_counts()` follows the same sole-owner rule and copies
separate saturating app/debug/OSLog-notification counters. MTP reports these after successful
preparation, making a zero-notification run distinguishable from one that used
the compatibility behavior.

Every AP grant is a zeroed, cache-cleaned pool slice followed by a completion
barrier before publication. Individual slices are never recycled or unmapped.
The caller retains the physical storage and must stop the IOP before calling
`rtkit_free()`; MTP instead retains all mappings/storage until physical reload.

## Repeatable offline replay

From the repository root, choose a new output directory:

```sh
python3 tools/windows_rtkit_e2e.py --output build/windows-rtkit-replay
```

The runner snapshots production `rtkit.c`, its dependencies and the mailbox
fixture, compiles with address/undefined-behavior sanitizers, and exercises a
complete boot through system endpoint startup, both power acknowledgements,
AP pool grants, SRAM adoption and runtime replies. It also checks full TX,
service bounds/reentry, pool exhaustion, failed mapping, absent/reversed HELLO,
repeated endpoint maps, missing AP acknowledgement, bounded app/debug traffic,
invalid endpoint IDs, the exact physical OSLog type-3/type-4 messages, all 255
non-buffer OSLog types, malformed grants, failed replies and crashes.
The output includes `result.json`, exact source hashes, compile/replay logs and
`mailbox.bin` containing the successful AP-to-IOP wire transcript. Hardware was
not accessed; this is production-code protocol validation with modeled ASC,
DART and cache boundaries, not physical MTP qualification.
