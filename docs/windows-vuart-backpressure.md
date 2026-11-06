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

## Implementation

Only the Windows build selects the bounded secondary CDC callbacks. Primary
proxy CDC operations and the default backend keep their original callbacks.
The secondary writer enqueues the entire request once or returns zero when
there is insufficient space; it does no event processing, endpoint commands,
or host-progress waiting. The UART status path and periodic UART polling
service the controller and start queued transfers. These existing controller
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
the exact `host-bytes.bin` stream, and `result.json`. Its sequence exercises
unbound/DTR-closed discard, an attached stalled host, the reserved ring slot,
all-or-nothing rejection, ignored-full overrun accounting, wraparound, resumed
drain, a final byte drained only by polling, and controller failure. Capture
receives every guest write independently. The separate serial replay covers
the production framed encoder and atomic transport.

The USB controller event and completion stubs model host progress; the native
replay does not execute DWC3 MMIO or the wrapper objects in `usb.c` and
`usb_dwc3.c`. Compile the changed USB and UART sources for ARM64 as an additional
integration check. Neither the native replay nor compilation establishes
physical disconnect/reconnect, bidirectional Windows KD, or Windows boot
qualification. Those require a target-bound hardware receipt before merge.
