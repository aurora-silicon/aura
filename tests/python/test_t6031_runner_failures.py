# SPDX-License-Identifier: MIT
"""Black-box runner control-flow checks with synthetic compiler/host outputs.

These checks establish execution and comparison behavior, not hardware behavior
or equivalence between production source revisions.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

import pytest


REPO = Path(__file__).resolve().parents[2]
HANDOFF = Path(os.environ.get("AURA_T6031_HANDOFF_RUNNER_SOURCE",
                             REPO / "tests/run-t6031-handoff.sh"))
UNCHANGED = Path(os.environ.get("AURA_T6031_UNCHANGED_RUNNER_SOURCE",
                               REPO / "tests/run-t6031-unchanged.sh"))

FAKE_TOOL = r'''
import json, os, pathlib, shlex, sys
mode = sys.argv[1]
config = json.loads(pathlib.Path(os.environ["T6031_FAKE_CONFIG"]).read_text())
if mode in ("rustc", "dtc", "compile"):
    args = sys.argv[2:]
    output = pathlib.Path(args[args.index("-o") + 1])
    if mode != "compile":
        output.write_bytes(b"synthetic tool output")
        sys.exit(0)
    role = "handoff" if output.name == "t6031-host" else output.parent.name
    settings = config.get(role, {})
    if settings.get("write_binary", True):
        command = " ".join(shlex.quote(x) for x in [sys.executable, __file__, "run", role])
        output.write_text('#!/bin/sh\nexec ' + command + ' "$@"\n')
        output.chmod(0o755)
    arguments = []
else:
    role = sys.argv[2]
    settings = config.get(role, {})
    arguments = sys.argv[3:]
with open(os.environ["T6031_FAKE_EVENTS"], "a") as log:
    log.write(json.dumps([mode, role] + arguments) + "\n")
if mode == "compile":
    sys.exit(settings.get("build_rc", 0))
scenario = arguments[2] if role == "handoff" else arguments[1] if role.startswith("disp-") else arguments[0]
if scenario == "list":
    sys.stdout.write(settings.get("list_text", "one\ntwo\n"))
    sys.exit(settings.get("list_rc", 0))
if role.startswith("disp-"):
    pathlib.Path(arguments[2]).write_bytes(b"compared DT bytes" + settings.get("dt_suffix", "").encode())
    print("handoff published")
print(("FAIL " if scenario == settings.get("fail_scenario") else "ok ") + scenario + settings.get("output_suffix", ""))
sys.exit(9 if scenario == settings.get("fail_scenario") else 0)
'''


@pytest.fixture
def runner(tmp_path):
    tree = tmp_path / "tree"
    for directory in ("src", "rust/src", "tests/t6031_host", "tests/g15_host",
                      "tests/t8122_display_host"):
        (tree / directory).mkdir(parents=True, exist_ok=True)
    for name, source in (("handoff", HANDOFF), ("unchanged", UNCHANGED)):
        (tree / f"tests/run-t6031-{name}.sh").write_bytes(source.read_bytes())
    (tree / "rust/src/adt.rs").write_text("// from_raw_parts(self.name.as_ptr() as *const u8, 32)\n")
    (tree / "src/kboot.c").write_text("""
