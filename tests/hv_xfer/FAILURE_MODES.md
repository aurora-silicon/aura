# HvXfer on Aura (Windows personality): failure modes

Written before the port and before the replay. Every entry is a way the
channel can go wrong. The replay (`run.sh`) has to show each one either
refused or handled. The ID in brackets is the check name that appears in
`result.json`. Entries marked *inspection* cannot be replayed off-hardware.
For those, the README names the code that enforces them.

The replay compiles the real `src/hv/windows/hv_xfer.c` and the real stage-2
code (`src/hv/hv_vm.c`, including `hv_handle_dabort()` and its instruction
decoder) against the real Aura headers. Guest accesses go in as real
AArch64 load/store instructions. The guest side is the real guest driver core
(`AppleHvXferCore.c`). The host side is the real Python `HV.handle_xfer()`
plus the real engine. So a bug in any of these layers, or in the glue
between them, has a chance to show up.

## A. EL2 doorbell decode (hv_xfer.c `handle_xfer`)

- A1 `width` is log2(bytes) (from `hv_emulate_rw_aligned`), not bytes. Read it
  as bytes and every 32-bit access hits the wrong register. [decode.id_version]
- A2 An unimplemented register reads all-ones. The guest then can't tell
  "present" from "absent". It must read 0. [decode.unimplemented_zero]
- A3 8-bit or 16-bit access to a register. Must be refused (hook returns
  false), with no state change. On Aura Windows, a refused access goes out
  as EXC_SYNC, and the Windows launcher turns that into a guest abort.
  [decode.subword_refused]
- A4 Unaligned 32-bit access. `hv_emulate_rw` splits it into byte and halfword
  chunks. The first chunk must be refused, so nothing commits.
  [decode.unaligned_refused]
- A5 A 64-bit store that covers CMD+OFF executes a command with an ambiguous
  OFF. It must be refused: STATUS=INVAL, ERRORS+1, no host call.
  [decode.cmd64_refused]
- A6 A 128-bit (STP / Q-register) access reaches the hook with width 4. It
  must be refused, not truncated. [decode.wide_refused]
- A7 NAME byte order is wrong for 32-bit word stores. Or an access past the
  64-byte NAME end runs off the field. Or the name the host reads at
  `name_phys` differs from what the guest wrote. [decode.name_words,
  decode.name_overrun_refused]
- A8 Writes to read-only registers change state. They must be absorbed.
  [decode.ro_absorbed]

## B. Commands

- B1 GET/PUT with LEN=0, OFF>=WIN_SIZE, or OFF+LEN past the window
  (including u32 wrap). Must be ST_RANGE with no host round trip.
  [cmd.range_*]
- B2 A host that never answers, or declines, gets read as success. EL2 poisons
  the status with NODEV. RESULT must be 0 on any failure. [cmd.no_engine,
  cmd.host_declined]
- B3 The host reports RESULT > LEN for GET/PUT. The guest would then copy past
  what it asked for. EL2 must turn that into ST_IO, RESULT=0.
  [cmd.result_overclaim]
- B4 Unknown command. Must be BADCMD, ERRORS+1, SEQ unchanged.
  [cmd.badcmd]
- B5 SEQ/ERRORS counters drift. Success must bump SEQ only, failure ERRORS
  only. [cmd.counters]
- B6 Cache maintenance is skipped, or runs over a range other than the one the
  host touched. Clean+invalidate must cover exactly [OFF, OFF+RESULT) on GET
  and [OFF, OFF+LEN) on PUT, clamped to the window. [cmd.sync_range]

## C. Guest-supplied window (SET_WINDOW) - the one guest pointer

- C1 Size 0, size > 64 MiB, IPA or size not 16 KiB aligned. [win.reject_shape]
- C2 IPA+size wraps, or lies beyond the stage-2 IPA space. In m1n1 that hits
  `assert(idx < ENTRIES_PER_L1_TABLE)` in `hv_pt_walk` and panics EL2 from
  the guest. Must refuse first. [win.reject_beyond_ipa]
- C3 A page in the range is unmapped. [win.reject_unmapped]
- C4 A page in the range is a software hook or SW mapping (doorbell, vUART).
  [win.reject_hook]
- C5 Pages are mapped but not physically contiguous. [win.reject_discontig]
- C6 Contiguous HW pages that are MMIO (outside DRAM), or start in DRAM and run
  out of it. [win.reject_mmio, win.reject_dram_edge]
- C7 A refused SET_WINDOW leaves the old window changed. [win.refusal_keeps_old]
- C8 IPA != PA (non-identity stage 2). The host must get the PA, and the guest
  must see its own IPA in WIN_LO/HI. [win.ipa_pa_split]
- C9 RESET_WINDOW doesn't restore the Aura-owned window. [win.reset]
- C10 Stage-2 L2 block mappings (32 MiB) resolve to the wrong page PA.
  [win.block_mapping]

## D. Mapping the device (hv_map_xfer / P_HV_MAP_XFER)

- D1 A second channel replaces the first. [map.second_refused]
- D2 Doorbell not 16 KiB aligned. [map.base_unaligned]
- D3 Doorbell inside DRAM (shadows guest RAM). [map.base_in_dram]
- D4 Doorbell beyond the IPA space (would assert in hv_pt_walk).
  [map.base_beyond_ipa]
