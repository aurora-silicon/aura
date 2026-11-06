#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay exact production Windows software SGI routing with modeled CPU topology."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = {
    "src/hv/windows/windows_compat.c": ["smp_cpu_count"],
    "src/hv/windows/hv_vgic.c": ["hv_vgic3_num_cpus", "hv_vgic3_cpu_for_frame"],
    "src/hv/windows/hv_exc.c": ["hv_gic_bitmap_set", "hv_gic_cpuif_send_sgi"],
}
def sha(data):
    return hashlib.sha256(data).hexdigest()
def extract(source, name):
    match = re.search(r"(?m)^(?:static )?(?:void|int|u16) " + name + r"\([^;]*?\)\n\{", source)
    if not match:
        raise ValueError("Production definition missing: " + name)
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "sources": {}, "functions": {}, "commands": []}
    def run(cmd, log):
        cmd = [str(x) for x in cmd]; report["commands"].append(cmd)
        result = subprocess.run(cmd, cwd=out, text=True, capture_output=True, timeout=30)
        (out / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout
    try:
        fragments = []
        names = list(FUNCTIONS) + ["tests/e2e/windows_sgi_host.c", "docs/windows-sgi-routing.md", "tools/windows_sgi_e2e.py"]
        for name in names:
            target = out / "source" / name; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target); report["sources"][name] = sha(target.read_bytes())
            for function in FUNCTIONS.get(name, []):
                body = extract(target.read_text(), function)
                report["functions"][function] = sha(body.encode()); fragments.append(body)
        (out / "production_sgi.inc").write_text("\n".join(fragments))
        binary = out / "sgi-host"
        run([args.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-fsanitize=address,undefined", "-I", out,
             out / "source/tests/e2e/windows_sgi_host.c", "-o", binary], "compile.log")
        results = [json.loads(line) for line in run([binary], "replay.log").splitlines()]
        assert len(results) == 9
        report.update(result="passed", cases=results, binary_sha256=sha(binary.read_bytes()))
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["result"] + ": " + str(out / "result.json"))
    return 0 if report["result"] == "passed" else 1
if __name__ == "__main__":
    raise SystemExit(main())
