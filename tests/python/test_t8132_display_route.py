# SPDX-License-Identifier: MIT
"""Which display handoff a T8132 boot gets, via the shared kboot_display_handoff.c helper.

Failure model:
- Any headless T8132 (/arm-io/disp0 marked external or absent) whose OS firmware matches a
  qualified row of dcp_soc_t8132 (dcp6_select_fw()) and whose ADT has a running dcpext0 or
  dcpext1 nub carrying that row's pinned DCP image goes to dt_set_display_handoff().
- Every other T8132 boot keeps the M4 handoff (dt_set_m4_dcp_handoff()):
  - any board with an internal panel (/arm-io/disp0 present without `external`);
  - a headless board on an unqualified OS firmware (26.6.2, 27.0, unknown), even if the UUID
    matches, so kboot routing and dt_set_display_handoff() cannot disagree;
  - a headless board running any other DCP image (e.g. 27.2.1 or missing UUID);
  - a headless board whose external DCP nubs are not running;
  - any non-T8132 chip.
- The pinned UUID and dcp6_select_fw() live in one place (src/kboot_display_handoff.c), not
  duplicated in src/kboot.c.
- dt_set_display() asks before it takes the M4 path, and returns what the external handoff
  returns (always 0).
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from host_compiler import host_cc
from test_t6041_display_handoff import LIBFDT, ROOT

HARNESS = r'''
#define main desktop26_main
#include "t6041_display_host.c"
#undef main

bool dt_t8132_external_display(void);

struct route_case {
    u32 chip;
    enum fw_version fw;
    bool has_disp0;
    bool disp0_external;
    const char *uuid[2];
    u32 running[2];
    bool has_running[2];
};

static void reset_adt(void)
{
    for (int i = 0; i < nnodes; i++) {
        for (int j = 0; j < nodes[i].nprops; j++)
            free(nodes[i].props[j]);
        nodes[i].nprops = 0;
        nodes[i].nreg = 0;
        nodes[i].path[0] = 0;
    }
    nnodes = 0;
}

static int check_case(const char *desc, struct route_case cfg, int want)
{
    static const char *const nubs[2] = {
        "/arm-io/dcpext0/iop-dcpext0-nub",
        "/arm-io/dcpext1/iop-dcpext1-nub",
    };

    reset_adt();
    chip_id = cfg.chip;
    os_firmware = fw_versions[cfg.fw];
    if (cfg.has_disp0) {
        prop_str("/arm-io/disp0", "compatible", "disp0,t8132");
        if (cfg.disp0_external)
            prop_set("/arm-io/disp0", "external", "", 0);
    }
    for (int i = 0; i < 2; i++) {
        if (cfg.uuid[i])
            prop_str(nubs[i], "uuid", cfg.uuid[i]);
        if (cfg.has_running[i])
            prop_u32(nubs[i], "running", cfg.running[i]);
    }
    int got = dt_t8132_external_display();
    printf("%s %s: %s\n", got == want ? "ok  " : "FAIL", desc,
           got ? "external handoff" : "M4 handoff");
    return got != want;
}

int main(void)
{
    static const char PINNED[] = "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF";
    static const char OTHER[] = "0F1E2D3C-4B5A-3968-8776-A5B4C3D2E1F0";
    int fails = 0;

    fails += check_case("headless disp0 external + 27.2 + dcpext0 pinned running",
                        (struct route_case){T8132, V27_2, true, true, {PINNED, PINNED},
                                            {1, 1}, {true, true}},
                        1);
    fails += check_case("headless disp0 absent + 27.2 + dcpext1 pinned running",
                        (struct route_case){T8132, V27_2, false, false, {OTHER, PINNED},
                                            {0, 1}, {true, true}},
                        1);
    fails += check_case("headless + 26.6.2 (dcp6_select_fw refuses even with pinned UUID)",
                        (struct route_case){T8132, V26_6_2, true, true, {PINNED, PINNED},
                                            {1, 1}, {true, true}},
                        0);
    fails += check_case("headless + 27.0 (dcp6_select_fw refuses even with pinned UUID)",
                        (struct route_case){T8132, V27_0, true, true, {PINNED, PINNED},
                                            {1, 1}, {true, true}},
                        0);
    fails += check_case("headless + 27.2 + other UUID (27.2.1)",
                        (struct route_case){T8132, V27_2, true, true, {OTHER, OTHER},
                                            {1, 1}, {true, true}},
                        0);
    fails += check_case("headless + 27.2 + pinned UUID but not running",
                        (struct route_case){T8132, V27_2, true, true, {PINNED, PINNED},
                                            {0, 0}, {true, true}},
                        0);
    fails += check_case("headless + 27.2 + missing UUID",
                        (struct route_case){T8132, V27_2, true, true, {NULL, NULL},
                                            {1, 1}, {true, true}},
                        0);
    fails += check_case("internal panel (disp0 not external) + 27.2 + dcpext0 pinned running",
                        (struct route_case){T8132, V27_2, true, false, {PINNED, PINNED},
                                            {1, 1}, {true, true}},
                        0);
    fails += check_case("wrong chip (T6041) + headless + 27.2 + dcpext0 pinned running",
                        (struct route_case){T6041, V27_2, true, true, {PINNED, PINNED},
                                            {1, 1}, {true, true}},
                        0);
    return fails != 0;
}
'''


def function(source, name, kind="static int "):
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


def test_t8132_route_follows_adt_and_fw_state(tmp_path):
    cc = host_cc()
    if not shutil.which(cc[0]):
        pytest.skip("no host C compiler")
    src = tmp_path / "src"
    src.mkdir()
    code = (ROOT / "src/kboot_display_handoff.c").read_text()
    if sys.platform == "darwin":
        code = code.replace('section(".init.t6050")', 'section("__TEXT,__t6050_init")')
    (src / "kboot_display_handoff.c").write_text(code)
    shutil.copy(ROOT / "tests/t6041_display_host.c", src)
    for header in (ROOT / "tests/t8122_display_host").glob("*.h"):
        shutil.copy(header, src)
    driver = src / "route_host.c"
    driver.write_text(HARNESS)
    binary = tmp_path / "t8132-route-host"
    subprocess.run(
        [*cc, "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wno-unused-function",
         "-Wno-format", "-Wno-format-truncation", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
         "-I", str(src), "-I", str(ROOT / "src"),
         str(driver), str(src / "kboot_display_handoff.c"),
         *(str(ROOT / "src/libfdt" / f) for f in LIBFDT), "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            env={"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout + result.stderr


def test_route_uses_shared_helper_and_single_uuid():
    kboot = (ROOT / "src/kboot.c").read_text()
    handoff = (ROOT / "src/kboot_display_handoff.c").read_text()
    uuid = "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF"
    assert uuid not in kboot
    assert handoff.count(uuid) == 1
    body = function(handoff, "dt_t8132_external_display", "bool ")
    assert "apple,j773g" not in body
    assert "dcp6_adt_headless()" in body
    assert "dcp6_select_fw(&dcp_soc_t8132)" in body


def test_dt_set_display_asks_before_the_m4_handoff():
    kboot = (ROOT / "src/kboot.c").read_text()
    body = function(kboot, "dt_set_display", "static int ")
    branch = body[body.index("if (chip_id == T8132) {"):]
    branch = branch[:branch.index("dt_set_m4_dcp_handoff()")]
    assert re.search(r"if \(dt_t8132_external_display\(\)\)\s+return dt_set_display_handoff\(dt\);",
                     branch), branch
