# SPDX-License-Identifier: MIT
"""Recorder for outbound Windows serial events; never interprets guest bytes."""
import hashlib
from pathlib import Path
import struct


def escape_serial(data):
    """A terminal-safe ASCII view; raw bytes remain authoritative."""
    return "".join("\n" if byte == 10 else "\\\\" if byte == 92 else
                   chr(byte) if 32 <= byte <= 126 else f"\\x{byte:02x}" for byte in data)


class WindowsSerialRecorder:
    HEADER = struct.Struct("<HHIQHH")
    MAX_BATCH = 128
    FINAL = 1

    def __init__(self, directory):
        self.directory = Path(directory)
        self.raw = (self.directory / "guest-serial.bin").open("xb")
        try:
            self.text = (self.directory / "guest-serial.log").open("x", encoding="ascii", newline="\n")
        except BaseException:
            self.raw.close()
            raise
        self.digest = hashlib.sha256()
        self.bytes = self.events = self.dropped = self.next_sequence = 0
        self.final = self.closed = False
        self.issues = []

    def incomplete(self, reason):
        # Bound metadata even if a malformed or hostile stream keeps arriving.
        if reason not in self.issues and len(self.issues) < 32:
            self.issues.append(reason)

    def receive(self, data):
        try:
            if self.closed or len(data) < self.HEADER.size:
                raise ValueError("truncated serial event or closed recorder")
            abi, flags, sequence, dropped, length, reserved = self.HEADER.unpack_from(data)
            if abi != 1 or flags & ~self.FINAL or reserved or length > self.MAX_BATCH:
                raise ValueError("invalid serial event ABI")
            if len(data) != self.HEADER.size + length:
                raise ValueError("serial event length mismatch")
            if self.final:
                self.incomplete("event-after-final")
            if sequence != self.next_sequence:
                self.incomplete("sequence-gap-or-reorder")
            if dropped < self.dropped:
                self.incomplete("decreasing-drop-counter")
            self.next_sequence = (sequence + 1) & 0xffffffff
            self.dropped = max(self.dropped, dropped)
            payload = data[self.HEADER.size:]
            self.raw.write(payload)
            self.digest.update(payload)
            self.bytes += length
            self.raw.flush()
            # Escape control/invalid bytes rather than emitting terminal codes.
            self.text.write(escape_serial(payload))
            self.text.flush()
            self.events += 1
            self.final |= bool(flags & self.FINAL)
        except BaseException:
            self.incomplete("event-or-file-error")
            raise

    def summary(self):
        return {"mode": "capture", "raw": "guest-serial.bin", "readable": "guest-serial.log",
                "bytes_received": self.bytes, "events_received": self.events,
                "sha256": self.digest.hexdigest(), "target_dropped_bytes": self.dropped,
                "drop_count_final": self.final,
                "received_final": self.final, "issues": list(self.issues),
                "capture_complete": self.final and not self.dropped and not self.issues,
                "bidirectional_kd": False}

    def close(self):
        if self.closed:
            return
        try:
            self.raw.close()
            self.text.close()
        except BaseException:
            self.incomplete("file-close-error")
            raise
        finally:
            self.closed = True
