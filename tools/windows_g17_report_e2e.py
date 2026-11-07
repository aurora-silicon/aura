#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Source-bound diagnostic Aura -> Mu -> Windows-reader artifact replay.

Written before the production diagnostic policy. The existing host lifecycle
runner performs real profile/admission/serialization and records every modeled
transport operation. This runner adds the separately selected diagnostic path.
"""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import traceback
import zlib

import windows_g17_e2e as preserve

ROOT = Path(__file__).resolve().parents[1]
POLICY = "j813-resource-report-v1"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_reader(table_path, folder):
    """Exercise real reader control flow against exact installed Mu bytes."""
    folder.mkdir()
    original = table_path.read_bytes()
    assert len(original) == 724
    fixtures = [("valid", "normal", original, True)]
    for scenario in ("missing", "duplicate", "enum-error", "enum-oversized",
                     "enum-unaligned", "enum-race", "get-error", "get-short", "get-race"):
        fixtures.append((scenario, scenario, original, False))
    for label, offset, width, value in (
            ("acpi-signature", 0, 4, 0), ("acpi-length", 4, 4, 723),
            ("acpi-version", 8, 1, 2), ("oem-id", 10, 1, 0),
            ("oem-table-id", 16, 1, 0), ("creator", 28, 1, 0),
            ("report-magic", 36, 4, 0), ("report-version", 40, 2, 2),
            ("header-size", 42, 2, 31), ("report-size", 44, 4, 689),
            ("authority", 48, 4, 1), ("preservation-size", 52, 4, 303),
            ("mmio-count", 56, 4, 7), ("irq-count", 60, 4, 5),
            ("wrong-chip", 76, 4, 0x8152), ("wrong-board", 80, 4, 0x25),
            ("preservation-authority", 88, 4, 3), ("guest-start", 92, 8, 0),
            ("wrong-firmware", 124, 1, 0), ("duplicate-region", 68+112+24, 4, 1),
            ("zero-region", 68+112+16, 8, 0),
            ("mmio-role", 374, 2, 1), ("mmio-parent", 376, 2, 1),
            ("mmio-authority", 378, 2, 1), ("wrong-mmio", 380, 8, 0x580000001),
            ("mmio-size", 388, 8, 0), ("mmio-reserved", 396, 4, 1),
            ("irq-role", 564, 2, 2), ("irq-event", 566, 2, 2),
            ("physical-irq", 568, 4, 1126), ("active-gsiv", 572, 4, 990),
            ("irq-authority", 576, 4, 1), ("irq-trigger", 580, 4, 2),
            ("irq-reserved", 584, 4, 1), ("uat-geometry", 660, 2, 40),
            ("uat-authority", 684, 4, 1), ("power-authority", 692, 4, 1),
            ("power-offset", 704, 4, 0x114)):
        value_bytes = bytearray(original)
        value_bytes[offset:offset+width] = value.to_bytes(width, "little")
        # Recompute every enclosing checksum to test semantic rejection.
        value_bytes[172:176] = bytes(4)
        struct.pack_into("<I", value_bytes, 172, zlib.crc32(value_bytes[68:372]))
        value_bytes[64:68] = bytes(4)
        struct.pack_into("<I", value_bytes, 64, zlib.crc32(value_bytes[36:]))
        value_bytes[9] = 0
        value_bytes[9] = (-sum(value_bytes)) & 255
        fixtures.append((label, "normal", bytes(value_bytes), False))
    for label, offset in (("acpi-checksum", 9), ("report-checksum", 64), ("preserve-checksum", 172)):
        value_bytes = bytearray(original)
        value_bytes[offset] ^= 1
        if offset != 9:
            if offset == 172:
                value_bytes[64:68] = bytes(4)
                struct.pack_into("<I", value_bytes, 64, zlib.crc32(value_bytes[36:]))
            value_bytes[9] = 0
            value_bytes[9] = (-sum(value_bytes)) & 255
        fixtures.append((label, "normal", bytes(value_bytes), False))
    fixtures.extend((("short-table", "normal", original[:-1], False),
                     ("long-table", "normal", original+b"\0", False)))
    for name, _, data, _ in fixtures:
        (folder / (name + ".bin")).write_bytes(data)
    executable = folder / "reader-replay"
    command = ["/usr/bin/clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-DG17_REPORT_REPLAY=1",
               str(ROOT / "tools/windows_g17_report_reader.c"),
               str(ROOT / "tools/e2e/g17_report_winshim.c"),
               "-I" + str(ROOT / "tools/e2e"), "-o", str(executable)]
    compiled = subprocess.run(command, text=True, capture_output=True)
    (folder / "compile.log").write_text(compiled.stdout + compiled.stderr)
    results = {"command": command, "compile_returncode": compiled.returncode,
               "scope": "production Windows reader with fixture firmware APIs; not Windows execution",
               "input_sha256": sha(table_path), "cases": [],
               "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (
                   ROOT/"tools/windows_g17_report_reader.c", ROOT/"tools/e2e/g17_report_winshim.c",
                   ROOT/"tools/e2e/g17_report_winshim.h", Path(__file__).resolve())}}
    (folder / "receipt.json").write_text(json.dumps(results, indent=2)+"\n")
    assert compiled.returncode == 0, compiled.stderr
    for name, scenario, data, accepted in fixtures:
        run = subprocess.run([str(executable), scenario, str(folder/(name+".bin"))],
                             text=True, capture_output=True)
        (folder/(name+".json")).write_text(run.stdout)
        (folder/(name+".trace")).write_text(run.stderr)
        decoded = json.loads(run.stdout)
        assert (run.returncode == 0) == accepted, (name, run.returncode, decoded)
        assert decoded["result"] == ("diagnostic-only" if accepted else "rejected")
        if accepted:
            assert bytes.fromhex(decoded["table_hex"]) == data
            assert decoded["activation_authority"] == 0
            assert decoded["published_gsivs"] == [0, 0, 0, 0]
        results["cases"].append({"name": name, "accepted": accepted, "returncode": run.returncode,
                                  "input_sha256": sha(folder/(name+".bin"))})
    results["result"] = "passed"
    results["executable_sha256"] = sha(executable)
    (folder / "receipt.json").write_text(json.dumps(results, indent=2)+"\n")
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mu-image", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    receipt = {"result": "failed", "scope": "offline diagnostic handoff", "cases": [],
               "hardware_accessed": False, "native_gpu_started": False,
               "host_sources_before": preserve.source_manifest()}
    try:
        image = args.mu_image.resolve() if args.mu_image else output / "synthetic-mu.fd"
        if not args.mu_image:
            image.write_bytes(bytes(65536))
        receipt["image"] = {"path": str(image), "sha256": sha(image), "native": bool(args.mu_image)}
        for name, case, policy, required in (
                ("report", "enabled", POLICY, True),
                ("disabled", "disabled", None, False),
                ("preserve-only", "enabled", None, False),
                ("late-report", "late-report", POLICY, True),
                ("wrong-board", "wrong-board", POLICY, True),
                ("unknown-build", "unknown-build", POLICY, True)):
            with (output / (name + ".log")).open("w") as log, contextlib.redirect_stdout(log):
                result = preserve.run_host(case, output / name, image, policy, required)
            result["scenario"] = name
            receipt["cases"].append(result)
        receipt["host_result"] = "passed"
        mu_command = [sys.executable, str(args.mu.resolve()/"Tools/g17-report-e2e/run.py"),
                      "--fixtures", str(output), "--output", str(output/"mu")]
        mu_run = subprocess.run(mu_command, text=True, capture_output=True)
        (output/"mu.log").write_text(mu_run.stdout+mu_run.stderr)
        receipt["mu_command"] = {"argv": mu_command, "returncode": mu_run.returncode}
        assert mu_run.returncode == 0, mu_run.stdout+mu_run.stderr
        receipt["reader"] = run_reader(output/"mu/admitted-report/AG17.bin", output/"reader")
        receipt["result"] = "passed"
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
        receipt["traceback"] = traceback.format_exc()
    finally:
        receipt["host_sources_after"] = preserve.source_manifest()
        receipt["sources"] = {str(p.relative_to(ROOT)): sha(p) for p in (
            Path(__file__).resolve(), ROOT / "tools/windows_g17_e2e.py",
            ROOT / "docs/windows-g17-report.md", ROOT / "docs/windows-g17-report-failure-model.md",
            ROOT / "tools/windows_g17_report_reader.c", ROOT / "tools/windows_g17_report_build.py",
            ROOT / "tools/e2e/g17_report_winshim.c", ROOT / "tools/e2e/g17_report_winshim.h")}
        receipt["artifacts"] = {str(p.relative_to(output)): sha(p) for p in output.rglob("*")
                                if p.is_file() and p.suffix in (".adt", ".bin", ".json", ".fd")
                                and p.name != "receipt.json"}
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(receipt["result"], output / "receipt.json")
    return receipt["result"] != "passed"


if __name__ == "__main__":
    raise SystemExit(main())
