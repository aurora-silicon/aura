# T8142 WFI wake and periodic housekeeping — pre-code design

## Source and observation

This isolated candidate starts at `583359af0d399577c78f982e7e03d97cbc292211`, tree
`77b66f8ed53f2a5654cac98013621c75a7ce4321`, matching the live boot117/118 native
freeze. No live source, image, capability bits, Mu image, boot guard or launch
policy is changed. The original image remains the rollback artifact.

The nominal T8142 primary housekeeping interval is one second. Guest WFI uses
the same physical CNTHP timer with a one millisecond wake interval. Every expired
CNTHP currently becomes `tick=true`, and on the interruptible CPU invokes the
entire housekeeping body. Captured boot117/118 IRQ-summary median intervals are
31.713/31.750ms. This establishes excess housekeeping entry, not a measured
CPU-speed or boot-time improvement. Counter and log rates are not CPU frequency.

## Deadline design

- Keep one owner-CPU periodic deadline and its initialized flag for each CPU.
  State uses the existing qualified `hv_host_counter()` clock, in architectural
  timer units, whose epoch already matches physical timer comparators.
- T8142 only: initialize the deadline when the CPU's periodic/wake timer is
  first armed. A wake does not move the periodic deadline. Program a relative
  timer value for the earlier of the one millisecond wake and the periodic
  deadline; if already due, program one tick, never a zero/negative delay.
- A normal rearm preserves a future deadline. Completing genuine primary
  housekeeping advances by whole periodic intervals to the first future
  deadline. Delayed or overlapping expiries coalesce into one housekeeping
  execution, with no catch-up storm or cumulative drift. Secondary expiry
  consumption advances that CPU's deadline without running primary work.
- Use wrap-safe unsigned subtraction and signed-half-range deadline comparisons;
  configured intervals are nonzero and below the half-range. Test wrap and
  delayed dispatch. Do not change CNTFRQ, offsets or counter redirection.
- Keep the FIQ entry and pending-timer classification unchanged. Guest Fast-IPI
  retry, pending guest timer reflection, PMU handling, rendezvous and shutdown
  checks retain their existing path. A wake-only expiry is still serviced.
- Wake-only primary entries retain bounded host I/O service: serial capture
  flush, USB/proxy polling, user-interrupt handling and virtual-UART polling
  behind the existing Mu/Windows readiness guard. They skip the full scan,
  diagnostic and framebuffer housekeeping body. Serial capture already sends
  on newline/full batch; retaining its fallback avoids reducing throughput.
- The existing WFI path still checks pending physical/virtual interrupts before
  sleeping and uses the original one millisecond bound. Do not lower that rate,
  alter CYC_OVRD, suppress guest IRQs, or bypass secondary-CPU safety.
- Non-T8142 timer programming and housekeeping behavior remain byte-equivalent
  in their branches. The new helper does not change existing capability bits.

## Failure model before implementation

1. Wake rearm postpones the genuine periodic deadline indefinitely.
2. Filtering a wake loses a due/overdue housekeeping tick or an overlapping
   genuine expiry; long housekeeping creates an immediate catch-up storm.
3. One CPU overwrites another CPU's timer purpose/deadline; initially deferred
   secondary timers become armed prematurely.
4. Deadline wrap, clock deltas, interval changes or a zero relative timer value
   create a huge sleep, divide by zero or a tight interrupt loop.
5. Wake-only filtering also filters guest interrupt reflection, Fast-IPI retry,
   rendezvous, watchdog/bugcheck observability or shutdown handling.
6. A pending virtual interrupt causes WFI or timer arming despite the original
   no-sleep predicate. CPU register retention workaround must remain intact.
7. Host command polling or serial buffering waits behind the reduced diagnostic
   frequency, slowing N1 transfers or hiding exit/drain evidence.
8. A branch affects another chip or firmware-before-readiness behavior.
9. A model reproduces intended pseudocode while production timer/dispatch code
   differs. E2E must compile actual selected source functions and record their
   hashes, hardware boundary substitutes, commands and complete time stream.
10. PASS is based only on fewer log messages. Required offline evidence is exact
    deadline/service behavior; actual speedup requires matched hardware A/B.

## Required E2E artifacts and hardware decision

Compile actual timer-arm/mask and housekeeping-dispatch functions with a modeled
per-CPU clock/register/I/O boundary. Run idle wake streams, active/no-WFI streams,
pending-interrupt cases, delayed/overlapping expiry, wrap, multiple CPUs,
readiness/user-interrupt service and non-T8142 flows. Verify full housekeeping
occurs at genuine deadlines while wake bounds and I/O/guest service remain.
Include source mutations that restore the old full-work-on-every-wake behavior
or postpone deadlines, and require rejection. Save JSON timelines and receipts.

Build the ARM64 native image from this checkout using the existing toolchain and
no downloads. Pin before/after source, compiler/tool bytes and output hashes.
Root reviews the exact candidate before any hardware activation. A later A/B
must match boot policy/packages, readiness milestones and clock domains, retain
all ten CPUs/health/shutdown evidence, and compare boot phases plus log/traffic
volume. No speedup is claimed from the offline test.
