#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise real Windows connection admission across retained checksum sessions.

Failure model is recorded in docs/windows-launcher.md before the fix. The
stateful endpoint retains negotiated features while host handles are replaced.
Actual UartInterface and HardwareSession encode/decode every transaction.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "proxyclient"))
from aura.proxy import UartInterface, UartChecksumError, Feature
from aura.windows import HardwareSession, source_manifest


def checksum(data):
    value = 0xdeadbeef
    for byte in data:
        value = (value * 31337 + (byte ^ 0x5a)) & 0xffffffff
    return value ^ 0xaddedbad


class Resident:
    def __init__(self, allow_disable=True, disabled=False, corrupt_nop=False):
        self.allow_disable, self.disabled = allow_disable, disabled
        self.corrupt_nop = corrupt_nop
        self.trace = []
        self.data = bytes(range(256)) * 257
        self.tx, self.rx = bytearray(), bytearray()

    def open(self):
        resident = self
        class Port:
            closed = False
            incoming = b""
            def flushOutput(self): pass
            def flushInput(self): pass
            def close(self): self.closed = True
            def read(self, size):
                block, self.incoming = self.incoming[:size], self.incoming[size:]
                return block
            def write(self, command):
                assert not self.closed and len(command) == 64
                assert checksum(command[:-4]) == struct.unpack("<I", command[-4:])[0]
                resident.tx.extend(command)
                kind = struct.unpack_from("<I", command)[0]
                payload = command[4:60]
                extra = b""
                if kind == UartInterface.REQ_NOP:
                    requested = struct.unpack_from("<Q", payload)[0]
                    resident.disabled = bool(requested & 1) and resident.allow_disable
                    result = struct.pack("<Q", int(resident.disabled)).ljust(24, b"\0")
                    resident.trace.append("nop")
                elif kind == UartInterface.REQ_PROXY:
                    opcode = struct.unpack_from("<Q", payload)[0]
                    assert opcode == 0xc80, "unexpected target operation before admission"
                    result = struct.pack("<QqQ", opcode, 0, 0x4157494e000101ff)
                    resident.trace.append("capabilities")
                elif kind == UartInterface.REQ_MEMREAD:
                    address, size = struct.unpack_from("<QQ", payload)
                    assert address + size <= len(resident.data)
                    extra = resident.data[address:address + size]
                    value = UartInterface.CHECKSUM_SENTINEL if resident.disabled else checksum(extra)
                    result = struct.pack("<I", value).ljust(24, b"\0")
                    if resident.disabled:
                        extra += struct.pack("<I", UartInterface.DATA_END_SENTINEL)
                    resident.trace.append("readmem")
                else:
                    raise AssertionError("unexpected command")
                reply = struct.pack("<Ii24s", kind, 0, result)
                value = checksum(reply) ^ (1 if resident.corrupt_nop and kind == UartInterface.REQ_NOP else 0)
                reply += struct.pack("<I", value) + extra
                resident.rx.extend(reply)
                self.incoming += reply
                return len(command)
        return Port()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "hardware_qualified": False,
              "scope": "real host constructor/framing over a stateful endpoint model",
              "cases": [], "host_sources": source_manifest()}
    try:
        # Demonstrate the original bug without relying on a changed constructor.
        resident = Resident(disabled=True)
        iface = UartInterface(resident.open())
        assert iface.enabled_features == 0
        try:
            iface.readmem(0, 256)
        except UartChecksumError as error:
            assert "Expected 0xd0decade" in str(error)
            report["cases"].append({"case": "original-reopen-mismatch", "error": str(error)})
        else:
            raise AssertionError("unnegotiated interface did not reproduce failure")

        for name, allow, prior in (("cdc-retained-disabled", True, True),
                                    ("cdc-retained-enabled", True, False),
                                    ("kis-no-feature", False, False),
                                    ("kis-after-disabled-carrier", False, True)):
            resident = Resident(allow_disable=allow, disabled=prior)
            for reopen in range(2):
                before = len(resident.trace)
                port = resident.open()
                session = HardwareSession(port, io.StringIO())
                assert resident.trace[before:] == ["nop"]
                assert session.capabilities() == 0x4157494e000101ff
                assert session.iface.readmem(0, len(resident.data)) == resident.data
                assert bool(session.iface.enabled_features & Feature.DISABLE_DATA_CSUMS) == allow
                assert port.incoming == b"", "read left a stale sentinel or reply"
                session.close()
                assert port.closed
            (out / (name + "-tx.bin")).write_bytes(resident.tx)
            (out / (name + "-rx.bin")).write_bytes(resident.rx)
            report["cases"].append({"case": name, "trace": resident.trace,
                "read_sha256": hashlib.sha256(resident.data).hexdigest(),
                "tx_sha256": hashlib.sha256(resident.tx).hexdigest(),
                "rx_sha256": hashlib.sha256(resident.rx).hexdigest()})
        resident = Resident(corrupt_nop=True)
        port = resident.open()
        try:
            HardwareSession(port, io.StringIO())
        except UartChecksumError:
            assert port.closed and resident.trace == ["nop"]
        else:
            raise AssertionError("constructor accepted invalid handshake")
        report["cases"].append({"case": "failed-negotiation-closes-before-proxy", "trace": resident.trace})
        report["e2e_source_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        report["replay"] = [sys.executable, str(Path(__file__).resolve()), "--out", "NEW_EMPTY_DIRECTORY"]
        report["result"] = "passed"
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(report['cases'])} connection admission cases: {out / 'result.json'}")


if __name__ == "__main__":
    main()
