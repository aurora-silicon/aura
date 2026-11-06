#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-boundary E2E for profile validation and ordered Windows launch admission."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aura.windows import CAPS_MAGIC, REQUIRED_CAPS, launch_profile, source_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    firmware = out / "mu.fd"
    firmware_bytes = bytes(range(256)) * 32
    firmware.write_bytes(firmware_bytes)
    ramdisk = out / "ramdisk.img"
    ramdisk_bytes = b"NTASIRDK" + bytes(range(256)) * 8
    ramdisk.write_bytes(ramdisk_bytes)
    baseline = {"schema": "aura.windows.boot.v1", "name": "model-fixture",
                "target": {"chip_id": "0x8142", "model": "Mac17,3"},
                "mu": {"path": firmware.name,
                       "sha256": hashlib.sha256(firmware.read_bytes()).hexdigest(),
                       "entry_offset": "0x800"}, "framebuffer": "native"}
    cases = []
    for case in ("success", "normal-resident", "wrong-abi", "wrong-chip", "wrong-model",
                 "wrong-hash", "missing-image", "entry-outside", "conversion-missing", "init-failure",
                 "compatibility-missing", "compatibility-wrong-chip", "guest-error", "poweroff", "reset",
                 "append-offset", "append-contiguous", "append-overlap", "append-too-large",
                 "append-wrong-hash", "usb-unsupported", "n1-capability-missing", "n1-wrong-model",
                 "reservation-capability-missing", "reservation-wrap", "reservation-bad-fields"):
        profile = json.loads(json.dumps(baseline))
        expected_image = firmware_bytes
        if case.startswith("append-"):
            part = {"path": ramdisk.name, "sha256": hashlib.sha256(ramdisk_bytes).hexdigest()}
            if case != "append-contiguous": part["offset"] = "0x4000"
            if case == "append-overlap": part["offset"] = "0x1000"
            if case == "append-too-large": part["offset"] = "0x40000000"
            if case == "append-wrong-hash": part["sha256"] = "0" * 64
            profile["append"] = [part]
            gap = 0 if case == "append-contiguous" else 0x2000
            expected_image = firmware_bytes + bytes(gap) + ramdisk_bytes
        if case == "usb-unsupported": profile["usb"] = "enabled"
        if case.startswith("n1-"): profile["n1"] = "j813-v1"
        if case == "n1-wrong-model": profile["target"]["model"] = "Mac17,1"
        if case.startswith("reservation-"):
            profile["reserved_memory"] = [{"name": "mu-handoff", "base": "0x10220000000", "size": "0x404000"}]
        if case == "reservation-wrap": profile["reserved_memory"][0]["base"] = "0xfffffffffffff000"
        if case == "reservation-bad-fields": profile["reserved_memory"][0]["from_ram_top"] = "0x8000000"
        if case == "wrong-hash":
            profile["mu"]["sha256"] = "0" * 64
        if case == "missing-image":
            profile["mu"]["path"] = "absent.fd"
        if case == "entry-outside":
            profile["mu"]["entry_offset"] = "0x2000"
        if case == "conversion-missing":
            profile["framebuffer"] = "bgra-shadow"
        if case.startswith("compatibility-"):
            profile["guest_compatibility"] = "t8142-experimental"
        if case == "compatibility-wrong-chip":
            profile["target"]["chip_id"] = "0x8103"
        filename = out / (case + ".json")
        filename.write_text(json.dumps(profile))
        trace = []

        class Endpoint:
            def __init__(self, device, log):
                trace.append("connect")
            def capabilities(self):
                trace.append("read-capabilities")
                if case == "normal-resident":
                    return 0
                return CAPS_MAGIC | ((2 if case == "wrong-abi" else 1) << 16) | REQUIRED_CAPS
            def identity(self):
                trace.append("read-identity")
                return {"chip_id": 0x6020 if case == "wrong-chip" else 0x8142,
                        "model": "Mac14,1" if case == "wrong-model" else "Mac17,3",
                        "resident_base": "0x10000000000"}
            def prepare(self, plan):
                trace.append("mutate-prepare")
                if case == "init-failure":
                    raise RuntimeError("injected hv_init failure")
            def load(self, plan):
                trace.append("mutate-load")
                assert plan.image == expected_image
                if case == "append-offset":
                    assert plan.image[0x4000:0x4008] == b"NTASIRDK"
                    assert plan.images[1]["offset"] == 0x4000
                return {"guest_base": "0x10001000000", "entry": "0x10001000800"}
            def start(self):
                trace.append("mutate-start")
                return {"guest-error": -1, "poweroff": 1, "reset": 2}.get(case, 0)
            def close(self):
                trace.append("close")

        passed = False
        expected_success = case in ("success", "poweroff", "reset", "append-offset", "append-contiguous")
        try:
            result = launch_profile(filename, "model-device", out / (case + "-evidence"), session_factory=Endpoint)
            passed = expected_success
            assert not result["hardware_qualified"] and not result["windows_ready"]
            if case == "poweroff": assert result["result"] == "guest-powered-off-unqualified"
            if case == "reset": assert result["result"] == "guest-reset-requested-unqualified"
        except (ValueError, OSError, RuntimeError):
            passed = not expected_success
        if not expected_success and case not in ("init-failure", "guest-error"):
            assert not any(x.startswith("mutate-") for x in trace), trace
        if case == "success":
            assert trace == ["connect", "read-capabilities", "read-identity", "mutate-prepare", "mutate-load", "mutate-start", "close"]
        if case == "init-failure":
            assert "mutate-load" not in trace and "mutate-start" not in trace
        assert passed, case
        cases.append({"case": case, "result": "passed", "trace": trace})
    command = [sys.executable, str(Path(__file__).with_name("boot_windows.py")), "validate",
               "--profile", str(out / "success.json")]
    completed = subprocess.run(command, capture_output=True, text=True)
    (out / "validate-cli.log").write_text(completed.stdout + completed.stderr)
    completed.check_returncode()
    cases.append({"case": "real-cli-validation", "result": "passed"})
    report = {"result": "passed", "scope": "Real host profile/CLI/orchestration with simulated transport boundary",
              "hardware_accessed": False, "hardware_qualified": False, "cases": cases,
              "host_sources": source_manifest(),
              "e2e_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "replay": [sys.executable, str(Path(__file__).resolve()), "--out", "NEW_EMPTY_DIRECTORY"]}
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(cases)} host admission cases: {out / 'result.json'}")


if __name__ == "__main__":
    main()
