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
    "acquire-success", "acquire-captured-params", "acquire-adt-large", "acquire-invalid-roots", "acquire-unlock", "acquire-stale",
    "acquire-pmgr-address", "acquire-pmgr-descendant", "acquire-pmgr-virtual", "acquire-pmgr-duplicate",
    "acquire-clock-consumer", "acquire-power-consumer", "acquire-adt-malformed",
    "acquire-adt-large-duplicate", "acquire-adt-property-limit",
    "acquire-extra-mapper", "acquire-extra-consumer", "acquire-duplicate-phandle",
    "acquire-mapper-missing", "acquire-piodma-sid", "acquire-dart-function",
    "acquire-power-failure", "acquire-protected-change", "acquire-version",
    "acquire-stream-count", "acquire-geometry", "acquire-protect", "acquire-busy",
    "acquire-tcr-bypass", "acquire-valid-root", "acquire-disable-drop",
    "acquire-root-drop", "acquire-command-timeout", "acquire-unlock-timeout",
    "acquire-exception-reserved", "acquire-exception-persistent", "acquire-block-error",
    "acquire-mask-drop", "acquire-final-dirty", "acquire-retry", "acquire-cold-retry",
    "success", "tunables", "cold-name", "cold-parent", "cold-alias", "cold-unknown", "cold-reset", "cold-auto", "cold-target", "cold-active", "cold-clkgate", "fabric", "finish-early", "power-enable", "power-inactive", "power-unknown",
    "chip", "board", "model", "resource", "perst", "locked", "dart",
    "busy", "pre-bme", "train", "post-bme", "secondary-bme", "tertiary-bme",
    "dart-load-fault", "post-dart-active", "identity", "bar-mask", "bar-high",
    "bar-drop", "rom", "clamp-fail",
    "smc-nub-missing", "smc-pmu-missing", "smc-pmu-outside",
    "smc-controller-compatible", "smc-nub-compatible", "smc-pmu-compatible",
    "smc-controller-reset", "smc-reset-handle", "smc-reset-op",
    "census-unprepared", "census-failed-prepare", "census-valid", "census-magic",
    "census-bsp", "census-version", "census-reserved", "census-address",
    "census-guard", "census-masked", "census-pending", "census-load-fault",
    "census-barrier-fault",
)
SOURCES = (
    "src/hv/windows/windows_n1_scope.h", "src/hv/windows/windows_n1_dart.h",
    ".github/aurora/AGENTS.md", "docs/windows-n1-acquire.md", "src/proxy.c", "src/proxy.h",
    "src/pmgr.c", "src/hv/windows/windows_n1.c", "src/hv/windows/windows_n1.h",
    "src/hv/windows/hv.h", "src/hv/windows/hv_exc.c", "src/j813_n1.c", "src/j813_n1.h",
    "src/smp.h", "src/pmgr.h",
    "src/adt.h", "sysinc/string.h", "src/types.h", "src/utils.h", "src/exception.h",
    "tests/e2e/windows_n1/producer_host.c", "tests/e2e/windows_n1/failure-model.md",
    "tests/e2e/windows_n1/j813_reset_provider.h",
    "tests/e2e/windows_n1/sequence_host.c", "tests/e2e/windows_n1/linux_sequence.txt",
    "src/smc.h", "src/tunables.h", "src/libfdt/libfdt.h",
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
            "PHY/SMC production sequence replay uses modeled MMIO, tunables and SMC boundaries",
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
            transcript = run([binary, case], case_dir, log, report, env)
            diagnostic_markers = {
                "success": ("Windows N1: BAR probe old=", "BAR control-memory-enable"),
                "acquire-captured-params": ("DART read complete 0x3008112",),
                "bar-mask": ("mask=0xfffe0001", "primary failure at control BAR assignment"),
                "bar-high": ("high_mask=0xfffffffe", "primary failure at control BAR assignment"),
                "bar-drop": ("readback=0x4 expected=0x80000004", "primary failure at control BAR assignment"),
                "rom": ("primary failure at control ROM",),
            }
            for marker in diagnostic_markers.get(case, ()):
                if marker not in transcript:
                    raise RuntimeError(f"{case}: missing diagnostic marker {marker!r}; see {log}")
            if "primary failure at " in transcript:
                primary = transcript.index("primary failure at ")
                cleanup = min((transcript.find(marker) for marker in (
                    "pre-port failure;", "cannot prove PERST/link drain;", "handoff rejected;")
                    if marker in transcript), default=-1)
                if cleanup < primary:
                    raise RuntimeError(f"{case}: primary failure missing before cleanup; see {log}")
            report["cases"].append({"case": case, "result": "passed",
                                     "transcript_sha256": digest(log)})
        descriptor = output / "cases/success/descriptor.bin"
        report["descriptor"] = {"path": str(descriptor),
                                "bytes": descriptor.stat().st_size,
                                "sha256": digest(descriptor)}
        acquired = output / "cases/acquire-success/descriptor.bin"
        if acquired.read_bytes() != descriptor.read_bytes():
            raise RuntimeError("acquisition changed the existing NW1H v1 wire contract")
        report["acquisition_descriptor"] = {"path": str(acquired),
            "bytes": acquired.stat().st_size, "sha256": digest(acquired),
            "matches_cold_descriptor": True}
        sequence = output / "sequence-host"
        run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-fsanitize=address,undefined", "-Isrc",
             "tests/e2e/windows_n1/sequence_host.c", "-o", sequence],
            snapshot, output / "sequence-compile.log", report)
        linux = run([sequence, "linux"], snapshot, output / "linux-sequence.log", report, env)
        expected = snapshot / "tests/e2e/windows_n1/linux_sequence.txt"
        if linux != expected.read_text():
            raise RuntimeError("default Linux hardware-action sequence differs from pre-split baseline")
        run([sequence, "windows"], snapshot, output / "windows-sequence.log", report, env)
        run([sequence, "windows-fault"], snapshot, output / "windows-fault.log", report, env)
        run([sequence, "windows-barrier-fault"], snapshot,
            output / "windows-barrier-fault.log", report, env)
        report["sequence"] = {
            "linux_baseline_commit": "c30bec09",
            "linux_transcript_sha256": digest(output / "linux-sequence.log"),
            "windows_transcript_sha256": digest(output / "windows-sequence.log"),
            "result": "passed",
        }
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
