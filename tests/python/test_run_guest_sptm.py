# SPDX-License-Identifier: MIT
"""The virtualized SPTM's host tools import and start (proxyclient/tools)."""

import ast
import importlib
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOLS = ("run_guest_sptm.py", "console_tail.py")


def load_runner():
    spec = importlib.util.spec_from_file_location(
        "run_guest_sptm", REPO / "proxyclient/tools/run_guest_sptm.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def aura_imports(path):
    """Every `from aura... import name` in a tool, including those inside functions."""
    for node in ast.walk(ast.parse(path.read_text())):
        if isinstance(node, ast.ImportFrom) and node.module and node.module.split(".")[0] == "aura":
            for alias in node.names:
                yield node.module, alias.name


@pytest.mark.parametrize("tool", TOOLS)
def test_aura_imports_resolve(tool, monkeypatch):
    monkeypatch.syspath_prepend(str(REPO / "proxyclient"))
    for module, name in aura_imports(REPO / "proxyclient/tools" / tool):
        mod = importlib.import_module(module)
        submodule = hasattr(mod, "__path__") and importlib.util.find_spec(f"{module}.{name}")
        assert hasattr(mod, name) or submodule, f"{tool}: {module} has no {name}"


@pytest.mark.parametrize("tool", TOOLS)
def test_help(tool):
    env = dict(os.environ, PYTHONPATH=str(REPO / "proxyclient"))
    r = subprocess.run([sys.executable, str(REPO / "proxyclient/tools" / tool), "--help"],
                       capture_output=True, text=True, env=env, timeout=60)
    assert r.returncode == 0, r.stderr
    assert "usage:" in r.stdout


@pytest.mark.parametrize(("port", "is_kis", "expected"), (
    ("/dev/ttyAMA10", False, True),
    ("/dev/ttyACM0", False, False),
    ("/dev/cu.usbmodemP_01", False, False),
    ("/dev/tty.kis-1", True, False),
))
def test_post_entry_drain_needed(port, is_kis, expected):
    runner = load_runner()
    iface = type("Iface", (), {
        "devpath": port,
        "dev": type("Serial", (), {"port": port})(),
        "is_kis": is_kis,
    })()
    assert runner.post_entry_drain_needed(iface) is expected


def test_post_entry_drain_resolves_m1n1_symlink(monkeypatch):
    runner = load_runner()
    monkeypatch.setattr(runner.os.path, "realpath", lambda path: "/dev/ttyACM0")
    iface = type("Iface", (), {
        "devpath": "/dev/m1n1",
        "dev": type("Serial", (), {"port": "/dev/m1n1"})(),
        "is_kis": False,
    })()
    assert runner.post_entry_drain_needed(iface) is False


def test_no_enter_can_return_without_reopening_stage1():
    source = (REPO / "proxyclient/tools/run_guest_sptm.py").read_text()
    assert 'if env_on("AURA_RETURN_AFTER_NO_ENTER")' in source
    assert 'p.reboot()' in source[source.index('if args.no_enter:'):]
