/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kboot_gpu_t8122_power.h"

#include "libfdt/libfdt.h"

#define MAX_PSTATES          16
#define G15_MAX_CLUSTERS     2
#define G15_NUM_REGIONS      4
#define G15_FW_SEGS          2
#define G15_FW_NODE_NAME_LEN 40
#define G15_SGX_ID_VERSION   0xd04000
#define G15_SGX_CORE_MASK    0xe01500
#define G15_SGX_ID_COUNTS_1  0xd04010
#define G15_SGX_ID_COUNTS_2  0xd04014
#define G15_ARRAY_CLUSTERS   4
#define G15_AUX_MAX          16
#define T8122                0x8122
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
struct fw_version_info {
    int version;
    const char *string;
    u32 num[3];
    int num_length;
};
enum { V14_8_3 = 1 };
static const struct fw_version_info firmware = {V14_8_3, "14.8.3", {14, 8, 3}, 3};
static void *adt;
static bool g15_powered;
static u32 g15_mbox_enabled;
static int g15_fw_undo;
static bool powered, fail_power, fail_read, fail_calibration, fail_gpu_enable, fail_firmware;
static unsigned enables, disables, reads, counts2_reads;
static u32 id, masks[3], counts, counts2;
static const u64 sgx_base = 0x10000000, asc_base = 0x20000000;

static int g15_guard_begin(void)
{
    return 0;
}
static int g15_guard_end(int before)
{
    return fail_read ? -1 : 0;
}
static u32 read32(u64 addr)
{
    assert(powered);
    reads++;
    switch (addr - sgx_base) {
        case G15_SGX_ID_VERSION:
            return id;
        case G15_SGX_ID_COUNTS_1:
            return counts;
        case G15_SGX_ID_COUNTS_2:
            counts2_reads++;
            return counts2;
        case G15_SGX_CORE_MASK:
            return masks[0];
        case G15_SGX_CORE_MASK + 4:
            return masks[1];
        case G15_SGX_CORE_MASK + 8:
            return masks[2];
        default:
            assert(0);
            return 0;
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
static int adt_path_offset(void *tree, const char *path)
{
    return 1;
}
static int g15_adt_u32(int node, const char *name, u32 *value)
{
    return -1;
}
static void *adt_getprop(void *tree, int node, const char *name, int *len)
{
    return NULL;
}
static int pmgr_adt_power_is_off(const char *path)
{
    return !powered;
}
static int pmgr_adt_power_enable(const char *path)
{
    assert(!powered);
    enables++;
    powered = true;
    return fail_power ? -1 : 0;
}
static int pmgr_adt_power_disable_recursive(const char *path)
{
    disables++;
    powered = false;
    return 0;
}
static int pmgr_adt_power_disable(const char *path)
{
    return pmgr_adt_power_disable_recursive(path);
}
static const struct fw_version_info *firmware_gfx_version_ex(u32 chip, bool experimental)
{
    return fail_firmware ? NULL : &firmware;
}
static int g15_get_pstates(const struct g15_soc *soc, int sgx, u32 *count, u32 *tables,
                           const struct perf_state **states)
{
    static const struct perf_state points[] = {{0, 0}, {500000000, 700}};
    *count = 2;
    *tables = 1;
    *states = points;
    return 0;
}
static int g15_get_power_props(int sgx, u32 count, u32 tables, const struct perf_state *states,
                               struct g15_power_values *values)
{
    return 0;
}
static int g15_get_region(void *dt, int sgx, struct g15_region *region)
{
    region->base = 0x30000000;
    region->size = 0x4000;
    return 0;
}
static int g15_check_regions(void *dt, const struct g15_region *regions)
{
    return 0;
}
static int g15_get_mbox(void *dt, int gpu, u32 *phandle)
{
    *phandle = 2;
    return 0;
}
static int g15_get_fw_segs(void *dt, int gpu, const struct g15_region *regions,
                           struct g15_fw_seg *segments)
{
    return -1;
}
static int g15_set_opps(void *dt, u32 phandle, u32 count, u32 tables, u32 clusters,
                        const struct perf_state *states, const u32 *power)
{
    return fail_calibration ? -1 : 0;
}
static int g15_set_float_array(void *dt, const char *name, float value, u32 clusters)
{
    return 0;
}
static int g15_set_power_props(void *dt, const struct g15_power_values *values)
{
    return 0;
}
static int firmware_set_fdt(void *dt, int node, const char *name, const struct fw_version_info *fw)
{
    fdt32_t compat[] = {cpu_to_fdt32(14), cpu_to_fdt32(8), cpu_to_fdt32(3)};
    return fdt_setprop(dt, node, name, compat, sizeof(compat));
}
static int g15_set_fw_segs(void *dt, const char *path, const struct g15_fw_seg *segments)
{
    return 0;
}
static int g15_unset_fw_segs(void *dt, const char *path)
{
    return 0;
}
static int g15_set_status(void *dt, const char *path, const char *status)
{
    if (!strcmp(path, "gpu") && !strcmp(status, "okay")) {
        int witness = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
        assert(witness >= 0);
        int len;
        const fdt32_t *result = fdt_getprop(dt, witness, "probe-result", &len);
        assert(result && len == 4 && fdt32_ld(result) == T8122_ID_PASS);
        if (fail_gpu_enable)
            return -1;
    }
    return fdt_setprop_string(dt, fdt_path_offset(dt, path), "status", status);
}
static int g15_set_mbox_status(void *dt, u32 phandle, const char *status)
{
    return fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, phandle), "status", status);
}
static int g15_get_fw_segs_nub(void *dt, int gpu, const struct g15_region *regions,
                               struct g15_fw_seg *segments, const char *nub)
{
    return g15_get_fw_segs(dt, gpu, regions, segments);
}

