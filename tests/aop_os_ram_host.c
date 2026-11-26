/* SPDX-License-Identifier: MIT */
/*
 * Host check of the locked-DART AOP OS-RAM reservation and failure handling extracted directly from
 * src/kboot.c (dt_find_aop, dt_try_reserve_ram_mem, dt_reserve_aop_os_ram and
 * dt_aop_locked_dart_handoff) against real src/fdt_disable.c and libfdt.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fdt_disable.h"

#include "libfdt/libfdt.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;

#define SZ_16K           0x4000UL
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define PACKED           __attribute__((packed))

struct adt_segment_ranges {
    u64 phys;
    u64 iova;
    u64 remap;
    u32 size;
    u32 unk;
} PACKED;

typedef struct asc_dev {
    int dummy;
} asc_dev_t;

static int bailed;
#define bail(...)                                                                                  \
    do {                                                                                           \
        bailed++;                                                                                  \
        printf("BAIL: " __VA_ARGS__);                                                              \
        return -1;                                                                                 \
    } while (0)

static void *dt;
static void *adt = (void *)1;

static u64 test_ram_base, test_ram_size;
static u64 test_dram_base, test_dram_size;
static bool test_chosen_ok;
static const void *test_segs;
static u32 test_segs_len;

static asc_dev_t test_asc;
static bool test_asc_ok;
static bool test_asc_running;

static u64 kboot_ram_base(void)
{
    return test_ram_base;
}

static u64 kboot_ram_size(void)
{
    return test_ram_size;
}

static int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/chosen"))
        return test_chosen_ok ? 1 : -1;
    return -1;
}

static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if (node == 2 && !strcmp(name, "segment-ranges")) {
        if (len)
            *len = test_segs_len;
        return test_segs;
    }
    if (len)
        *len = 0;
    return NULL;
}

static int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    (void)tree;
    if (node == 1 && !strcmp(name, "dram-base") && len == sizeof(u64)) {
        memcpy(out, &test_dram_base, sizeof(u64));
        return (int)sizeof(u64);
    }
    if (node == 1 && !strcmp(name, "dram-size") && len == sizeof(u64)) {
        memcpy(out, &test_dram_size, sizeof(u64));
        return (int)sizeof(u64);
    }
    return -1;
}

#define ADT_GETPROP(tree, node, name, val) adt_getprop_copy(tree, node, name, (val), sizeof(*(val)))

static asc_dev_t *asc_init(const char *path)
{
    assert(!strcmp(path, "/arm-io/aop"));
    return test_asc_ok ? &test_asc : NULL;
}

static bool asc_cpu_running(asc_dev_t *asc)
{
    assert(asc == &test_asc);
    return test_asc_running;
}

static void asc_free(asc_dev_t *asc)
{
    assert(asc == &test_asc);
}

/* SOURCE FUNCTIONS */

#define TREE_SIZE 8192

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

static int build_tree(void *buf, size_t cap, const char *aop_status, bool pack_tree)
{
    assert(!fdt_create(buf, (int)cap));
    assert(!fdt_finish_reservemap(buf));
    assert(!fdt_begin_node(buf, ""));
    assert(!fdt_property_u32(buf, "#address-cells", 2));
    assert(!fdt_property_u32(buf, "#size-cells", 2));

    assert(!fdt_begin_node(buf, "reserved-memory"));
    assert(!fdt_property_u32(buf, "#address-cells", 2));
    assert(!fdt_property_u32(buf, "#size-cells", 2));
    assert(!fdt_property(buf, "ranges", "", 0));
    assert(!fdt_end_node(buf));

    assert(!fdt_begin_node(buf, "soc"));
    assert(!fdt_begin_node(buf, "aop@290c00000"));
    assert(!fdt_property_string(buf, "compatible", "apple,t6050-aop"));
    assert(!fdt_property_u32(buf, "phandle", 42));
    if (aop_status)
        assert(!fdt_property_string(buf, "status", aop_status));
    assert(!fdt_begin_node(buf, "audio"));
    assert(!fdt_property_u32(buf, "phandle", 43));
    assert(!fdt_end_node(buf));
    assert(!fdt_end_node(buf));
    assert(!fdt_end_node(buf));

    assert(!fdt_end_node(buf));
    assert(!fdt_finish(buf));

    int packed_size = fdt_totalsize(buf);
    if (!pack_tree)
        assert(!fdt_open_into(buf, buf, (int)cap));
    dt = buf;
    return packed_size;
}

