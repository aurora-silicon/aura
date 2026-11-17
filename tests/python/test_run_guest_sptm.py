# SPDX-License-Identifier: MIT
"""The virtualized SPTM's host tools import and start (proxyclient/tools)."""

import ast
import importlib
import importlib.util
import os
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

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


@pytest.mark.parametrize(("chip", "model", "override", "expected_mib"), [
    (0x8142, "Mac17,3", None, 112),
    (0x8142, "Mac17,4", None, 128),
    (0x8140, "Mac17,2", None, 128),
    (0x6041, "Mac16,7", None, 128),
    (0x8142, "Mac17,3", "128", 128),
    (0x6041, "Mac16,7", "96", 96),
])
def test_aura_heap_reservation_before_proxy_limit(chip, model, override, expected_mib, monkeypatch):
    monkeypatch.syspath_prepend(str(REPO / "proxyclient"))
    from aura import proxyutils

    path = REPO / "proxyclient/tools/run_guest_sptm.py"
    spec = importlib.util.spec_from_file_location("run_guest_sptm", path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    if override is None:
        monkeypatch.delenv("AURA_AURA_HEAP_MIB", raising=False)
    else:
        monkeypatch.setenv("AURA_AURA_HEAP_MIB", override)
    monkeypatch.delenv("AURAHEAP", raising=False)

    proxy = MagicMock()
    proxy.get_bootargs_rev.return_value = (0x1000, 3)
    proxy.heapblock_alloc.return_value = 0x40000000
    proxy.iface.readstruct.return_value = SimpleNamespace(
        devtree=0x80001000, virt_base=0x80000000, phys_base=0x100000, devtree_size=32,
    )
    proxy.iface.readmem.return_value = b"adt"
    tree = MagicMock(model=model)
    tree.__getitem__.return_value = SimpleNamespace(chip_id=chip)
    monkeypatch.setattr(proxyutils.adt, "load_adt", lambda data: tree)

    utils = proxyutils.ProxyUtils(proxy, heap_size=2 * 1024 * 1024, aura_heap=tool.aura_heap_size)
    proxy.heapblock_set_limit.assert_called_once_with(0x40000000 + expected_mib * 1024 * 1024)
    if override is None:
        calls = [call[0] for call in proxy.mock_calls]
        assert calls.index("iface.readmem") < calls.index("heapblock_set_limit")
    else:
        proxy.iface.readmem.assert_not_called()
    assert utils.adt.model == model
    proxy.iface.readmem.assert_called_once_with(0x101000, 32)


@pytest.mark.parametrize(("offset", "instruction", "selected", "hidden_radio"), [
    (0x1BDF0C, 0xAA0003E8, True, False),   # T6041 uses only the NVRAM write quirk.
    (0x3B53C20, 0x540002A9, True, True),   # T8142 keeps its distinct fingerprint.
    (0x3B53C20, 0xD503201F, False, False), # The release string alone is insufficient.
])
def test_same_release_selects_exact_kernel(offset, instruction, selected, hidden_radio, monkeypatch):
    path = REPO / "proxyclient/tools/run_guest_sptm.py"
    spec = importlib.util.spec_from_file_location("run_guest_sptm", path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    monkeypatch.setattr(tool, "kernelcache_word",
                        lambda _kernel, seg, off: instruction if (seg, off) == ("__TEXT_EXEC", offset) else None)
    release, quirks = tool.xnu_quirks(b"xnu-13432.41.4~")
    assert (release == "xnu-13432.41.4") == selected
    assert ("/arm-io/centauri" in quirks["hide"]) == hidden_radio


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
