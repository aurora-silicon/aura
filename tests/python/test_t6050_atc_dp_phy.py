# SPDX-License-Identifier: MIT
"""Host tests for T6050 ATC tunables on a fixed DisplayPort PHY (src/kboot_atc.c).

An HDMI port's ATC (ATC3 on the 14-inch M5 Pro/Max MacBook Pros) has no USB side: no usb-drdN
in the ADT. Its PHY node is apple,t6050-dp-phy and takes the nine tables the fixed DP sequence
uses (AXI2AF through AUSPLL), all or nothing; a combo PHY still needs its usb-drdN and takes all
fourteen. Synthetic tunable values only; any MMIO access aborts the test.
"""

from pathlib import Path
import shutil
import subprocess
import sys

import pytest
from host_compiler import host_cc

REPO = Path(__file__).resolve().parents[2]
LIBFDT = [
    "fdt.c",
    "fdt_ro.c",
    "fdt_rw.c",
    "fdt_wip.c",
    "fdt_sw.c",
    "fdt_empty_tree.c",
    "fdt_strerror.c",
    "fdt_addresses.c",
]

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kboot_atc.h"
#include "soc.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

void *adt = (void *)1;
u32 chip_id;
u32 board_id;

void host_mmio(u64 addr)
{
    fprintf(stderr, "unexpected MMIO access at 0x%llx\n", (unsigned long long)addr);
    abort();
}

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* ---- synthetic ADT: /arm-io/atc-phy1 + usb-drd1 (a USB-C port), atc-phy3 (no usb-drd3) ---- */

static const char *omit_prop;
static const u32 rec_phy[] = {0x20000004, 0x000000ff, 0x00000011};
static const u32 phandle1 = 0x101, phandle3 = 0x103;

/* 11, 13: atc-phy1, atc-phy3; 21: usb-drd1 */
int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/"))
        return 0;
    if (!strcmp(path, "/arm-io/atc-phy1"))
        return 11;
    if (!strcmp(path, "/arm-io/atc-phy3"))
        return 13;
    if (!strcmp(path, "/arm-io/usb-drd1"))
        return 21;
    return -1;
}

int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    int off = adt_path_offset(tree, path);
    if (off >= 0 && offsets) {
        offsets[0] = off;
        offsets[1] = 0;
    }
    return off;
}

bool adt_is_compatible(const void *tree, int nodeoffset, const char *compat)
{
    (void)tree;
    (void)nodeoffset;
    (void)compat;
    return false;
}

bool adt_is_compatible_at(const void *tree, int nodeoffset, const char *compat, size_t index)
{
    (void)tree;
    return (nodeoffset == 11 || nodeoffset == 13) && index == 0 && !strcmp(compat, "atc-phy,t6050");
}

static u64 core_of(int port)
{
    return port == 3 ? 0x38b000000ULL : 0x2ab000000ULL;
}

/* An ATC PHY's ADT regs at the indices the tunable lists name, as on T6050 */
int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)tree;
    (void)prop;
    int node = path[0];
    u64 core = core_of(node % 10);
    static const struct { int idx; s64 off; } phy[] = {
        {41, -0x3000000}, {40, 0x244000}, {43, 0x250000}, {42, -0x2fe0000}, {20, 0xd000},
        {29, 0x1d000},    {18, 0xb000},   {27, 0x1b000},  {8, 0x3200},       {5, 0xa00},
        {10, 0x3a00},     {0, -0x570000}, {2, 0},
    };
    if (node == 21 && idx == 3) {
        *addr = core - 0x57c000;
        if (size)
            *size = 0x4000;
        return 0;
    }
    if (node != 11 && node != 13)
        return -1;
    for (size_t i = 0; i < ARRAY_SIZE(phy); i++)
        if (phy[i].idx == idx) {
            *addr = core + phy[i].off;
            if (size)
                *size = 0x4000;
            return 0;
        }
    return -1;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    const void *p = NULL;
    u32 n = 0;
    if (omit_prop && !strcmp(omit_prop, name))
        return NULL;
    if ((node == 11 || node == 13) && !strcmp(name, "AAPL,phandle")) {
        p = node == 11 ? &phandle1 : &phandle3;
        n = 4;
    } else if (node == 21 && !strcmp(name, "atc-phy-parent")) {
        p = &phandle1;
        n = 4;
    } else if ((node == 11 || node == 13 || node == 21) && !strncmp(name, "tunable_", 8)) {
        p = rec_phy;
        n = sizeof(rec_phy);
    }
    if (p && len)
        *len = n;
    return p;
}

