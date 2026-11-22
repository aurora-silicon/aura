# Guest TLBI trap count (Windows personality)

## Why

The Windows trap handler, `hv_handle_msr_unlocked()` in
`src/hv/windows/hv_exc.c` (added in 478f3bcc), re-executes a trapped
outer-shareable invalidate as its inner-shareable form: VMALLE1OS becomes
`tlbi vmalle1is`, and VAE1OS, ASIDE1OS and RVAE1OS become VAE1IS, ASIDE1IS
and RVAE1IS. A device that only observes outer-shareable maintenance (the GPU's
translation cache is the case in question) would then miss the guest's
invalidate. Whether these instructions trap at all on T8142 is not known:
the Windows build's HCR_EL2 sets none of TTLB, TTLBIS, TTLBOS or FB. One boot
answers it if the hypervisor says how many TLBIs it saw. This adds that count.
It is a diagnostic only. The rewrite is not changed.

## The line

Printed once, by the boot CPU, at the end of `hv_start()` in
`src/hv/windows/hv.c`, after `hv_join_cpus()` has returned and after the
existing `HV: All CPUs exited` line (which only a clean drain prints):

    HV TLBI TRAPS os=<n> is=<n> other=<n>

Each count is a decimal total over all CPUs, for the whole guest run.

- `os`    the four encodings the rewrite handles, exactly: VMALLE1OS,
          VAE1OS, ASIDE1OS and RVAE1OS (op0 1, op1 0, CRn 8, CRm 1 op2 0-2,
          or CRm 5 op2 1).
- `is`    any EL1 inner-shareable TLBI: op0 1, op1 0, CRn 8, CRm 3
          (VMALLE1IS, VAE1IS, ASIDE1IS, VAAE1IS, VALE1IS, VAALE1IS) or
          CRm 2 (the range forms), any op2.
- `other` every other TLBI encoding (op0 1, CRn 8 or 9). This includes the
          outer-shareable forms the rewrite does NOT handle (VAAE1OS, VALE1OS,
          VAALE1OS, RVAAE1OS, RVALE1OS, RVAALE1OS), the non-shareable forms,
          and all nXS (CRn 9) forms.

A trap is counted by its encoding, whatever its direction, and whether or not
a handler then accepts it.

Reading it: `os=0 is=0 other=0` on a run that reached the line means no guest
TLBI trapped to EL2, so the rewrite never ran and the guest's outer-shareable
invalidates reached the hardware as written. `os` above zero means the rewrite
ran that many times. No line means no answer (see F12), never zero.

## Failure modes (written before the code)

Ways the counter could change behaviour:

- F1. The rewrite changes. Any edited line inside the four cases, or in
  `SYSREG_MAP`, changes what the guest's invalidate does. Rule: the counter
  lives outside the `switch`; the change adds lines only and removes or edits
  none in `hv_exc.c`; the rewrite block is byte-identical to 187846f1's.
- F2. The counter claims or redirects the trap. A counter that returns early,
  or alters `reg`, `rt`, `is_read` or `regs[]`, would stop the rewrite or skip
  a non-TLBI handler. Rule: it takes `reg` by value, returns nothing, and is
  called as a plain statement before the existing dispatch.
- F3. The counter has side effects. A system-register write, barrier, lock,
  print or TLB operation on the trap path would change the guest's view or its
  timing. Rule: it reads the CPU number (TPIDR_EL2, as every per-CPU record in
  this file does) and stores to memory, nothing else.
- F4. Hot-path timing. Every MSR trap goes through this function, including
  the T8142 ICC_* traffic taken on each interrupt. A shared counter line, or an
  atomic read-modify-write, would add cross-CPU cache traffic to each one.
  Rule: non-TLBI traps pay one mask-and-compare and a branch; TLBI traps also
  pay one plain store into the trapping CPU's own 64-byte-aligned record
  (single writer, the same pattern as `struct hv_el2_acct`).
- F5. Out-of-bounds write. A CPU number at or above `MAX_CPUS` would write
  past the array. Rule: bounds-checked; such a trap is not counted.
- F6. Image layout. New code and a 4 KiB `.bss` array move the image. The DCP
  boundary assertion in `aura-raw.ld` must still hold; the margin is reported.
