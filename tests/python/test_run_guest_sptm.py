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
