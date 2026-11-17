# J813 E-cluster step-up

The J813 firmware hands the E cluster over at command state 2 (972 MHz) and the
P cluster at state 10 (3516 MHz). States above the handoff values are
unqualified outside macOS. This change adds two pieces:

- `proxyclient/tools/qualify_t8142_estates.py` steps the E cluster up one
  state at a time through the resident proxy and proves each step natively.
- An explicit Windows profile key, `"ecpu_boot_state"`, makes the Windows
  hypervisor enter the guest with the E cluster at a chosen state.

The P cluster is never written by either piece.

## Failure model (before implementation)

### Qualification tool

1. Wrong target or firmware: chip, board or model differ; the ADT E ladder is
   not the J813 ladder, so state-to-kHz targets are wrong; `CNTFRQ_EL0` is not
   the 1 GHz the T8142 code qualifies, so the MHz arithmetic is wrong; the
   command registers are not the exact handoff values (a previous session or
   tool already changed them); the exception count is not zero (the target is
   already unhealthy); the ADT CPU topology is not cpu0-5 = E, cpu6 = boot P.
2. Tool-induced writes: the P register is written; a value clobbers unrelated
   bits (22, 42); a write is issued while BUSY, from a stale read, for a state
   outside 2..9, twice for one step, or again after a failure.
3. Transition failures: BUSY never clears (the cluster stays mid-transition);
   BUSY clears with a wrong echo (firmware refused or clamped the request); the
   transition is slower than the resident setter's 400 us bound, so the
   hypervisor path would time out on the same transition; the P register
   changes as a side effect.
4. Measurement errors: the probe runs on the wrong core; the counter view is
   frozen or not 1 GHz; the window is too short for the counter resolution; an
   interrupt inflates one window; the one-iteration-per-cycle assumption of the
   documented loop does not hold; the achieved frequency differs from the
   table (clamp or wrong mapping); the boot core's frequency moves.
5. Stability failures: a wrong computation on any E core under load; a
   secondary that does not acknowledge or finish; an exception on any core;
   firmware throttling under six-core load; a silent proxy (CDC drop, resident
   crash or reboot).
6. Secondaries: start fails on the production resident (reset-vector relay
   refused); a core is absent or has the wrong MPIDR; code is not visible to
   secondaries; a secondary stores into the CTRR_D-locked boot object; a later
   chainload runs over running secondaries.
7. Host: the dry run opens a device; the artifact is lost on a crash; a host
   exception or Ctrl-C after a write leaves the state unknown.
8. End state: the return to state 2 fails and is retried, or success is
   claimed without a verified final echo and frequency.

### Hypervisor boot state

1. Profile: the key has the wrong type (bool, string, float), is out of range
   or targets another machine.
2. The chainloaded image lacks the feature and silently ignores the request.
3. The ADT E ladder differs from the J813 ladder the host expects.
4. Configuration after initialization, twice, on the wrong chip or board, for
   a state not in the ADT table, or with a host kHz that disagrees with the ADT.
5. Apply: the registers are not at the exact handoff values; BUSY before the
   write; BUSY timeout or wrong echo after it; unrelated E bits change; the P
   register changes. Any of these must refuse guest entry, quarantine the
   session and never retry on the same resident.
6. Ordering: applying after the framebuffer or MTP handoff would leave devices
   half handed over on failure.
7. Host proxy `CPUFREQ_INIT` (state 1 then E2) during or after the session
   would undo the boot state or race the guest.
8. With the key absent there must be no new proxy request, register access or
   ADT change; the default (non-Windows) image must be unaffected.
9. `AppleCpuFreq` 0.2.0.3 admits only E1..E2; on a higher boot state its D0
   entry fails. With the ownership flag also set, the hypervisor must still
   apply the state exactly as without it.
10. A Mu or ACPI change must not be needed.
11. Found on boot 145: Mu 45087b59 asserts when the ownership marker is
    absent, so the key must not require `"cpufreq"` disabled.

## Qualification tool