static void reset_env(const void *segs, u32 len)
{
    bailed = 0;
    test_dram_base = 0x10000000000ULL;
    test_dram_size = 0x01800000000ULL; /* 96 GiB */
    test_ram_base = 0x10002000000ULL;
    test_ram_size = 0x017fe000000ULL;
    test_chosen_ok = true;
    test_segs = segs;
    test_segs_len = len;
    test_asc_ok = true;
    test_asc_running = false;
}

static void add_resv_node(const char *name, u64 base, u64 size, bool nomap, bool with_reg)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    assert(resv >= 0);
    int n = fdt_add_subnode(dt, resv, name);
    assert(n >= 0);
    if (with_reg) {
        u64 reg[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
        assert(!fdt_setprop(dt, n, "reg", reg, sizeof(reg)));
    }
    assert(!fdt_setprop_string(dt, n, "compatible", "apple,other-mem"));
    if (nomap)
        assert(!fdt_setprop_empty(dt, n, "no-map"));
}

static bool has_resv(const char *name, u64 want_base, u64 want_size)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return false;
    int n = fdt_subnode_offset(dt, resv, name);
    if (n < 0)
        return false;
    int rlen = 0;
    const fdt64_t *reg = fdt_getprop(dt, n, "reg", &rlen);
    if (!reg || rlen != 16 || !fdt_getprop(dt, n, "no-map", NULL))
        return false;
    return fdt64_ld(&reg[0]) == want_base && fdt64_ld(&reg[1]) == want_size;
}

