Current requirements are in the dated phase revisions below; earlier entries
retain the assumptions tested by prior experiments. The producer must withhold
the descriptor on wrong chip, board, model, ADT mapping, reset GPIO/function or
DART geometry. Cold-gate admission rejects an existing or unknown power owner
before resets. DART and endpoint checks never clear or adopt an active owner.
After training, absent/mismatched identities, any BME, invalid BAR sizing,
dropped BAR/bridge/command write, live ROM or changed DART state must hold PERST
and prove link/port drain before returning a clean failure. A failed clamp must
return a distinct quarantined failure. No failure may leave a valid descriptor.
Repeated successful calls must not retrain an owned device. The descriptor must
have exactly the Mu v1 ABI, with six fixed resource subranges.

N1 census HVC failure model (written before the census implementation):

- No completed successful preparation, including a preparation that failed after
  training, must authorize a register read.
- A request on any CPU other than the boot CPU must be rejected without MMIO.
- A wrong call magic must remain unhandled with every argument unchanged.
- A wrong version or nonzero reserved argument must return an explicit failure,
  distinct acknowledgement, version 1 and zero data, without MMIO.
- Only the 17 aligned 32-bit physical addresses used by Mu's current census are
  readable. Adjacent registers, DART command registers, unaligned addresses and
  the rest of each advertised resource are not authorized.
- An existing exception guard, masked SError or pending SError must reject the
  read without altering the guard or touching MMIO.
- A load fault or a fault delivered at the completion barrier must return failure
  and zero data, with the previous guard restored. A fault marker is not data.
- Valid calls return the exact zero-extended 32-bit value, never write device
  registers, and never retrain, reset or change BME/DART ownership.

The protocol model injects exceptions through the same exception counter used by
the production guard. It does not prove physical SError delivery or recovery.

SMC reset-provider failure model (recorded before the admission fix):
- The captured J813 ADT uses three distinct nodes: SMC controller, RTBuddy nub,
  and its `smc-pmu` child. The PCI bridge's reset function references the child,
  not the controller. Rejecting this valid topology prevents all N1 preparation.
- The accepted provider must remain at the exact path below the known SMC
  controller, with the captured controller/nub/provider compatible strings.
  A missing ancestor, wrong type, arbitrary matching phandle elsewhere, or a
  reset function pointing at the controller must fail before any MMIO or train.
- Preserve the exact pKW4/gP13/reset-value tuple and reject changes to it.
- The regression fixture contains only these public node paths, compatible
  strings, phandles and reset words from the captured ADT, without serial
  numbers, device identifiers, keys, or unrelated tree contents.
- Run the existing complete production preparation/census replay against that
  fixture before editing production code, then rerun with the corrected gate.
  Modeled training/MMIO still do not establish physical handoff success.
# Power admission regression, recorded before the correction

The J813 hardware run reached the N1 producer after identity validation but
lost the proxy before any training log. The producer currently reads DART
registers before the public Linux helper powers APCIE. An unpowered fabric
read may never complete; an exception guard cannot make it safe.

- Validate the complete target identity before requesting power.
- Enable the same ADT APCIE power domains as Linux before any N1 register
  access, then require the domains to report active.
- A failed enable, inactive readback or unknown power state must produce no
  N1 MMIO, training, descriptor, or retry. Keep the resident one-shot.
- After power admission, retain every existing clean-DART, BME, reset and
  descriptor check. Power admission does not prove an idle DMA owner.
- Record phase boundaries before power, clean-state checks and training so
  a physical stall can be localized without another blind register probe.

# Cold fabric phases, recorded before splitting the Linux sequence

The power-first hardware attempt still stopped inside the combined DART/port
admission phase. It does not identify the exact register that failed. Linux
performs PHY/PLL/pclk/AXI and root-controller setup before any port access, and
never reads DART. Its successful sequence therefore does not prove DART is
readable immediately after PMGR activation.

