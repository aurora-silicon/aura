#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Complete combined-profile launcher admission over a modeled endpoint boundary."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "proxyclient"))
from aura.windows import launch_profile, source_manifest
from aura import windows_cdc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / "mu.fd"
    image.write_bytes(bytes(65536))
    native = (ROOT / "src/proxy.c").read_text()
    match = re.search(r"reply->retval = (0x4157494e[0-9a-f]+)ULL", native)
    assert match, "missing native Windows capability producer"
    capabilities = int(match[1], 16)
    base = {"schema": "aura.windows.boot.v1", "name": "combined-profile-replay",
            "target": {"chip_id": "0x8142", "model": "Mac17,3"},
            "mu": {"path": str(image), "sha256": hashlib.sha256(image.read_bytes()).hexdigest(), "entry_offset": 0},
            "framebuffer": "bgra-shadow", "guest_compatibility": "t8142-experimental",
            "usb": "disabled", "serial": "cdc", "mtp": "j813-v1",
            "cpufreq": "j813-manual-v1", "n1": "j813-acquire-v1",
            "reserved_memory": [{"name": "mu-boot-handoff", "base": "0x10220000000", "size": "0x404000"}]}
    report = {"result": "failed", "hardware_accessed": False, "hardware_qualified": False,
              "scope": "Actual combined profile/capability/launcher flow, modeled endpoint",
              "capabilities_from_native_source": hex(capabilities), "cases": [],
              "host_sources": source_manifest(),
              "native_proxy_sha256": hashlib.sha256(native.encode()).hexdigest(),
              "e2e_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    try:
        for name, remove, accepted in (("combined", 0, True), ("missing-cpu", 0x0800, False),
                ("missing-n1", 0x1000, False), ("missing-mtp", 0x0400, False),
                ("missing-shadow", 0x10, False), ("missing-cdc", 0x200, False),
                ("missing-configuration", 0x40, False), ("missing-reservation", 0x80, False),
                ("prepare-failed", 0, False)):
            profile = out / (name + ".json")
            profile.write_text(json.dumps(base, indent=2) + "\n")
            trace = []
            class Endpoint:
                def __init__(self, device, log): trace.append("connect")
                def capabilities(self): trace.append("capabilities"); return capabilities & ~remove
                def identity(self): trace.append("identity"); return {"chip_id": 0x8142, "model": "Mac17,3"}
                def validate_serial_transport(self, plan): trace.append("validate-cdc"); return {"mode": "modeled-cdc"}
                def prepare(self, plan):
                    trace.append("mutate-prepare")
                    assert (plan.cpufreq, plan.n1, plan.mtp, plan.framebuffer, plan.serial) == ("j813-manual-v1", "j813-acquire-v1", "j813-v1", "bgra-shadow", "cdc")
                    if name == "prepare-failed": raise RuntimeError("injected preparation failure")
                def load(self, plan): trace.append("mutate-load"); return {"guest_base": "0x10001000000"}
                def start(self): trace.append("mutate-start"); return 0
                def close(self): trace.append("close")
            class CdcBoundary:
                def __init__(self, folder, device): trace.append("cdc-open-modeled")
                def wait_ready(self): trace.append("cdc-ready")
                def check(self): pass
                def close(self): trace.append("cdc-close-modeled")
                def summary(self): return {"mode": "modeled-cdc", "hardware_accessed": False}
            result, rejection = False, None
            try:
                with patch.object(windows_cdc, "CdcSerialRecorder", CdcBoundary):
                    launch_profile(profile, "model-device", out / (name + "-evidence"), session_factory=Endpoint, guest_serial_device="model-guest-cdc")
                result = True
            except (ValueError, RuntimeError) as error: rejection = str(error)
            assert result == accepted, (name, trace)
            if remove: assert not any(x.startswith("mutate-") for x in trace), trace
            if name == "prepare-failed": assert "mutate-load" not in trace and "mutate-start" not in trace
            report["cases"].append({"case": name, "trace": trace, "rejection": rejection, "result": "passed"})
        assert capabilities & 0xffff == 0x1fff, "combined image must advertise CPU and N1 capabilities"
        report["result"] = "passed"
    except Exception as error:
        report["error"] = repr(error)
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["result"], out / "result.json")
    return report["result"] != "passed"


if __name__ == "__main__":
    raise SystemExit(main())
