# Windows exception abort and CPU drain

## Failure model recorded before the fix

An unhandled exception on an application CPU previously returned
`EXC_EXIT_GUEST` to the Windows exception proxy, rendezvoused the other CPUs,
then exited only that CPU. A rendezvous pauses peers; it does not request
their exit. The boot CPU kept running, so the normal `hv_start()` join path
did not run until a later user interrupt. That second event also overwrote
the first exception in Python.

Required behavior and failures to test:

- An application-CPU fault must request exit on every CPU, including a
  nonzero boot CPU and any setup-pending CPU, before releasing the exception
  lock. No peer may resume normal guest work after observing that request.
- The current CPU must leave through the existing rendezvous/unlock/exit
  sequence; do not recursively acquire the already-held big hypervisor lock.
- Returning the boot CPU must execute the existing watchdog stop and complete
  join path. A failed join remains a failure and retains pending state.
- Abort must return failure, never a stale SYSTEM_OFF/RESET success. A
  simultaneous or later voluntary exit cannot overwrite an abort result.
- Ordinary SYSTEM_OFF/RESET must retain results 1/2 and the same all-CPU drain.
- Python must retain the first abort reason, including if a later user
  interrupt arrives, and still send EXIT_GUEST for every forwarded event.
- Diagnostic consumers needing exact exception bytes must capture them before
  returning to firmware: an exited CPU's stack may immediately overwrite its
  saved context. This change retains the first reason, without new memory reads.

The replay compiles the production exit-request, exception-proxy, CPU-exit
checks and join functions. CPU scheduling, architectural exits and transport
are modeled. It executes the production boot-CPU return tail and Python abort
method, records ordered lifecycle events, and retains source hashes and logs.
It does not establish physical interrupt delivery or a hardware drain.

The MTP integration adds a device-stop boundary after the join. Before
extending the replay, its failure model is: a device stop must never run while
a guest or setup-pending CPU remains live, and a failed stop must quarantine
the resident and return failure even after a voluntary SYSTEM_OFF. A failed
CPU join or watchdog stop must not attempt MTP shutdown. The replay models the
device stop result while executing the actual boot-CPU return tail.

## Physical AP-fault smoke failure model

The separate `windows_abort_smoke.py` runner must choose a waiting AP from the
validated live ADT and prove it differs from the running boot CPU. The guest
starts that AP through PSCI, then the AP waits until it observes the boot CPU's
heartbeat before reading a deliberately unmapped IPA. The runner checks the
stage-2 mapping is absent before entry; it never points the fault at a device.

ADT `reg` omits Aff2=1 on performance cores. CPU selection must include the
validated E/P cluster type when forming the PSCI affinity and checking the
guest MPIDR; comparing a performance core directly with `reg` rejects a valid
run. Unknown cluster types must fail admission.

The first exception bytes must be saved inside the callback before EXIT_GUEST,
not recovered from the reused exception stack afterward. The faulting CPU,
PC, FAR, nonce and result state must match the prepared guest. The P_HV_START wire reply must carry S_ERROR with its zero-initialized return
value, because the public proxy maps a negative hv_start() result to status
rather than copying it into retval. The completed CPU-join log and a responsive
proxy are also required. A harness that instead requires retval == -1 would
reject a correctly drained abort before collecting its final result page. The boot CPU has a ten-second guest deadline and returns SYSTEM_OFF
with a failure marker if the abort incorrectly leaves it running. That path
must fail qualification, as must a host timeout, an unexpected exception,
successful return, missing heartbeat, or any continuation past the fault.

This test does not reset or install anything. It requires an explicit resident
device and writes a new receipt directory. Build-only is the default, and its
receipt does not qualify hardware behavior.

Build the freestanding AArch64 guest twice and preserve its disassembly,
symbols, source snapshots and hashes without opening a device:

```sh
python3 tools/windows_abort_smoke.py --chip-id 0x8142 --model Mac17,3 \
  --output build/windows-abort-smoke-build
```

On a freshly prepared resident supporting the Windows backend, the hardware
operator supplies the exact proxy device explicitly:

```sh
python3 tools/windows_abort_smoke.py --chip-id 0x8142 --model Mac17,3 \
  --run --device /dev/cu.EXPLICIT_DEVICE --output build/windows-abort-smoke-run
```

Each output directory must be new. The receipt binds the profile, nonce, image,
live CPU selection, first exception bytes, raw failure reply, result readback
and console logs. Local source hashes are not an attestation of the resident
binary; retain the separate resident build/load receipt with the test result.
The guest disables USB, N1 and MTP handoffs and uses native framebuffer format.
It does not boot Mu or Windows. A capture failure still acknowledges the abort
but cannot produce a passing receipt. A timeout leaves guest state unqualified;
the runner closes its transport and does not attempt recovery or reset.
