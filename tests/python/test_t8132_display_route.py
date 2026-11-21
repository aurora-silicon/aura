# SPDX-License-Identifier: MIT
"""Which display handoff a T8132 boot gets, with kboot.c's own routing function.

Failure model:
- Any headless T8132 (/arm-io/disp0 marked external or absent) whose ADT has a running dcpext0 or
  dcpext1 nub carrying the pinned 27.2 DCP image goes to the external-processor handoff of
  kboot_display_handoff.c (dt_set_display_handoff()).
- Every other T8132 boot keeps the M4 handoff (dt_set_m4_dcp_handoff()):
  - any board with an internal panel (/arm-io/disp0 present without `external`);
  - a headless board running any other DCP image (26.6.2, 27.0, 27.2.1, or missing UUID);
  - a headless board whose external DCP nubs are not running.
- The decision comes from the ADT alone: no board compatible or OS firmware release check.
- dt_set_display() asks before it takes the M4 path, and returns what the external handoff
  returns (always 0).
"""

import re
import subprocess
from pathlib import Path

import pytest
from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, name, kind="static bool "):
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct fake_adt {
    bool has_disp0;
    bool disp0_external;
    const char *uuid[2];
    u32 running[2];
    bool has_running[2];
};
static struct fake_adt cur;
static void *adt = &cur;

enum { NODE_DISP0 = 1, NODE_EXT0 = 2, NODE_EXT1 = 3 };

static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/disp0"))
        return cur.has_disp0 ? NODE_DISP0 : -1;
    if (!strcmp(path, "/arm-io/dcpext0/iop-dcpext0-nub"))
        return NODE_EXT0;
    if (!strcmp(path, "/arm-io/dcpext1/iop-dcpext1-nub"))
        return NODE_EXT1;
    return -1;
}

static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (len)
        *len = 0;
    if (node == NODE_DISP0) {
        if (!strcmp(name, "external") && cur.disp0_external)
            return "";
        return NULL;
    }
    int idx = node == NODE_EXT0 ? 0 : node == NODE_EXT1 ? 1 : -1;
    if (idx < 0)
        return NULL;
    if (!strcmp(name, "uuid") && cur.uuid[idx]) {
        if (len)
            *len = (u32)(strlen(cur.uuid[idx]) + 1);
        return cur.uuid[idx];
    }
    if (!strcmp(name, "running") && cur.has_running[idx]) {
        if (len)
            *len = sizeof(u32);
        return &cur.running[idx];
    }
    return NULL;
}

%s

static int check_case(const char *desc, struct fake_adt cfg, int want)
{
    cur = cfg;
    int got = dt_t8132_external_display();
    printf("%%s %%s: %%s\n", got == want ? "ok  " : "FAIL", desc,
           got ? "external handoff" : "M4 handoff");
    return got != want;
}

int main(void)
{
    static const char PINNED[] = "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF";
    static const char OTHER[] = "0F1E2D3C-4B5A-3968-8776-A5B4C3D2E1F0";
    int fails = 0;

    fails += check_case("headless disp0 external + dcpext0 pinned running",
                        (struct fake_adt){true, true, {PINNED, PINNED}, {1, 1}, {true, true}}, 1);
    fails += check_case("headless disp0 absent + dcpext1 pinned running",
                        (struct fake_adt){false, false, {OTHER, PINNED}, {0, 1}, {true, true}}, 1);
    fails += check_case("headless + other UUID (26.6.2 / 27.0 / 27.2.1)",
                        (struct fake_adt){true, true, {OTHER, OTHER}, {1, 1}, {true, true}}, 0);
    fails += check_case("headless + pinned UUID but not running",
                        (struct fake_adt){true, true, {PINNED, PINNED}, {0, 0}, {true, true}}, 0);
    fails += check_case("headless + missing UUID",
                        (struct fake_adt){true, true, {NULL, NULL}, {1, 1}, {true, true}}, 0);
    fails += check_case("internal panel (disp0 not external) + dcpext0 pinned running",
                        (struct fake_adt){true, false, {PINNED, PINNED}, {1, 1}, {true, true}}, 0);
    return fails != 0;
}
'''


def test_t8132_route_follows_adt_state(tmp_path):
    cc = host_cc()
    kboot = (ROOT / "src/kboot.c").read_text()
    src = tmp_path / "route.c"
    src.write_text(HARNESS % function(kboot, "dt_t8132_external_display"))
    binary = tmp_path / "route"
    try:
        subprocess.run([*cc, "-std=gnu11", "-Wall", "-Werror", str(src), "-o", str(binary)],
                       check=True)
    except FileNotFoundError:
        pytest.skip("no host C compiler")
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_route_uses_adt_not_board_or_os_version():
    body = function((ROOT / "src/kboot.c").read_text(), "dt_t8132_external_display")
    assert "apple,j773g" not in body
    assert "os_firmware" not in body
    assert "firmware_os_components" not in body
    assert '"external"' in body


def test_dt_set_display_asks_before_the_m4_handoff():
    kboot = (ROOT / "src/kboot.c").read_text()
    body = function(kboot, "dt_set_display", "static int ")
    branch = body[body.index("if (chip_id == T8132) {"):]
    branch = branch[:branch.index("dt_set_m4_dcp_handoff()")]
    assert re.search(r"if \(dt_t8132_external_display\(\)\)\s+return dt_set_display_handoff\(dt\);",
                     branch), branch