- Validate all local tunable widths, alignment and bounds before changing a
  reset signal or power state. Invalid firmware data may not escape the known
  PHY, PHY-IP, AXI, port or first 4 KiB root DBI aperture.
- A cold ownership predicate is required before asserting GPIO PERST or
  powering the fabric. It must not infer cold state from shared ANS/ancestor
  gates. Active/unknown N1 ownership is rejected, never silently reset.
- Hold and read back the validated GPIO PERST before accessibility setup.
  A failed GPIO operation cannot be followed by PHY setup or training.
- Split the existing Linux sequence after root-controller readiness and before
  its SMC N1 reset. Preserve the Linux entry's exact ordered hardware actions.
  The Windows prefix must not access port/ECAM/DART or release PERST.
- After the prefix, log each DART address before its guarded read and log
  completion afterward. Keep all geometry/protection/TCR/TTBR/busy checks.
  Readability at this new phase still needs physical qualification.
- Only a clean DART allows the finish phase to reset/train the endpoint.
  Never read endpoint config while the link is down. Validate all present
  functions' bus-master bits after link establishment before BAR programming.
- Before finish proves port/root readiness, failure cleanup uses only the
  validated GPIO. It cannot call the existing port/ECAM drain routine. Retain
  the reset clamp and return quarantined failure because link drain is unproved.
- After readiness, preserve the existing full drain requirement. Any failure
  leaves a zero descriptor and cannot authorize census reads or retry.
- A guarded write fault before readiness is not successful setup. Before
  marking the port ready or starting LTSSM, check that the finish phase's
  exception count has not changed; otherwise retain GPIO-only quarantine.
- Replay the production producer with inaccessible pre-prefix DART and
  pre-ready PORT/ECAM boundaries, and compare the actual Linux sequence before
  and after the split using the same deterministic MMIO/mailbox transcript.


# First DART read after Linux link setup (recorded before implementation)

The physical held-reset probe completed the PMGR and Linux PHY/root prefix,
then stalled on its first 32-bit DART read at 0x488000000 with SError. The prefix
alone is not a qualified DART accessibility boundary. The public Linux J813 N1
DTS at 3a0da31bcc027d4b0a28b0501e649522c7a232f6 instead requires the loader to
train the link before Linux owns the ordinary T8110 DART. It is not a tunneled
PCIe-C DART and must not use that driver's parameter-read workaround.

This supersedes the earlier tentative pre-finish DART boundary above:
- Preserve exact target, tunable and seven cold-gate admission before resets.
  Active, automatic, clock-retained, reset or unknown gates remain rejected
  before any power/reset change; never reset an admitted existing owner.
- Run the unchanged public Linux reset/port/refclk/link sequence before the
  first DART read. No DART access is permitted on failed or incomplete finish.
- Verify link and the identity and BME-clear command of every present endpoint
  before any DART access or BAR assignment. Linux setup does not write endpoint
  commands or load Control firmware; no endpoint bus-master enable is added.
- Preserve both complete clean-DART checks: after endpoint admission and again
  after BAR assignment. Reject active translation, protection, invalid geometry,
  busy commands and changes during assignment; never repair or clear them.
- A failed DART read must not be accepted as a parameter value or repeated.
  Return no descriptor, no census authority and no further DART access. Use
  the already-established port's existing reset/drain outcome, quarantining
  if it cannot be proven. This model cannot establish real SError recovery.
- BME on Control, Wi-Fi or Bluetooth must block all DART reads and assignment.
- The default Linux sequence must remain byte-for-byte identical in the retained
  production MMIO transcript. This change is default-disabled and unqualified
  until a new bounded physical run and recovery receipt are retained.

Before the production edit, strengthen the existing full-producer replay so
all DART reads require completed link setup and a prior endpoint-command check.
The old production sequence must fail this real phase-order regression. Extend
that same replay for other-function BME, first-DART fault and state changed at
BAR assignment; keep distinct identity, cold-owner and census coverage.
