/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfdt/libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SZ_16K         (1 << 14)
#define TREE_SIZE      (2 * SZ_16K)
#define ARRAY_SIZE(s)  (sizeof(s) / sizeof((s)[0]))
#define T6000          0x6000
#define T8112          0x8112
#define T8122          0x8122
#define T6041          0x6041
#define T6050          0x6050
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
static void *dt, *adt;
static int dt_bufsize = TREE_SIZE;
static u32 chip_id, bdid, dvid, capacity, variant[2], pmc_val, pmc_pmgr_val;
static u32 bdid_len, dvid_len, capacity_len, variant_len, pmc_len, pmc_pmgr_len;
static bool chosen_present, nub_present, capacity_present, bdid_present, dvid_present;
static bool variant_present, pmgr_present, pmc_present, pmc_pmgr_present;
static unsigned adt_reads;
/*
 * T6050's ADT: /arm-io has an unnamed non-PMP child, a DART that shares pmp0's PMP window
 * (not a PMP), pmp0, pmp1, and an optional malformed-name PMP. The PMP nodes' first reg is the
 * ASC, as in the ADT; the DT's first is the PMP window.
 */
enum {
    ARM_IO = 3,
    PMP0 = 4,
    NUB0 = 5,
    PMP1 = 6,
    NUB1 = 7,
    DART_PMP0 = 8,
    PMGR = 9,
    UNNAMED_OTHER = 10,
    BADNAME_PMP = 11,
};
static int arm_io_children[] = {UNNAMED_OTHER, DART_PMP0, BADNAME_PMP, PMP0, PMP1};
static unsigned arm_io_child_count;
static const u64 pmp0_regs[] = {0x284e00000, 0x284850000, 0x284500000, 0x284250000};
static const u64 pmp1_regs[] = {0x2284e00000, 0x2284500000};
static const u64 dart_regs[] = {0x284500000};
static bool nub0_present, nub1_present, pmp0_moved, pmp1_claims_pmp0, fail_malloc;
static int pmp0_bad_name;
static void *test_malloc(size_t size)
{
    return fail_malloc ? NULL : malloc(size);
}
/* kboot_pmp.c's section attribute (types.h), which a host build leaves out */
#define AURA_XTEXT
#define malloc test_malloc
static const unsigned char calibration[] = {0, 0x81, 0xff, 0x12, 0, 0x45, 0x67};
struct adt_property {
    const char *name;
    const void *value;
    u32 size;
};
static struct adt_property props[] = {
    {"soc-device", calibration, sizeof(calibration)},
    {"ptd-range", calibration, sizeof(calibration)},
    {"compatible", "iop-nub,rtbuddy-v2", 18},
    {"name", "pmp", 4},
    {"dram-capacity", &capacity, 4},
    {"region-base", calibration, sizeof(calibration)},
};
#define ADT_FOREACH_PROPERTY(tree, node, prop)                                                     \
    for (const struct adt_property *prop = ((void)(node), props); prop < props + sizeof(props) / sizeof(*props);   \
         prop++)
