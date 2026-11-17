#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay the actual J813 E boot-state source offline: no device, proxy or hardware.

Extracts the real T8142 setter and table code from src/cpufreq.c, the
configuration, latch, apply and hv_start() prologue from src/hv/windows/hv.c and
the proxy cases from src/proxy.c, then compiles them under ASan/UBSan with
tests/e2e/windows_ecpu_boot.c, which models only registers, polling and the ADT.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def between(text, begin, end, include_end=True):
    start = text.index(begin)
    stop = text.index(end, start) + (len(end) if include_end else 0)
    return text[start:stop]


def function(text, signature):
    return between(text, signature, "\n}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cpufreq, hv, proxy = (ROOT / p for p in ("src/cpufreq.c", "src/hv/windows/hv.c", "src/proxy.c"))
    soc, types, harness = ROOT / "src/soc.h", ROOT / "src/types.h", ROOT / "tests/e2e/windows_ecpu_boot.c"
    text = cpufreq.read_text()
    source = between(text, "#define CLUSTER_PSTATE ", "\nint cpufreq_init_cluster(", False)
    source += "\n" + between(text, "static const struct cluster_t t8142_clusters[] = {", "\n};")
    source += "\n" + between(text, "#ifdef AURA_WINDOWS\n/* J813 firmware handoff commands", "\n#endif\n")
    (out / "ecpu-cpufreq.inc").write_text(source)
    macros = [l for l in soc.read_text().splitlines()
              if re.fullmatch(r"#define\s+(?:T\d+|S[A-Z0-9]+)\s+0x[0-9a-fA-F]+", l)]
    for name in ("BIT", "GENMASK", "_FIELD_LSB", "FIELD_PREP", "FIELD_GET"):
        lines = [l for l in types.read_text().splitlines() if l.startswith("#define " + name + "(")]
        assert len(lines) == 1, name
        macros += lines
    (out / "ecpu-defines.inc").write_text("\n".join(macros) + "\n")
    text = hv.read_text()
    prologue = between(text, "int hv_start(void *entry, u64 regs[4])", "    hv_windows_entered = true;\n")
    # Guest entry must follow the apply; everything after it is outside this replay.
    assert prologue.index("hv_windows_apply_ecpu()") < prologue.index("hv_windows_entered = true;")
    pieces = [function(text, s) for s in ("int hv_windows_configure_ecpu(u64 state, u64 khz)",
                                           "bool hv_windows_cpufreq_owned(void)",
                                           "static bool hv_windows_apply_ecpu(void)")]
    pieces.append(prologue + "    (void)entry;\n    (void)regs;\n    return 0;\n}\n")
    (out / "ecpu-hv.inc").write_text("\n".join(pieces))
    text = proxy.read_text()
    cases = between(text, "        case P_CPUFREQ_INIT:", "        case P_READ_GIGALOCKER:", False)
    cases += between(text, "#ifdef AURA_WINDOWS\n        /* Not handled by the default image", "#endif\n")
    (out / "ecpu-proxy.inc").write_text(cases)
    command = ["clang", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(out), str(harness),
               "-o", str(out / "replay")]
    report = {"schema": 1, "hardware_accessed": False, "command": command, "passed": False,
              "sources": {str(p): sha(p) for p in (cpufreq, hv, proxy, soc, types, harness,
                                                   Path(__file__).resolve())}}
    result = subprocess.run(command, capture_output=True, timeout=120)
    (out / "compile.log").write_bytes(result.stdout + result.stderr)
    report["compile_rc"] = result.returncode
    if result.returncode == 0:
        result = subprocess.run([str(out / "replay")], capture_output=True, timeout=60)
        (out / "trace.log").write_bytes(result.stdout)
        (out / "stderr.log").write_bytes(result.stderr)
        report["run_rc"] = result.returncode
        report["passed"] = result.returncode == 0 and b'"result":"PASS"' in result.stdout
    report["artifacts"] = {p.name: {"sha256": sha(p), "bytes": p.stat().st_size}
                           for p in sorted(out.iterdir()) if p.is_file()}
    report["replay"] = ["python3", str(Path(__file__).resolve()), "--out", "NEW_EMPTY_DIRECTORY"]
    (out / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "receipt": str(out / "receipt.json")}))
    return not report["passed"]


if __name__ == "__main__":
    raise SystemExit(main())