- D5 Doorbell lands on an existing stage-2 entry. That includes a 4-byte L4
  sub-page hook that covers only part of the page (for example a C-side vGIC
  or AIC hook Python does not know about). [map.base_overlaps_hook,
  map.base_overlaps_subpage_hook]
- D6 Own window zero, unaligned, over 64 MiB, outside DRAM, wrapping, or
  already mapped into the guest (would alias guest RAM). [map.window_*]
- D7 The hook map fails after the window was mapped, and the window stays
  mapped with no device. [inspection: rollback in hv_map_xfer]
- D8 The doorbell is touched while xfer is disabled (nothing mapped). Must be
  an unmapped-IPA dabort failure, never a silent read.
  [map.disabled_doorbell_faults]
- D9 Opcode 0x1700 already used in Aura, or the case gets compiled into the
  non-Windows image. [build.default_image_identical]
- D10 The hook runs without the big HV lock. [inspection: only
  `hv_handle_dabort()` reaches hooks, and Windows `hv_exc_sync()` calls it
  only after `hv_exc_entry()`]

## E. EL2 <-> host ABI

- E1 `struct hv_xfer_exc_info` drifts from Python `XferExcInfo` (size 56).
  [abi.exc_info_layout]
- E2 `HV_XFER` enum value (9) differs from `HV_EVENT.XFER`. The event then
  reaches no handler and the proxy deadlocks. [abi.event_number]
- E3 The handler isn't registered while xfer is disabled. A stray event then
  deadlocks the proxy instead of getting NODEV. [host.no_engine_nodev]
- E4 Guest header vs Aura header drift. [abi.guest_parity]
- E5 The capability bit is missing, or the host enables xfer on a resident
  without it. [host.caps_required]

## F. Host engine policy

- F1 Stream name escapes serve_dir or inbox: '/', '\\', '..', absolute,
  leading '.', empty, non-ASCII, >63 chars, all-NUL. [engine.bad_names]
- F2 A symlink in serve_dir, pointing inside or outside. Must be refused at
  open time (O_NOFOLLOW), not just by a pre-check. [engine.symlink_refused]
- F3 Directory or FIFO in serve_dir. A FIFO must not block the engine.
  [engine.nonregular_refused]
- F4 A file added to serve_dir after launch is invisible. Names must be
  resolved at OPEN. [engine.lazy_open]
- F5 The file changes or is replaced mid-stream. What's served must be the
  OPEN-time snapshot, and the SHA-256 recorded at OPEN must equal the SHA-256
  of the bytes actually served. [engine.snapshot]
- F6 File over the size cap (and always over 4 GiB-1, which OPEN's 32-bit
  RESULT can't express). [engine.size_cap]
- F7 Multi-megabyte GET until EOF isn't byte-exact (offset, chunking, last
  short chunk, EOF=OK/0). [e2e.get_multi_mb]
- F8 GET on a PUT stream, PUT on a GET stream, unknown stream id, CLOSE of
  an unknown id. [engine.wrong_stream]
- F9 Reopening a sid leaks the stale stream. A PUT reopen must leave a
  visible `.part`. [engine.reopen]
- F10 PUT overwrites an existing file. Create-only means O_EXCL on `.part`,
  and a no-replace link at CLOSE. Inbox equal to or nested in serve_dir.
  [engine.put_create_only, profile.inbox_overlap]
- F11 PUT without an inbox: must be NODEV. [engine.put_disabled]
- F12 PUT over the size cap. [engine.put_cap]
- F13 An engine exception escapes into the proxy loop. The guest vCPU stays
  parked and the Windows launcher dies. Must become ST_IO.
  [engine.exception_contained]
- F14 A command isn't logged, or per-stream bytes/SHA-256 are missing or
  wrong. [engine.log_complete]
- F15 The engine writes outside [win_phys+off, +len). [e2e.window_fence]

## G. Profile / launcher

- G1 Unknown key in "xfer", wrong types, doorbell/window misaligned,
  window 0 or > 64 MiB, serve_dir relative / missing / not a directory /
  symlink, inbox equal to or nested in serve_dir. [profile.*]
- G2 Doorbell overlaps DRAM, an ADT device range, an existing host tracer hook
  (vUART), or the C-side vGIC windows. Must be refused before guest entry.
  [launch.doorbell_overlap]
- G3 xfer disabled. The old launch path must be unchanged: no attach, the
  receipt says disabled, the old caps are still accepted. [profile.default_disabled]
- G4 The receipt lacks the doorbell base or window, so the operator can't
  attach the guest driver. [launch.receipt]
- G5 The attach happens after pt_update, or without RESERVED tracers, so the
  page-table refresh at start() overwrites the doorbell hook with the
  /arm-io HW passthrough. [launch.reserved_tracers]

## H. Not replayable here (hardware-only; see README "Risks")

- H1 Stall length per window (proxy link rate). Rendezvous of every vCPU on
  each command.
- H2 Stolen-time accounting: CNTVOFF covers the virtual counter only. T8142
  has no CNTPOFF.
- H3 Real cache coherency between Windows' MmCached mapping and EL2's
  maintenance.
- H4 Concurrency: two vCPUs on the doorbell while hv_exc_proxy drops bhl in
  its pinned-CPU wait loop.
