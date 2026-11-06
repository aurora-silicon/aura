# Physical CDC vUART smoke test

## Failure model recorded before implementation

- Opening an unrelated CDC port can disrupt another machine. Require explicit
  primary and guest paths, match their live macOS USB device ancestor and CDC
  data interfaces (1 and 3), then validate the primary's chip/model. Do not
  discover a target by trying ports or by a hardcoded serial number.
- A guessed UART address could reach real hardware. Derive UART0 from the
  identity ADT, require the Windows loader's mapped UART to match, and put that
  address only in the bounded image's input/result block. Never host-write MMIO.
- Pausing user-space reads does not immediately stop USB/kernel draining.
  The guest must actually observe TX-not-ready; reaching a byte/time limit
  without that observation fails qualification rather than claiming pressure.
- A momentarily full queue may drain on the next UART access. Before probing
  full writes, require a continuous 200-ms full interval, restarting the interval
  and refilling if readiness returns within the same 45-second/eight-MiB budget.
  Later progress can still admit probe bytes legitimately; classify that run as
  inconclusive, not as a backend failure or a pass.
- Transfer a literal `HVLOG: ` prefix, the run nonce, proxy-framing bytes and
  newlines. Require exact secondary delivery and no nonce-bearing isolation
  marker on the primary console, so the physical run exercises suppression.
- An old blocking writer may hang on a full FIFO. Time four deliberate writes
  while full, require their completion before the host resumes, and verify none
  appears in the subsequently drained byte stream. The full-wait loop must
  continue making observable progress in the guest result block.
- Closed DTR disables guest RX. Use a nonce command/ACK followed by a bounded
  timed window: guest waits two seconds, finishes discard writes within the
  next second, host closes within one second of sending the command, and host
  reopens at least four seconds after receiving the ACK. Verify all bounds.
- Partial reads, stale bytes, dropped/duplicated data, a wrong nonce, unexpected
  stage markers, early return, and timeouts must fail. Bound guest loops, host
  reads, fill bytes and total wall time. Save partial results on failure.
- Final-marker review: bytes prefetched beyond DONE or arriving after it must
  not be silently ignored. After verified SYSTEM_OFF/drain, inspect the saved
  receive buffer and perform a bounded final CDC read, retaining fresh bytes
  in the RX artifact and rejecting any unexpected tail.
- Keep results and the guest stack inside its allocated image. Use virtual
  PSCI SYSTEM_OFF and require return status 1, result readback, and a responsive
  primary proxy. No host scratch addresses, target disk writes or physical reset.

This is a host/guest protocol test, not Windows KD qualification. A build-only
receipt proves only reproducible assembly and image structure. A hardware pass
must include exact TX/RX streams, nonce/result bytes, timing bounds, primary
identity and USB binding, hashes and a final proxy response.

## Failure capture model, recorded before the diagnostic follow-up

A failed host protocol can leave the primary thread inside `hv_start` until
the hard timeout. The guest's 220-second fallback is not sufficient evidence
of a completed drain: guest counter scaling or a stuck EL2 operation can keep
that fallback from executing. The follow-up runner must retain these boundaries:

- Do not interrupt the intentional 60-second reader pause or a normal run.
- After 230 seconds of guest execution, send one supported HV interrupt byte,
  leaving the existing 250-second overall hard deadline in force.
- Capture exact exception bytes, CPU, PC, ESR, FAR, registers and the reserved
  guest result page inside the callback, before acknowledging EXIT_GUEST.
- Capture ordinary faults immediately with their actual reason. A scheduled
  diagnostic interrupt must not reclassify an earlier fault or allow a pass.
- A failed snapshot must still acknowledge the abort, and must be recorded.
- Record the raw P_HV_START reply before its error status raises in the proxy.
  A negative status has no implied signed return payload; preserve both fields.
- Only a received start reply establishes return to the host. If the interrupt
  cannot reach a guest stuck in EL2, the hard timeout leaves its state unknown.

The offline replay models the transport/callback boundary and advances a fake
clock through normal, diagnostic, fault and capture-error paths. It retains
the production capture helper, event ordering and generated artifacts; it does
not establish physical interrupt delivery or counter frequency.

## Build and explicit run

```sh
python3 tools/windows_vuart_smoke.py --chip-id 0x8142 --model Mac17,3 \
  --output build/vuart-smoke-build

python3 tools/windows_vuart_smoke.py --run \
  --device /dev/cu.EXPLICIT_PRIMARY --guest-device /dev/cu.EXPLICIT_GUEST \
  --chip-id 0x8142 --model Mac17,3 --output build/vuart-smoke-hardware
```

Use the project's Python dependencies and LLVM tools. Each output directory
must be new. The default invocation builds twice, compares the raw templates,
and records ELF symbols/disassembly and hashes. Its UART input remains zero;
only hardware mode obtains and inserts the validated live ADT address. The
physical runner currently supports macOS, where the two explicit serial paths
are checked against IORegistry before either is opened.

Start with an idle, cold Windows Aura resident over CDC and no other reader of
the guest port. The test uses native framebuffer, disabled guest USB and N1,
and no experimental guest compatibility. The 60-second reader pause is
intentional. The guest must sustain a full queue for 200 ms within 45 seconds
and eight MiB, refilling and restarting that interval if readiness returns;
otherwise this run fails to qualify backpressure. This may expose host CDC
buffering differences rather than a target defect.

The host sends five commands, each with the run nonce. It verifies two complete
0..255 byte sequences, a nonce-bearing literal `HVLOG: ` binary/newline payload,
exact stage markers, and every accepted filler byte. It checks that the
isolation payload's marker did not reach the primary console. A pass requires
the four deliberate full-write probe bytes and 1024 closed-DTR bytes to be
absent from the received stream. If later host progress legitimately admits
probe bytes, or readiness returns before the post-probe wait loop, the tool
drains and exits cleanly but records `inconclusive` (exit code 2). Unexpected
bytes or other contract violations remain failures. After the final received marker, the host acknowledges
completion and keeps the port open until the guest has returned via SYSTEM_OFF.

Guest timing uses `CNTPCT_EL0` and its own `CNTFRQ_EL0`; host timing uses
`time.monotonic()`. Their absolute values are never compared. The closure proof
uses ordering and conservative intervals: the target timestamps before sending
its ACK, the host closes within one second of initiating that command, target
writes occur 2..3 seconds after the timestamp, and reopening occurs no sooner
than four seconds after the ACK was received. Missed timing bounds fail the run.

`receipt.json`, `host-protocol.json`, the exact RX/TX streams and before/after
result blocks form the evidence. A 230-second diagnostic interrupt additionally
saves `diagnostic-events.json`, exact `exception-NN.bin` snapshots and
`result-at-exception-NN.bin` before requesting abort. The raw start reply is
saved separately; it proves host return independently of an error raised by
the Python proxy. Any diagnostic interrupt or earlier exception fails the run.
If initialization consumes the available time, the 250-second overall limit
wins without an early diagnostic interrupt. An unanswered interrupt or hard
timeout leaves guest state unqualified;
the tool does not reboot, reset or silently retry the machine. Local backend
source hashes do not establish which resident binary was loaded. Record that
binary separately when qualifying a candidate.
