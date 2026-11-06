# First ID-register read during Mu entry

Failure model recorded before removing the legacy ID-read callback.

The base Mu hardware attempt in
`build/windows-hardware/mu-diagnostic-01/exception.txt` stopped at
`ELR=0x1001a069000` on CPU 6 with `ESR=0x6230000f`. This is a read of
`ID_AA64MMFR0_EL1` (`S3_0_C0_C7_0`) into x0. The register key after removing
the direction and target-register fields is `0x30000e`, exactly the existing
case in `hv_handle_msr_unlocked`. The preprocessed case calls
`hv_ring_id_doorbell()` and returns false before executing the register read.
The launcher therefore receives an exception despite the implemented handler.

The repair must avoid these failure modes:

- The first supported ID read on any CPU still exits to the host only to notify
  a debug checkpoint consumer that is absent from the public launch path.
- A fabricated constant or uninitialized x0 is returned instead of the actual
  register value.
- Removing the callback bypasses the existing DFR0 PMU-version mask or the PFR0
  GIC-interface advertisement.
- A successful first read hides a different failure later in Mu and is reported
  as a complete Windows boot.

Repeat the same base Mu launch with the repaired Aura candidate. Retain the
candidate hash, launch command, console log and any forwarded exception in a
new hardware-receipt directory. The focused pass condition is progression past
the captured MMFR0 read without that ID-read notification reaching Python.
Record any later stop separately; it does not invalidate this focused repair
or establish complete Mu/Windows qualification. Verify register values against
the host value if the hardware trace exposes the returned value.

Compile the production exception file for ARM64 and inspect the expanded ID
cases to ensure normal reads still execute MRS, DFR0 still applies its mask,
and PFR0 still applies its GIC bit. These are build/source checks, not hardware
proof. The existing physical failure and its replay are the E2E regression;
no source-pattern unit test duplicates that replay.

## Mu sysreg-assist readiness notification

The next replay, `build/windows-hardware/mu-diagnostic-02/launch/console.log`,
progressed to Mu timer readiness, then logged a repaired SP_EL0 on CPU 6.
Its next diagnostic reported a failed rendezvous waiting for CPU mask `0x40`,
which includes that same current CPU. This failure model was recorded before
removing the notification.

`hv_exc_exit()` had already unlocked `bhl` and marked its own CPU in-guest
before it sent `HV_SYSREG_ASSIST_READY`. The proxy/rendezvous path requires
that lock and a CPU already removed from the in-guest mask. The notification
also expected an old host debugger to patch guest vectors; the public launcher
does not implement that consumer.

Remove only that readiness notification and its one-shot state. Keep the
explicitly enabled T8142 SP repair and preserve genuine guest exceptions,
device requests, timer callbacks and user interrupts. Keep the event-number
slot intact to avoid changing the existing event ABI.

The physical replay must progress beyond the repaired-SP message without a
self-rendezvous or host debug notification from this path. Any later guest
exception remains a separate failure and must still be reported normally.
