#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the N1 preparation and guarded census model against a source snapshot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "success", "chip", "board", "model", "resource", "perst", "locked", "dart",
    "busy", "pre-bme", "train", "post-bme", "identity", "bar-mask", "bar-high",
    "bar-drop", "rom", "clamp-fail",
    "census-unprepared", "census-failed-prepare", "census-valid", "census-magic",
    "census-bsp", "census-version", "census-reserved", "census-address",
    "census-guard", "census-masked", "census-pending", "census-load-fault",
    "census-barrier-fault",
)
SOURCES = (
    "src/hv/windows/windows_n1.c", "src/hv/windows/windows_n1.h",
    "src/hv/windows/hv.h", "src/hv/windows/hv_exc.c", "src/j813_n1.c", "src/j813_n1.h",
    "src/smp.h",
    "src/adt.h", "sysinc/string.h", "src/types.h", "src/utils.h", "src/exception.h",
    "tests/e2e/windows_n1/producer_host.c", "tests/e2e/windows_n1/failure-model.md",
    "tools/windows_n1_e2e.py", "docs/windows-n1.md",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, cwd, log, report, env=None):
    command = [str(arg) for arg in argv]
    report["commands"].append({"argv": command, "cwd": str(cwd), "log": str(log)})
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False, timeout=120)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"command exited {result.returncode}; see {log}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for source copies, commands, logs and receipt")
    parser.add_argument("--cc", default="clang", help="Clang with ASan/UBSan support")
    args = parser.parse_args()
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "result": "failed", "hardware_accessed": False, "hardware_qualified": False,
        "scope": "production N1 preparation and census with independent ADT/MMIO model",
        "limitations": [
            "Linux PHY/SMC training call is modelled, not executed",
            "No physical reset/drain, Windows PnP, interrupts or DMA qualification",
            "Host preparation gate is stubbed enabled; this is not a launcher lifecycle test",
            "The HVC instruction/dispatcher and physical fault delivery are not executed",
        ],
        "commands": [], "sources": {}, "cases": [],
    }
    try:
        snapshot = output / "source"
        for relative in SOURCES:
            source = ROOT / relative
            target = snapshot / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            report["sources"][relative] = digest(target)
        compiler = shutil.which(args.cc)
        if not compiler:
            raise RuntimeError(f"compiler not found: {args.cc}")
        compiler = str(Path(compiler).resolve())
        report["compiler"] = run([compiler, "--version"], snapshot,
                                 output / "compiler.log", report).strip()
        binary = output / "producer-host"
        run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-I" + str(snapshot / "src"),
             snapshot / "tests/e2e/windows_n1/producer_host.c", "-o", binary],
            snapshot, output / "compile.log", report)
        report["binary_sha256"] = digest(binary)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        report["sanitizer_environment"] = {name: env[name] for name in
                                           ("ASAN_OPTIONS", "UBSAN_OPTIONS")}
        for case in CASES:
            case_dir = output / "cases" / case
            case_dir.mkdir(parents=True)
            log = case_dir / "transcript.log"
            run([binary, case], case_dir, log, report, env)
            report["cases"].append({"case": case, "result": "passed",
                                     "transcript_sha256": digest(log)})
        descriptor = output / "cases/success/descriptor.bin"
        report["descriptor"] = {"path": str(descriptor),
                                "bytes": descriptor.stat().st_size,
                                "sha256": digest(descriptor)}
        report["result"] = "passed"
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        receipt = output / "result.json"
        receipt.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['result']}: {len(report['cases'])}/{len(CASES)} scenarios; {receipt}")
    return 0 if report["result"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
