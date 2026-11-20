# SPDX-License-Identifier: MIT
"""Which display handoff a T8132 boot gets, with kboot.c's own routing function.

Failure model, recorded before kboot.c routed any T8132 elsewhere:
- Only a J773g whose OS firmware components are 27.2 goes to the external-processor handoff of
  kboot_display_handoff.c (dt_set_display_handoff()). Its HDMI port is dcpext0's there, and the
  M4 handoff (written for 26.6.2) must not touch its DCPs.
- Every other T8132 boot keeps the M4 handoff (dt_set_m4_dcp_handoff()) exactly as before:
  - the J773g on 26.6.2;
  - a J773g on 26.7 with an iBoot of the 18000.161.10 train, which loads the 26.6.2 images;
  - the J773g on 27.0 or on an unknown release, which the M4 handoff refuses;
  - the J713 on any firmware, 27.2 included.
- dt_set_display() asks before it takes the M4 path, and returns what the external handoff
  returns (always 0).
"""

import re
import subprocess
from pathlib import Path

import pytest
from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]
LIBFDT = ["fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
          "fdt_strerror.c"]


def function(source, name, kind="static bool "):
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
enum fw_version { V_UNKNOWN, V26_6_2, V26_7, V27_0, V27_2 };
static enum fw_version components;
static enum fw_version firmware_os_components(void) { return components; }
static char dtbuf[4096];
static void *dt = dtbuf;
%s
static int route(const char *root, size_t len, enum fw_version fw)
{
    components = fw;
    fdt_create_empty_tree(dt, sizeof(dtbuf));
    fdt_setprop(dt, 0, "compatible", root, len);
    return dt_t8132_external_display();
}
int main(void)
{
    static const char j773g[] = "apple,j773g\0apple,t8132\0apple,arm-platform";
    static const char j713[] = "apple,j713\0apple,t8132\0apple,arm-platform";
    int fails = 0;
#define CASE(root, fw, want)                                                                       \
    do {                                                                                           \
        int got = route(root, sizeof(root), fw);                                                   \
        printf("%%s %%s %%s: %%s\n", got == want ? "ok  " : "FAIL", #root, #fw,                   \
               got ? "external handoff" : "M4 handoff");                                           \
        fails += got != want;                                                                      \
    } while (0)
    CASE(j773g, V27_2, 1);
    CASE(j773g, V26_6_2, 0);
    CASE(j773g, V27_0, 0);
    CASE(j773g, V_UNKNOWN, 0);
    CASE(j713, V27_2, 0);
    CASE(j713, V26_6_2, 0);
    return fails != 0;
}
'''


def test_t8132_route_follows_board_and_firmware(tmp_path):
    cc = host_cc()
    kboot = (ROOT / "src/kboot.c").read_text()
    src = tmp_path / "route.c"
    src.write_text(HARNESS % function(kboot, "dt_t8132_external_display"))
    lib = ROOT / "src/libfdt"
    binary = tmp_path / "route"
    try:
        subprocess.run([*cc, "-std=gnu11", "-Wall", "-Werror", "-I", str(lib), str(src),
                        *(str(lib / f) for f in LIBFDT), "-o", str(binary)], check=True)
    except FileNotFoundError:
        pytest.skip("no host C compiler")
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_26_7_train_keeps_the_m4_handoff():
    """The route asks firmware_os_components(), which maps an 18000.161.10 iBoot to 26.6.2."""
    body = function((ROOT / "src/kboot.c").read_text(), "dt_t8132_external_display")
    assert "firmware_os_components() == V27_2" in body
    assert "os_firmware.version" not in body


def test_dt_set_display_asks_before_the_m4_handoff():
    kboot = (ROOT / "src/kboot.c").read_text()
    body = function(kboot, "dt_set_display", "static int ")
    branch = body[body.index("if (chip_id == T8132) {"):]
    branch = branch[:branch.index("dt_set_m4_dcp_handoff()")]
    assert re.search(r"if \(dt_t8132_external_display\(\)\)\s+return dt_set_display_handoff\(dt\);",
                     branch), branch