/* ---- synthetic FDT: the K114s's ATC1 combo PHY and its HDMI port's fixed DP PHY (ATC3) ---- */

static void reg(void *dt, int node, u64 base, u64 size)
{
    assert(!fdt_appendprop_u64(dt, node, "reg", base));
    assert(!fdt_appendprop_u64(dt, node, "reg", size));
}

static int add_phy(void *dt, const char *name, const char *compat, int port)
{
    u64 core = core_of(port);
    int n = fdt_add_subnode(dt, 0, name);
    assert(n >= 0);
    assert(!fdt_setprop_string(dt, n, "compatible", compat));
    assert(!fdt_setprop_u32(dt, n, "apple,port-index", port));
    if (!strcmp(compat, "apple,t6050-dp-phy")) {
        static const char names[] = "core\0reset\0fabric\0axi2af\0lioa\0common-cfg";
        assert(!fdt_setprop(dt, n, "reg-names", names, sizeof(names)));
        reg(dt, n, core, 0x2d000);
        reg(dt, n, core + 0x200000, 0x4000);
        reg(dt, n, core + 0x244000, 0x4000);
        reg(dt, n, core - 0x3000000, 0x8000);
        reg(dt, n, core - 0x2fe0000, 0x4000);
        reg(dt, n, core + 0x250000, 0x4000);
    } else {
        static const char names[] =
            "core\0usb2phy\0reset\0fabric\0axi2af\0lioa\0common-cfg\0pipehandler";
        assert(!fdt_setprop(dt, n, "reg-names", names, sizeof(names)));
        reg(dt, n, core, 0x2c000);
        reg(dt, n, core - 0x570000, 0x4000);
        reg(dt, n, core + 0x200000, 0x4000);
        reg(dt, n, core + 0x244000, 0x4000);
        reg(dt, n, core - 0x3000000, 0x8000);
        reg(dt, n, core - 0x2fe0000, 0x4000);
        reg(dt, n, core + 0x250000, 0x4000);
        reg(dt, n, core - 0x57c000, 0x4000);
    }
    return n;
}

static void build_dt(void *dt, size_t cap, const char *atc3_compat)
{
    assert(!fdt_create_empty_tree(dt, (int)cap));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    add_phy(dt, "phy@2ab000000", "apple,t6050-combo-phy", 1);
    add_phy(dt, "phy@38b000000", atc3_compat, 3);
}

static bool has(const void *dt, const char *node, const char *prop)
{
    int n = fdt_path_offset(dt, node);
    assert(n >= 0);
    return fdt_getprop(dt, n, prop, NULL) != NULL;
}

static const char *const dp_tables[] = {
    "apple,tunable-axi2af",         "apple,tunable-fabric",       "apple,tunable-common-cfg",
    "apple,tunable-lioa",           "apple,tunable-lane0-rx-clock", "apple,tunable-lane1-rx-clock",
    "apple,tunable-lane0-rx-eq",    "apple,tunable-lane1-rx-eq",  "apple,tunable-auspll",
};
static const char *const usb_tables[] = {
    "apple,tunable-aus40cmn-shm", "apple,tunable-cio4pll", "apple,tunable-usb2-default",
    "apple,tunable-usb2-host", "apple,tunable-pipe-default",
};