The resident cannot command E states above 2 itself: `P_CPUFREQ_INIT` replays
the fixed `t8142_clusters` defaults (state 1, then E2/P10), and
`P_CPUFREQ_SET_CLUSTER_PSTATE` reaches `cpufreq_set_cluster_pstate()`, which
fails on any chip but T8152. The tool therefore runs the `set_pstate()`
register protocol itself, from a register-only routine on the boot core: one
read (BUSY must be clear and the value must equal the host's read), one write of
`(value & ~0x1f) | BIT(25) | state` to `0x210e20020`, then BUSY polling bounded
at 20 ms with the elapsed counter ticks recorded. The host accepts the step only
if the echo carries the state with BUSY clear, no other bit changed, the P
register is unchanged and BUSY cleared within 400 us, the resident setter's own
bound. The planned writes are `0x2400103` .. `0x2400109`, then `0x2400102`.

Each step then measures cpu0 (E) and the boot CPU (P, the control) three times
with the documented dependent decrement loop timed against `CNTPCT_EL0`
(50,000,000 iterations, 1 GHz counter). Every sample must lie within 2% of the
table: the J813 measurements reproduced every table entry measured so far, and
the closest E entries (2964/3048 MHz) are 2.8% apart, so 2% still identifies
the state. A soak follows: three rounds of about one second in which all six E
cores run a checked add/multiply-accumulate loop at once. Every result must
match its closed form, every core's rate must stay within 2% of a solo
reference measured just before, and the exception count must stay zero. A final
cpu0 probe catches a clamp that outlasts the load. Three seconds per state is
long enough to see a marginal ALU or a firmware clamp under six-core load and
short enough not to heat the passively cooled chassis.

Any failure stops the run without retries. After a stop that follows a write
the tool issues no further frequency request and says so; the E state is as
last read. By default an accepted run ends with one write back to E2, verified
by echo and frequency. Secondaries stay running, so the machine must be reset
before any chainload. `--dry-run` opens no device: it runs the same control
flow against a nominal model and prints the plan and every planned write.
`proxyclient/tools/qualify_t8142_estates_e2e.py` replays every stop condition
above against that model.

## Hypervisor boot state

The profile key `"ecpu_boot_state": N` (integer 2..9, J813 only, with either
`"cpufreq"` policy) selects the E command state Windows starts at. Absent, the
host sends nothing new and the hypervisor touches no register. The host checks
the ADT E ladder against the J813 table before any target mutation, requires
capability bit 14, then sends `P_HV_WINDOWS_ECPU_BOOT_STATE` (0xc86) with the
state and its kHz after `P_HV_WINDOWS_CONFIG` and before `hv_init`. The
hypervisor accepts it once, before initialization, only if its own read of
`voltage-states1-sram` gives the same kHz. From then on host `CPUFREQ_INIT/SET`
are refused, as for Windows ownership.

`hv_start()` applies it first, before the framebuffer and MTP handoffs: both
command registers must still hold `0x400102`/`0x40010a`, or nothing is written;
then `set_pstate()` raises E one state at a time (BUSY before and after, echo
checked, 400 us bounds); finally E must show the state, no other bit changed and
P must be unchanged. Any failure refuses guest entry, quarantines the session
and is never retried on that resident. State 2 performs the checks and writes
nothing.

With `"cpufreq": "j813-manual-v1"` both flags are set: the ownership latch is
the same, and `hv_start()` still finds the handoff values because nothing else
writes the controllers before it. Boots keep that policy. Mu 45087b59 must see the
ownership marker: without it, `AcpiPlatform.c:4220-4223` frees the rejected CPUF
SSDT and re-reads the same section without clearing the pointer, the second
rejection frees it again and DxeCore asserts in `Pool.c(730)` before Windows
(boot 145). With the marker, Mu's gate reads only `/chosen` properties and the
SSDT is a constant `_STA`/`_CRS`; nothing in Mu reads the E state.
`AppleCpuFreq` 0.2.0.3 exposes E levels 1..2 only; its D0 entry fails with
`STATUS_DEVICE_DATA_ERROR` on a higher boot level after reads alone, so
`AURA8142` shows a start failure. Its only register write is in the SET IOCTL,
which requires a successful D0.

`tools/windows_ecpu_boot_e2e.py` compiles the actual setter, table, apply,
latch, `hv_start()` prologue and proxy cases against a register and ADT model.
