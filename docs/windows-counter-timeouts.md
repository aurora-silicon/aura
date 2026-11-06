# Windows counter and timeout boundaries

## Failure model recorded before implementation

The virtual UART runs USB event and endpoint operations inside an EL2 guest
exception callback. Endpoint command polling calls `poll32()` and `udelay()`;
SuperSpeed transfer retirement also calls a millisecond delay. A stopped
counter inside any of those helpers prevents return to EL1, so the stress
guest's own deadline cannot recover the session.

The Windows backend already supplies guest CNTPCT reads from an Apple counter
alias on T8142. Its inherited comment claims the alias has CNTFRQ units, but
the hardware guest reports CNTFRQ of 1 GHz and the alias rate is not yet
established. That comment is not sufficient evidence to change all host
timeouts to the raw alias.

- A frozen architectural counter must be reproduced at the production delay
  and endpoint-command boundary, with an external model watchdog so the
  offline campaign itself remains bounded.
- The chosen counter and its frequency must describe the same time domain.
  A monotonic counter with the wrong rate must fail duration checks.
- Timeout creation and expiration must use the same counter source and epoch,
  including transitions between proxy and guest-exception contexts.
- A successful endpoint command must still return promptly. A permanently
  busy endpoint must fail within its configured budget, without relying on
  further guest execution or host USB progress.
- Linux and non-T8142 behavior must remain unchanged. Early identification
  cannot read an unsupported Apple register on another CPU.
- Clock selection or scaling requires measured/public evidence; do not infer
  the rate from the name of a system register or the CPU clock rate.

The replay uses production timing and endpoint-command code with modeled
system-register reads, MMIO and an external watchdog. It must retain source
hashes, exact commands, measured model durations and logs. This does not prove
physical counter behavior or resolve the CDC smoke's leaked bytes by itself.

## Qualified implementation

A bounded read-only J813 calibration measured the architectural counter near
1 GHz and the raw alias near 24 MHz. The conversion is therefore 125/3, not an
identity mapping. Saved J813 ADT metadata also names a 24 MHz timebase. The
implementation accepts this specific T8142/1 GHz combination; it does not
apply an assumed rate to other chips.

Before changing hypervisor/device state, the boot CPU checks CNTFRQ and samples
both counters across at least one architectural millisecond. This qualification
uses a fixed iteration limit, not a timeout based on the unqualified clock.
Both alias samples are bracketed by architectural reads. A rate mismatch or
invalid bracket fails initialization. One shared offset aligns the scaled
alias to the final architectural bracket; no common power-on epoch or separate
per-CPU calibration is assumed. Publication uses release/acquire ordering.

After qualification, `get_ticks()`, microsecond delays, common timeout helpers,
Windows host bookkeeping and reflected guest CNTPCT reads use the same scaled
clock. Before qualification, and in Linux or on other chips, the architectural
counter remains selected. Physical guest CVAL and host CNTHP TVAL values remain
in CNTFRQ units. Virtual diagnostic time subtracts CNTVOFF from the physical
clock; `stolen_time` is accumulated in the same architectural units. MTP's
`CNTFRQ/100` service interval consequently represents ten milliseconds.

## Repeatable offline replay

```sh
python3 tools/windows_counter_e2e.py --output build/windows-counter-replay
```

The runner snapshots and compiles production timer helpers, USB endpoint-command
polling, Windows host-counter selection and the guest CNTPCT case. Its modeled
architectural counter stops during an EL2 callback while the 24 MHz alias keeps
advancing. It checks one-millisecond delay and command-timeout duration,
successful command completion, one-second guest progress, a different counter
epoch, unsupported frequency/rate rejection, virtual offset accounting and
unchanged default/other-chip selection. An external model watchdog reproduces
the old unbounded wait without hanging the test process. Output includes exact
source hashes, generated production excerpts, sanitizer logs and `result.json`.

The original implementation can be replayed with `--expect-unfixed` against
its saved source snapshot. Offline success does not replace physical timed
guest and USB reopen qualification.

## Qualification rejection diagnostics

Candidate14 refused the first physical VUART smoke before guest entry, but its
single generic message did not distinguish frequency, sample ordering, sample
width, iteration exhaustion or rate mismatch. Preserve every acceptance bound
while reporting a distinct rejection reason and the sampled values. Print only
after rejection so console traffic cannot change the measured interval.

The iteration cap is an execution bound, not a portable time guarantee. A
sufficiently fast read loop could exhaust it before one architectural
millisecond; a frozen counter also exhausts it. Replay both cases and retain
observed progress in the diagnostic. Separately inject backward and overly
wide first/final brackets, backward polling reads and alias reversal. All must
refuse initialization without publishing the new clock. These diagnostics do
not establish whether barriers or a larger cap are needed on hardware.

The subsequent physical in-call measurement identified the iteration-bound
failure: one million plain reads take 284,468 ns on this M5, while one million
reads each preceded by ISB take 11,092,172 ns. Calibration now serializes each
read, including both bracket boundaries and the intervening alias sample.
The same one-million iteration cap, one-millisecond minimum interval,
ten-microsecond maximum bracket and rate tolerance remain in force. Replay
these measured rates before the fix, then require admission after serialization;
a genuinely frozen raw counter must still fail at the iteration bound.
