# SPDX-License-Identifier: MIT
"""The J773g boot display path keeps its four hardware invariants together."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    end, depth = text.index("{", start) + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def test_j773g_display_is_admitted_and_never_powered_or_reset():
    display = (ROOT / "src/display.c").read_text()
    dcp = (ROOT / "src/dcp.c").read_text()
    config = re.search(r"static const display_config_t display_config_j773g = \{.*?\n\};",
                       display, re.S).group(0)
    init = function(display, "int display_init(void)")
    assert '.dptx_phy = "/arm-io/dptx-phy"' in config
    assert '.dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio0"' in config
    assert ".fw_version = V26_6_2" in config
    assert ".no_reset = true" in config
    assert "kboot_is_j773g_machine()" in init
    assert re.search(r"if \(!cfg->no_reset\)\s+pmgr_adt_power_enable\(cfg->dcp\);", dcp)
    assert re.search(r"ret > 0 && !cfg->no_reset", dcp)


def test_t8122_phy_has_all_supported_rates_and_target_from_adt():
    source = (ROOT / "src/dcp/dptx_phy.c").read_text()
    table = re.search(r"t8122_pll_cfgs\[\] = \{(.*?)\n\};", source, re.S).group(1)
    assert [int(code, 16) for code in re.findall(r"\{(0x[0-9a-f]+),", table)] == [6, 10, 20, 30]
    assert 'adt_is_compatible(adt, node, "dptx-phy,t8122")' in source
    assert '"dp-switch-dfp-endpoint"' in source
    assert re.search(r"case DPTX_PHY_T8122:\s+return phy->dfp_endpoint;", source)


def test_j773g_rtkit_session_restores_dva_bit_and_echoes_oslog():
    dcp = (ROOT / "src/dcp.c").read_text()
    rtkit = (ROOT / "src/rtkit.c").read_text()
    assert "rtkit_set_dva_base(dcp->rtkit, dart_dva_base(dcp->dart_dcp));" in dcp
    assert "rtkit_set_oslog_echo(dcp->rtkit, true);" in dcp
    assert re.search(r"OSLOG_NOTIFY_TYPE\s+GENMASK\(59, 56\)", rtkit)
    oslog = rtkit[rtkit.index("case RTKIT_EP_OSLOG:"):]
    assert "rtkit_send(rtk, msg)" in oslog
    assert "firmware's own; not answered" in oslog


def test_j773g_services_dcp_while_waiting_for_hpd():
    source = (ROOT / "src/display.c").read_text()
    configure = function(source, "int display_configure(")
    assert "bool j773g = kboot_is_j773g_machine();" in configure
    assert "int status_retries = j773g ? 150" in configure
    assert "dcp_work(dcp);\n        hpd = dcp_ib_get_hpd" in configure
    assert "if (retries < status_retries)\n            mdelay(DISPLAY_STATUS_DELAY);" in configure
    assert "while (!timeout_expired" not in configure
