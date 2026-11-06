#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay Windows UART backpressure through production queue and I/O code."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def production_function(source, name):
    match = re.search(
        r"(?m)^(?:static )?(?:void|size_t|ssize_t) "
        + re.escape(name)
        + r"\([^;]*?\)\n\{",
        source,
    )
    if match is None:
        raise ValueError("Missing production function: " + name)
    end = source.index("\n}", match.end()) + 2
    return source[match.start() : end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--expect-unfixed", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "result": "failed",
        "hardware_accessed": False,
        "commands": [],
        "sources": {},
        "scope": "production VUART, I/O dispatch, DWC3 queue, OUT starter/read and ringbuffer replay",
        "limitations": [
            "USB register events and host transfer completion are modeled",
            "USB wrapper objects are checked by separate ARM64 compilation",
            "No physical disconnect, KD or Windows qualification",
        ],
    }

    def run(command, cwd, log):
        command = [str(arg) for arg in command]
        report["commands"].append({"argv": command, "cwd": str(cwd)})
        result = subprocess.run(
            command, cwd=cwd, capture_output=True, text=True, timeout=30
        )
        (output / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    try:
        controller = (ROOT / "src/usb_dwc3_impl.inc").read_text()
        generated = output / "production_rx.inc"
        generated.write_text(
            "\n".join(
                production_function(controller, name)
                for name in [
                    "usb_dwc3_cdc_start_bulk_out_xfer",
                    "usb_dwc3_can_read",
                    "usb_dwc3_read",
                ]
            )
        )
        report["production_rx_sha256"] = sha(generated)
        flags = ["-Isrc", "-idirafter", "sysinc", "-I", str(output)]
        deps = run(
            [args.cc, "-MM", *flags, "tests/e2e/windows_vuart_host.c"], ROOT, "deps.log"
        )
        paths = [
            (ROOT / part).resolve()
            for part in deps.replace("\\\n", " ").split(":", 1)[1].split()
        ]
        names = {
            str(path.relative_to(ROOT))
            for path in paths
            if path.is_relative_to(ROOT) and not path.is_relative_to(output)
        }
        names.update(
            (
                "tools/windows_vuart_e2e.py",
                "tools/windows_vuart_smoke.py",
                "tests/e2e/windows_vuart_diagnostics.py",
                "docs/windows-vuart-backpressure.md",
                "docs/windows-vuart-smoke.md",
                "sysinc/malloc.h",
                "src/usb.c",
                "src/usb_dwc3.c",
                "src/usb_dwc3.h",
                "src/usb_dwc3_hs.c",
                "src/usb_dwc3_ss.c",
                "src/usb_dwc3_impl.inc",
            )
        )
        snapshot = output / "source"
        for name in sorted(names):
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
            report["sources"][name] = sha(target)
        report["engines"] = {}
        for mode, superspeed in [("hs", 0), ("ss", 1)]:
            binary = output / ("vuart-host-" + mode)
            stream = output / (
                "host-bytes.bin" if not superspeed else "host-bytes-ss.bin"
            )
            mode_flags = ["-DUSB_DWC3_SUPERSPEED=" + str(superspeed)]
            if args.expect_unfixed:
                mode_flags.append("-DEXPECT_UNFIXED")
            run(
                [
                    args.cc,
                    "-std=gnu11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-function",
                    "-Wno-missing-field-initializers",
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-fsanitize=address,undefined",
                    *flags,
                    *mode_flags,
                    "tests/e2e/windows_vuart_host.c",
                    "-o",
                    binary,
                    "-Wl,-dead_strip"
                    if sys.platform == "darwin"
                    else "-Wl,--gc-sections",
                ],
                snapshot,
                "compile-" + mode + ".log",
            )
            result = run([binary, stream], snapshot, "replay-" + mode + ".log")
            observation = json.loads(result)
            assert observation["rx_rearmed"] is (not args.expect_unfixed)
            assert stream.read_bytes() == b"abcdefghijklmnz"
            report["engines"][mode] = {
                "observations": observation,
                "binary_sha256": sha(binary),
                "host_bytes_sha256": sha(stream),
            }
        report["observations"] = report["engines"]["hs"]["observations"]
        result = run(
            [
                sys.executable,
                "tests/e2e/windows_vuart_diagnostics.py",
                snapshot / "tools/windows_vuart_smoke.py",
                output / "diagnostics",
            ],
            snapshot,
            "diagnostics.log",
        )
        report["diagnostics"] = json.loads(result)
        report["result"] = "unfixed-reproduced" if args.expect_unfixed else "passed"
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["result"] + ": " + str(output / "result.json"))
    return 0 if report["result"] in ("passed", "unfixed-reproduced") else 1


if __name__ == "__main__":
    raise SystemExit(main())