- F7. The line breaks a parser. The launcher's receipt (`guest_exit_status`)
  comes from the proxy's return value, not from console text, but the health
  gate and tools match console substrings: `HV: All CPUs exited`, `SYSTEM_OFF`,
  `guest bugcheck`, `now running:`, `User interrupt`, `guest WFIs held at EL2:`,
  `emulated IRQ traffic (ack/eoi):`, `redirected sync vectors:`. Rule: the line
  holds none of them, has its own fixed prefix, is one `printf` well under the
  512-byte buffer, and comes after the existing lines, so their order is
  unchanged.
- F8. Printing where printing is unsafe. Console output from a FIQ/IRQ path,
  under bhl or during a rendezvous outlasts the rendezvous budget. Rule:
  printed only from `hv_start()` on the boot CPU after the join, with no lock
  held, where `HV: All CPUs exited` is already printed.
- F9. Printed more than once. Rule: one call site, on the boot CPU, behind a
  once flag.

Ways the count could be wrong:

- F10. The `os` set drifts from the rewrite. It is a second copy of the four
  case labels. The desk test reads the labels out of the rewrite block itself
  and checks that exactly those encodings, and no others, count as `os`.
- F11. Field decoding. A wrong shift or mask would count an MSR as a TLBI, or
  an IS form as `other`. Rule: decode with the same `ESR_ISS_MSR_*` and
  `SYSREG_ISS` macros the handler uses. The desk test drives every op0, op1,
  CRn, CRm and op2 value (16,384 encodings) in both directions and compares
  with an independent table.
- F12. Some TLBIs are never seen. A count placed after the PMU/GIC/AIC
  pre-dispatchers, or inside the `switch`, would miss traps those claim or that
  reach `default`. Rule: count at the top of `hv_handle_msr_unlocked()`, which
  both the ESR_EC_MSR and the Apple IMPDEF MSR (ESR_EC_IMPDEF) paths call.
  Not seen, by design: a TLBI that does not trap (the measurement), and a TLBI
  that is UNDEFINED at EL1, which goes to the guest's undefined vector
  (redirected to the HVC trampoline on T8142), never reaches this function,
  and is not emulated there either (the assist only takes PMU and GIC
  registers); the guest would take its own undefined exception.
- F13. Double counting. An unhandled trap goes on to the locked
  `hv_handle_msr()`. Rule: count only in the unlocked handler; the locked one
  and the T8142 sysreg assist are untouched.
- F14. Lost updates between CPUs. Concurrent `++` on one shared counter loses
  increments. Rule: one record per CPU, written only by that CPU with a relaxed
  atomic store; the boot CPU sums the records with relaxed atomic loads, so no
  read is torn.
- F15. Read before the writers stop. After a failed drain a stuck CPU can still
  be in the guest; the line is printed anyway and may miss its later traps.
- F16. Stale counts. The Windows personality admits one guest per resident
  session, a RAM chainload loads a fresh image, and `.bss` is zeroed at image
  start, so every run starts at zero.
- F17. Misreading the kinds. `os` is not "every outer-shareable TLBI"; it is
  the four rewritten forms. The other OS forms are in `other`.
- F18. No line at all. An EL2 panic, a watchdog reset, a guest that never
  exits, a setup failure before guest entry, or a host that stops reading
  before the guest returns all leave no line. The line is printed whether or
  not the drain succeeded, but a missing line is "no answer", not zero.
- F19. Format. `%lu` of a u64 must print in full; the desk test parses the
  printed line back.
- F20. Build drift. New warnings, a changed default (non-Windows) image, or a
  changed E boot-state trace would mean the change reached further than
  intended. Rule: same warning set as 187846f1, default image byte-identical
  with the tag pinned, E boot replay trace byte-identical.

## Desk test

    python3 tests/windows_tlbi_count/run.py --output NEW_FOLDER

It compiles the production record, counter, report and ISS macros verbatim,
with the rewrite's own case labels and the handler's own ISS mask statement
read out of `hv_handle_msr_unlocked()`, under ASan/UBSan, and models only the
CPU number and the console. It checks the call sites in the source text (F2,
F3, F8, F9, F12, F13), drives every encoding in both directions across all CPU
slots (F10, F11, F14), out-of-range CPUs (F5), eight concurrent writers (F14)
and the printed line (F7, F9, F19), and requires nine mutants to be rejected.
It writes `result.json` with the hashes of every input and artifact. It does
not run the hypervisor, Mu or Windows, and says nothing about whether the
instructions trap on hardware.