static void run_case(const char *name)
{
    static u8 dt[65536];
    chip_id = T6050;
    omit_prop = NULL;

    if (!strcmp(name, "dp-phy-takes-the-dp-tables")) {
        build_dt(dt, sizeof(dt), "apple,t6050-dp-phy");
        assert(!kboot_setup_atc(dt));
        for (size_t i = 0; i < ARRAY_SIZE(dp_tables); i++)
            assert(has(dt, "/phy@38b000000", dp_tables[i]) && has(dt, "/phy@2ab000000", dp_tables[i]));
        for (size_t i = 0; i < ARRAY_SIZE(usb_tables); i++) {
            assert(!has(dt, "/phy@38b000000", usb_tables[i]));
            assert(has(dt, "/phy@2ab000000", usb_tables[i]));
        }
        return;
    }
    if (!strcmp(name, "dp-phy-all-or-nothing")) {
        build_dt(dt, sizeof(dt), "apple,t6050-dp-phy");
        omit_prop = "tunable_AUSPLL_CORE";
        assert(!kboot_setup_atc(dt));
        for (size_t i = 0; i < ARRAY_SIZE(dp_tables); i++)
            assert(!has(dt, "/phy@38b000000", dp_tables[i]));
        return;
    }
    if (!strcmp(name, "combo-phy-without-usb-controller")) {
        /* A combo PHY still needs its usb-drdN: ATC3 has none */
        build_dt(dt, sizeof(dt), "apple,t6050-combo-phy");
        assert(!kboot_setup_atc(dt));
        assert(!has(dt, "/phy@38b000000", "apple,tunable-axi2af"));
        assert(has(dt, "/phy@2ab000000", "apple,tunable-pipe-default"));
        return;
    }
    fprintf(stderr, "unknown case %s\n", name);
    abort();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    run_case(argv[1]);
    printf("%s: pass\n", argv[1]);
    return 0;
}
'''

CASES = [
    "dp-phy-takes-the-dp-tables",
    "dp-phy-all-or-nothing",
    "combo-phy-without-usb-controller",
]


@pytest.fixture(scope="module")
def host_t6050_atc(tmp_path_factory):
    cc = host_cc()
    if not shutil.which(cc[0]):
        pytest.skip("no host C compiler")
    tmp = tmp_path_factory.mktemp("t6050-atc")
    src = tmp / "src"
    src.mkdir()
    code = (REPO / "src/kboot_atc.c").read_text()
    if sys.platform == "darwin":
        code = code.replace('section(".init.t6050")', 'section("__TEXT,__t6050_init")')
    (src / "kboot_atc.c").write_text(code)
    shutil.copy(REPO / "tests/t6030_host/utils.h", src / "utils.h")
    shutil.copy(REPO / "tests/t8122_display_host/malloc.h", src / "malloc.h")
    # Host libc headers (especially macOS SDK) already define uintptr_t, ptrdiff_t, and ssize_t
    # as long/unsigned long, colliding with src/types.h's u64/s64 typedefs. Pre-include a host-safe
    # copy so TYPES_H is set before src/kboot_atc.h pulls in src/types.h.
    types_h = (REPO / "src/types.h").read_text()
    for line in ("typedef u64 uintptr_t;\n", "typedef s64 ptrdiff_t;\n", "typedef s64 ssize_t;\n"):
        assert line in types_h
        types_h = types_h.replace(line, "")
    types_h = types_h.replace("#include <stdint.h>\n", "#include <stdint.h>\n#include <sys/types.h>\n", 1)
    (src / "types.h").write_text(types_h)
    (src / "harness.c").write_text(HARNESS)
    binary = tmp / "t6050-atc-host"
    subprocess.run(
        [
            *cc,
            "-std=gnu11",
            "-D_GNU_SOURCE",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-Wno-unused-parameter",
            "-Wno-sign-compare",
            "-Wno-attributes",
            "-Wno-format",
            "-Wno-format-truncation",
            "-Wno-missing-field-initializers",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-sanitize-recover=undefined",
            "-I",
            str(src),
            "-I",
            str(REPO / "src"),
            "-include",
            str(src / "types.h"),
            str(src / "harness.c"),
            str(src / "kboot_atc.c"),
            *(str(REPO / "src/libfdt" / f) for f in LIBFDT),
            "-o",
            str(binary),
        ],
        check=True,
    )
    return binary


@pytest.mark.parametrize("case", CASES)
def test_t6050_atc_dp_phy(host_t6050_atc, case):
    env = {"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"}
    res = subprocess.run([str(host_t6050_atc), case], capture_output=True, text=True, env=env)
    assert res.returncode == 0, res.stdout + res.stderr
    assert res.stdout.rstrip().endswith(f"{case}: pass"), res.stdout
