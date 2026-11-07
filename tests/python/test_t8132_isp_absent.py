# SPDX-License-Identifier: GPL-2.0-only
"""Run the production T8132 rule for a machine whose ADT has no camera (the J773g)."""
from pathlib import Path

from test_j713_dapf_instance import block
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

# What can go wrong: a tree that enables the T8132 ISP or its DARTs on a Mac with no ISP in
# its ADT (the J773g has none) makes the OS probe a camera and power domains that are not
# there; the rule touches a machine that does have an ISP, or another SoC; it disables a
# DART that another device shares; a malformed iommus list stops the boot.
MAIN = r'''
static unsigned char blob[4096];
static void tree(bool enabled, bool shared)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    static const char *const names[] = {"iommu@48e8c0000", "iommu@48e8e0000", "iommu@48e900000"};
    for (int i = 0; i < 3; i++) {
        int dart = fdt_add_subnode(dt, 0, names[i]);
        assert(dart >= 0 && !fdt_setprop_u32(dt, dart, "phandle", 10 + i));
        assert(!fdt_setprop_u32(dt, dart, "#iommu-cells", 1));
    }
    int isp = fdt_add_subnode(dt, 0, "isp@48c000000");
    assert(isp >= 0 && !fdt_setprop(dt, isp, "compatible", "apple,t8132-isp\0apple,isp",
                                     sizeof("apple,t8132-isp\0apple,isp")));
    fdt32_t iommus[] = {cpu_to_fdt32(10), 0, cpu_to_fdt32(11), 0, cpu_to_fdt32(12), 0};
    assert(!fdt_setprop(dt, isp, "iommus", iommus, sizeof(iommus)));
    if (!enabled)
        assert(!fdt_setprop_string(dt, isp, "status", "disabled"));
    int other = fdt_add_subnode(dt, 0, "other@0");
    assert(other >= 0);
    if (shared) {
        fdt32_t ref[] = {cpu_to_fdt32(12), cpu_to_fdt32(1)};
        assert(!fdt_setprop(dt, other, "iommus", ref, sizeof(ref)));
    }
}
static const char *status(const char *path)
{
    const char *s = fdt_getprop(dt, fdt_path_offset(dt, path), "status", NULL);
    return s ? s : "okay";
}
static bool off(const char *path) { return !strcmp(status(path), "disabled"); }
int main(void)
{
    /* No ISP in the ADT: the ISP and its three DARTs end disabled. */
    tree(true, false);
    assert(!dt_disable_t8132_absent_isp(dt));
    assert(off("/isp@48c000000") && off("/iommu@48e8c0000") && off("/iommu@48e8e0000") &&
           off("/iommu@48e900000") && !off("/other@0"));
    /* A tree that already leaves it disabled is left exactly as it is. */
    tree(false, false);
    unsigned char before[sizeof(blob)];
    memcpy(before, blob, sizeof(blob));
    assert(!dt_disable_t8132_absent_isp(dt) && !memcmp(before, blob, sizeof(blob)));
    /* A DART another device also uses stays as it is. */
    tree(true, true);
    assert(!dt_disable_t8132_absent_isp(dt));
    assert(off("/isp@48c000000") && off("/iommu@48e8c0000") && off("/iommu@48e8e0000") &&
           !off("/iommu@48e900000"));
    /* A broken iommus list still leaves the ISP disabled. */
    tree(true, false);
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/isp@48c000000"), "iommus", "\0\0\0\x63\0", 5));
    assert(!dt_disable_t8132_absent_isp(dt) && off("/isp@48c000000"));
    assert(!off("/iommu@48e8c0000"));
    /* A machine with an ISP (the J713), or another SoC, is not touched. */
    for (int c = 0; c < 3; c++) {
        tree(true, false);
        memcpy(before, blob, sizeof(blob));
        adt_isp = c == 0; adt_isp0 = c == 1; chip_id = c == 2 ? 0x8122 : T8132;
        assert(!dt_disable_t8132_absent_isp(dt) && !memcmp(before, blob, sizeof(blob)));
        adt_isp = adt_isp0 = false; chip_id = T8132;
    }
    return 0;
}
'''


def test_t8132_isp_absent(tmp_path):
    kboot = (ROOT / "src/kboot_t8132.c").read_text()
    run_c(tmp_path, "t8132-isp-absent", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
#define T8132 0x8132
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt, *adt;
static u32 chip_id = T8132;
static bool adt_isp, adt_isp0;
static int adt_path_offset(const void *tree, const char *path)
{
    if (!strcmp(path, "/arm-io/isp")) return adt_isp ? 1 : -1;
    if (!strcmp(path, "/arm-io/isp0")) return adt_isp0 ? 2 : -1;
    return -1;
}
''' + block(kboot, "static bool dt_iommu_shared(") + "\n"
          + block(kboot, "int dt_disable_t8132_absent_isp(") + MAIN)
