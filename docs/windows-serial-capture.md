# Windows serial capture

## Failure model, before implementation

- A guest can emit any byte, including the proxy sync word, NUL, invalid UTF-8,
  terminal escapes, or KD packets. Preserve bytes inside framed events; never
  send guest bytes as unframed proxy console text or interpret them as commands.
- A missing or slow host must not block the guest or leave a partial event on
  the control connection. Only an explicitly supported, all-or-none,
  nonblocking carrier write may transmit a frame; ordinary blocking writes
  and partial nonblocking writes are not substitutes.
- Output can exceed both the bounded capture buffer and carrier capacity.
  Count discarded bytes, sequence successful events, and retain those facts
  in the artifact. A missing final event or transport failure cannot establish
  a complete capture, even when no received event reports a drop.
- Concurrent guest CPUs, newline flushes, periodic flushes, and final draining
  must preserve queued-byte order without allocating memory in the exception
  path. Every flush has a finite work bound.
- Capture is an explicit Windows-only profile/configuration capability. Old
  residents and unsupported carriers must reject opt-in before guest entry;
  default profiles and the normal Aura backend keep their existing behavior.
- Host decoding must reject invalid versions, lengths, flags and reserved
  fields. Malformed events, sequence gaps, decreasing counters, and file errors
  must never produce a claim of lossless or complete logging.
- Raw bytes and a safely escaped readable view must be written while the guest
  runs, with hashes and final status in repeatable artifacts. This outbound
  logging path does not provide bidirectional KD or transfer serial ownership.

The production-path replay test must compile the actual C capture and event
encoder, feed binary/newline/overflow/no-host cases through their bounded
carrier boundary, and pass emitted frames through the real Python proxy event
decoder and HV handler. It must retain frames, raw capture, readable capture,
source hashes, results, and its replay command. This is offline evidence;
physical KIS/Mu/Windows capture still requires hardware validation.

## Use and scope

Add `"serial": "capture"` to the hash-bound Windows launch profile, then use
the normal `proxyclient/tools/boot_windows.py boot` command with an explicit
resident device and a new output directory. The default is `disabled`.
The host checks capability bit 8 and configuration bit 2 before guest entry.
Configuration additionally rejects a carrier without an atomic write hook.
DockChannel/KIS currently supplies that hook. Other carriers can implement
the same strict contract without changing the event ABI; they must not fall
back to a blocking write or emit a partial frame.

While `hv_start` is pending, the existing proxy event reader dispatches each
event to the Windows HV handler. The recorder immediately flushes raw bytes
to `guest-serial.bin` and an ASCII view to `guest-serial.log`. LF remains a
newline, backslashes are doubled, and other control/non-ASCII bytes are shown
as `\xNN`. No guest byte is interpreted as a terminal command. Capture mode
suppresses the old `HVLOG: ` unframed passthrough. The separate USB VUART pipe
keeps its existing behavior; this change neither creates that pipe over KIS
nor adds input or a working Windows KD session.

Only bytes actually written by firmware or Windows to the mapped guest UART
can be captured. A Windows boot without a configured UART logging/debugging
producer may generate no Windows serial output. A firmware line is not proof
that the Windows kernel reached readiness.

## Wire contract

Event ID `0x80` uses the existing `REQ_EVENT` envelope and its negotiated
checksum mode. Its little-endian payload is `struct <HHIQHH>` followed by the
raw data:

| Offset | Field | Contract |
| --- | --- | --- |
| 0 | ABI, u16 | 1 |
| 2 | flags, u16 | bit 0 FINAL; other bits zero |
| 4 | sequence, u32 | Starts at zero; increments on each accepted complete frame |
| 8 | dropped, u64 | Cumulative target-buffer discarded bytes |
| 16 | length, u16 | 0–128 |
| 18 | reserved, u16 | Zero |
| 20 | data | Exactly `length` bytes |

The target has a fixed 4096-byte FIFO. A newline or 128 queued bytes attempts
one batch; the Windows host tick attempts another batch. A busy carrier keeps
queued bytes, and a full FIFO discards new bytes while incrementing the drop
counter. Each attempt transmits at most 160 bytes including both headers and
checksum, and never waits for the host to drain its receive side.

After guest CPUs stop, finish attempts at most 32 data batches, counts any
remaining buffered bytes as discarded, then attempts one empty FINAL event.
There is no final-event retry loop. Thus an absent/disconnected host can lose
the final status too. `target_dropped_bytes` is only the last received value;
`drop_count_final` states whether final status arrived. `capture_complete`
requires FINAL, no drops, and no sequence/protocol/launch errors. Even a
complete capture describes observed UART bytes, not OS or hardware support.

## Repeatable artifact

```sh
python3 tools/windows_serial_e2e.py --output /tmp/windows-serial-UNIQUE
python3 proxyclient/tools/windows_hv_e2e.py --out /tmp/windows-hv-UNIQUE
python3 proxyclient/tools/windows_launcher_e2e.py --out /tmp/windows-admission-UNIQUE
```

Use the proxyclient Python environment and a Clang supporting ASan/UBSan.
The serial test snapshots its source dependencies, compiles the actual VUART,
capture, event encoder and atomic I/O dispatch, and feeds the resulting frames
through the actual Python proxy parser and HV handler. It retains input bytes,
frames, captured output, compiler logs, source hashes, and `result.json` with a
replay command. FIFO availability and locks are modeled; the artifact does
not replace physical KIS/CDC, interrupt, Mu or Windows qualification.
