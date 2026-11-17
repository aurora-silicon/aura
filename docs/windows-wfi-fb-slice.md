# Bounded framebuffer conversion in the held guest WFI

## Why

Boot 144: every held guest WFI lasted 21-24 ms on every core, the boot core included, and ended
with one host-timer FIQ. The 1 ms WFI wake bound was intact; the time went into the 160-line
framebuffer slice `hv_handle_wfx()` converts before it waits, about 130 us per scanline. During
that slice the core does not look at its interrupts, so an IPI, a guest timer or a device
interrupt aimed at an idle core waited up to ~21 ms: every thread wake and I/O completion that
lands on an idle core.

The slice is slow because the guest's BGRA shadow is read through the host's Normal-NC mapping
(`hv_fb_init()`), four bytes per load: each load is an uncached round trip to DRAM. The scanout is
also Normal-NC but writes are posted. Production's fb.c says the same of its own NC framebuffer
reads (over a second for 30 MB) and copies through a cacheable mapping instead; the old "read NC
20 GB/s" note in hv.h does not match this machine at run time.

## Change

`hv_fb_convert_idle()` converts the idle slice in steps of `HV_FB_SLICE_STEP` (4) scanlines and
stops as soon as ISR_EL1 shows a pending physical interrupt; `hv_handle_wfx()` calls it instead of
one 160-line `hv_fb_convert_slice()`. A core now notices a wake within about one step (~0.5 ms at
the measured rate), then arms the wake timer and executes WFI, which returns at once. When nothing
is pending the same 160 lines are converted, so the refresh rate of an idle machine is unchanged.
The tick's 64-line floor under bhl is unchanged.

A cheaper copy is possible without changing what the panel shows: read the shadow through a
cacheable alias after a clean+invalidate of the lines (as fb.c does), or with wider loads.
Neither is done here: both change how the host reads guest-written memory and need a hardware
check that the panel output is identical.

## Failure model (written before the code)

1. A pending interrupt during conversion is not noticed for more than ~1 ms.
2. Steady interrupts starve the refresh entirely (the tick floor must still convert).
3. Lines are counted twice or skipped: the shared cursor and the fps report go wrong.
4. A board without a shadow spends time in the new loop.
5. The check costs more than the conversion it bounds.
6. The pre-wait predicate (no pending physical or virtual interrupt) changes.

## Verification

`tests/windows_el2_acct` compiles the actual `hv_fb_convert_idle()` and `hv_handle_wfx()` with a
modeled line cost and an interrupt that becomes pending mid-slice: the stay must end within one
step of it, convert fewer lines, and convert the full slice when nothing is pending. Mutations
that drop the check or convert the whole slice in one step are rejected. Responsiveness on the
machine needs a hardware boot.
