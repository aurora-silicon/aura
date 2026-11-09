#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation controls for the HvXfer replay: prove it fails when it should.

Each mutant copies the tree, plants one realistic bug (a removed refusal, a
wrong decode, a lost tracer, the unported m1n1 device), reruns run.sh against
the copy, and requires the replay to FAIL -- with the named checks failing
where the bug is specific enough to name. A mutant that survives means a
failure mode the replay claims to cover is not actually covered.

Usage: mutants.py <out-dir>    (PYTHON / HVXFER_GUEST_DIR are passed through)
HVXFER_M1N1_DIR must name an m1n1 checkout: its src/hv_xfer.c is the unported device.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
M1N1_DIR = os.environ.get("HVXFER_M1N1_DIR")

C = "src/hv/windows/hv_xfer.c"
XPY = "proxyclient/aura/hv/xfer.py"
HVPY = "proxyclient/aura/hv/__init__.py"
WPY = "proxyclient/aura/windows.py"
TPY = "proxyclient/aura/hv/types.py"

MUTANTS = [
    # Without the bound, a guest UWIN_HI walks hv_pt_walk() off its L1 table:
    # the EL2 assert() fires (on hardware: an HV panic triggered by the guest).
    ("el2.no_ipa_bound", C, "    if (!xfer_ipa_range_ok(ipa, size))\n        return -1;\n\n    for", "    for",
     ["replay.el2_abort"]),
    ("el2.width_as_bytes", C, "bytes = 1u << width;", "bytes = width;", ["decode.id_version"]),
    ("el2.no_overclaim_clamp", C, "dev->result > dev->len)", "dev->result > 0xffffffffu)",
     ["cmd.result_overclaim"]),
    ("el2.no_dram_check", C, "    if (!xfer_dram_range_ok(first, size))\n        return -1;\n", "",
     ["win.reject_mmio", "win.reject_dram_edge"]),
    ("el2.no_contiguity", C, "else if (pa != first + off)", "else if (0)", ["win.reject_discontig"]),
    ("el2.cmd_on_64bit", C, "if (!isolated32) {", "if (!isolated32 && 0) {", ["decode.cmd64_refused"]),
    ("el2.replace_existing_mapping", C,
     "    if (xfer_range_mapped(base, HV_XFER_REGS_SIZE, 4)) {",
     "    if (0 && xfer_range_mapped(base, HV_XFER_REGS_SIZE, 4)) {",
     ["map.base_overlaps_hook", "map.base_overlaps_subpage_hook"]),
    ("el2.unimplemented_all_ones", C, "            return 0;\n    }\n}\n\nstatic void xfer_write_reg",
     "            return 0xffffffff;\n    }\n}\n\nstatic void xfer_write_reg", ["decode.unimplemented_zero"]),
    ("el2.sync_len_not_result", C, "cmd == HV_XFER_CMD_GET ? dev->result : dev->len", "dev->len",
     ["cmd.sync_range"]),
    ("host.event_number_drift", TPY, "    XFER = 9", "    XFER = 11", ["abi.event_number"]),
    ("host.handler_conditional", HVPY,
     "        self.iface.set_handler(START.HV, HV_EVENT.XFER, self.handle_xfer)",
     "        if self.windows: self.iface.set_handler(START.HV, HV_EVENT.XFER, self.handle_xfer)",
     ["host.handler_unconditional"]),
    ("host.no_reserved_doorbell_tracer", HVPY,
     '        self.add_tracer(irange(base, HV_XFER_REGS_SIZE), "HVXFER", TraceMode.RESERVED)\n', "",
     ["launch.reserved_tracers"]),
    ("engine.follows_symlinks", XPY, "flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | _CLOEXEC",
     "flags = os.O_RDONLY | os.O_NONBLOCK | _CLOEXEC", ["engine.symlink_refused"]),
    ("engine.put_overwrites", XPY, "            os.link(stream.part, stream.name,", "            os.replace(stream.part, stream.name,",
     ["engine.put_create_only"]),
    ("engine.dotdot_allowed", XPY, '    if ".." in raw or "/" in raw', '    if "/" in raw', ["engine.bad_names"]),
    ("profile.inbox_overlap_allowed", WPY,
     '        require(os.path.commonpath([serve_dir, inbox]) not in (serve_dir, inbox),',
     '        require(True or os.path.commonpath([serve_dir, inbox]) not in (serve_dir, inbox),',
     ["profile.inbox_overlap"]),
    ("profile.caps_unchecked", WPY, '        require(value & XFER, "resident lacks the HvXfer bulk channel")',
     "        pass", ["host.caps_required"]),
    # The m1n1 j813 device as it was, before this port's refusals.
    ("el2.m1n1_original", C, None, None, []),
]


def copy_tree(dst):
    for name in ("src", "sysinc", "proxyclient", "tests/hv_xfer"):
        shutil.copytree(REPO / name, dst / name, symlinks=True,
                        ignore=shutil.ignore_patterns("out", "out-*", "__pycache__", "*.pyc"))
    shutil.copy2(REPO / "config.h", dst / "config.h")


def main():
    if not M1N1_DIR:
        sys.exit("mutants.py: set HVXFER_M1N1_DIR to an m1n1 checkout (its src/hv_xfer.c is the unported device)")
    out = Path(sys.argv[1]).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    results, survivors = [], []
    for label, rel, old, new, expect in MUTANTS:
        with tempfile.TemporaryDirectory(prefix="hvxfer-mutant-", dir=os.environ.get("TMPDIR")) as tmp:
            tree = Path(tmp) / "repo"
            copy_tree(tree)
            target = tree / rel
            if old is None:
                text = (Path(M1N1_DIR) / "src/hv_xfer.c").read_text()
                text = text.replace('#include "hv_xfer.h"', '#include "hv/windows/hv_xfer.h"')
            else:
                text = target.read_text()
                if text.count(old) != 1:
                    sys.exit(f"mutant {label}: anchor not found exactly once in {rel}")
                text = text.replace(old, new)
            target.write_text(text)
            run_out = Path(tmp) / "out"
            env = dict(os.environ, HVXFER_OUT=str(run_out))
            proc = subprocess.run(["sh", str(tree / "tests/hv_xfer/run.sh")], env=env,
                                  capture_output=True, text=True, timeout=600)
            result_path = run_out / "result.json"
            result = json.loads(result_path.read_text()) if result_path.exists() else None
            failed = result["failed"] if result else ["no result.json"]
            crashed = result is None or result["verdict"] == "crash"
            killed = proc.returncode != 0 and result is not None and result["verdict"] in ("fail", "crash")
            named = all(name in failed for name in expect)
            ok = killed and named
            tail = (proc.stdout + proc.stderr).strip().splitlines()[-3:]
            entry = {"mutant": label, "file": rel, "killed": killed, "crashed": crashed,
                     "expected_failures": expect, "expected_seen": named,
                     "failed_checks": failed[:12], "failed_count": len(failed), "tail": tail}
            results.append(entry)
            if not ok:
                survivors.append(label)
            print(f"[{'KILLED' if ok else 'SURVIVED'}] {label}: "
                  f"{'crash' if crashed else f'{len(failed)} failing checks'}", flush=True)
    summary = {"schema": "aura.hvxfer.mutants.v1", "mutants": len(results),
               "killed": len(results) - len(survivors), "survivors": survivors, "results": results}
    (out / "mutants.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"mutants: {summary['killed']}/{summary['mutants']} killed")
    sys.exit(1 if survivors else 0)


if __name__ == "__main__":
    main()
