# SPDX-License-Identifier: MIT
"""dt_set_display()'s T8132 route, from kboot.c's own text, against stubs.

The external processors follow the internal M4 DCP handoff's reported outcome: there is no root
compatible, chip id or OS release test in the route. dt_set_dcpext_handoff() is given that outcome
whatever it is, with kboot.c's firmware-27 clocks rule (dt_dcp_fw_clocks()); the firmware-ready path
(dt_reserve_j713_dcpext()) runs only when the ADT path declines (firmware not given clocks), and only
after an internal handoff that was not refused.
"""
import subprocess
from pathlib import Path

import pytest
from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def route_block():
    kboot = (ROOT / "src/kboot.c").read_text()
    body = kboot[kboot.index("static int dt_set_display(void)\n{"):]
    start = body.index("    if (chip_id == T8132) {")
    end = body.index("\n    }\n", start) + len("\n    }\n")
    return body[start:end]


def test_route_has_no_board_or_release_test():
    block = route_block()
    for banned in ("fdt_node_check_compatible", "apple,j", "os_firmware", "firmware_os_components",
                   "V27_", "V26_"):
        assert banned not in block, banned


# (M4 return, M4 outcome, dcpext path claims the image) -> (outcome passed on, firmware-ready ran)
CASES = [
    (0, "DCP_INTERNAL_COMPLETE", 1, True, False),   # J713 27.2: ADT path, beside the panel
    (0, "DCP_INTERNAL_NONE", 1, True, False),       # J713 27.2, walk failed: ADT path says no
    (-1, "DCP_INTERNAL_NONE", 1, True, False),      # refused internal handoff: nothing
    (-1, "DCP_INTERNAL_NO_PANEL", 1, True, False),  # J773g 27.2: headless ADT path
    (0, "DCP_INTERNAL_COMPLETE", 0, True, True),    # 26.6.2 image: firmware-ready path, as before
    (0, "DCP_INTERNAL_NONE", 0, True, True),        # 26.6.2 without the maps: as before
    (-1, "DCP_INTERNAL_NO_PANEL", 0, True, False),  # J773g 26.6.2: nothing, as before
    (-1, "DCP_INTERNAL_NONE", 0, True, False),      # refused, unknown image: nothing, as before
]


@pytest.mark.parametrize("m4_ret,outcome,claims,called,ready", CASES)
def test_route(tmp_path, m4_ret, outcome, claims, called, ready):
    header = (ROOT / "src/kboot.h").read_text()
    start = header.index("enum dcp_internal {")
    enum = header[start:header.index("\n};", start) + 3]
    source = f'''
#include <assert.h>
#include <stdio.h>
#define T8132 0x8132
{enum}
static int chip_id = T8132, seen = -1, ready_calls;
static void *dt;
static int dt_set_m4_dcp_handoff(enum dcp_internal *internal)
{{
    *internal = {outcome};
    return {m4_ret};
}}
static int dt_dcp_fw_clocks(void) {{ return 1; }}
static int dt_set_dcpext_handoff(void *tree, enum dcp_internal internal, int fw_clocks)
{{
    (void)tree;
    assert(fw_clocks == 1);
    seen = internal;
    return {claims};
}}
static void dt_reserve_j713_dcpext(void) {{ ready_calls++; }}
static int route(void)
{{
{route_block()}
    return 1;
}}
int main(void)
{{
    assert(route() == 0);
    assert(seen == ({outcome if called else -1}));
    assert(ready_calls == {1 if ready else 0});
    return 0;
}}
'''
    (tmp_path / "route.c").write_text(source)
    binary = tmp_path / "route"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", str(tmp_path / "route.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
