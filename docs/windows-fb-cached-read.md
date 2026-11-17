# Shadow framebuffer: cached reads, paced idle conversion

## Where the time goes (boot 146, from the 1 Hz lines and the source)

- Path: Windows (BasicDisplay on Mu's GOP) draws BGRA into the shadow, which `hv_fb_init()`
  allocated from the heap and mapped into the guest at its own address (`hv_map_hw()`, stage-2
  memory attribute "unchanged", so the guest's own stage-1 attribute decides; BasicDisplay
  normally maps a GOP framebuffer write-combined, which we cannot see from here). EL2 converts it
  to X2R10G10B10 in the panel's scanout (iBoot's framebuffer).
- Host mappings: the shadow's identity VA is remapped Normal-NC by `hv_fb_init()`; the scanout is
  Normal-NC (fb.c, `fb_init()`). The heap's other aliases, among them `PA | REGION_RW_EL0`, stay
  Normal write-back (memory.c `mmu_add_default_mappings()`), so a cacheable alias of the shadow
  already exists today; it is simply not used.
- Cost: a quiet held WFI on an idle P core lasted 21-22 ms (max ~23 ms) for its 160-line slice,
  about 130 us per scanline; the boot core's 1 Hz housekeeping spent 9.4 ms/s in the tick and held
  bhl 9.5 ms/s, nearly all of it the 64-line tick slice. E and P cores pay the same per line
  (boot 144), so the loop is bound by memory latency, not compute. A scanline is 2560 four-byte
  loads from Normal-NC memory (each an uncached DRAM round trip, ~50 ns), 2560 ALU conversions
  (~1 ns each) and 2560 Normal-NC stores, which are posted and gathered. Reading is nearly all of
  it; production's fb.c notes the same and copies through a cacheable mapping.
- Who copies: any core holding a guest WFI (unlocked, `hv_fb_convert_idle()`, 160 lines in steps
  of 4 that stop at a pending interrupt), and the boot core's 1 Hz tick (64 lines under bhl).
- What gets copied: nothing tracks changes. A shared cursor walks the frame round-robin, so the
  whole 17 MB frame is re-read and re-written continuously (median 64 fps, ~107k lines/s) whether
  or not anything on screen changed.

## Options, by gain per risk

1. **Read the shadow through its existing cacheable alias, clean+invalidate each band first**
   (chosen). Large gain: cache-line fills with prefetch instead of one DRAM round trip per pixel.
   No guest-visible change: no stage-2 or guest mapping changes, the guest's memory contents are
   untouched (clean+invalidate only writes back what the guest itself wrote), only host timing
   changes. Paired with a 60 fps pacing of the idle conversion, because a ~10x cheaper slice would
   otherwise let idle cores convert hundreds of frames a second and spend the saving on memory
   traffic.
2. Wider loads from the NC mapping: 2-4x at best, still one uncached round trip per access. Not
   needed once reads are cached.
3. Dirty tracking by stage-2 write protection: the biggest gain for a static screen, but it
   changes what the guest observes (permission faults and their timing on every first write to a
   page) and touches stage-2. Rejected here; it would need your review first.
4. Compare against a copy of the last frame: the read of the shadow is the cost, so a compare does
   not remove it, and it doubles memory. Rejected.
5. Only idle E cores convert: halves nothing by itself, and loses the P cores' idle time. Rejected.
6. Drop the bhl hold of the tick slice: with option 1 the 64-line slice costs a fraction of a
   millisecond; the floor is kept as is.
7. Let DCP scan out BGRA: the T8142 DCP path hangs (hv.c comment). Rejected.

## Failure model (written before the code)

1. Stale pixels: a line of the shadow sits in a cache (an earlier pass, or a speculative fill
   through any write-back alias) and is read instead of what the guest wrote since. Every band must
   be cleaned and invalidated, and the maintenance complete, before it is read.
2. Lost guest updates: an invalidate-only would discard the guest's dirty lines if the guest maps
   the buffer write-back. Use clean+invalidate; EL2 never writes the shadow after initialization.
3. Torn frames: rows from different guest frames in one panel frame. Unchanged by this design (no
   frame sync exists); bounded to one pass, as today.
4. Changed output: the panel must show exactly the old conversion of the same pixels.
5. Pacing starves the refresh (arithmetic error, wrap, zero height or frequency) or allows bursts
   (catch-up after a busy period converting many frames back to back).