static int dt_fill_pmp_from(void) { return 0;
}
static int dt_fill_pmp(void) { return 0;
}
static void dt_pass_pmp(void) {
}
static int dt_set_pmp_t6031(void) { return 0;
}
static const struct pmp_values_soc pmp_values_t6031 = {
};
""")
    for name in ("kboot_display_handoff.c", "kboot_gpu.c", "kboot_gpu26.c",
                 "kboot_gpu_t6031.c", "kboot_gpu_t8122_power.c", "kboot_t6031.c",
                 "firmware.c", "pmgr.c"):
        (tree / "src" / name).write_text("/* synthetic input */\n")
    for name in ("t6031_host", "g15_host", "t8122_display_host"):
        (tree / "tests" / name / "stub.h").write_text("/* synthetic header */\n")
    for name in ("t6031_host.c", "kboot_gpu_g15_host.c", "t8122_display_host.c",
                 "t6031-j516c-handoff.dts", "t8122-j613-display.dts"):
        (tree / "tests" / name).write_text("/* synthetic fixture */\n")
    (tree / "tests/t6031_adt.py").write_text(
        "import pathlib,sys\npathlib.Path(sys.argv[2]).write_bytes(b'synthetic ADT')\n")
    allow = tmp_path / "allowlist.txt"
    allow.write_text("synthetic input\n")
    tool = tmp_path / "fake_tool.py"
    tool.write_text(FAKE_TOOL)
    for name, mode in (("fake-cc", "compile"), ("rustc", "rustc"), ("fake-dtc", "dtc")):
        wrapper = tmp_path / name
        wrapper.write_text("#!/bin/sh\nexec " + " ".join(
            shlex.quote(x) for x in [sys.executable, str(tool), mode]) + ' "$@"\n')
        wrapper.chmod(0o755)
    subprocess.run(["git", "init", "-q", str(tree)], check=True)
    subprocess.run(["git", "add", "."], cwd=tree, check=True)
    subprocess.run(["git", "-c", "user.name=Runner Test", "-c",
                    "user.email=runner@example.invalid", "-c", "commit.gpgsign=false",
                    "-c", "core.hooksPath=/dev/null", "commit", "-qm", "synthetic inputs"],
                   cwd=tree, check=True)
    config_path = tmp_path / "config.json"
    events = tmp_path / "events.jsonl"
    environment = os.environ.copy()
    for variable in ("T6031_DTB", "T6031_OUT", "CC", "DTC"):
        environment.pop(variable, None)
    environment.update(CC="fake-cc", DTC="fake-dtc",
                       PATH=str(tmp_path) + os.pathsep + environment["PATH"],
                       T6031_FAKE_CONFIG=str(config_path), T6031_FAKE_EVENTS=str(events))

    def run(kind="handoff", config=None, selection=()):
        config_path.write_text(json.dumps({} if config is None else config))
        script = tree / f"tests/run-t6031-{kind}.sh"
        args = [str(allow), *selection] if kind == "handoff" else ["HEAD"]
        result = subprocess.run(["sh", str(script), *args], cwd=tree, env=environment,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        calls = [json.loads(line) for line in events.read_text().splitlines()] if events.exists() else []
        return result, calls

    return run


def scenario_calls(calls, role):
    runs = [c[2:] for c in calls if c[:2] == ["run", role]]
    index = 2 if role == "handoff" else 1 if role.startswith("disp-") else 0
    return [args[index] for args in runs if args[index] != "list"]


def rejected(result):
    assert result.returncode != 0, result.stdout
    assert "every scenario passes" not in result.stdout, result.stdout
    assert "PASS: every other" not in result.stdout, result.stdout


@pytest.mark.parametrize("kind,role", [("handoff", "handoff"), ("unchanged", "g15-base")])
@pytest.mark.parametrize("text", ["", "one\n"])
def test_failed_discovery_never_executes_partial_stdout(runner, kind, role, text):
    result, calls = runner(kind, {role: {"list_rc": 8, "list_text": text}})
    rejected(result)
    assert any(c[:2] == ["run", role] and c[-1] == "list" for c in calls)
    assert not scenario_calls(calls, role)
    assert not scenario_calls(calls, "g15-new")


@pytest.mark.parametrize("kind,role", [("handoff", "handoff"), ("unchanged", "g15-base")])
@pytest.mark.parametrize("text", ["", " \t\n\t \n"])
def test_empty_discovery_is_not_a_pass(runner, kind, role, text):
    result, calls = runner(kind, {role: {"list_text": text}})
    rejected(result)
    assert not scenario_calls(calls, role)


@pytest.mark.parametrize("selection", ["", " \t\n "])
def test_empty_handoff_selection_is_not_a_pass(runner, selection):
    result, calls = runner(selection=(selection,))
    rejected(result)
    assert not scenario_calls(calls, "handoff")


@pytest.mark.parametrize("selection,expected", [((), ["one", "two"]), (("two",), ["two"])])
def test_handoff_positive_counts_require_executions(runner, selection, expected):
    result, calls = runner(selection=selection)
    assert result.returncode == 0, result.stdout
    assert scenario_calls(calls, "handoff") == expected
    assert f"T6031 handoff: {len(expected)} scenarios; every scenario passes" in result.stdout


def test_handoff_scenario_failure_is_not_a_pass(runner):
    result, calls = runner(config={"handoff": {"fail_scenario": "one"}})
    rejected(result)
    assert scenario_calls(calls, "handoff") == ["one", "two"]


def test_unchanged_positive_control_executes_actual_comparisons(runner):
    result, calls = runner("unchanged")
    assert result.returncode == 0, result.stdout
    assert scenario_calls(calls, "g15-base") == ["one", "two"]
    assert scenario_calls(calls, "g15-new") == ["one", "two"]
    for role in ("disp-base", "disp-new"):
        assert len(scenario_calls(calls, role)) == 25
    assert "G15 GPU setup: 2 scenarios identical" in result.stdout
    assert "Display handoff: 25 J613" in result.stdout
    assert "PASS: every other SoC's handoff results" in result.stdout


@pytest.mark.parametrize("role,setting", [("g15-new", "output_suffix"),
                                         ("disp-new", "output_suffix"),
                                         ("disp-new", "dt_suffix")])
def test_unchanged_comparison_difference_is_not_a_pass(runner, role, setting):
    result, calls = runner("unchanged", {role: {setting: " DIFFERENT"}})
    rejected(result)
    assert scenario_calls(calls, role)
    assert "differs from HEAD" in result.stdout


def test_unchanged_one_sided_scenario_failure_is_not_a_pass(runner):
    result, calls = runner("unchanged", {"g15-new": {"fail_scenario": "one"}})
    rejected(result)
    assert scenario_calls(calls, "g15-new") == ["one", "two"]
    assert "G15 scenario one differs" in result.stdout
