# Explicit Windows RTKit service mode

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
  parse crash-log memory, discard application traffic as success, or hide
  malformed grants/unsupported message types. Failure remains latched.
- A failed send or boot after granting storage must preserve ownership for
  quarantine; no allocator rollback may reclaim memory still reachable by DMA.

The production-code replay must cover a complete synthetic mailbox handshake,
fixed-pool and physical-buffer grants, logging acknowledgements, full TX,
budget limits, malformed grants and deadline failure. Record the exact
mailbox transcript, source hashes and sanitizer/build output. The mailbox,
cache operation and DART boundary are modeled; physical MTP qualification
remains a separate requirement.

## Caller contract

The four APIs exist only in a Windows build. Before any boot attempt, configure
the physical SRAM window and a 16 KiB aligned fixed pool, enable early AP power,
then use `rtkit_boot_timed()` with a nonzero total deadline. The pool is mapped
through the supplied DART/IOVA domain during configuration. A failed mapping or
boot cannot be reset with setters. This mode does not adopt an inherited session.

The caller must be the sole ASC owner during boot and service. Runtime service
uses a non-waiting per-object guard and checks a TX slot before consuming RX;
other mailbox writers, ordinary `rtkit_recv()`, and concurrent boot/free are
outside this contract. A return of zero means no work or TX backpressure, and a
negative return requires the caller's quarantine path. Unknown application or
system messages are errors, not successful draining. No logging buffers are
parsed by this path.

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
repeated endpoint maps, missing AP acknowledgement and unexpected app traffic.
The output includes `result.json`, exact source hashes, compile/replay logs and
`mailbox.bin` containing the successful AP-to-IOP wire transcript. Hardware was
not accessed; this is production-code protocol validation with modeled ASC,
DART and cache boundaries, not physical MTP qualification.
