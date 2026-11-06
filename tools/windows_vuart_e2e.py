#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay Windows UART backpressure through production queue and I/O code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "commands": [], "sources": {},
              "scope": "production VUART, I/O dispatch, DWC3 queue and ringbuffer replay",
              "limitations": ["USB register events and host transfer completion are modeled",
                              "USB wrapper objects are checked by separate ARM64 compilation",
                              "No physical disconnect, KD or Windows qualification"]}

    def run(command, cwd, log):
        command = [str(arg) for arg in command]
        report["commands"].append({"argv": command, "cwd": str(cwd)})
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=30)
        (output / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    try:
        flags = ["-Isrc", "-idirafter", "sysinc"]
        deps = run([args.cc, "-MM", *flags, "tests/e2e/windows_vuart_host.c"], ROOT, "deps.log")
        paths = [(ROOT / part).resolve() for part in deps.replace("\\\n", " ").split(":", 1)[1].split()]
        names = {str(path.relative_to(ROOT)) for path in paths if path.is_relative_to(ROOT)}
        names.update(("tools/windows_vuart_e2e.py", "docs/windows-vuart-backpressure.md",
                      "sysinc/malloc.h",
                      "src/usb.c", "src/usb_dwc3.c", "src/usb_dwc3.h",
                      "src/usb_dwc3_hs.c", "src/usb_dwc3_ss.c", "src/usb_dwc3_impl.inc"))
        snapshot = output / "source"
        for name in sorted(names):
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
            report["sources"][name] = sha(target)
        binary = output / "vuart-host"
        run([args.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-missing-field-initializers", "-ffunction-sections",
             "-fdata-sections", "-fsanitize=address,undefined", *flags,
             "tests/e2e/windows_vuart_host.c", "-o", binary,
             "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"],
            snapshot, "compile.log")
        result = run([binary, output / "host-bytes.bin"], snapshot, "replay.log")
        report["observations"] = json.loads(result)
        assert (output / "host-bytes.bin").read_bytes() == b"abcdefghijklmnz"
        report.update(result="passed", binary_sha256=sha(binary),
                      host_bytes_sha256=sha(output / "host-bytes.bin"))
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["result"] + ": " + str(output / "result.json"))
    return 0 if report["result"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
