# SPDX-License-Identifier: GPL-2.0-only
"""The T8132 USB3 bundle is chosen by the PHY compatibles, not by the board."""

from pathlib import Path


def test_t8132_usb3_selected_by_phy_compatible():
    source = (Path(__file__).resolve().parents[2] / "src/kboot_atc.c").read_text()
    body = source[source.index("static void dt_copy_atc_tunables("):]
    gate = body[:body.index("dt_copy_t8132_usb3_tunables(dt, adt_node, fdt_node);")]
    gate = gate[gate.rindex("if ("):]
    assert '"apple,t8132-atcphy"' in gate
    assert '"atc-phy,t8132"' in gate
    assert "apple,j" not in gate
    assert "chip_id" not in gate
