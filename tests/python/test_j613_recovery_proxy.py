"""Keep the J613 recovery command independent of host MMIO and USB recovery."""
from pathlib import Path


def test_recovery_proxy_is_firmware_only_and_terminal():
    repo = Path(__file__).resolve().parents[2]
    proxy_c = (repo / "src/proxy.c").read_text()
    case = proxy_c[proxy_c.index("case P_WDT_RECOVERY:"):]
    case = case[:case.index("case ", 5)]

    assert "wdt_arm_recovery(request->args[0])" in case
    assert "while (true)" in case
    assert 'sysop("wfe")' in case
    assert "uartproxy_run" not in case
    assert "read32" not in case
    assert "write32" not in case

    proxy_py = (repo / "proxyclient/aura/proxy.py").read_text()
    method = proxy_py[proxy_py.index("    def wdt_recovery("):]
    method = method[:method.index("    def ", 5)]
    assert "no_reply=True" in method
    assert ".write32(" not in method
