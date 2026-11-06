#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay production Windows VUART/event C through the real Python event reader."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "hardware_qualified": False,
              "scope": "actual C VUART, capture, framing and atomic dispatcher; real Python event decoder/HV recorder",
              "limitations": ["carrier FIFO and locks are modeled", "no physical KIS, Mu or Windows execution"],
              "cases": [], "sources": {}}
    try:
        snapshot = out / "source"
        deps = subprocess.run([args.cc, "-MM", "-Isrc", "tests/e2e/windows_serial_host.c"],
                              cwd=ROOT, capture_output=True, text=True, check=True).stdout
        dependency_paths = [(ROOT / x).resolve() for x in deps.replace("\\\n", " ").split(":", 1)[1].split()]
        names = {str(path.relative_to(ROOT)) for path in dependency_paths if path.is_relative_to(ROOT)}
        names.update(str(p.relative_to(ROOT)) for p in (ROOT / "proxyclient/aura").rglob("*")
                     if p.is_file() and "__pycache__" not in p.parts)
        names.update(("tools/windows_serial_e2e.py", "docs/windows-serial-capture.md",
                      "proxyclient/tools/boot_windows.py"))
        for name in sorted(names):
            dest = snapshot / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, dest)
            report["sources"][name] = digest(dest)
        binary = out / "serial-host"
        command = [args.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-missing-field-initializers",
                   "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                   "-Isrc", "tests/e2e/windows_serial_host.c",
                   "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                   "-o", str(binary)]
        compiled = subprocess.run(command, cwd=snapshot, capture_output=True, text=True)
        (out / "compile.log").write_text(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        report["compile"] = command
        report["binary_sha256"] = digest(binary)
        sys.path.insert(0, str(snapshot / "proxyclient"))
        from aura.proxy import UartInterface, Feature, EVENT
        from aura.hv import HV
        from aura.windows_serial import WindowsSerialRecorder
        from aura.windows import validate_capabilities, launch_profile, HardwareSession
        import struct

        class Stream:
            def __init__(self, data): self.data = io.BytesIO(data)
            def read(self, count): return self.data.read(count)
            def flushOutput(self): pass
            def flushInput(self): pass
            def close(self): pass

        def decode(folder, stream, sentinel=False):
            recorder = WindowsSerialRecorder(folder)
            hv = HV.__new__(HV)
            hv.windows, hv.windows_serial, hv.windows_serial_recorder = True, "capture", recorder
            iface = UartInterface(Stream(b""))
            if sentinel: iface.enabled_features = Feature.DISABLE_DATA_CSUMS
            reply = struct.pack("<Ii24s", iface.REQ_PROXY, 0, bytes(24))
            reply += struct.pack("<I", iface.checksum(reply))
            iface.dev.data = io.BytesIO(stream + reply)
            iface.set_event_handler(EVENT.WINDOWS_SERIAL, hv.handle_windows_serial)
            # Any unframed byte from guest output is a failure, not console text.
            iface.unkhandler = lambda byte: (_ for _ in ()).throw(AssertionError("unframed byte: " + repr(byte)))
            try:
                assert iface.reply(iface.REQ_PROXY) == bytes(24)
            except BaseException:
                recorder.incomplete("transport-error")
                raise
            finally:
                recorder.close()
            return recorder.summary()

        binary_data = b"HVLOG: " + bytes(range(256)) + b"\xff\x55\xaa\x05\0\r\nTAIL"
        cases = {"binary": binary_data, "sentinel": binary_data,
                 "newline": b"first\nsecond\n", "tick": b"unterminated",
                 "overflow": b"Z" * 5000, "no-host": b"N" * 5000,
                 "final-blocked": b"P" * 128, "disabled": b"ordinary\n",
                 "unsupported": b"X" * 256}
        for name, data in cases.items():
            folder = out / name
            folder.mkdir()
            (folder / "input.bin").write_bytes(data)
            run = subprocess.run([str(binary), name, str(folder / "input.bin"), str(folder / "frames.bin")],
                                 capture_output=True, text=True, timeout=10)
            (folder / "native.log").write_text(run.stdout + run.stderr)
            run.check_returncode()
            native = json.loads(run.stdout)
            wire = (folder / "frames.bin").read_bytes()
            summary = decode(folder, wire, name == "sentinel")
            expected = data[:4096] if name == "overflow" else b"" if name in ("no-host", "disabled", "unsupported") else data
            assert (folder / "guest-serial.bin").read_bytes() == expected, name
            readable = (folder / "guest-serial.log").read_text()
            assert all(byte == 10 or 32 <= byte <= 126 for byte in readable.encode("ascii"))
            if name == "binary": assert "\\x00" in readable and "\\x1b" in readable
            assert summary["sha256"] == hashlib.sha256(expected).hexdigest()
            assert summary["capture_complete"] == (name in ("binary", "sentinel", "newline", "tick"))
            if name == "overflow": assert summary["target_dropped_bytes"] == 904 and summary["received_final"]
            if name in ("no-host", "final-blocked"): assert not summary["received_final"]
            if name == "no-host": assert native["dropped"] == 5000 and wire == b""
            if name in ("disabled", "unsupported"): assert native["atomic_calls"] == 0
            report["cases"].append({"case": name, "native": native, "capture": summary,
                                     "frames_sha256": digest(folder / "frames.bin")})

        # Corrupt metadata with a valid outer checksum: the real handler must
        # distinguish loss/reordering from invalid protocol and retain raw bytes.
        def event(payload):
            iface = UartInterface(Stream(b""))
            header = struct.pack("<IHH", iface.REQ_EVENT, len(payload), int(EVENT.WINDOWS_SERIAL))
            return header + payload + struct.pack("<I", iface.checksum(header + payload))
        for name, payload, reject in (
            ("sequence-gap", struct.pack("<HHIQHH", 1, 1, 3, 0, 1, 0) + b"x", False),
            ("malformed-version", struct.pack("<HHIQHH", 2, 0, 0, 0, 0, 0), True),
            ("malformed-flags", struct.pack("<HHIQHH", 1, 2, 0, 0, 0, 0), True),
            ("malformed-reserved", struct.pack("<HHIQHH", 1, 0, 0, 0, 0, 1), True),
            ("malformed-length", struct.pack("<HHIQHH", 1, 0, 0, 0, 2, 0) + b"x", True)):
            folder = out / name
            folder.mkdir()
            wire = event(payload)
            (folder / "frames.bin").write_bytes(wire)
            try:
                summary = decode(folder, wire)
                assert not reject and not summary["capture_complete"]
            except ValueError:
                assert reject
            report["cases"].append({"case": name, "rejected": reject})

        for caps in (0x4157494e000100ff, 0x4157494e000101bf):
            try:
                validate_capabilities(caps, serial="capture")
            except ValueError:
                pass
            else:
                raise AssertionError("capture accepted without required capabilities")
        assert validate_capabilities(0x4157494e000101ff, serial="capture") == 0x1ff
        report["cases"].append({"case": "capability-admission"})

        # Real launch orchestration and HardwareSession cleanup retain the
        # streaming recorder's final summary, including on transport failure.
        image = out / "mu.bin"
        image.write_bytes(bytes(16))
        profile = out / "profile.json"
        profile.write_text(json.dumps({"schema": "aura.windows.boot.v1", "name": "serial-replay",
            "target": {"chip_id": "0x8142", "model": "Mac17,3"},
            "mu": {"path": image.name, "sha256": digest(image), "entry_offset": 0},
            "serial": "capture"}))
        class Session(HardwareSession):
            fail = False
            def __init__(self, device, log):
                self.iface = UartInterface(Stream(b""))
                self.iface.tty_log = log
                self.serial_capture = None
            def capabilities(self): return 0x4157494e000101ff
            def identity(self): return {"chip_id": 0x8142, "model": "Mac17,3"}
            def prepare(self, plan):
                assert plan.serial == "capture"
                self.serial_capture = WindowsSerialRecorder(Path(self.iface.tty_log.name).parent)
                self.hv = HV.__new__(HV)
                self.hv.windows, self.hv.windows_serial = True, "capture"
                self.hv.windows_serial_recorder = self.serial_capture
                self.iface.set_event_handler(EVENT.WINDOWS_SERIAL, self.hv.handle_windows_serial)
            def load(self, plan): return {"model": True}
            def start(self):
                reply = struct.pack("<Ii24s", self.iface.REQ_PROXY, 0, bytes(24))
                reply += struct.pack("<I", self.iface.checksum(reply))
                self.iface.dev.data = io.BytesIO((out / "binary/frames.bin").read_bytes() + reply)
                self.iface.reply(self.iface.REQ_PROXY)
                assert (self.serial_capture.directory / "guest-serial.bin").read_bytes() == binary_data
                if self.fail: raise OSError("modeled disconnect after final event")
                return 1
        for fail in (False, True):
            Session.fail = fail
            folder = out / ("launch-error" if fail else "launch-success")
            try:
                launch_profile(profile, "modeled", folder, Session)
                assert not fail
            except OSError:
                assert fail
            receipt = json.loads((folder / "launch.json").read_text())
            assert receipt["serial"]["capture_complete"] is not fail
            assert receipt["serial"]["sha256"] == hashlib.sha256(binary_data).hexdigest()
            assert not receipt["windows_ready"] and not receipt["hardware_qualified"]
            report["cases"].append({"case": folder.name, "serial": receipt["serial"]})
        report["result"] = "passed"
        report["replay"] = [sys.executable, str(snapshot / "tools/windows_serial_e2e.py"),
                            "--output", "NEW_EMPTY_DIRECTORY"]
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(report['cases'])} serial production-path cases: {out / 'result.json'}")


if __name__ == "__main__":
    main()
