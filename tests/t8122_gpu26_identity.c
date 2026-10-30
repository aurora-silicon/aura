/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kboot_gpu_t8122_power.h"
#include "libfdt/libfdt.h"

#define MAX_PSTATES        16
#define G15_MAX_CLUSTERS   2
#define G15_SGX_ID_VERSION 0xd04000
#define G15_SGX_CORE_MASK  0xe01500
#define G15_SGX_ID_COUNTS_1 0xd04010
#define G15_SGX_ID_COUNTS_2 0xd04014
#define T8122              0x8122
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/* SOURCE STRUCTS */

struct g15_power_values {
    u32 unused;
    u32 min_sram_uv;
};

static void *adt;
static bool g15_powered;
static bool t8122_fuse_valid = true, t8122_fuse_use;
static bool powered, fail_read, fail_release, fail_release_stuck;
static unsigned enables, disables, reads;
static u32 hw_id, hw_masks[3], hw_counts, hw_counts2;
static const u64 sgx_base = 0x10000000, asc_base = 0x20000000;
static u64 dt_sgx_override;
static struct perf_state perf[2] = {{1000, 700}, {2000, 800}};
static struct perf_state sram_perf[2] = {{1000, 700}, {2000, 800}};

static int g15_guard_begin(void) { return 0; }
static int g15_guard_end(int before) { return fail_read ? -1 : 0; }
static u32 read32(u64 addr)
{
    assert(powered);
    reads++;
    switch (addr - sgx_base) {
        case G15_SGX_ID_VERSION: return hw_id;
        case G15_SGX_ID_COUNTS_1: return hw_counts;
        case G15_SGX_ID_COUNTS_2: return hw_counts2;
        case G15_SGX_CORE_MASK: return hw_masks[0];
        case G15_SGX_CORE_MASK + 4: return hw_masks[1];
        case G15_SGX_CORE_MASK + 8: return hw_masks[2];
        default: assert(0); return 0;
    }
}
static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    trace[0] = strstr(path, "gfx-asc") ? 2 : 1;
    return trace[0];
}
static int adt_get_reg(void *tree, int *trace, const char *name, int idx, u64 *base, u64 *size)
{
    *base = trace[0] == 2 ? asc_base : sgx_base;
    if (size)
        *size = G15_SGX_CORE_MASK + 12;
    return 0;
}
static int adt_path_offset(void *tree, const char *path) { return 1; }
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    if (!strcmp(name, "perf-states-sram")) {
        *len = sizeof(sram_perf);
        return sram_perf;
    }
    return NULL;
}
static int pmgr_adt_power_is_off(const char *path) { return !powered; }
static int pmgr_adt_power_enable(const char *path)
{
    assert(!powered);
    enables++;
    powered = true;
    return 0;
}
static int pmgr_adt_power_disable_recursive(const char *path)
{
    disables++;
    if (fail_release)
        return -1;
    if (!fail_release_stuck)
        powered = false;
    return 0;
}
static int g15_get_pstates(const struct g15_soc *soc, int sgx, u32 *count, u32 *tables,
                           const struct perf_state **out)
{
    *count = 2;
    *tables = 1;
    *out = perf;
    return 0;
}
static int g15_get_power_props(int sgx, u32 count, u32 tables, const struct perf_state *core,
                               struct g15_power_values *pv)
{
    return 0;
}
static void calc_power_t8122_standin(u32 count, const struct perf_state *core, float leak,
                                     u32 enabled, u32 *max_pwr)
{
    for (u32 i = 0; i < count; i++)
        max_pwr[i] = 5000;
}
static int g15_set_opps(void *dt, u32 ph, u32 count, u32 tables, u32 clusters,
                        const struct perf_state *ps, const u32 *pwr)
{
    return 0;
}
static int g15_set_power_props(void *dt, const struct g15_power_values *pv) { return 0; }
static int g15_set_float_array(void *dt, const char *name, float val, int count) { return 0; }
static int stub_read_leak(const struct g15_soc *soc, float *leak)
{
    *leak = 100.0f;
    return 0;
}
static const struct g15_soc g15_soc_t8122_standin = {
    .chip = T8122,
    .name = "T8122",
    .gpu_name = "G15G",
    .compatible = "apple,agx-t8122",
    .optin = "asahi,t8122-gpu",
    .id_gen = 7,
    .id_variant = 2,
    .dies = 1,
    .mask2_bits = 32,
    .power_closure = true,
    .read_leak = stub_read_leak,
};

