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
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define T6000 0x6000
#define T8112 0x8112
#define T8122 0x8122
#define T6041 0x6041
#define T6050 0x6050
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
static void *dt, *adt;
static int dt_bufsize = 8192;
static u32 chip_id, bdid, dvid, capacity, variant[2];
static u32 bdid_len, dvid_len, capacity_len;
static bool chosen_present, nub_present, capacity_present, bdid_present, dvid_present;
static unsigned adt_reads;
/*
 * T6050's ADT: /arm-io has a DART that shares pmp0's PMP window (not a PMP), pmp0 and pmp1. The
 * PMP nodes' first reg is the ASC, as in the ADT; the DT's first is the PMP window.
 */
enum { ARM_IO = 3, PMP0 = 4, NUB0 = 5, PMP1 = 6, NUB1 = 7, DART_PMP0 = 8 };
static int arm_io_children[] = {DART_PMP0, PMP0, PMP1};
static unsigned arm_io_child_count;
static const u64 pmp0_regs[] = {0x284e00000, 0x284850000, 0x284500000, 0x284250000};
static const u64 pmp1_regs[] = {0x2284e00000, 0x2284500000};
static const u64 dart_regs[] = {0x284500000};
static bool nub0_present, nub1_present, pmp0_moved, pmp1_claims_pmp0, fail_malloc;
static void *test_malloc(size_t size)
{
    return fail_malloc ? NULL : malloc(size);
}
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
    for (const struct adt_property *prop = props; prop < props + sizeof(props) / sizeof(*props);   \
         prop++)
#define ADT_FOREACH_CHILD(tree, node)                                                              \
    for (unsigned _i = (assert((node) == ARM_IO), 0);                                              \
         _i < arm_io_child_count && ((node) = arm_io_children[_i], 1); _i++)
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    adt_reads++;
    if (!strcmp(path, "/chosen"))
        return chosen_present ? 1 : -1;
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
static const char *adt_get_name(void *tree, int node)
{
    (void)tree;
    return node == PMP0 ? "pmp0" : node == PMP1 ? "pmp1" : node == DART_PMP0 ? "dart-pmp0" : NULL;
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
    if ((node == NUB0 || node == NUB1) && !strcmp(name, "soc-chip-variant")) {
        p = &variant[node == NUB1];
        size = 4;
    }
    if ((node == PMP0 || node == PMP1) && !strcmp(name, "device_type")) {
        p = "pmp";
        size = 4;
    }
    if (node == DART_PMP0 && !strcmp(name, "device_type")) {
        p = "dart";
        size = 5;
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
    bdid_len = dvid_len = capacity_len = 4;
    chosen_present = nub_present = capacity_present = bdid_present = dvid_present = true;
    nub0_present = nub1_present = true;
    pmp0_moved = pmp1_claims_pmp0 = fail_malloc = false;
    arm_io_child_count = ARRAY_SIZE(arm_io_children);
    variant[0] = 0;
    variant[1] = 0x2;
    adt_reads = 0;
    assert(!fdt_create_empty_tree(dt, 8192));
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    int pmp = fdt_add_subnode(dt, soc, "pmp@1000");
    assert(pmp >= 0);
    assert(!fdt_setprop_string(dt, pmp, "compatible",
                               chip == T8112 ? "apple,t8112-pmp-v2" : "apple,t6000-pmp-v2"));
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
    assert(!fdt_setprop_string(dt, pmp, "apple,tunable-firmware-name", "t6050pmp"));
    if (status)
        assert(!fdt_setprop_string(dt, pmp, "status", status));
    return pmp;
}
static void setup_t6050(char *tree, const char *status, bool second)
{
    setup(tree, T6050, false, "disabled");
    assert(!fdt_create_empty_tree(dt, 8192));
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
    static char before[8192];
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
    check_scalar(pmp, "apple,pmc", 1);
    check_scalar(pmp, "apple,pmc-pmgr", 63);
    check_scalar(pmp, "apple,soc-chip-variant", chip_variant);
    assert(!strcmp(fdt_getprop(dt, pmp, "apple,tunable-firmware-name", NULL), "t6050pmp"));
    assert(!fdt_getprop(dt, pmp, "apple,dram-channel-disable", NULL));
    assert(!fdt_getprop(dt, pmp, "apple,pmc-msg-disabled", NULL));
}
static void t6050_cases(void)
{
    char tree[8192];
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
    /* No dram-capacity in the nub: none in the DT */
    setup_t6050(tree, "okay", false);
    capacity_present = false;
    assert(!dt_set_pmp());
    check_t6050("/soc/pmp@284e00000", 0);

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
    /* No memory for the snapshot: nothing written */
    setup_t6050(tree, "okay", false);
    fail_malloc = true;
    check_unchanged("no memory");
    /* A node without reg is not paired */
    setup_t6050(tree, "okay", false);
    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/pmp@284e00000"), "reg"));
    check_unchanged("a node without reg");
}
int main(void)
{
    (void)dt_fill_pmp; /* Also keep the helper used in the old-code control. */
    char tree[8192];
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
    setup(tree, T6041, false, "disabled");
    assert(!dt_set_pmp() && !adt_reads);
    setup(tree, T8122, true, "disabled");
    assert(!dt_set_pmp() && !adt_reads);
    t6050_cases();
    puts("PMP boot data: T6000/T8112 aliases, status, byte order, missing/malformed ADT, FDT "
         "capacity, other-SoC regression and T6050 pmp0 pairing by window PASS");
}
