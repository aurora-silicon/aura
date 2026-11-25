# SPDX-License-Identifier: MIT
"""Host integration tests for T6041 (J616C/J614C) and J614s ATC USB2 device tunables.

Failure model recorded before changing src/kboot_atc.c:
- J616C and J614C (T6041) enter j614s_export_atc() via t6041_max_atc() for USB-C ports 0..2
  and leave port 3 (HDMI fixed DP PHY) untouched; J614s (T6040, board_id 4) also exports the
  9 DP-only tunables on port 3 and sets kboot_j614s_hdmi_ready().
- When both tunable_USB2PHY_DFLT and tunable_USB2PHY_DEV are present and valid in /arm-io/atc-phyN,
  both apple,tunable-usb2-default and apple,tunable-usb2-device are exported alongside all 16
  required PHY and usb-drdN controller tunables, and both PHY and controller are set status="okay".
- When either tunable_USB2PHY_DFLT or tunable_USB2PHY_DEV is missing or empty, neither
  apple,tunable-usb2-default nor apple,tunable-usb2-device may be published (and any stale pair
  in the DT must be removed), while all required tunables are still exported and the port stays
  enabled for USB2 host / USB3.
- When either tunable_USB2PHY_DFLT or tunable_USB2PHY_DEV is malformed (truncated, wrong width,
  unaligned offset, or offset outside bank0), it must NOT abort j614s_export_atc() or disable the
  port: stale USB2 device properties are removed, no half-pair is left behind, required tunables
  are exported, and the port stays status="okay".
- When a required tunable (e.g. tunable_USB2PHY_HOST or tunable_DRD_USB31_GBL_DEFAULT) is missing
  or invalid, the port's scratch transaction rolls back and both PHY and controller are disabled.
- Synthetic tunable values only; any MMIO read/write aborts the test.
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

/* ---- synthetic ADT ---- */

static const char *adt_root_compat;
static int fault_port = -1;
static const char *omit_prop;
static const char *replace_prop;
static const void *replace_data;
static u32 replace_len;

/* Valid synthetic (offset | 32-bit width, mask, value) records */
static const u32 rec_phy[] = {0x20000004, 0x000000ff, 0x00000011};
static const u32 rec_usb2_host[] = {0x20000010, 0x000000ff, 0x000000aa};
static const u32 rec_usb2_dflt[] = {0x20000014, 0x000000ff, 0x00000055};
static const u32 rec_usb2_dev[] = {0x20000018, 0x000000ff, 0x000000cc};
static const u32 rec_drd[] = {0x20000010, 0x0000000f, 0x00000005};

/* Invalid synthetic records for bank0 (size 0x4000) */
static const u32 bad_unaligned[] = {0x20000002, 0x1, 0x1};
static const u32 bad_outside[] = {0x20004000, 0x1, 0x1};
static const u32 bad_width[] = {0x10000008, 0x1, 0x1};

/*
 * Synthetic ADT node IDs:
 *   0: root
 *   10..13: /arm-io/atc-phy0..3
 *   20..22: /arm-io/usb-drd0..2
 */
int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/"))
        return 0;
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/arm-io/atc-phy%d", i);
        if (!strcmp(path, buf))
            return 10 + i;
    }
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/arm-io/usb-drd%d", i);
        if (!strcmp(path, buf))
            return 20 + i;
    }
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
    return nodeoffset == 0 && adt_root_compat && !strcmp(adt_root_compat, compat);
}

bool adt_is_compatible_at(const void *tree, int nodeoffset, const char *compat, size_t index)
{
    (void)tree;
    (void)nodeoffset;
    (void)compat;
    (void)index;
    return false;
}

int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)tree;
    (void)path;
    (void)prop;
    (void)idx;
    (void)addr;
    (void)size;
    return -1;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    int port = node >= 20 ? node - 20 : node >= 10 ? node - 10 : -1;
    if (port < 0)
        return NULL;
    if (port == fault_port) {
        if (omit_prop && !strcmp(omit_prop, name))
            return NULL;
        if (replace_prop && !strcmp(replace_prop, name)) {
            if (len)
                *len = replace_len;
            return replace_data;
        }
    }

    const void *p = NULL;
    u32 n = 0;
    if (node >= 10 && node <= 13) {
        if (!strcmp(name, "tunable_USB2PHY_HOST")) {
            p = rec_usb2_host;
            n = sizeof(rec_usb2_host);
        } else if (!strcmp(name, "tunable_USB2PHY_DFLT")) {
            p = rec_usb2_dflt;
            n = sizeof(rec_usb2_dflt);
        } else if (!strcmp(name, "tunable_USB2PHY_DEV")) {
            p = rec_usb2_dev;
            n = sizeof(rec_usb2_dev);
        } else if (!strncmp(name, "tunable_", 8)) {
            p = rec_phy;
            n = sizeof(rec_phy);
        }
    } else if (node >= 20 && node <= 22) {
        if (!strncmp(name, "tunable_", 8)) {
            p = rec_drd;
            n = sizeof(rec_drd);
        }
    }
    if (p && len)
        *len = n;
    return p;
}

/* ---- synthetic FDT matching t604x.dtsi + t604x-extdisp.dtsi ---- */

static const char *const phys[4] = {"phy@382a90000", "phy@38aa90000", "phy@392a90000",
                                    "phy@39aa90000"};
static const char *const usbs[3] = {"usb@382280000", "usb@38a280000", "usb@392280000"};
static const u64 phy_bases[4] = {0x382a90000ULL, 0x38aa90000ULL, 0x392a90000ULL, 0x39aa90000ULL};
static const u64 usb_bases[3] = {0x382280000ULL, 0x38a280000ULL, 0x392280000ULL};

static void add_reg_pair(void *dt, int node, u64 base, u64 size)
{
    assert(!fdt_appendprop_u64(dt, node, "reg", base));
    assert(!fdt_appendprop_u64(dt, node, "reg", size));
}

static void build_dt(void *dt, size_t cap, const char *board_compat, bool add_stale_usb2)
{
    static const char phy_reg_names[] =
        "bank0\0pipehandler\0core\0axi2af\0axi2af-lioa\0fabric\0common-cfg";
    static const char usb_reg_names[] = "dwc3-core\0dwc3-apple\0drd-cfg";
    static const fdt32_t stale[3] = {0, 0, 0};

    assert(!fdt_create_empty_tree(dt, (int)cap));
    assert(!fdt_setprop_string(dt, 0, "compatible", board_compat));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));

    for (int i = 0; i < 4; i++) {
        int phy = fdt_add_subnode(dt, 0, phys[i]);
        assert(phy >= 0);
        assert(!fdt_setprop_string(dt, phy, "compatible", "apple,t6040-atcphy"));
        assert(!fdt_setprop_string(dt, phy, "status", "disabled"));
        assert(!fdt_setprop(dt, phy, "reg-names", phy_reg_names, sizeof(phy_reg_names)));
        add_reg_pair(dt, phy, phy_bases[i], 0x4000);          /* bank0 */
        add_reg_pair(dt, phy, phy_bases[i] - 0xc000, 0x4000); /* pipehandler */
        add_reg_pair(dt, phy, phy_bases[i] + 0x570000, 0x30000); /* core */
        add_reg_pair(dt, phy, phy_bases[i] - 0x2a90000, 0x8000); /* axi2af */
        add_reg_pair(dt, phy, phy_bases[i] - 0x2a70000, 0x4000); /* axi2af-lioa */
        add_reg_pair(dt, phy, phy_bases[i] + 0x7b4000, 0x4000);  /* fabric */
        add_reg_pair(dt, phy, phy_bases[i] + 0x7c0000, 0x4000);  /* common-cfg */
        if (i == 3)
            assert(!fdt_setprop_u32(dt, phy, "apple,tunable-axi2af", 0xdeadbeef));
        if (add_stale_usb2 && i < 3) {
            assert(!fdt_setprop(dt, phy, "apple,tunable-usb2-default", stale, sizeof(stale)));
            assert(!fdt_setprop(dt, phy, "apple,tunable-usb2-device", stale, sizeof(stale)));
        }
    }

    for (int i = 0; i < 3; i++) {
        int usb = fdt_add_subnode(dt, 0, usbs[i]);
        assert(usb >= 0);
        assert(!fdt_setprop_string(dt, usb, "compatible", "apple,t6040-dwc3"));
        assert(!fdt_setprop_string(dt, usb, "status", "disabled"));
        assert(!fdt_setprop(dt, usb, "reg-names", usb_reg_names, sizeof(usb_reg_names)));
        add_reg_pair(dt, usb, usb_bases[i], 0xcd00);          /* dwc3-core */
        add_reg_pair(dt, usb, usb_bases[i] + 0xcd00, 0x3200); /* dwc3-apple */
        add_reg_pair(dt, usb, usb_bases[i] - 0x80000, 0x4000); /* drd-cfg */
    }
}

static int phy_off(const void *dt, int port)
{
    char path[32];
    snprintf(path, sizeof(path), "/%s", phys[port]);
    int off = fdt_path_offset(dt, path);
    assert(off >= 0);
    return off;
}

static int usb_off(const void *dt, int port)
{
    char path[32];
    snprintf(path, sizeof(path), "/%s", usbs[port]);
    int off = fdt_path_offset(dt, path);
    assert(off >= 0);
    return off;
}

static void expect_status(const void *dt, int node, const char *want)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    assert(s && !strcmp(s, want));
}

static void expect_triple(const void *dt, int node, const char *prop, u32 off, u32 mask, u32 val)
{
    int len = 0;
    const fdt32_t *v = fdt_getprop(dt, node, prop, &len);
    assert(v && len == 12);
    assert(fdt32_ld(v) == off && fdt32_ld(v + 1) == mask && fdt32_ld(v + 2) == val);
}

static void expect_usb2_pair(const void *dt, int port, bool present)
{
    int phy = phy_off(dt, port);
    if (present) {
        expect_triple(dt, phy, "apple,tunable-usb2-default", 0x14, 0xff, 0x55);
        expect_triple(dt, phy, "apple,tunable-usb2-device", 0x18, 0xff, 0xcc);
    } else {
        assert(!fdt_getprop(dt, phy, "apple,tunable-usb2-default", NULL));
        assert(!fdt_getprop(dt, phy, "apple,tunable-usb2-device", NULL));
    }
}

static void expect_port_enabled_with_required(const void *dt, int port)
{
    int phy = phy_off(dt, port), usb = usb_off(dt, port);
    expect_status(dt, phy, "okay");
    expect_status(dt, usb, "okay");
    expect_triple(dt, phy, "apple,tunable-usb2-host", 0x10, 0xff, 0xaa);
    expect_triple(dt, phy, "apple,tunable-axi2af", 0x4, 0xff, 0x11);
    expect_triple(dt, phy, "apple,tunable-pipehandler", 0x10, 0xf, 0x5);
    expect_triple(dt, usb, "apple,tunable-gbl-default", 0x10, 0xf, 0x5);
    expect_triple(dt, usb, "apple,tunable-dbg", 0x10, 0xf, 0x5);
    expect_triple(dt, usb, "apple,tunable-link", 0x10, 0xf, 0x5);
}

static void reset_faults(void)
{
    fault_port = -1;
    omit_prop = NULL;
    replace_prop = NULL;
    replace_data = NULL;
    replace_len = 0;
}

static void run_case(const char *name)
{
    static u8 dt[65536];
    reset_faults();

    if (!strcmp(name, "j616c-all-ports") || !strcmp(name, "j614c-all-ports")) {
        bool is_j616c = !strcmp(name, "j616c-all-ports");
        chip_id = T6041;
        board_id = 0;
        adt_root_compat = is_j616c ? "J616cAP" : "J614cAP";
        build_dt(dt, sizeof(dt), is_j616c ? "apple,j616c" : "apple,j614c", false);
        assert(!kboot_setup_atc(dt));
        for (int p = 0; p < 3; p++) {
            expect_port_enabled_with_required(dt, p);
            expect_usb2_pair(dt, p, true);
        }
        /* Port 3 HDMI fixed DP PHY stays untouched on T6041 laptops. */
        int p3 = phy_off(dt, 3);
        int len = 0;
        const fdt32_t *keep = fdt_getprop(dt, p3, "apple,tunable-axi2af", &len);
        assert(keep && len == 4 && fdt32_ld(keep) == 0xdeadbeef);
        expect_status(dt, p3, "disabled");
        expect_usb2_pair(dt, 3, false);
        assert(!kboot_j614s_hdmi_ready());
        return;
    }

    if (!strcmp(name, "j614s-port3-dp-only")) {
        chip_id = T6040;
        board_id = 4;
        adt_root_compat = "J614sAP";
        build_dt(dt, sizeof(dt), "apple,j614s", false);
        assert(!kboot_setup_atc(dt));
        for (int p = 0; p < 3; p++) {
            expect_port_enabled_with_required(dt, p);
            expect_usb2_pair(dt, p, true);
        }
        int p3 = phy_off(dt, 3);
        expect_triple(dt, p3, "apple,tunable-axi2af", 0x4, 0xff, 0x11);
        expect_status(dt, p3, "disabled");
        assert(!fdt_getprop(dt, p3, "apple,tunable-usb2-host", NULL));
        expect_usb2_pair(dt, 3, false);
        assert(kboot_j614s_hdmi_ready());
        return;
    }

    /* All remaining scenarios exercise J616C (T6041) port 1 with stale DT USB2 properties. */
    chip_id = T6041;
    board_id = 0;
    adt_root_compat = "J616cAP";
    build_dt(dt, sizeof(dt), "apple,j616c", true);
    fault_port = 1;

    if (!strcmp(name, "missing-dflt-no-half-pair")) {
        omit_prop = "tunable_USB2PHY_DFLT";
        assert(!kboot_setup_atc(dt));
        expect_port_enabled_with_required(dt, 0);
        expect_usb2_pair(dt, 0, true);
        expect_port_enabled_with_required(dt, 1);
        expect_usb2_pair(dt, 1, false);
        expect_port_enabled_with_required(dt, 2);
        expect_usb2_pair(dt, 2, true);
        return;
    }

    if (!strcmp(name, "missing-dev-no-half-pair")) {
        omit_prop = "tunable_USB2PHY_DEV";
        assert(!kboot_setup_atc(dt));
        expect_port_enabled_with_required(dt, 1);
        expect_usb2_pair(dt, 1, false);
        return;
    }

    if (!strcmp(name, "empty-dflt-no-half-pair") || !strcmp(name, "empty-dev-no-half-pair")) {
        replace_prop =
            !strcmp(name, "empty-dflt-no-half-pair") ? "tunable_USB2PHY_DFLT" : "tunable_USB2PHY_DEV";
        replace_data = rec_usb2_dev;
        replace_len = 0;
        assert(!kboot_setup_atc(dt));
        expect_port_enabled_with_required(dt, 1);
        expect_usb2_pair(dt, 1, false);
        return;
    }

    if (!strcmp(name, "invalid-dflt-keeps-port-enabled") ||
        !strcmp(name, "invalid-dev-keeps-port-enabled")) {
        const struct {
            const void *data;
            u32 len;
        } bads[] = {
            {bad_unaligned, sizeof(bad_unaligned)},
            {bad_outside, sizeof(bad_outside)},
            {bad_width, sizeof(bad_width)},
            {rec_usb2_dev, sizeof(rec_usb2_dev) - 1},
        };
        const char *prop = !strcmp(name, "invalid-dflt-keeps-port-enabled")
                               ? "tunable_USB2PHY_DFLT"
                               : "tunable_USB2PHY_DEV";
        for (size_t i = 0; i < ARRAY_SIZE(bads); i++) {
            build_dt(dt, sizeof(dt), "apple,j616c", true);
            replace_prop = prop;
            replace_data = bads[i].data;
            replace_len = bads[i].len;
            assert(!kboot_setup_atc(dt));
            expect_port_enabled_with_required(dt, 0);
            expect_usb2_pair(dt, 0, true);
            expect_port_enabled_with_required(dt, 1);
            expect_usb2_pair(dt, 1, false);
            expect_port_enabled_with_required(dt, 2);
            expect_usb2_pair(dt, 2, true);
        }
        return;
    }

    if (!strcmp(name, "missing-required-disables-port")) {
        omit_prop = "tunable_USB2PHY_HOST";
        assert(!kboot_setup_atc(dt));
        expect_port_enabled_with_required(dt, 0);
        expect_usb2_pair(dt, 0, true);
        expect_status(dt, phy_off(dt, 1), "disabled");
        expect_status(dt, usb_off(dt, 1), "disabled");
        /* Rollback: required tunables were not partially committed on port 1. */
        assert(!fdt_getprop(dt, phy_off(dt, 1), "apple,tunable-axi2af", NULL));
        expect_port_enabled_with_required(dt, 2);
        expect_usb2_pair(dt, 2, true);
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
    "j616c-all-ports",
    "j614c-all-ports",
    "j614s-port3-dp-only",
    "missing-dflt-no-half-pair",
    "missing-dev-no-half-pair",
    "empty-dflt-no-half-pair",
    "empty-dev-no-half-pair",
    "invalid-dflt-keeps-port-enabled",
    "invalid-dev-keeps-port-enabled",
    "missing-required-disables-port",
]


@pytest.fixture(scope="module")
def host_atc(tmp_path_factory):
    cc = host_cc()
    if not shutil.which(cc[0]):
        pytest.skip("no host C compiler")
    tmp = tmp_path_factory.mktemp("t6041-atc")
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
    binary = tmp / "t6041-atc-host"
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
def test_t6041_atc_usb2_device(host_atc, case):
    env = {"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"}
    res = subprocess.run([str(host_atc), case], capture_output=True, text=True, env=env)
    assert res.returncode == 0, res.stdout + res.stderr
    assert res.stdout.rstrip().endswith(f"{case}: pass"), res.stdout