/* The T6031 hooks of the setup: a T8122 setup never takes them */
static const struct fw_version_info *g15_g15c_fw;
static int g15_get_aux(void *dt, int gpu, int sgx, u32 dies, struct g15_aux *aux)
{
    assert(0);
    return -1;
}
static int g15_csafr_sram(const struct g15_aux aux[2], u32 floor, u32 *out)
{
    assert(0);
    return -1;
}
static int g15_probed_hw(void *dt, int gpu, const struct g15_soc *soc, u32 *counts, u32 *clusters,
                         u32 *cores, float *leak)
{
    assert(0);
    return -1;
}
static int g15_set_csafr(void *dt, const struct g15_aux aux[2], u32 floor, u32 dies)
{
    assert(0);
    return -1;
}
static int g15_set_standins(void *dt, const struct g15_soc *soc)
{
    assert(0);
    return -1;
}

/* SOURCE FUNCTIONS */

static int leak(const struct g15_soc *soc, float *value)
{
    assert(powered);
    *value = 2;
    return 0;
}
static void power(u32 count, const struct perf_state *states, float leak, u32 cores, u32 *out)
{
    assert(cores == 8 || cores == 10);
    out[0] = 0;
    out[1] = 2000000;
}
static const struct g15_soc soc = {
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
    .fw_experimental = true,
    .power_props = true,
    .read_leak = leak,
    .calc_power = power,
};

static void setup(void *dt, bool armed)
{
    powered = fail_power = fail_read = fail_calibration = fail_gpu_enable = fail_firmware = false;
    enables = disables = reads = counts2_reads = 0;
    id = 0x07022000;
    counts = 0x0011010a;
    counts2 = 0x00040404;
    masks[0] = 0x3ff;
    masks[1] = masks[2] = 0;
    assert(!fdt_create_empty_tree(dt, 8192));
    static const char root[] = "apple,j613\0apple,t8122";
    assert(!fdt_setprop(dt, 0, "compatible", root, sizeof(root)));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int node = fdt_add_subnode(dt, 0, "chosen");
    if (armed)
        assert(!fdt_setprop_string(dt, node, "asahi,t8122-gpu", "1"));
    node = fdt_add_subnode(dt, 0, "gpu");
    assert(!fdt_setprop_string(dt, node, "compatible", "apple,agx-t8122"));
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
    assert(!fdt_setprop_u32(dt, node, "operating-points-v2", 1));
    static const char names[] = "sgx\0asc";
    assert(!fdt_setprop(dt, node, "reg-names", names, sizeof(names)));
    fdt64_t regs[] = {cpu_to_fdt64(sgx_base), cpu_to_fdt64(G15_SGX_CORE_MASK + 12),
                      cpu_to_fdt64(asc_base), cpu_to_fdt64(0x10000)};
    assert(!fdt_setprop(dt, node, "reg", regs, sizeof(regs)));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(!fdt_setprop_string(dt, aliases, "gpu", "/gpu"));
    node = fdt_add_subnode(dt, 0, "opps");
    assert(!fdt_setprop_u32(dt, node, "phandle", 1));
    node = fdt_add_subnode(dt, 0, "mailbox");
    assert(!fdt_setprop_u32(dt, node, "phandle", 2));
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    for (unsigned i = 0; i < G15_NUM_REGIONS; i++) {
        int parent = fdt_path_offset(dt, "/reserved-memory");
        const char *name = strrchr(g15_region_names[i].dt_path, '/') + 1;
        node = fdt_add_subnode(dt, parent, name);
        assert(node >= 0);
        assert(!fdt_setprop_string(dt, node, "status", "disabled"));
    }
}

