#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay full Windows launch with a dedicated, asynchronous CDC reader.

Failure model: docs/windows-serial-capture.md. Only primary target operations
and serial device I/O are modeled; admission, threading, artifact writes,
cleanup and receipts run through production code.
"""
import hashlib
import json
from pathlib import Path
import queue
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "proxyclient"))
from aura.windows import CAPS_MAGIC, REQUIRED_CAPS, HardwareSession, launch_profile, source_manifest
from aura.windows_serial import escape_serial


def wait_for(predicate, timeout=2):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("modeled endpoint did not make progress")
        time.sleep(0.002)


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / "mu.bin"
    image.write_bytes(bytes(range(256)))
    base_profile = {"schema": "aura.windows.boot.v1", "name": "cdc-replay",
                    "target": {"chip_id": "0x8142", "model": "Mac17,3"},
                    "mu": {"path": image.name, "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
                           "entry_offset": 0}, "serial": "cdc"}
    report = {"result": "failed", "hardware_accessed": False, "hardware_qualified": False,
              "host_sources": source_manifest(), "cases": []}
    try:
        for case in ("old-resident-error", "missing-config-error", "success", "empty", "guest-error", "prepare-error", "open-error",
                     "initial-read-error", "running-read-error", "cancel-blocked-reader", "wrong-carrier-error"):
            trace, ports = [], []
            folder = out / case
            profile = out / (case + ".json")
            profile.write_text(json.dumps(base_profile))
            binary = bytes(range(256)) * 256 + b"\xff\x55\xaa\x05\0\r\nutf8:\xe2\x82\xac\\end"

            class Port:
                def __init__(self, device, baud, **kwargs):
                    trace.append("open-guest")
                    assert trace[-2] == "carrier" and device == "guest-cdc" and baud == 115200
                    assert kwargs["timeout"] == 0.1
                    if case == "open-error": raise OSError("injected guest open failure")
                    self.queue, self.closed, self.dtr = queue.Queue(), False, False
                    self.reads, self.cancelled = 0, False
                    ports.append(self)
                def read(self, size):
                    assert self.dtr and size == 16384
                    self.reads += 1
                    if case == "initial-read-error": raise OSError("injected initial disconnect")
                    if case == "cancel-blocked-reader" and self.reads > 1:
                        wait_for(lambda: self.cancelled or self.closed, timeout=2.5)
                        return b""
                    try: value = self.queue.get(timeout=0.01)
                    except queue.Empty: return b""
                    if isinstance(value, Exception): raise value
                    return value
                def cancel_read(self):
                    trace.append("cancel-read")
                    self.cancelled = True
                def close(self):
                    self.closed = True
                    trace.append("close-guest")

            class Session:
                def __init__(self, device, log):
                    assert device == "primary-cdc"
                    trace.append("open-primary")
                    self.proxy = SimpleNamespace(iodev_whoami=self.carrier)
                def carrier(self):
                    trace.append("carrier")
                    return 1 if case == "wrong-carrier-error" else 4
                validate_serial_transport = HardwareSession.validate_serial_transport
                def capabilities(self):
                    trace.append("capabilities")
                    flags = REQUIRED_CAPS | 0x40 | 0x200
                    if case == "old-resident-error": flags &= ~0x200
                    if case == "missing-config-error": flags &= ~0x40
                    return CAPS_MAGIC | (1 << 16) | flags
                def identity(self):
                    trace.append("identity")
                    return {"chip_id": 0x8142, "model": "Mac17,3"}
                def prepare(self, plan):
                    trace.append("prepare")
                    assert plan.serial == "cdc" and ports[0].dtr and ports[0].reads
                    if case == "prepare-error": raise RuntimeError("injected preparation error")
                def load(self, plan):
                    trace.append("load")
                    return {"modeled": True}
                def start(self):
                    trace.append("start")
                    if case not in ("empty", "cancel-blocked-reader"):
                        for offset in range(0, len(binary), 509):
                            ports[0].queue.put(binary[offset:offset + 509])
                        wait_for(lambda: (folder / "guest-serial.bin").stat().st_size == len(binary))
                        wait_for(lambda: (folder / "guest-serial.log").read_text() == escape_serial(binary))
                        # Receipt still says guest-running: output is available
                        # before start returns, not reconstructed afterward.
                        assert json.loads((folder / "launch.json").read_text())["result"] == "guest-running"
                        trace.append("drained-while-running")
                    if case == "running-read-error":
                        ports[0].queue.put(OSError("injected running disconnect"))
                        wait_for(lambda: ports[0].closed)
                    if case == "guest-error": raise RuntimeError("injected guest failure")
                    if case == "cancel-blocked-reader": wait_for(lambda: ports[0].reads > 1)
                    return 1
                def close(self): trace.append("close-primary")

            expect_failure = case.endswith("error")
            start = time.monotonic()
            with patch("serial.Serial", Port):
                try:
                    launch_profile(profile, "primary-cdc", folder, Session,
                                   guest_serial_device="guest-cdc")
                    assert not expect_failure
                except (OSError, RuntimeError, ValueError):
                    assert expect_failure
            elapsed = time.monotonic() - start
            assert elapsed < 4 and trace[-1] == "close-primary"
            assert all(port.closed for port in ports)
            receipt = json.loads((folder / "launch.json").read_text())
            assert receipt["serial"]["mode"] == "cdc"
            assert not receipt["windows_ready"] and not receipt["hardware_qualified"]
            if case in ("old-resident-error", "missing-config-error"):
                assert trace == ["open-primary", "capabilities", "close-primary"] and not ports
            if case in ("old-resident-error", "missing-config-error", "open-error", "initial-read-error", "wrong-carrier-error"):
                assert not receipt["target_mutated"] and "prepare" not in trace
            if case not in ("old-resident-error", "missing-config-error", "open-error", "wrong-carrier-error"):
                capture = receipt["serial"]
                assert capture["reader_stopped"] and not capture["capture_complete"]
                assert capture["target_dropped_bytes"] is None and not capture["received_final"]
                data = (folder / "guest-serial.bin").read_bytes()
                assert capture["bytes_received"] == len(data)
                assert capture["sha256"] == hashlib.sha256(data).hexdigest()
                if case in ("initial-read-error", "running-read-error"): assert capture["issues"]
                if case == "cancel-blocked-reader": assert "cancel-read" in trace
            report["cases"].append({"case": case, "trace": trace, "elapsed": elapsed,
                                     "serial": receipt["serial"]})

        # No endpoint may open before profile/port-pair admission, including
        # aliases resolving to the same underlying pathname.
        primary = out / "primary"
        primary.touch()
        alias = out / "alias"
        alias.symlink_to(primary)
        for case, mode, guest in (("missing-guest", "cdc", None),
                                 ("same-port", "cdc", str(primary)),
                                 ("same-alias", "cdc", str(alias)),
                                 ("unexpected-guest", "disabled", "guest-cdc")):
            profile = out / (case + ".json")
            profile.write_text(json.dumps({**base_profile, "serial": mode}))
            with patch("serial.Serial", side_effect=AssertionError("opened invalid guest port")):
                try:
                    launch_profile(profile, str(primary), out / case,
                                   lambda *args: (_ for _ in ()).throw(AssertionError("opened invalid primary")),
                                   guest_serial_device=guest)
                except ValueError:
                    pass
                else:
                    raise AssertionError(case + " passed")
            report["cases"].append({"case": case, "rejected_before_open": True})
        cli = subprocess.run([sys.executable, str(ROOT / "proxyclient/tools/boot_windows.py"),
                              "boot", "--profile", str(out / "same-port.json"),
                              "--device", str(primary), "--guest-serial-device", str(alias),
                              "--out", str(out / "cli-rejected")], capture_output=True, text=True)
        assert cli.returncode and "guest serial device must differ" in cli.stderr
        (out / "cli.log").write_text(cli.stdout + cli.stderr)
        report["cases"].append({"case": "real-cli-port-admission"})
        report["e2e_source_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        report["replay"] = [sys.executable, str(Path(__file__).resolve()), "--out", "NEW_EMPTY_DIRECTORY"]
        report["result"] = "passed"
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(report['cases'])} dedicated CDC launch cases: {out / 'result.json'}")


if __name__ == "__main__":
    main()