/* SOURCE FUNCTIONS */

static int calibrate_txn(void *dt, void *arg)
{
    return dt_gpu_t8122_26_calibrate(dt);
}

static void reset_hw(void)
{
    powered = fail_read = fail_release = fail_release_stuck = false;
    enables = disables = reads = 0;
    hw_id = 0x07022000;
    hw_masks[0] = 0x3ff;
    hw_masks[1] = hw_masks[2] = 0;
    hw_counts = 0x0011010a;
    hw_counts2 = 0x00040404;
    dt_sgx_override = 0;
    t8122_fuse_valid = true;
}

#define DT_SIZE 8192
static void make_dt(char *dt, const char *intent, bool with_opps, bool preexisting_identity)
{
    assert(!fdt_create_empty_tree(dt, DT_SIZE));
    static const char compat[] = "apple,j613\0apple,t8122";
    assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    assert(!fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu@292400000"));
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    if (intent)
        assert(!fdt_setprop_string(dt, chosen, "asahi,t8122-gpu", intent));
    if (preexisting_identity) {
        int node = fdt_add_subnode(dt, chosen, "asahi,t8122-gpu-powered-identity");
        assert(node >= 0);
        assert(!fdt_setprop_u32(dt, node, "schema-version", 1));
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(dt, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, soc, "#size-cells", 2));
    int opp = fdt_add_subnode(dt, soc, "opp-table");
    assert(opp >= 0);
    assert(!fdt_setprop_u32(dt, opp, "phandle", 0x42));
    assert(fdt_add_subnode(dt, opp, "opp-0") >= 0);
    assert(fdt_add_subnode(dt, opp, "opp-1") >= 0);
    int gpu = fdt_add_subnode(dt, soc, "gpu@292400000");
    assert(gpu >= 0);
    assert(!fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t8122"));
    assert(!fdt_setprop_string(dt, gpu, "status", "disabled"));
    static const char regnames[] = "sgx\0asc";
    assert(!fdt_setprop(dt, gpu, "reg-names", regnames, sizeof(regnames)));
    fdt64_t reg[4];
    fdt64_st(&reg[0], dt_sgx_override ? dt_sgx_override : sgx_base);
    fdt64_st(&reg[1], G15_SGX_CORE_MASK + 12);
    fdt64_st(&reg[2], asc_base);
    fdt64_st(&reg[3], 0x1000);
    assert(!fdt_setprop(dt, gpu, "reg", reg, sizeof(reg)));
    if (with_opps)
        assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 0x42));
}

static u32 identity_prop(void *dt, const char *name)
{
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    assert(node >= 0);
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, name, &len);
    assert(v && len == 4);
    return fdt32_ld(v);
}

static void expect_rollback(char *dt, char *before, int size)
{
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity") < 0 ||
           identity_prop(dt, "probe-result") != T8122_ID_PASS);
    assert(!memcmp(dt, before, size));
}