static void check(void *dt, bool success, unsigned expected_enables, unsigned expected_reads)
{
    assert(!dt_set_gpu_g15(dt, &soc));
    assert(enables == expected_enables && reads == expected_reads);
    assert(powered == success);
    assert(disables == (expected_enables && !success ? 1 : 0));
    int gpu = fdt_path_offset(dt, "gpu");
    assert(!strcmp(fdt_getprop(dt, gpu, "status", NULL), success ? "okay" : "disabled"));
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    if (node >= 0) {
        int len;
        const fdt32_t *result = fdt_getprop(dt, node, "probe-result", &len);
        assert(result && len == 4 && fdt32_ld(result) == (success ? 1u : 0u));
        if (success) {
            const fdt32_t *mask = fdt_getprop(dt, node, "core-mask-0", &len);
            assert(mask && len == 4 && fdt32_ld(mask) == masks[0]);
        }
    } else
        assert(!success);
}
int main(void)
{
    char dt[8192];
    setup(dt, true);
    check(dt, true, 1, 6);
    setup(dt, true);
    masks[0] = 0x37e;
    check(dt, true, 1, 6);
    setup(dt, false);
    check(dt, false, 0, 0);
    setup(dt, true);
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j713"));
    check(dt, false, 0, 0);
    setup(dt, true);
    fail_firmware = true;
    check(dt, false, 0, 0);
    setup(dt, true);
    fail_power = true;
    check(dt, false, 1, 0);
    setup(dt, true);
    fail_read = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    id = 0x07032000;
    check(dt, false, 1, 6);
    setup(dt, true);
    id = 0x07022100;
    check(dt, false, 1, 6);
    setup(dt, true);
    masks[0] = 0x7ff;
    check(dt, false, 1, 6);
    setup(dt, true);
    counts = 0x0011020a;
    check(dt, false, 1, 6);
    setup(dt, true);
    fail_calibration = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    fail_gpu_enable = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    int gpu = fdt_path_offset(dt, "gpu");
    fdt64_t bad_regs[] = {cpu_to_fdt64(sgx_base + 4), cpu_to_fdt64(G15_SGX_CORE_MASK + 12),
                          cpu_to_fdt64(asc_base), cpu_to_fdt64(0x10000)};
    assert(!fdt_setprop(dt, gpu, "reg", bad_regs, sizeof(bad_regs)));
    check(dt, false, 0, 0);

    setup(dt, true);
    powered = true; /* A pre-existing vote must not be touched or observed. */
    assert(!dt_set_gpu_g15(dt, &soc));
    assert(powered && !enables && !disables && !reads);
    int witness = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    assert(witness >= 0);
    const fdt32_t *result = fdt_getprop(dt, witness, "probe-result", NULL);
    assert(result && fdt32_ld(result) == T8122_ID_NOT_RUN);

    /* GPU26's calibration passes NULL and keeps the prior read set, without a witness. */
    setup(dt, true);
    u32 cores[G15_MAX_CLUSTERS], clusters, enabled;
    float fused;
    gpu = fdt_path_offset(dt, "gpu");
    assert(!g15_read_hw(dt, gpu, &soc, cores, &clusters, &enabled, &fused, NULL));
    assert(reads == 5 && !counts2_reads && enabled == 10 && clusters == 1);
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity") < 0);

    /* J615 follows the same full-start path with its own MMIO identity. */
    static const char j615[] = "apple,j615\0apple,t8122";
    const u32 j615_masks[] = {0x3ff, 0xff, 0x37e};
    for (unsigned i = 0; i < sizeof(j615_masks) / sizeof(j615_masks[0]); i++) {
        setup(dt, true);
        assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
        masks[0] = j615_masks[i];
        check(dt, true, 1, 6);
    }
    setup(dt, false);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    check(dt, false, 0, 0);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    fail_power = true;
    check(dt, false, 1, 0);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    fail_read = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    counts = 0x0011020a;
    check(dt, false, 1, 6);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    fail_calibration = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    fail_gpu_enable = true;
    check(dt, false, 1, 6);
    setup(dt, true);
    assert(!fdt_setprop(dt, 0, "compatible", j615, sizeof(j615)));
    fail_firmware = true;
    check(dt, false, 0, 0);

    /* T6030 retains its fixed topology and its existing ID revision behavior. */
    setup(dt, true);
    struct g15_soc pro = soc;
    pro.chip = 0x6030;
    pro.id_variant = 3;
    pro.cores_per_cluster = 10;
    pro.clusters = 2;
    id = 0x07033000;
    masks[0] = 0xfffff;
    gpu = fdt_path_offset(dt, "gpu");
    assert(!g15_read_hw(dt, gpu, &pro, cores, &clusters, &enabled, &fused, NULL));
    assert(reads == 4 && !counts2_reads && enabled == 20 && clusters == 2);
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity") < 0);
    puts("actual full14 start callers: 15 inherited and 10 J615 start/refusal controls, existing-vote refusal, "
         "GPU26/T6030 read-path preservation PASS");
}
