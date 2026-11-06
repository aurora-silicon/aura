# Windows CDC UART backpressure

## Failure model, recorded before implementation

- An unbound CDC UART or a host with DTR closed must not stop guest boot. It
  continues to advertise transmit readiness and discards output, as before.
- An attached host which stops draining its buffer must not trap EL2 in the
  USB2 writer's unbounded retry loop. A UART write may copy once, never wait for
  host progress, and never call the normal blocking USB writer.
- A full attached queue must clear UART transmit-ready status and set FIFO-full
  status. A guest that ignores this status must increment an overrun counter;
  it must not overwrite queued bytes or partially enqueue a request.
- The ring buffer reserves one slot: its `get_free()` includes that slot.
  Advertising it as writable would silently lose the last byte.
- Queue wraparound, one remaining slot, host progress and subsequent reconnect
  must preserve byte order and restore readiness. Endpoint work is deferred to
  polling, including the final queued byte if no further guest writes occur.
- DTR closure or a failed/disconnected controller must restore discard/ready
  behavior instead of leaving a guest spinning on a dead debug connection.
- Primary CDC proxy writes, the ordinary non-Windows backend, receive traffic,
  and Windows framed capture must retain their existing behavior. Capture is
  independent of whether the raw CDC UART accepts or drops a byte.
- Follow-up audit before the poll fix: when the debug UART reports ready while
  unbound, the generic I/O event handler can flush pending output on unrelated
  consoles through their blocking write fallback. Windows UART polling must
  service the debug controller without triggering that global console flush.

The offline replay must use the production bounded queue implementation, real
ring buffer, I/O dispatch and UART MMIO handler. Only the physical controller's
event/transfer completion and the host reader are modeled. Save source hashes,
commands, output bytes and a receipt. Physical disconnect/reconnect and Windows
KD transport still require hardware qualification.

## Receive rearm failure model, recorded before the follow-up fix

The physical CDC smoke received its close acknowledgement, reopened the port,
and then waited for the guest to receive the next command. In the controller,
`XFERNOTREADY` calls the bulk-OUT starter, but the starter returns when DTR is
closed. Opening DTR only sets `ready`; it does not replay that event. The UART
checks `can_read` before calling `read`, so an empty ring cannot reach the
existing receive-start operation inside `read`. Polling only bulk IN therefore
leaves an idle OUT endpoint waiting for an event which need not recur.
The same hardware run also observed unexpected closed-window payload bytes;
that separate timing/queue observation is not explained or qualified by this
receive-side model.

Independent review identified a second boundary before the follow-up capacity
change: `ringbuffer_get_free()` includes the reserved empty/full slot. A bulk
OUT transfer of `XFER_SIZE` bytes therefore requires strictly more than
`XFER_SIZE` reported free slots. The replay must reject a start with exactly
that count, allow it after one additional byte is drained, then complete a
full-size transfer and verify every byte. Otherwise a maximum-size host packet
can lose its last byte despite the endpoint having passed its admission check.

The follow-up replay must consume a not-ready event while DTR is closed,
reopen without another event, and deliver a host byte through the production
OUT starter, ring, I/O dispatch and UART register read. Before the fix it must
reproduce empty RX without starting a transfer. Afterward it must also prove:

- Repeated polling never starts a duplicate active transfer.
- Closed DTR, a failed controller, an unavailable TRB, a missing receive ring,
  or insufficient receive-ring space cannot start a transfer.
- Reading enough queued data restores receive progress without a new event.
- Only secondary CDC OUT is primed; TX order, backpressure, discard behavior,
  and the primary proxy remain unchanged.

The new poll operation may try the existing bounded endpoint starter once; it
must not allocate, wait for host data, consume input, or call the blocking
reader. The fixture runs the production OUT starter and reader for both HS
and SS compile modes, while modeling USB event delivery, DMA completion and
endpoint command submission. Physical reopen recovery still requires rerunning
the hardware smoke; this model does not qualify controller behavior.

## Implementation

Only the Windows build selects the bounded secondary CDC callbacks. Primary
proxy CDC operations and the default backend keep their original callbacks.
The secondary writer enqueues the entire request once or returns zero when
there is insufficient space; it does no event processing, endpoint commands,
or host-progress waiting. The UART status path and periodic UART polling
service the controller, start queued IN transfers, and try the secondary OUT
starter once even when the receive ring is empty. The OUT helper preserves its
existing active-transfer, DTR and receive-capacity guards. These controller
operations still have their normal bounded hardware polling.
Windows UART polling holds the secondary I/O lock and invokes the debug
controller poll directly; it does not use the global console-flushing event
dispatcher.

An absent device, a failed controller, or closed DTR consumes and discards
output, while reporting ready. USB2 disconnect clears the secondary ready flag
without changing primary proxy state or freeing active DMA storage. USB3 keeps
its existing disconnect handling. A full attached queue clears `UTRSTAT` TX
readiness and sets `UFSTAT` TX-full. Guest writes made anyway increment the
saturating `vuart_tx_overruns` ELF symbol; framed-capture loss accounting remains
separate. Mapping the UART resets this counter.

## Repeatable offline replay

```sh
python3 tools/windows_vuart_e2e.py --output build/windows-vuart-replay
python3 tools/windows_serial_e2e.py --output build/windows-serial-replay
```

Use a new output directory for each run. The first runner saves a source
snapshot, dependency and compiler commands, ASan/UBSan output, executable hash,
the exact `host-bytes.bin` (HS) and `host-bytes-ss.bin` streams, extracted
`production_rx.inc`, and `result.json`. Its sequence exercises
unbound/DTR-closed discard, an attached stalled host, the reserved ring slot,
all-or-nothing rejection, ignored-full overrun accounting, wraparound, resumed
drain, a final byte drained only by polling, controller failure, and receive
rearm after a lost closed-DTR not-ready event. Capture
receives every guest write independently. The separate serial replay covers
the production framed encoder and atomic transport.

The USB controller event and completion stubs model host progress; the native
replay does not execute DWC3 MMIO or the wrapper objects in `usb.c` and
`usb_dwc3.c`. Compile the changed USB and UART sources for ARM64 as an additional
integration check. Neither the native replay nor compilation establishes
physical disconnect/reconnect, bidirectional Windows KD, or Windows boot
qualification. Those require a target-bound hardware receipt before merge.