6. Lock holds that delay wake-ups: no new work under bhl; the idle conversion still stops within
   one step of a pending interrupt.
7. Guest starvation: conversion still runs only on cores holding a guest WFI and in the 1 Hz tick.
8. The alias address is invalid (physical address bits overlapping the alias bit), faulting at
   EL2: checked at initialization, falling back to the NC reads.
9. fb_capture (host-driven screenshots) keeps reading the NC identity mapping: unchanged.
10. The maintenance costs more than it saves: measured on the next boot by a new 1 Hz line.

## Verification

`tests/windows_fb_path/run.py` compiles the actual `hv_fb_convert_slice()`,
`hv_fb_convert_idle()` and pacing from hv.c, and the previous `hv_fb_convert_slice()` from
c470c540, against a model in which the cacheable alias is a separate "cache view" that only
`dc_civac_range()` refreshes from memory. It checks identical output to the old converter, no
stale pixel after guest writes, 60 fps pacing without bursts after a busy gap, the one-step stop
on a pending interrupt, and the unpaced tick slice, and rejects mutations of each. Real per-line
cost, cache behaviour and panel output need a boot.

## Start-up self-test of the cached read (written before its code)

The design rests on one machine property: after `dc civac` and a DSB, a write-back read of a line
returns what a non-cacheable write put in DRAM, on any core, whatever another core still holds.
Apple cores have a chicken bit that turns DC by-VA operations into no-ops (m1n1 sets it on M1-M3;
`features_m4`, which T8142 uses, sets nothing, and iBoot's choice cannot be read: the HID registers
are locked here), and production's `mmu_map_uncached()` comment reports mismatched views between
cores on this chip. The panel cannot be seen from any probe, so the machine must prove it first.

`hv_fb_init()`, after the NC remap and before the guest exists, runs on the first 1 MiB of the
shadow (larger than L1, so lines sit in L1 and L2):

- same core (boot CPU): write A through NC; clean+invalidate and read A; read through the alias
  (lines now cached); write B through NC; read without maintenance ("stale seen", informational);
  clean+invalidate and read: every word must be B.
- cross core, for each of e-p (E holds, P checks), p-e (P holds, E checks) and a same-cluster pair:
  the boot CPU writes A and cleans it; holder X reads through the alias; writer Z writes C through
  NC; checker Y reads without maintenance (informational), then cleans+invalidates and reads: must
  be C; then X reads without maintenance (informational: did Y's maintenance reach X's lines?), then
  cleans+invalidates and reads: must be C.
- the region is written back to zero through NC and cleaned; the participating secondaries are
  brought up with `mmu_init_secondary()` for the test and returned to MMU-off with `mmu_disable()`
  (which also flushes their caches by set/way), so the later CPU_ON path is exactly as before.

Any wrong word, any exception (the whole test runs under `GUARD_MARK | GUARD_SILENT`), or fewer
than two non-boot CPUs in each cluster: the converter reads non-cacheable as before, and one line
says which case failed. A secondary that does not answer within two seconds refuses guest entry.

Failure modes of the self-test itself:

1. False pass: reads optimized away or served from registers (volatile accesses), a region too
   small to leave L1 (1 MiB), patterns that match by accident (pattern XOR word index, distinct
   per phase), checking a subset (every word is checked), a maintenance that only works on the
   issuing core but leaves another core's stale copy to be forwarded (the cross-core checks), a
   DminLine above 64 bytes (harmless: lines are maintained twice) or below it (lines skipped: the
   check fails).
2. The guest racing it: impossible, it runs before guest entry; Mu and Windows first see the shadow
   after `hv_fb_init()` returns.
3. Leaving the shadow non-zero: the region is rewritten to zero through NC and cleaned. If
   maintenance does not work at all, clean lines holding test patterns may stay cached until
   evicted; the participating secondaries are flushed by set/way when their MMU goes off, the boot
   core is flushed by set/way too in that case, and a guest that maps the buffer write-back would
   otherwise read a pattern only where it reads before writing. That residue exists only on the
   failure path, which also makes the converter read NC.
4. A secondary that faults or hangs: faults are counted and fail the test; a hang refuses guest
   entry (a CPU that cannot run a function now could not run Windows either).
5. Leaving a secondary in a new state: its MMU is turned off again, as it was.
6. Start-up time: estimated 30-60 ms (about ten 1 MiB NC fills and alias reads, eight 1 MiB
   clean+invalidate passes, four secondary MMU on/off cycles with a set/way flush each); the line
   prints the measured figure.

