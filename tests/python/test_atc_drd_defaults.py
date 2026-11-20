# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""The usb-drdN controller defaults go to the dwc3 node whose first PHY is that ATC PHY: all three
lists or none, stale lists replaced, other controllers untouched. The PHY is found by its alias in
an /aliases node that follows /soc, after the PHY node has grown (as the calibration copy does), so
a path read before that would point at moved data. Synthetic tunable values."""
from pathlib import Path
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]

# name: (ADT property left out, malformed property, controller found, expected)
CASES = {
    "all-three": (None, None, True, "copied"),
    "gbl-missing": ("tunable_DRD_USB31_GBL_DEFAULT", None, True, "none"),
    "link-missing": ("tunable_LINK_REGS_DEFAULT", None, True, "none"),
    "dbg-out-of-range": (None, "tunable_DRD_USB31_DBG_DEFAULT", True, "none"),
    "no-controller": (None, None, False, "none"),
}


def _harness(omit, bad, controller):
    source = (REPO / "src/kboot_atc.c").read_text()
    definitions = source[source.index("struct atc_tunable {"):source.index("struct atc_fuse_info {")]
    start = source.index("static int dt_append_atc_tunable(")
    append = source[start:source.index("\n}", start) + 2] + "\n"
    block = source[source.index("static const struct adt_tunable_info drd_default_tunables[]"):
                   source.index("/* Publish the complete USB3 bundle")]
    text = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libfdt.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
void *adt;
static const char *omit = @OMIT@, *bad = @BAD@;
/* (offset | 32-bit access, mask, value) triples, as the ADT stores them */
static const u32 gbl[] = {0x20000010, 0x0000ff00, 0x00001200, 0x20000020, 0x1, 0x1};
static const u32 dbg[] = {0x20000004, 0x00000003, 0x00000002};
static const u32 dbg_bad[] = {0x20000800, 0x1, 0x1};
static const u32 link[] = {0x20000040, 0xffff0000, 0x00340000};
const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    const u32 *p = NULL;
    u32 n = 0;
    if (node != 7 || (omit && !strcmp(name, omit)))
        return NULL;
    if (!strcmp(name, "tunable_DRD_USB31_GBL_DEFAULT")) { p = gbl; n = sizeof(gbl); }
    if (!strcmp(name, "tunable_DRD_USB31_DBG_DEFAULT")) {
        p = bad && !strcmp(name, bad) ? dbg_bad : dbg;
        n = bad && !strcmp(name, bad) ? sizeof(dbg_bad) : sizeof(dbg);
    }
    if (!strcmp(name, "tunable_LINK_REGS_DEFAULT")) { p = link; n = sizeof(link); }
    if (p && len) *len = n;
    return p;
}
int adt_path_offset(const void *tree, const char *path)
{
    return strcmp(path, "/arm-io/usb-drd1") ? -1 : 7;
}
''' + definitions + append + block + r'''
static int cells(void *dt, int node, const char *name, const u32 *want, int count)
{
    int len;
    const fdt32_t *data = fdt_getprop(dt, node, name, &len);
    if (!data || len != count * 4)
        return 0;
    for (int i = 0; i < count; i++)
        if (fdt32_to_cpu(data[i]) != want[i])
            return 0;
    return 1;
}
int main(void)
{
    static char dt[16384];
    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    /* libfdt puts a new node first among its siblings: /aliases first, so /soc lands before it */
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "atcphy1", "/soc/phy@1"));
    int soc = fdt_add_subnode(dt, 0, "soc");
    int phy0 = fdt_add_subnode(dt, soc, "phy@0"), phy1 = fdt_add_subnode(dt, soc, "phy@1");
    assert(soc >= 0 && phy0 >= 0 && phy1 >= 0);
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/phy@0"), "phandle", 10));
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/phy@1"), "phandle", 11));
    /* usb@0 on phy@0, a non-dwc3 user of phy@1, then usb@1 on phy@1 if wanted */
    const char names[] = "dwc3-core\0dwc3-apple";
    u32 phys0[] = {cpu_to_fdt32(10), cpu_to_fdt32(3)}, phys1[] = {cpu_to_fdt32(11), cpu_to_fdt32(3)};
    int usb0 = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "usb@0");
    assert(!fdt_setprop(dt, usb0, "phys", phys0, sizeof(phys0)));
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/usb@0"), "reg-names", names, sizeof(names)));
    int other = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "dp@1");
    assert(!fdt_setprop(dt, other, "phys", phys1, sizeof(phys1)));
    if (@CONTROLLER@) {
        int usb1 = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "usb@1");
        assert(!fdt_setprop(dt, usb1, "phys", phys1, sizeof(phys1)));
        usb1 = fdt_path_offset(dt, "/soc/usb@1");
        assert(!fdt_setprop(dt, usb1, "reg-names", names, sizeof(names)));
        /* a stale list from an earlier stage must not survive */
        assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/usb@1"), "apple,tunable-dbg", 0x99));
    }
    /* What the caller held before its calibration copy grew the PHY node */
    const char *early = fdt_get_alias(dt, "atcphy1");
    assert(early && !strcmp(early, "/soc/phy@1"));
    static const u32 cal[64];
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/phy@1"), "apple,tunable-usb3-common", cal,
                        sizeof(cal)));
    assert(strcmp(early, "/soc/phy@1")); /* the early pointer now reads moved data */
    dt_copy_drd_default_tunables(dt, 1, "atcphy1");
    int usb1 = fdt_path_offset(dt, "/soc/usb@1");
    int u0 = fdt_path_offset(dt, "/soc/usb@0"), d1 = fdt_path_offset(dt, "/soc/dp@1");
    for (size_t i = 0; i < ARRAY_SIZE(drd_default_tunables); i++) {
        assert(!fdt_getprop(dt, u0, drd_default_tunables[i].fdt_name, NULL));
        assert(!fdt_getprop(dt, d1, drd_default_tunables[i].fdt_name, NULL));
    }
    const u32 want_gbl[] = {0x10, 0xff00, 0x1200, 0x20, 0x1, 0x1};
    const u32 want_dbg[] = {0x4, 0x3, 0x2};
    const u32 want_link[] = {0x40, 0xffff0000, 0x340000};
    if (usb1 >= 0 && cells(dt, usb1, "apple,tunable-gbl-default", want_gbl, 6) &&
        cells(dt, usb1, "apple,tunable-dbg", want_dbg, 3) &&
        cells(dt, usb1, "apple,tunable-link", want_link, 3)) {
        printf("RESULT copied\n");
        return 0;
    }
    for (size_t i = 0; usb1 >= 0 && i < ARRAY_SIZE(drd_default_tunables); i++)
        assert(!fdt_getprop(dt, usb1, drd_default_tunables[i].fdt_name, NULL));
    printf("RESULT none\n");
    return 0;
}
'''
    return text.replace("@OMIT@", f'"{omit}"' if omit else "NULL").replace(
        "@BAD@", f'"{bad}"' if bad else "NULL").replace("@CONTROLLER@", "1" if controller else "0")


@pytest.mark.parametrize("case", sorted(CASES))
def test_atc_drd_defaults(tmp_path, case):
    omit, bad, controller, expected = CASES[case]
    cfile = tmp_path / "drd.c"
    binary = tmp_path / "drd"
    cfile.write_text(_harness(omit, bad, controller))
    libfdt = REPO / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    subprocess.run(["cc", "-D_GNU_SOURCE", "-Wall", "-Werror", "-Wno-unused-function", "-I",
                    str(libfdt), str(cfile), *sources, "-o", str(binary)], check=True)
    out = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
    assert out.strip().splitlines()[-1] == f"RESULT {expected}"