int main(void)
{
    static char tree[TREE_SIZE];

    /* 1. Normal aligned OS-RAM log reserved; non-OS-RAM segments skipped */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x10000ecc000ULL, .size = 0x154000},
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        check(ret == 0 && bailed == 0 &&
                  has_resv("asc-firmware@1000ff18000", 0x1000ff18000ULL, 0x2c000) &&
                  fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"),
                                     "asc-firmware@10000ecc000") < 0 &&
                  !fdt_check_full(dt, TREE_SIZE),
              "aligned OS-RAM segment reserved no-map, non-OS-RAM skipped");
    }

    /* 2. Unaligned r.phys spanning a 16K boundary rounds down start and rounds up end */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff19000ULL, .size = 0x5000}, /* spans 0x1000ff18000..0x1000ff20000 */
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret == 0 && has_resv("asc-firmware@1000ff18000", 0x1000ff18000ULL, 0x8000) &&
                  !fdt_check_full(dt, TREE_SIZE),
              "unaligned r.phys spanning 16K boundary reserves both pages");
    }

    /* 3. u64 wrap on r.phys + r.size is refused (never silently skipped) */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = UINT64_MAX - 0x7fffULL, .size = 0x10000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret < 0, "u64 wrap on r.phys + size fails closed");
    }

    /* 4. u64 wrap on 16K ALIGN_UP of end is refused */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = UINT64_MAX - 0xfffULL, .size = 0xfff},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret < 0, "u64 wrap on 16K rounding of end fails closed");
    }

    /* 5. Segment count > 64 is refused */
    {
        struct adt_segment_ranges segs[65];
        memset(segs, 0, sizeof(segs));
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret < 0, "segment list > 64 entries refused");
    }

    /* 6. Segment in OS RAM outside ADT DRAM is refused */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x11800000000ULL, .size = 0x4000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        test_ram_size = 0x01900000000ULL;
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret < 0, "OS-RAM segment outside ADT DRAM refused");
    }

    /* 7. Already covered by a larger existing no-map node: succeeds without clobbering */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        add_resv_node("other-carveout@1000ff00000", 0x1000ff00000ULL, 0x100000, true, true);
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret == 0 && has_resv("other-carveout@1000ff00000", 0x1000ff00000ULL, 0x100000) &&
                  fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"),
                                     "asc-firmware@1000ff18000") < 0,
              "already covered by existing no-map reservation: untouched, no duplicate");
    }

    /* 8. Partial overlap with existing node: refused and existing node untouched */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        add_resv_node("asc-firmware@1000ff18000", 0x1000ff18000ULL, 0x10000, true, true);
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        check(ret < 0 && has_resv("asc-firmware@1000ff18000", 0x1000ff18000ULL, 0x10000),
              "partial overlap / smaller same-name node refused without clobbering");
    }

    /* 9. Overlap with existing non-no-map node: refused and existing node untouched */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", false);
        reset_env(segs, sizeof(segs));
        add_resv_node("dma-pool@1000ff00000", 0x1000ff00000ULL, 0x100000, false, true);
        int ret = dt_reserve_aop_os_ram(2, "/soc/aop@290c00000");
        int n =
            fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), "dma-pool@1000ff00000");
        check(ret < 0 && n >= 0 && !fdt_getprop(dt, n, "no-map", NULL),
              "overlap with non-no-map reservation refused without clobbering");
    }

    /* 10. Packed FDT + halted AOP ("okay"): fails reservation, sets "fail" in place, no bail */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        int sz = build_tree(tree, TREE_SIZE, "okay", true);
        reset_env(segs, sizeof(segs));
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        int aop = fdt_path_offset(dt, "/soc/aop@290c00000");
        const char *st = aop >= 0 ? fdt_getprop(dt, aop, "status", NULL) : NULL;
        check(ret == 0 && bailed == 0 && st && !strcmp(st, "fail") && !fdt_check_full(dt, sz),
              "packed FDT + halted AOP (okay): disabled in place as fail, no bail");
    }

    /* 11. Packed FDT + halted AOP ("ok" / absent status): whole node + child + phandles NOP'd */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        int sz = build_tree(tree, TREE_SIZE, "ok", true);
        reset_env(segs, sizeof(segs));
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        check(ret == 0 && bailed == 0 && fdt_path_offset(dt, "/soc/aop@290c00000") < 0 &&
                  fdt_node_offset_by_phandle(dt, 42) < 0 &&
                  fdt_node_offset_by_phandle(dt, 43) < 0 && !fdt_check_full(dt, sz),
              "packed FDT + halted AOP (ok): node, child and phandles NOP'd, no bail");
    }

    /* 12. Packed FDT + running AOP: calls bail() */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", true);
        reset_env(segs, sizeof(segs));
        test_asc_running = true;
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        check(ret < 0 && bailed == 1, "packed FDT + running AOP: bails");
    }

    /* 13. Packed FDT + asc_init() fails: conservatively treated as running, calls bail() */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        build_tree(tree, TREE_SIZE, "okay", true);
        reset_env(segs, sizeof(segs));
        test_asc_ok = false;
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        check(ret < 0 && bailed == 1, "packed FDT + asc_init failure: conservatively bails");
    }

    /* 14. Tight FDT where fdt_add_subnode fits but properties run out of space: rolled back */
    {
        struct adt_segment_ranges segs[] = {
            {.phys = 0x1000ff18000ULL, .size = 0x2c000},
        };
        int sz = build_tree(tree, TREE_SIZE, "okay", true);
        int tight = sz + 64; /* enough for subnode header, not enough for all properties */
        assert(!fdt_open_into(tree, tree, tight));
        reset_env(segs, sizeof(segs));
        int ret = dt_aop_locked_dart_handoff(2, "/soc/aop@290c00000");
        int resv = fdt_path_offset(dt, "/reserved-memory");
        int half = fdt_subnode_offset(dt, resv, "asc-firmware@1000ff18000");
        int aop = fdt_path_offset(dt, "/soc/aop@290c00000");
        const char *st = aop >= 0 ? fdt_getprop(dt, aop, "status", NULL) : NULL;
        check(ret == 0 && bailed == 0 && half < 0 && st &&
                  (!strcmp(st, "disabled") || !strcmp(st, "fail")) && !fdt_check_full(dt, tight),
              "tight FDT mid-creation NOSPACE: partial subnode rolled back, halted AOP disabled");
    }

    printf(failures ? "aop os ram: %d FAILED\n" : "aop os ram: all cases pass\n", failures);
    return !!failures;
}