static int test_child_at(int parent, unsigned idx)
{
    if (parent == ARM_IO)
        return idx < arm_io_child_count ? arm_io_children[idx] : -1;
    if (parent == PMP0)
        return (idx == 0 && nub0_present) ? NUB0 : -1;
    if (parent == PMP1)
        return (idx == 0 && nub1_present) ? NUB1 : -1;
    assert(0);
    return -1;
}
#define ADT_FOREACH_CHILD(tree, node)                                                              \
    for (int _parent = (node), _next = 0, _i = 0;                                                  \
         (_next = test_child_at(_parent, (unsigned)_i)) >= 0 && ((node) = _next, 1); _i++)
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    adt_reads++;
    if (!strcmp(path, "/chosen"))
        return chosen_present ? 1 : -1;
    if (!strcmp(path, "/arm-io/pmgr"))
        return pmgr_present ? PMGR : -1;
    if (!strcmp(path, "/arm-io/pmp0/iop-pmp0-nub"))
        return nub0_present ? NUB0 : -1;
    if (!strcmp(path, "/arm-io/pmp1/iop-pmp1-nub"))
        return nub1_present ? NUB1 : -1;
    assert(!strcmp(path, "/arm-io/pmp/iop-pmp-nub"));
    return nub_present ? 2 : -1;
}
static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    adt_reads++;
    assert(!strcmp(path, "/arm-io"));
    memset(trace, 0, 8 * sizeof(*trace));
    trace[0] = ARM_IO;
    return ARM_IO;
}
static int adt_get_reg(void *tree, int *trace, const char *prop, int index, u64 *addr, u64 *size)
{
    (void)tree;
    adt_reads++;
    assert(trace[0] == ARM_IO && !trace[2] && !strcmp(prop, "reg") && !size);
    const u64 *regs = trace[1] == PMP0 ? pmp0_regs : trace[1] == PMP1 ? pmp1_regs : dart_regs;
    int count = trace[1] == PMP0   ? (int)ARRAY_SIZE(pmp0_regs)
                : trace[1] == PMP1 ? (int)ARRAY_SIZE(pmp1_regs)
                                   : (int)ARRAY_SIZE(dart_regs);
    assert(trace[1] == PMP0 || trace[1] == PMP1 || trace[1] == DART_PMP0);
    if (index < 0 || index >= count)
        return -1;
    *addr = regs[index];
    if (trace[1] == PMP0 && pmp0_moved)
        *addr += 0x100000000;
    if (trace[1] == PMP1 && pmp1_claims_pmp0 && index == 1)
        *addr = 0x284500000;
    return 0;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    adt_reads++;
    const void *p = NULL;
    u32 size = 0;
    if (node == 1 && bdid_present && !strcmp(name, "board-id")) {
        p = &bdid;
        size = bdid_len;
    }
    if (node == 1 && dvid_present && !strcmp(name, "dram-vendor-id")) {
        p = &dvid;
        size = dvid_len;
    }
    if ((node == 2 || node == NUB0 || node == NUB1) && !strcmp(name, "dram-capacity") &&
        capacity_present) {
        p = &capacity;
        size = capacity_len;
    }
    if ((node == 2 || node == NUB0 || node == NUB1) && !strcmp(name, "soc-chip-variant") &&
        variant_present) {
        p = &variant[node == NUB1];
        size = variant_len;
    }
    if (node == PMGR && pmc_present && !strcmp(name, "pmc")) {
        p = &pmc_val;
        size = pmc_len;
    }
    if (node == PMGR && pmc_pmgr_present && !strcmp(name, "pmc-pmgr")) {
        p = &pmc_pmgr_val;
        size = pmc_pmgr_len;
    }
    if ((node == PMP0 || node == PMP1 || node == BADNAME_PMP) && !strcmp(name, "device_type")) {
        p = "pmp";
        size = 4;
    }
    if (node == DART_PMP0 && !strcmp(name, "device_type")) {
        p = "dart";
        size = 5;
    }
    if (node == PMP0 && !strcmp(name, "name")) {
        if (pmp0_bad_name == 1) {
            p = "pmp0";
            size = 4; /* missing NUL terminator */
        } else if (pmp0_bad_name == 2) {
            p = "\xff\xfe";
            size = 3; /* invalid UTF-8 */
        } else if (pmp0_bad_name == 3) {
            p = "pmp/0";
            size = 6; /* path separator */
        } else {
            p = "pmp0";
            size = 5;
        }
    }
    if (node == PMP1 && !strcmp(name, "name")) {
        p = "pmp1";
        size = 5;
    }
    if (node == NUB0 && !strcmp(name, "name")) {
        p = "iop-pmp0-nub";
        size = 13;
    }
    if (node == NUB1 && !strcmp(name, "name")) {
        p = "iop-pmp1-nub";
        size = 13;
    }
    if (node == BADNAME_PMP && !strcmp(name, "name")) {
        p = "bad";
        size = 3; /* missing NUL terminator */
    }
    if (len)
        *len = size;
    return p;
}
static int adt_copy(void *tree, int node, const char *name, void *out, size_t len)
{
    u32 size;
    const void *p = adt_getprop(tree, node, name, &size);
    if (!p || size != len)
        return -1;
    memcpy(out, p, len);
    return 0;
}
#define ADT_GETPROP(tree, node, name, val) adt_copy(tree, node, name, val, sizeof(*(val)))
/* SOURCE FUNCTIONS */
static int setup(char *tree, u32 chip, bool alias, const char *status)
{
    dt = tree;
    chip_id = chip;
    bdid = 0x12345678;
    dvid = 0x89abcdef;
    capacity = 0x10203040;
    bdid_len = dvid_len = capacity_len = variant_len = pmc_len = pmc_pmgr_len = 4;
    pmc_val = 0x1357;
    pmc_pmgr_val = 0x2468;
    chosen_present = nub_present = capacity_present = bdid_present = dvid_present = true;
    variant_present = pmgr_present = pmc_present = pmc_pmgr_present = true;
    nub0_present = nub1_present = true;
    pmp0_moved = pmp1_claims_pmp0 = fail_malloc = false;
    pmp0_bad_name = 0;
    arm_io_child_count = ARRAY_SIZE(arm_io_children);
    variant[0] = 0;
    variant[1] = 0x2;
    adt_reads = 0;
    assert(!fdt_create_empty_tree(dt, TREE_SIZE));
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    int pmp = fdt_add_subnode(dt, soc, "pmp@1000");
    assert(pmp >= 0);
    assert(!fdt_setprop_string(dt, pmp, "compatible",
                               chip == T8112 ? "apple,t8112-pmp-v2" : "apple,t6000-pmp-v2"));
    if (status)
        assert(!fdt_setprop_string(dt, pmp, "status", status));
    if (alias) {
        int aliases = fdt_path_offset(dt, "/aliases");
        assert(!fdt_setprop_string(dt, aliases, "pmp", "/soc/pmp@1000"));
    }
    return fdt_path_offset(dt, "/soc/pmp@1000");
}
static void check_scalar(int node, const char *name, u32 expected)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, name, &len);
    assert(v && len == 4 && fdt32_ld(v) == expected);
}
static void check_data(int pmp, const char *status)
{
    check_scalar(pmp, "apple,board-id", bdid);
    check_scalar(pmp, "apple,dram-vendor-id", dvid);
    if (capacity_present && capacity != UINT32_MAX)
        check_scalar(pmp, "apple,dram-capacity", capacity);
    else
        assert(!fdt_getprop(dt, pmp, "apple,dram-capacity", NULL));
    int len;
    const void *p = fdt_getprop(dt, pmp, "apple,tunable-soc-device", &len);
    assert(p && len == sizeof(calibration) && !memcmp(p, calibration, len));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-compatible", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-name", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-dram-capacity", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-region-base", NULL));
    assert(!strcmp(fdt_getprop(dt, pmp, "status", NULL), status));
}
/* A T6050 board DT: pmp0 (and pmp1) with no pmp alias, and another Mac's values in pmp0 */
static const unsigned char stale[] = {0xde, 0xad};
static int add_t6050_pmp(int soc, const char *name, u64 window, const char *status)
{
    static const char compatible[] = "apple,t6050-pmp-v2\0apple,t6000-pmp-v2";
    int pmp = fdt_add_subnode(dt, soc, name);
    assert(pmp >= 0);
    assert(!fdt_setprop(dt, pmp, "compatible", compatible, sizeof(compatible)));
    fdt64_t reg[4] = {cpu_to_fdt64(window), cpu_to_fdt64(0x100000), cpu_to_fdt64(window + 0x900000),
                      cpu_to_fdt64(0x4000)};
    assert(!fdt_setprop(dt, pmp, "reg", reg, sizeof(reg)));
    assert(!fdt_setprop_u32(dt, pmp, "apple,board-id", 8));
    assert(!fdt_setprop_u32(dt, pmp, "apple,dram-vendor-id", 255));
    assert(!fdt_setprop_u32(dt, pmp, "apple,pmc", 1));
    assert(!fdt_setprop_u32(dt, pmp, "apple,pmc-pmgr", 63));
    assert(!fdt_setprop(dt, pmp, "apple,tunable-soc-device", stale, sizeof(stale)));
    assert(!fdt_setprop_string(dt, pmp, "apple,tunable-stale-only", "other-unit"));
    assert(!fdt_setprop_u32(dt, pmp, "apple,dram-channel-disable", 0xdeadbeef));
    assert(!fdt_setprop_u32(dt, pmp, "apple,dram-capacity", 0x99999999));
    if (status)
        assert(!fdt_setprop_string(dt, pmp, "status", status));
    return pmp;
}
static void setup_t6050(char *tree, const char *status, bool second)
{
    setup(tree, T6050, false, "disabled");
    assert(!fdt_create_empty_tree(dt, TREE_SIZE));
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(dt, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, soc, "#size-cells", 2));
    assert(!fdt_setprop(dt, soc, "ranges", NULL, 0));
    add_t6050_pmp(soc, "pmp@284e00000", 0x284500000, status);
    if (second)
        add_t6050_pmp(fdt_path_offset(dt, "/soc"), "pmp@2284e00000", 0x2284500000, "okay");
}
/* The DT as it was: a refusal or no PMP leaves every byte, and the boot goes on */
static void check_unchanged(const char *what)
{
    static char before[TREE_SIZE];
    memcpy(before, dt, sizeof(before));
    assert(!dt_set_pmp());
    if (memcmp(before, dt, sizeof(before))) {
        printf("FAIL: %s changed the DT\n", what);
        assert(0);
    }
}
static void check_t6050(const char *path, u32 chip_variant)
{
    int pmp = fdt_path_offset(dt, path);
    check_data(pmp, "okay");
    check_scalar(pmp, "apple,pmc", pmc_val);
    check_scalar(pmp, "apple,pmc-pmgr", pmc_pmgr_val);
    if (variant_present)
        check_scalar(pmp, "apple,soc-chip-variant", chip_variant);
    else
        assert(!fdt_getprop(dt, pmp, "apple,soc-chip-variant", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-stale-only", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,dram-channel-disable", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,pmc-msg-disabled", NULL));
}
static void t6050_cases(void)
{
    char tree[TREE_SIZE];
    /* Paired by its window: this Mac's values replace the board DT's, status stays */
    setup_t6050(tree, "okay", false);
    assert(!dt_set_pmp());
    check_t6050("/soc/pmp@284e00000", 0);
    /* An enabled node without status is enabled */
    setup_t6050(tree, NULL, false);
    assert(!dt_set_pmp());
    assert(fdt32_ld(fdt_getprop(dt, fdt_path_offset(dt, "/soc/pmp@284e00000"), "apple,board-id",
                                NULL)) == bdid);
    /* Two dies: each PMP from its own nub */
    setup_t6050(tree, "okay", true);
    assert(!dt_set_pmp());
    check_t6050("/soc/pmp@284e00000", 0);
    check_t6050("/soc/pmp@2284e00000", 0x2);
    /* No dram-capacity in the nub: stale DT value is dropped */
    setup_t6050(tree, "okay", false);
    capacity_present = false;
    assert(!dt_set_pmp());
    check_t6050("/soc/pmp@284e00000", 0);
    /* Clean DT with no board-id, dram-vendor-id, pmc, pmc-pmgr, or tunables: filled from ADT */
    setup_t6050(tree, "okay", false);
    int pmp0 = fdt_path_offset(dt, "/soc/pmp@284e00000");
    assert(!fdt_delprop(dt, pmp0, "apple,board-id"));
    assert(!fdt_delprop(dt, pmp0, "apple,dram-vendor-id"));
    assert(!fdt_delprop(dt, pmp0, "apple,pmc"));
    assert(!fdt_delprop(dt, pmp0, "apple,pmc-pmgr"));
    assert(!fdt_delprop(dt, pmp0, "apple,tunable-soc-device"));
    assert(!fdt_delprop(dt, pmp0, "apple,tunable-stale-only"));
    pmc_val = 2;
    pmc_pmgr_val = 0x3f;
    assert(!dt_set_pmp());
    check_t6050("/soc/pmp@284e00000", 0);
    /* Malformed optional bootarg or pmgr u32 rolls back atomically (no mixed DT) */
    setup_t6050(tree, "okay", false);
    variant_len = 2;
    check_unchanged("a malformed optional boot argument");
    setup_t6050(tree, "okay", false);
    pmc_pmgr_len = 2;
    check_unchanged("a malformed pmgr pmc-pmgr");
    /* A PMP node with an unterminated, non-UTF-8 or slash ADT name is skipped safely */
    for (int bad = 1; bad <= 3; bad++) {
        setup_t6050(tree, "okay", false);
        pmp0_bad_name = bad;
        check_unchanged("a PMP with a malformed ADT name");
    }

    /* A board that leaves pmp0 off: no ADT read, nothing written */
    setup_t6050(tree, "disabled", false);
    check_unchanged("a disabled pmp0");
    assert(!adt_reads);
    /* Only the DART has the window */
    setup_t6050(tree, "okay", false);
    pmp0_moved = true;
    check_unchanged("an unpaired window");
    /* Two ADT PMPs have it */
    setup_t6050(tree, "okay", false);
    pmp1_claims_pmp0 = true;
    check_unchanged("an ambiguous window");
    /* No /arm-io children at all */
    setup_t6050(tree, "okay", false);
    arm_io_child_count = 0;
    check_unchanged("an ADT without PMPs");
    /* The paired PMP has no nub */
    setup_t6050(tree, "okay", false);
    nub0_present = false;
    check_unchanged("a missing nub");
    /* /chosen and the scalars it must hold */
    setup_t6050(tree, "okay", false);
    chosen_present = false;
    check_unchanged("a missing /chosen");
    setup_t6050(tree, "okay", false);
    bdid_present = false;
    check_unchanged("a missing board-id");
    setup_t6050(tree, "okay", false);
    dvid_present = false;
    check_unchanged("a missing dram-vendor-id");
    const u32 bad_lengths[] = {0, 1, 3, 8};
    for (unsigned field = 0; field < 3; field++) {
        for (unsigned n = 0; n < 4; n++) {
            setup_t6050(tree, "okay", false);
            u32 *len = field == 0 ? &bdid_len : field == 1 ? &dvid_len : &capacity_len;
            *len = bad_lengths[n];
            check_unchanged("a malformed scalar");
        }
    }
    /* No room for the copy: every byte back */
    setup_t6050(tree, "okay", false);
    assert(!fdt_pack(dt));
    check_unchanged("a full DT");
    /* Absent /arm-io/pmgr still clears stale apple,pmc and apple,pmc-pmgr */
    setup_t6050(tree, "okay", false);
    pmp0 = fdt_path_offset(dt, "/soc/pmp@284e00000");
    assert(!fdt_setprop_u32(dt, pmp0, "apple,pmc", 0xdeadbeef));
    assert(!fdt_setprop_u32(dt, pmp0, "apple,pmc-pmgr", 0xfeedface));
    pmgr_present = false;
    assert(!dt_set_pmp());
    pmp0 = fdt_path_offset(dt, "/soc/pmp@284e00000");
    assert(!fdt_getprop(dt, pmp0, "apple,pmc", NULL));
    assert(!fdt_getprop(dt, pmp0, "apple,pmc-pmgr", NULL));
    /* Legacy alias path keeps skipping malformed optional bootargs instead of aborting boot */
    int pmp_alias = setup(tree, T6041, true, "disabled");
    variant_len = 2;
    assert(!dt_set_pmp());
    check_data(pmp_alias, "okay");
    assert(!fdt_getprop(dt, pmp_alias, "apple,soc-chip-variant", NULL));
    /* Legacy alias path still propagates FDT write failures from optional bootargs */
    pmp_alias = setup(tree, T6041, true, "disabled");
    assert(!dt_set_pmp());
    check_scalar(pmp_alias, "apple,soc-chip-variant", variant[0]);
    pmp_alias = setup(tree, T6041, true, "disabled");
    assert(!fdt_setprop_u32(dt, pmp_alias, "apple,board-id", bdid));
    assert(!fdt_setprop_u32(dt, pmp_alias, "apple,dram-vendor-id", dvid));
    assert(!fdt_setprop_u32(dt, pmp_alias, "apple,dram-capacity", capacity));
    assert(
        !fdt_setprop(dt, pmp_alias, "apple,tunable-soc-device", calibration, sizeof(calibration)));
    assert(
        !fdt_setprop(dt, pmp_alias, "apple,tunable-ptd-range", calibration, sizeof(calibration)));
    assert(!fdt_pack(dt));
    /* A packed FDT fails the up-front fit check: the PMP stays disabled and no bootarg lands */
    assert(!dt_set_pmp());
    assert(!strcmp(fdt_getprop(dt, pmp_alias, "status", NULL), "disabled"));
    assert(!fdt_getprop(dt, pmp_alias, "apple,soc-chip-variant", NULL));
    /* No memory for the snapshot: nothing written */
    setup_t6050(tree, "okay", false);
    fail_malloc = true;
    check_unchanged("no memory");
    /* A node without reg is not paired */
    setup_t6050(tree, "okay", false);
    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/pmp@284e00000"), "reg"));
    check_unchanged("a node without reg");
    /* An unterminated status property is ignored safely (not paired) */
    setup_t6050(tree, "okay", false);
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/pmp@284e00000"), "status", "okay", 4));
    check_unchanged("an unterminated status");
}
static void check_shortfall(char *tree)
{
    // Missing status also means enabled. Check both representations at the space limit.
    const char *statuses[] = {"okay", NULL, "disabled"};
    for (unsigned i = 0; i < ARRAY_SIZE(statuses); i++) {
        for (unsigned room = 0; room < 2; room++) {
            int pmp = setup(tree, T8112, true, statuses[i]);
            assert(!fdt_pack(dt));
            assert(!fdt_open_into(dt, dt, fdt_totalsize(dt) + room * 32));
            assert(pmp_alias_space(2) > room * 32);

            bool can_disable = room || i == 2;
            int ret = dt_set_pmp();
            if (can_disable) {
                assert(!ret);
                assert(!strcmp(fdt_getprop(dt, pmp, "status", NULL), "disabled"));
            } else {
                assert(ret < 0);
            }
            assert(!fdt_getprop(dt, pmp, "apple,board-id", NULL));
            assert(!fdt_getprop(dt, pmp, "apple,tunable-soc-device", NULL));
        }
    }

    int pmp = setup(tree, T8112, true, "okay");
    int strings_size = fdt_size_dt_strings(dt);
    fdt_set_size_dt_strings(dt, fdt_totalsize(dt));
    assert(dt_set_pmp() < 0);
    fdt_set_size_dt_strings(dt, strings_size);
    assert(!fdt_getprop(dt, pmp, "apple,board-id", NULL));
}

int main(void)
{
    (void)dt_fill_pmp; /* Also keep the helper used in the old-code control. */
    char tree[TREE_SIZE];
    u32 chips[] = {T6000, T8112};
    const char *statuses[] = {"disabled", "okay"};
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned alias = 0; alias < 2; alias++) {
            for (unsigned s = 0; s < 2; s++) {
                int pmp = setup(tree, chips[c], alias, statuses[s]);
                bdid += c + alias;
                dvid -= c + s;
                assert(!dt_set_pmp());
                /* The alias opts the node in: dt_set_pmp() enables it, as before. */
                check_data(pmp, alias ? "okay" : statuses[s]);
            }
        }
        int pmp = setup(tree, chips[c], false, "disabled");
        capacity_present = false;
        assert(!dt_set_pmp());
        check_data(pmp, "disabled");
        pmp = setup(tree, chips[c], false, "disabled");
        capacity = UINT32_MAX;
        assert(!dt_set_pmp());
        check_data(pmp, "disabled");
        const u32 bad_lengths[] = {0, 1, 3, 8};
        for (unsigned field = 0; field < 3; field++) {
            for (unsigned n = 0; n < 4; n++) {
                pmp = setup(tree, chips[c], false, "disabled");
                u32 *len = field == 0 ? &bdid_len : field == 1 ? &dvid_len : &capacity_len;
                *len = bad_lengths[n];
                assert(dt_set_pmp() < 0);
                assert(!fdt_getprop(dt, pmp, "apple,board-id", NULL));
                assert(!strcmp(fdt_getprop(dt, pmp, "status", NULL), "disabled"));
            }
        }
        pmp = setup(tree, chips[c], false, "disabled");
        chosen_present = false;
        assert(dt_set_pmp() < 0);
        pmp = setup(tree, chips[c], false, "disabled");
        bdid_present = false;
        assert(dt_set_pmp() < 0);
        pmp = setup(tree, chips[c], false, "disabled");
        dvid_present = false;
        assert(dt_set_pmp() < 0);
        pmp = setup(tree, chips[c], false, "disabled");
        nub_present = false;
        assert(dt_set_pmp() < 0);
        pmp = setup(tree, chips[c], false, "disabled");
        assert(!fdt_del_node(dt, pmp));
        assert(!dt_set_pmp() && !adt_reads);
        pmp = setup(tree, chips[c], false, "disabled");
        assert(!fdt_pack(dt));
        assert(dt_set_pmp() < 0);
        assert(!strcmp(fdt_getprop(dt, pmp, "status", NULL), "disabled"));
    }
    int pmp = setup(tree, T6041, true, "disabled");
    assert(!dt_set_pmp());
    check_data(pmp, "okay");
    // The flag must not change the size estimate or the bytes copied.
    props[0].size |= 0x80000000U;
    pmp = setup(tree, T6041, true, "disabled");
    size_t need = 9 * sizeof(struct fdt_property) + 2 * 8 + 6 * 4 + 8 +
                  sizeof("apple,tunable-soc-device") + sizeof("apple,tunable-ptd-range") +
                  sizeof("apple,board-id") + sizeof("apple,dram-vendor-id") +
                  sizeof("apple,dram-capacity") + sizeof("apple,dram-channel-disable") +
                  sizeof("apple,pmc-msg-disabled") + sizeof("apple,soc-chip-variant") +
                  sizeof("status");
    assert(pmp_alias_space(2) == need);
    assert(!fdt_pack(dt));
    int packed = fdt_totalsize(dt);
    assert(!fdt_open_into(dt, dt, packed + need - 1));
    assert(!dt_set_pmp());
    assert(!strcmp(fdt_getprop(dt, pmp, "status", NULL), "disabled"));
    assert(!fdt_getprop(dt, pmp, "apple,board-id", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-soc-device", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,tunable-ptd-range", NULL));
    assert(!fdt_open_into(dt, dt, packed + need));
    assert(!dt_set_pmp());
    check_data(pmp, "okay");
    check_shortfall(tree);
    setup(tree, T6041, false, "disabled");
    assert(!dt_set_pmp() && !adt_reads);
    setup(tree, T8122, true, "disabled");
    assert(!dt_set_pmp() && !adt_reads);
    t6050_cases();
    puts("PMP boot data: T6000/T8112 aliases, status, byte order, missing/malformed ADT, FDT "
         "capacity, other-SoC regression and T6050 pmp0 pairing by window PASS");
}
