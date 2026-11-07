# SPDX-License-Identifier: GPL-2.0-only
"""Run the production T8132 rule for a machine whose ADT has no camera (the J773g)."""
from pathlib import Path

from test_j713_dapf_instance import block
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

# What can go wrong: a tree that enables the T8132 ISP or its DARTs on a Mac with no ISP in
# its ADT (the J773g has none) makes the OS probe a camera and power domains that are not
# there; the rule touches a machine that does have an ISP, or another SoC; it disables a
# DART that another device shares; a malformed iommus list stops the boot; an earlier
# handoff step disables the ISP first, so the rule skips it and leaves its DARTs enabled.
TREE = r'''
static unsigned char blob[4096];
static void tree(bool enabled, bool shared)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "isp", "/isp@48c000000"));
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
'''

MAIN = TREE + r'''
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


HEADER = r'''
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
'''


def rule():
    kboot = (ROOT / "src/kboot_t8132.c").read_text()
    return (block(kboot, "static bool dt_iommu_shared(") + "\n"
            + block(kboot, "int dt_disable_t8132_absent_isp(") + "\n")


def test_t8132_isp_absent(tmp_path):
    run_c(tmp_path, "t8132-isp-absent", HEADER + rule() + MAIN)


# kboot_prepare_dt() runs the generic dt_set_isp() too. On a Mac with no ISP in its ADT it
# cannot reserve the ISP firmware and disables the node the isp alias names. Run the real
# dt_set_isp() and the real call sequence: the rule must still disable the DARTs only the
# ISP uses, and a machine with an ISP must still reach the generic step.
ORDER = r'''
#define T6040 0x6040
#define T6050 0x6050
#define T8140 0x8140
#define T8142 0x8142
typedef unsigned long u64;
static u32 board_id;
static int generic_ran;
static bool adt_is_compatible(const void *tree, int node, const char *c) { return false; }
static int dt_t6050_isp_transaction(void) { return -1; }
static int isp_get_heap(u64 *phys, u64 *iova, u64 *size) { return -1; }
static u64 isp_iova_base(void) { return 0; }
static int dt_set_isp_fwdata(void) { return 0; }
/* As dt_reserve_asc_segments_at(): no ADT node, no reservation. */
static int dt_reserve_asc_firmware(const char *adt_path, const char *adt_path_alt,
                                   const char *fdt_path, bool remap, u64 base)
{
    generic_ran++;
    return adt_path_offset(adt, adt_path) < 0 && adt_path_offset(adt, adt_path_alt) < 0 ? -1 : 0;
}
static void dt_set_j713_isp(void) {}
'''


def test_t8132_isp_absent_in_handoff_order(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    prepare = block(kboot, "int kboot_prepare_dt(")
    start = prepare.index("    if (dt_set_aop_firmware())\n        return -1;\n")
    start = prepare.index("\n", prepare.index("return -1;", start)) + 1
    steps = prepare[start:prepare.index("    if (dt_set_pmgr())", start)]
    assert "dt_set_isp()" in steps and "dt_disable_t8132_absent_isp(dt)" in steps
    run_c(tmp_path, "t8132-isp-absent-order", HEADER + rule() + ORDER
          + block(kboot, "static int dt_set_isp(void)") + TREE
          + "static int handoff(void)\n{\n" + steps + "    return 0;\n}\n" + r'''
int main(void)
{
    /* No ISP in the ADT: whichever step disables the ISP, its DARTs end disabled. */
    tree(true, false);
    assert(!handoff() && generic_ran == 1);
    assert(off("/isp@48c000000") && off("/iommu@48e8c0000") && off("/iommu@48e8e0000") &&
           off("/iommu@48e900000") && !off("/other@0"));
    /* A DART another device also uses stays as it is. */
    tree(true, true);
    assert(!handoff());
    assert(off("/iommu@48e8c0000") && off("/iommu@48e8e0000") && !off("/iommu@48e900000"));
    /* With an ISP in the ADT the rule does nothing and the generic step reserves it. */
    adt_isp = true;
    tree(true, false);
    generic_ran = 0;
    assert(!handoff() && generic_ran == 1);
    assert(!off("/isp@48c000000") && !off("/iommu@48e8c0000"));
    return 0;
}
''')
