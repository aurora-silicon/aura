The producer must withhold the descriptor on wrong chip, board, model, ADT mapping,
reset GPIO/function, or DART geometry before training. Any already-active DART,
busy TLB or endpoint bus master is an existing owner and must not be reset.
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
