# Per-CPU EL2 time accounting (Windows personality)

## Why

Boot 143 measured a register-only loop at 0.86 ns/iteration on the boot core
(hypervisor cpu6) and 1.26-1.31 ns on the other three P-cores, with Windows
charging the thread the full wall time. Roughly a third of every non-boot core
disappears below Windows, and the console says nothing about where. This adds
the one number that does: per hypervisor CPU, how long each second it spent at
EL2, split by cause.

## The 1 Hz line

Printed by the boot CPU from the T8142 housekeeping tick, after the existing
WFI/vector/IRQ lines, one line per live CPU (boot CPU included), one `printf`
per line:

    HV: el2 cpu7 win=1000412 el2=331012 max=812 tick=5001:310220 irq=48:912 abs=96:240 vec=0:0 sync=2:31 wfi=0:0 lock=5003:19400 hold=21012

All times are microseconds within the window. `name=count:us`.

- `win`   window length (time since the previous report).
- `el2`   sum of the seven exclusive buckets below; `win - el2` is guest time
          plus exception entry/return and the vector stubs.
- `max`   longest single EL2 stay that ended in this window.
- `tick`  host CNTHP FIQs: the periodic host tick or a WFI wake expiry.
- `irq`   interrupt reflection: the IRQ vector, guest timer / Fast-IPI FIQs,
          and ICC_* (IAR/EOIR/SGI1R/PMR...) absent-register sites.
- `abs`   other absent-register emulation (PMCCNTR and friends) through the
          indexed HVC sites or the redirected undefined-instruction vector.
- `vec`   guest exceptions passed through a redirected vector (EL0 syscalls,
          page faults).
- `sync`  every other synchronous trap (MSR traps, PSCI/SMC, MMIO aborts,
          N1 HVCs) and SError.
- `wfi`   guest WFI held at EL2, including the idle framebuffer slice.
- `lock`  count = big-hypervisor-lock (bhl) acquisitions, us = time spent
          waiting for it; subtracted from the stay's own bucket.
- `hold`  time this CPU held bhl (overlaps the buckets above).

A stay is attributed whole to the cause recorded last before it returned;
vector entry supplies the default (sync, irq, irq, sync for SError).

## Failure model (written before the code)

1. A host-timer FIQ, WFI, PMCCNTR site, ICC site or vector pass-through lands
   in the wrong bucket, or a vector without a classifier gets no default.
2. Lock wait is counted twice (bucket and `lock`), not at all, or leaks from
   one stay into the next.
3. Guest time is counted as EL2 time, or EL2 time is lost: the buckets plus
   guest time must equal the window.
4. One CPU's stay is credited to another, or per-CPU records share a cache
   line and the measurement perturbs the cores it measures.
5. The first report has no baseline; deltas wrap; a non-1 GHz counter is
   scaled wrongly; the window and the deltas disagree.
6. The reporter reads a record mid-update (torn value) or its max reset races
   the writer badly enough to report nonsense.
7. A stay in flight across a report (an idle CPU in WFI) disappears instead of
   landing in the next window and in `max`.
8. A bad cause index writes outside the record.
9. A line exceeds the 512-byte printf buffer, is split over several printf
   calls (interleaving with other CPUs), contains a substring the health gate
   parses, or is printed from a hot path; the report floods on a chip whose
   housekeeping tick is not 1 Hz.
10. The hot path gains console output, a lock, or a barrier-heavy atomic.
11. The accounting commit changes guest-visible behaviour or an existing
    console line; the default (non-Windows) image changes at all.

## Verification

`tests/windows_el2_acct/run.py` compiles the production accounting, reporter,
FIQ, WFI and timer functions verbatim against a modeled clock and register
file, drives the exception stream a busy secondary, an idle secondary and the
boot CPU would produce, and checks the printed lines against the model. It
rejects source mutations for failures 1, 2, 3, 5 and 7. Cost on hardware and
the absence of perturbation (failure 4 and 10) can only be shown by booting.