### As built (changes found while writing and replaying it)

- Secondaries are started with a bounded call (`smp_call4()` + `smp_wait_timed()`, 2 s) instead of
  `mmu_init_secondary()`, whose `smp_wait()` waits 300 s. A CPU whose MMU was already on stays on;
  only the ones the test turned on are turned off again (`mmu_disable()`, which also flushes that
  CPU by set/way).
- CPUs already handed to the guest or being started for it (`hv_started_cpus`,
  `hv_setup_pending`) are never chosen. Clusters are told apart by MPIDR Aff1 and Aff2 together,
  and each case is labelled by the class of the CPU holding the stale lines and of the one checking
  them: `pe` (held on P, checked on E), `ep`, and `ee` or `pp` for the same-cluster pair.
- After the first failed or timed-out call no further call is made. A call that timed out may
  still run later (`smp_call4()` has no cancel), and a late write step would put test patterns into
  the guest's framebuffer, so that case refuses guest entry instead of falling back; a call that
  could not be issued at all only fails the test.
- An exception is counted as a failure before the set/way decision, so a skipped maintenance op
  also gets the boot CPU flushed.
- "stale" before the maintained read is informational: it shows that a cached line was there to
  catch. If the hardware made a non-cacheable store invalidate the cached copies, "fresh" would be
  printed and the maintained read would still be exact; the converter, which faces the same
  sequence (guest store, EL2 read), would then be right with or without the maintenance.
- Console line, once per guest entry, just before the `HV: BGRA shadow ...` line:
  `HV: fb cached-read self-test: PASS in <us> us, 1024 KiB (same stale/0; pe checker stale/0
  holder fresh/0; ep checker ...; ee checker ...)`; on failure `FAIL (<case>) ...` with the cases
  not reached shown as `-`, and ` -- reading the shadow non-cacheable` at the end. Each note is
  "<before maintenance>/<wrong words after it>".
- Revised start-up estimate: twelve 1 MiB clean+invalidate passes (16384 lines each), nine 1 MiB NC
  write passes, 22 MiB of word-by-word reads through the alias, and four MMU off cycles each with a
  set/way flush of that CPU: about 15-60 ms, the spread being the unknown per-line cost of `dc
  civac` that the boot's `HV: fb cost:` line measures. Once, before Windows exists.

### Line size

`dc_civac_range()` is `CACHE_RANGE_OP(dc_civac_range, "dc civac")` in src/memory.c: it steps a
fixed `CACHE_LINE_SIZE` of 64 bytes from the given address and does not consult CTR_EL0.DminLine
(`do_dcache_maintenance_by_mva` in memory_asm.S does, but is not used here). Apple's L1D line is 64
bytes, so the stride matches; a larger line would only be maintained more than once, a smaller one
would leave every other line stale, which the self-test catches (the `lines-32-bytes` variant
below). The loop also does not round the start down, so a range that does not start on a 64-byte
boundary misses the line holding its last bytes. The converter's bands start at
`shadow + row * stride`; the shadow is 16 KiB aligned and the J813 panel's stride is 10240 bytes,
so every band is line aligned. A panel whose stride is not a multiple of 64 would not be.

### Verification of the self-test

`tests/windows_fb_path/run.py` now also compiles the self-test functions verbatim and runs ten
variants against a model with a cache per CPU (a read of a line it does not hold fills from another
CPU's copy, stale first, else DRAM): `normal` (PASS, read write-back; every converter check),
`maintenance-noop` and `lines-32-bytes` (FAIL same core), `maintenance-not-broadcast` and
`maintenance-cluster-only` (FAIL cross-cluster), `too-few-cpus` (FAIL, no call made),
`guest-owned-cpus` and `secondary-mmu-on` (PASS, those CPUs untouched or left as they were),
`secondary-fault` (FAIL exception) and `secondary-hangs-writing` (guest entry refused, no call
after the hang). Each failure must end in `read non-cacheable` with the region zero in DRAM and no
test pattern left in any modeled cache. 18 mutations of the self-test are rejected, among them
skipping it, dropping the maintenance before the check, skipping the cross-core cases, not zeroing
the region, leaving a secondary's MMU on, running unguarded, ignoring exceptions, unbounded waits
and not refusing entry after a hang.
