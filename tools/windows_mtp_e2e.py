#!/usr/bin/env python3
"""Replay the production MTP lifecycle against a bounded device model."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = """disabled success chip board model sid mapper compatible data-reg
segment-size segment-overflow segment-alias gates-size ram-outside heap-overlap
adt-overlap firmware-overlap power-fail power-inactive asc-allocation running locked
active-root active-stream bypass tlbi-busy mmio-fault dapf dart-allocation
iova-allocation dart-map translation rtkit-allocation sram-window pool-map
boot-timeout no-init service-fail service-mmio readiness-mmio stage2-map final-map
runtime-fail stop-fail""".split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    snapshot = out / "source"
    for path in [*ROOT.glob("src/*.h"), ROOT / "src/hv/windows/hv.h",
                 ROOT / "src/hv/windows/mtp_handoff.c", ROOT / "src/hv/windows/mtp_handoff.h",
                 ROOT / "tests/e2e/windows_mtp_host.c", ROOT / "tools/windows_mtp_e2e.py",
                 ROOT / "docs/windows-mtp-handoff.md"]:
        dest = snapshot / path.relative_to(ROOT)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)
    report = {"result": "failed", "hardware_accessed": False,
              "scope": "Production MTP state machine with modeled ADT/MMIO, allocator and RTKit; not hardware qualification",
              "cases": [], "replay": "python3 source/tools/windows_mtp_e2e.py --output /tmp/mtp-replay-new"}
    try:
        command = ["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-I", str(snapshot), "-I", str(snapshot / "src"),
                   str(snapshot / "tests/e2e/windows_mtp_host.c"), "-o", str(out / "mtp-host")]
        report["compile_command"] = command
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (out / "compile.log").write_text(result.stdout + result.stderr)
        result.check_returncode()
        for case in CASES:
            result = subprocess.run([out / "mtp-host", case], capture_output=True, text=True, timeout=10)
            (out / f"{case}.log").write_text(result.stdout + result.stderr)
            result.check_returncode()
            expected = {
                "no-init": "phase7 readiness timeout",
                "service-fail": "phase7 RTKit rejected reason=3 msg0=0x123456789abcdef0 msg1=0x2",
                "service-mmio": "phase7 RTKit MMIO fault",
                "readiness-mmio": "phase7 DockChannel MMIO fault",
            }.get(case)
            if expected:
                assert expected in result.stdout, (case, result.stdout)
                assert "failed; allocations retained" in result.stdout
            if case in {"success", "runtime-fail", "stop-fail", "final-map"}:
                assert "phase7 notifications app=2 debug=1 oslog=3" in result.stdout
            report["cases"].append(case)
        report["result"] = "passed"
    finally:
        report["artifacts"] = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in sorted(out.rglob("*")) if path.is_file()}
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(out / "result.json")


if __name__ == "__main__":
    main()
