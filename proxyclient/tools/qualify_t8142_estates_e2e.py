#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay qualify_t8142_estates.py's real control flow against modeled failures.

Each case injects one failure from docs/t8142-ecpu-stepup.md into the tool's
ModelTarget and checks the stop reason, whether a write preceded it, that no
frequency request followed it, and that only the planned E-register values were
written. The CLI dry run is run too. No device is opened.
"""
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import random
import sys

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parent))
import qualify_t8142_estates as tool  # noqa: E402

ALL = [3, 4, 5, 6, 7, 8, 9]
# name, faults, max_state, return, result, stop reason, stopped after a write, accepted states
CASES = [
    ("nominal", {}, 9, True, "passed", None, False, ALL),
    ("nominal-e4-no-return", {}, 4, False, "passed", None, False, [3, 4]),
    ("wrong-chip", {"chip": 0x8152}, 9, True, "stopped", "wrong chip", False, []),
    ("ladder-mismatch", {"ladder": True}, 9, True, "stopped", "E ladder differs from J813", False, []),
    ("handoff-mismatch", {"handoff": 0x400103}, 9, True, "stopped",
     "command registers are not the J813 handoff", False, []),
    ("start-fails", {"start_fails": True}, 9, True, "stopped", "proxy refused", False, []),
    ("baseline-throttle", {"throttle": 2}, 9, True, "stopped",
     "out-of-tolerance frequency under load", False, []),
    ("baseline-exception", {"exception": 2}, 9, True, "stopped", "exception", False, []),
    ("busy-race-e3", {"busy_race": 3}, 9, True, "stopped", "busy before write", False, []),
    ("busy-race-e5", {"busy_race": 5}, 9, True, "stopped", "busy before write", True, [3, 4]),
    ("changed-race-e3", {"changed_race": 3}, 9, True, "stopped",
     "register changed before write", False, []),
    ("busy-timeout-e5", {"busy_timeout": 5}, 9, True, "stopped", "busy timeout", True, [3, 4]),
    ("wrong-echo-e4", {"wrong_echo": 4}, 9, True, "stopped", "wrong echo", True, [3]),
    ("unrelated-bit-e3", {"bit_changes": 3}, 9, True, "stopped", "unrelated E bits changed", True, []),
    ("slow-busy-e6", {"slow_busy": 6}, 9, True, "stopped",
     "slower than the resident setter bound", True, [3, 4, 5]),
    ("p-changes-e3", {"p_changes": 3}, 9, True, "stopped", "P command register changed", True, []),
    ("e-out-of-tolerance-e7", {"e_slow": 7}, 9, True, "stopped", "out-of-tolerance frequency", True,
     [3, 4, 5, 6]),
    ("boot-core-moves-e4", {"boot_shift": 4}, 9, True, "stopped", "out-of-tolerance frequency", True,
     [3]),
    ("exception-e8", {"exception": 8}, 9, True, "stopped", "exception", True, [3, 4, 5, 6, 7]),
    ("wrong-result-e5", {"wrong_result": 5}, 9, True, "stopped", "wrong loop result", True, [3, 4]),
    ("throttle-under-load-e9", {"throttle": 9}, 9, True, "stopped",
     "out-of-tolerance frequency under load", True, [3, 4, 5, 6, 7, 8]),
    ("hung-secondary-e6", {"hang": 6}, 9, True, "stopped", "silent proxy", True, [3, 4, 5]),
    ("silent-after-write-e3", {"silent_read": 3}, 9, True, "stopped", "silent proxy", True, []),
    ("silent-during-step-e4", {"silent_step": 4}, 9, True, "stopped", "silent proxy", True, [3]),
]


def run_case(name, faults, max_state, back, result, reason, after_write, accepted, out):
    code, offsets = CODE
    models = []
    def open_target():
        models.append(tool.ModelTarget(offsets, faults))
        return models[-1]
    generator = random.Random(name)
    qualifier = tool.Qualifier(open_target, "model", code, offsets, max_state, back, 3.0,
                               lambda: generator.getrandbits(64), out / f"{name}.json")
    report = qualifier.run()
    model = models[0]
    events = report["events"]
    assert report["result"] == result, (name, report.get("stop"))
    assert report["accepted_states"] == accepted, (name, report["accepted_states"])
    stop = report.get("stop")
    if reason is None:
        assert stop is None
        expected_final = tool.HANDOFF_STATE if back else max_state
        assert model.e & tool.STATE_MASK == expected_final and model.p_reg == tool.P_HANDOFF
    else:
        assert stop["reason"] == reason, (name, stop)
        assert stop["after_write"] == after_write, (name, stop)
        assert report["frequency_requests_closed"] == after_write
        stop_index = next(i for i, e in enumerate(events) if e["kind"] == "stop")
        assert all(e["kind"] != "call:dvfs_step" for e in events[stop_index:]), name
        if stop["proxy_silent"]:
            assert events[-1]["kind"] == "stop", name
        # The read-only final snapshot needs a responsive proxy and loaded code.
        loaded = any(e["kind"] == "load_code" for e in events)
        assert ("final" in report) == (loaded and not stop["proxy_silent"]), name
    # Every write is the planned value for its state, in order; the P register is never written.
    planned = [tool.command_value(tool.E_HANDOFF, s) for s in range(3, max_state + 1)]
    if back and reason is None:
        planned.append(tool.command_value(tool.E_HANDOFF, tool.HANDOFF_STATE))
    assert model.writes == planned[:len(model.writes)], (name, [hex(w) for w in model.writes])
    if not after_write and reason is not None:
        assert model.writes == [] or name.startswith("busy-race"), name
    steps = [e for e in events if e["kind"] == "call:dvfs_step"]
    assert len(steps) == len({e["state"] for e in steps}), "a state was requested twice"
    return {"case": name, "faults": faults, "result": report["result"],
            "stop": stop and {k: stop[k] for k in ("reason", "after_write", "proxy_silent")},
            "accepted": report["accepted_states"], "writes": [hex(w) for w in model.writes],
            "events": len(events),
            "artifact_sha256": hashlib.sha256((out / f"{name}.json").read_bytes()).hexdigest()}


def cli_cases(out):
    results = []
    for argv, code in ((["--dry-run", "--output", str(out / "dry-run.json")], 0),
                       (["--dry-run", "--device", "/dev/null"], 2), ([], 2),
                       (["--dry-run", "--max-state", "10"], 2), (["--dry-run", "--max-state", "2"], 2),
                       (["--dry-run", "--soak-seconds", "0.5"], 2)):
        stdout = io.StringIO()
        try:
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                status = tool.main(argv)
        except SystemExit as exit:
            status = exit.code
        assert status == code, (argv, status)
        results.append({"argv": argv, "exit": status})
    assert "aura.proxy" not in sys.modules, "a dry run imported the device transport"
    text = stdout_of_dry_run(out)
    assert "PLAN write64 0x210e20020 <- 0x2400103 (E3)" in text
    return results


def stdout_of_dry_run(out):
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        assert tool.main(["--dry-run", "--max-state", "9", "--output", str(out / "dry-run-2.json")]) == 0
    (out / "dry-run.txt").write_text(stdout.getvalue())
    return stdout.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    global CODE
    CODE = tool.assemble()
    cases = [run_case(*case, out) for case in CASES]
    cli = cli_cases(out)
    sources = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
               (HERE, HERE.parent / "qualify_t8142_estates.py",
                HERE.parents[2] / "docs/t8142-ecpu-stepup.md")}
    report = {"result": "passed", "hardware_accessed": False,
              "scope": "Real Qualifier control flow and CLI; modeled target, no MMIO or measurement",
              "code_sha256": hashlib.sha256(CODE[0]).hexdigest(), "cases": cases, "cli": cli,
              "sources": sources,
              "replay": [sys.executable, str(HERE), "--out", "NEW_EMPTY_DIRECTORY"]}
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(cases)} control-flow cases, {len(cli)} CLI cases: {out / 'result.json'}")


if __name__ == "__main__":
    main()
