/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libfdt/libfdt.h"
typedef uint32_t u32;
#define T6000 0x6000
#define T8112 0x8112
#define T8122 0x8122
#define T6041 0x6041
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
static void *dt, *adt;
static u32 chip_id, bdid, dvid, capacity;
static u32 bdid_len, dvid_len, capacity_len;
static bool chosen_present, nub_present, capacity_present, bdid_present, dvid_present;
static unsigned adt_reads;
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
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/chosen"))
        return chosen_present ? 1 : -1;
    assert(!strcmp(path, "/arm-io/pmp/iop-pmp-nub"));
    return nub_present ? 2 : -1;
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
    if (node == 2 && !strcmp(name, "dram-capacity") && capacity_present) {
        p = &capacity;
        size = capacity_len;
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
    puts("PMP boot data: T6000/T8112 aliases, status, byte order, missing/malformed ADT, FDT "
         "capacity and other-SoC regression PASS");
}
