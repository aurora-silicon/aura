# SPDX-License-Identifier: MIT
"""Continuous, outbound-only logging from an explicit dedicated CDC port."""
import hashlib
from pathlib import Path
import threading

from .windows_serial import escape_serial


class CdcSerialRecorder:
    def __init__(self, directory, device):
        import serial
        self.directory = Path(directory)
        self.device = device
        self.stop = threading.Event()
        self.finished = threading.Event()
        self.first_read = threading.Event()
        self.issues = []
        self.state = {"bytes_received": 0, "chunks_received": 0,
                      "sha256": hashlib.sha256().hexdigest()}
        self.dev = self.raw = self.text = self.thread = None
        try:
            self.raw = (self.directory / "guest-serial.bin").open("xb")
            self.text = (self.directory / "guest-serial.log").open("x", encoding="ascii", newline="\n")
            self.dev = serial.Serial(device, 115200, timeout=0.1, write_timeout=1)
            self.dev.dtr = True
            self.thread = threading.Thread(target=self._read, name="aura-guest-cdc", daemon=True)
            self.thread.start()
        except BaseException:
            for resource in (self.dev, self.raw, self.text):
                if resource is not None:
                    try:
                        resource.close()
                    except Exception:
                        pass
            raise

    def _issue(self, phase, error):
        if len(self.issues) < 16:
            self.issues.append(phase + ": " + type(error).__name__ + ": " + str(error)[:512])

    def _read(self):
        digest = hashlib.sha256()
        count = chunks = 0
        try:
            while not self.stop.is_set():
                data = self.dev.read(16384)
                self.first_read.set()
                if not data:
                    continue
                self.raw.write(data)
                self.raw.flush()
                digest.update(data)
                count += len(data)
                chunks += 1
                # Publish an immutable snapshot; receipt reads cannot race a
                # digest update or observe mismatched count/hash fields.
                self.state = {"bytes_received": count, "chunks_received": chunks,
                              "sha256": digest.hexdigest()}
                self.text.write(escape_serial(data))
                self.text.flush()
        except Exception as error:
            if not self.stop.is_set():
                self._issue("read-or-file", error)
        finally:
            for name, resource in (("port", self.dev), ("raw", self.raw), ("readable", self.text)):
                try:
                    resource.close()
                except Exception as error:
                    self._issue("close-" + name, error)
            self.finished.set()
            self.first_read.set()

    def wait_ready(self):
        if not self.first_read.wait(1):
            raise RuntimeError("dedicated CDC reader did not complete its initial timed read")
        self.check()

    def check(self):
        if self.issues or (self.finished.is_set() and not self.stop.is_set()):
            raise RuntimeError("dedicated CDC reader failed: " + "; ".join(self.issues))

    def close(self):
        self.stop.set()
        if self.thread is None:
            return
        self.thread.join(1)
        if self.thread.is_alive():
            try:
                cancel = getattr(self.dev, "cancel_read", None)
                if cancel is not None:
                    cancel()
            except Exception as error:
                self._issue("cancel-read", error)
            self.thread.join(1)
        if self.thread.is_alive():
            try:
                self.dev.close()
            except Exception as error:
                self._issue("force-close-port", error)
            self.thread.join(1)
        if self.thread.is_alive():
            self._issue("cleanup", RuntimeError("reader did not stop within three bounded joins"))

    def summary(self):
        return {"mode": "cdc", "device": self.device,
                "raw": "guest-serial.bin", "readable": "guest-serial.log", **self.state,
                "reader_stopped": self.finished.is_set(), "issues": list(self.issues),
                "target_dropped_bytes": None, "drop_count_final": False,
                "received_final": False, "capture_complete": False,
                "completeness": "unavailable-on-raw-cdc", "bidirectional_kd": False}