int main(void)
{
    static char dt[DT_SIZE], before[DT_SIZE];
    int size;

    /* Genuine read published: 10-core and 8-core masks, power released. */
    const u32 good_masks[] = {0x3ff, 0xff};
    for (unsigned i = 0; i < sizeof(good_masks) / sizeof(good_masks[0]); i++) {
        reset_hw();
        hw_masks[0] = good_masks[i];
        make_dt(dt, "1", true, false);
        assert(!t8122_fdt_transaction(dt, calibrate_txn, NULL));
        assert(reads >= 5 && enables == 1 && disables == 1 && !powered);
        assert(identity_prop(dt, "schema-version") == 1);
        assert(identity_prop(dt, "probe-result") == T8122_ID_PASS);
        assert(identity_prop(dt, "id-version") == hw_id);
        assert(identity_prop(dt, "core-mask-0") == good_masks[i]);
        assert(identity_prop(dt, "core-mask-1") == 0);
        assert(identity_prop(dt, "core-mask-2") == 0);
        assert(identity_prop(dt, "id-counts-1") == hw_counts);
    }

    /* Failed read: rollback, power released, no PASS. */
    reset_hw();
    fail_read = true;
    make_dt(dt, "1", true, false);
    size = fdt_totalsize(dt);
    memcpy(before, dt, size);
    assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
    expect_rollback(dt, before, size);
    assert(!powered && disables == 1);

    /* Bad identity fields: rollback. */
    const struct { u32 id, mask0, mask1, counts; } bad[] = {
        {0x07022100, 0x3ff, 0, 0x0011010a}, /* wrong revision byte */
        {0x07022000, 0x7ff, 0, 0x0011010a}, /* bit beyond 0..9 */
        {0x07022000, 0x1ff, 0, 0x0011010a}, /* 9 cores */
        {0x07022000, 0x3ff, 1, 0x0011010a}, /* core-mask-1 nonzero */
        {0x07022000, 0x3ff, 0, 0x00110108}, /* counts topology mismatch */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_hw();
        hw_id = bad[i].id;
        hw_masks[0] = bad[i].mask0;
        hw_masks[1] = bad[i].mask1;
        hw_counts = bad[i].counts;
        make_dt(dt, "1", true, false);
        size = fdt_totalsize(dt);
        memcpy(before, dt, size);
        assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
        expect_rollback(dt, before, size);
        assert(!powered);
    }

    /* Power-release failure (error or still on): no PASS, rollback. */
    for (int mode = 0; mode < 2; mode++) {
        reset_hw();
        if (mode == 0)
            fail_release = true;
        else
            fail_release_stuck = true;
        make_dt(dt, "1", true, false);
        size = fdt_totalsize(dt);
        memcpy(before, dt, size);
        assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
        expect_rollback(dt, before, size);
        powered = false; /* fixture cleanup only */
    }

    /* Missing fields never power the GPU. */
    reset_hw();
    make_dt(dt, "1", false, false); /* no operating-points-v2 */
    size = fdt_totalsize(dt);
    memcpy(before, dt, size);
    assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
    expect_rollback(dt, before, size);
    assert(enables == 0 && reads == 0);

    reset_hw();
    make_dt(dt, NULL, true, false); /* no chosen intent: prepare refuses */
    size = fdt_totalsize(dt);
    memcpy(before, dt, size);
    assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
    expect_rollback(dt, before, size);
    assert(enables == 0 && reads == 0);

    /* A pre-existing witness is never replaced. */
    reset_hw();
    make_dt(dt, "1", true, true);
    size = fdt_totalsize(dt);
    memcpy(before, dt, size);
    assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
    assert(enables == 0 && reads == 0);
    assert(!memcmp(dt, before, size));

    /* SGX window mismatch: no power-on. */
    reset_hw();
    dt_sgx_override = sgx_base + 0x100000;
    make_dt(dt, "1", true, false);
    size = fdt_totalsize(dt);
    memcpy(before, dt, size);
    assert(t8122_fdt_transaction(dt, calibrate_txn, NULL));
    expect_rollback(dt, before, size);
    assert(enables == 0 && !powered);

    puts("gpu26 powered identity: genuine publish, rollback, bad fields, failed read and "
         "power-release controls PASS");
    return 0;
}
