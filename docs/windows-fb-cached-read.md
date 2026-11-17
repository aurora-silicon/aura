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
