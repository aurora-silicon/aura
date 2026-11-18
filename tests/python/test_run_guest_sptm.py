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


def test_j613_has_no_ctrr_stash_write():
    runner = load_runner()
    class Proxy:
        def __init__(self):
            self.writes = []
        def read32(self, _pa):
            return 0x11111111
        def write32(self, pa, value):
            self.writes.append((pa, value))
        def call(self, *_args):
            return 0
    proxy = Proxy()
    runner.patch_sptm(proxy, {"sptm_virt_flush": 0x10,
                              "sptm_virt_ctrr_stash_off": 0x20}, 0x1000,
                      {"patches": [], "mte": None, "ctrr_stash_off": None})
    assert proxy.writes == []


def test_go_wake_alarm_covers_non_timer_commit():
    source = (REPO / "src/sptm_virt_s2r.c").read_text()
    assert """s2r_usb_setup();
    s2r_rtc_required = chip_id == T8122 && s2r_go && sptm_virt_s2r_wake_s;
    if (s2r_go && sptm_virt_s2r_wake_s)
        s2r_rtc_setup();""" in source
    commit = source[source.index("if (timer && !shadow_timer) {"):]
    commit = commit[:commit.index("\n    }") + 6]
    assert commit.index("rtc_ok = s2r_rtc_arm();") < commit.index("s2r_commit();")
    assert "if (!rtc_ok)" in commit
    assert commit.index("s2r_stop_before_timer") < commit.index("s2r_commit();")
    assert "s2r_rtc_required = chip_id == T8122" in source

    help_text = (REPO / "proxyclient/tools/run_guest_sptm.py").read_text()
    line = next(line for line in help_text.splitlines() if "AURA_S2R_WAKE_S=<s>" in line)
    assert "timer-only" not in line


def test_j613_go_accepts_rtc_wake_without_physical_wake(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_WFI_WATCH": "2",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    monkeypatch.delenv("AURA_J613_PHYSICAL_WAKE", raising=False)
    runner.validate_j613_go("disable-uart-irq=1 -nobsdmgroot wdt=-1")


def test_j613_go_still_requires_a_wake_source(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_DEBUG_PORT": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_WFI_WATCH": "2",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    monkeypatch.delenv("AURA_J613_PHYSICAL_WAKE", raising=False)
    monkeypatch.delenv("AURA_S2R_WAKE_S", raising=False)
    with pytest.raises(SystemExit, match="PHYSICAL_WAKE=1 or WAKE_S>0"):
        runner.validate_j613_go("disable-uart-irq=1 -nobsdmgroot wdt=-1")


def test_j613_go_rejects_verbose_guest_console(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_WFI_WATCH": "2",
        "AURA_S2R_QUIESCE": "24",
    }.items():
        monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match="no -v or serial="):
        runner.validate_j613_go("disable-uart-irq=1 serial=3 -v -nobsdmgroot wdt=-1")


def test_j613_go_requires_uart0_with_dockchannel_present(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_WFI_WATCH": "2",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    monkeypatch.delenv("AURA_XNU_SERIAL", raising=False)
    with pytest.raises(SystemExit, match="XNU_SERIAL=uart0"):
        runner.validate_j613_go("disable-uart-irq=1 -nobsdmgroot wdt=-1")


def test_j613_go_requires_polled_vuart(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_WFI_WATCH": "2",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match="disable-uart-irq=1"):
        runner.validate_j613_go("-nobsdmgroot wdt=-1")


def test_j613_go_rejects_removed_dockchannel(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_HIDE_DOCKCHANNEL": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_WFI_WATCH": "2",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match="DockChannel node present"):
        runner.validate_j613_go("disable-uart-irq=1 -nobsdmgroot wdt=-1")


def test_j613_go_requires_hypervisor_liveness(monkeypatch):
    runner = load_runner()
    for name, value in {
        "AURA_S2R_TRACE": "1",
        "AURA_S2R_DROP_USB": "1",
        "AURA_S2R_USB_RETAKE": "1",
        "AURA_S2R_CRUMBS": str(3 | 32 | 64),
        "AURA_J613_RECOVERY_SECS": "0",
        "AURA_S2R_WAKE_S": "30",
        "AURA_DEBUG_PORT": "1",
        "AURA_XNU_SERIAL": "uart0",
        "AURA_KEEP_ERRH": "1",
        "AURA_AMCC_CLEAR": "1",
    }.items():
        monkeypatch.setenv(name, value)
    monkeypatch.delenv("AURA_WFI_WATCH", raising=False)
    with pytest.raises(SystemExit, match="AURA_WFI_WATCH>0"):
        runner.validate_j613_go("disable-uart-irq=1 -nobsdmgroot wdt=-1")
