/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "kboot.h"
#include "adt.h"
#include "assert.h"
#include "exception.h"
#include "firmware.h"
#include "gfx_fw_image_info.h"
#include "kboot_gpu_t6031.h"
#include "kboot_gpu_t8122_power.h"
#include "kboot_t6031.h"
#include "malloc.h"
#include "math.h"
#include "mcc.h"
#include "pmgr.h"
#include "soc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

#define MAX_PSTATES  16
#define MAX_CLUSTERS 8
#define MAX_DIES     2

struct perf_state {
    u32 freq;
    u32 volt;
};

struct aux_perf_state {
    u64 volt;
    u64 freq;
};

struct aux_perf_states {
    u64 dies;
    u64 count;
    struct aux_perf_state states[];
};

int32_t rust_gpu_initdata_size(uint32_t compat_maj, uint32_t compat_min, size_t *data_a,
                               size_t *data_b, size_t *globals);

struct initdata_inputs {
    size_t perf_state_table_count;
    size_t perf_state_count;
    const struct perf_state *c_perf_states;
    uint32_t *max_pwr;
    float *core_leak;
    float *sram_leak;
    float *cs_leak;
    float *afr_leak;
    size_t n_perf_states_cs;
    const struct aux_perf_state *pstates_cs;
    size_t n_perf_states_afr;
    const struct aux_perf_state *pstates_afr;
    uint32_t compat_maj;
    uint32_t compat_min;
};

int32_t rust_fill_gpu_initdata(struct initdata_inputs *ins, void *data_a, void *data_b,
                               void *globals);

static int get_core_counts(u32 *count, u32 nclusters, u32 ncores)
{
    u64 base;
    pmgr_adt_power_enable("/arm-io/sgx");

    int adt_sgx_path[8];
    if (adt_path_offset_trace(adt, "/arm-io/sgx", adt_sgx_path) < 0)
        bail("ADT: GPU: Failed to get sgx\n");

    if (adt_get_reg(adt, adt_sgx_path, "reg", 0, &base, NULL) < 0)
        bail("ADT: GPU: Failed to get sgx reg 0\n");

    u32 cores[3] = {0, 0, 0};

    switch (chip_id) {
        case T6002:
            cores[1] = read32(base + 0xd01514);
            /* fallthrough */
        case T8103:
        case T8112:
        case T6000:
        case T6001:
            cores[0] = read32(base + 0xd01500);
            break;
        case T6020:
        case T6021:
        case T6022:
            cores[0] = read32(base + 0xe01500);
            cores[1] = read32(base + 0xe01504);
            cores[2] = read32(base + 0xe01508);
            break;
    }

    for (u32 i = 0; i < nclusters; i++) {
        count[i] = __builtin_popcount(cores[0] & MASK(ncores));

        for (u32 j = 0; j < ARRAY_SIZE(cores); j++) {
            cores[j] >>= ncores;
            if (j < (ARRAY_SIZE(cores) - 1))
                cores[j] |= cores[j + 1] << (32 - ncores);
        }
    }

    return 0;
}

static void adjust_leakage(float *val, u32 clusters, u32 *cores, u32 max, float uncore_fraction)
{
    for (u32 i = 0; i < clusters; i++) {
        float uncore = val[i] * uncore_fraction;
        float core = val[i] - uncore;

        val[i] = uncore + (cores[i] / (float)max) * core;
    }
}

static void load_fuses(float *out, u32 count, u64 base, u32 start, u32 width, float scale,
                       float offset, bool flip)
{
    for (u32 i = 0; i < count; i++) {
        base += (start / 32) * 4;
        start &= 31;

        u32 low = read32(base);
        u32 high = read32(base + 4);
        u32 val = (((((u64)high) << 32) | low) >> start) & MASK(width);

        float fval = (float)val * scale + offset;

        if (flip)
            out[count - i - 1] = fval;
        else
            out[i] = fval;

        start += width;
    }
}

static u32 t8103_pwr_scale[] = {0, 63, 80, 108, 150, 198, 210};

static int calc_power_t8103(u32 count, u32 table_count, const struct perf_state *core,
                            const struct perf_state *sram, const struct aux_perf_states *cs,
                            u32 *max_pwr, float *core_leak, float *sram_leak, float *cs_leak,
                            float *afr_leak)
{
    UNUSED(sram);
    UNUSED(cs);
    UNUSED(core_leak);
    UNUSED(sram_leak);
    UNUSED(cs_leak);
    UNUSED(afr_leak);
    u32 *pwr_scale;
    u32 pwr_scale_count;
    u32 core_count;
    u32 max_cores;

    switch (chip_id) {
        case T8103:
            pwr_scale = t8103_pwr_scale;
            pwr_scale_count = ARRAY_SIZE(t8103_pwr_scale);
            max_cores = 8;
            break;
        default:
            bail("ADT: GPU: Unsupported chip\n");
    }

    if (get_core_counts(&core_count, 1, max_cores))
        return -1;

    if (table_count != 1)
        bail("ADT: GPU: expected 1 perf state table but got %d\n", table_count);

    if (count != pwr_scale_count)
        bail("ADT: GPU: expected %d perf states but got %d\n", pwr_scale_count, count);

    for (u32 i = 0; i < pwr_scale_count; i++)
        max_pwr[i] = (u32)core[i].volt * (u32)pwr_scale[i] * 100;

    core_leak[0] = 1000.0;
    sram_leak[0] = 45.0;

    adjust_leakage(core_leak, 1, &core_count, max_cores, 0.12);
    adjust_leakage(sram_leak, 1, &core_count, max_cores, 0.2);

    return 0;
}

static int calc_power_t600x(u32 count, u32 table_count, const struct perf_state *core,
                            const struct perf_state *sram, const struct aux_perf_states *cs,
                            u32 *max_pwr, float *core_leak, float *sram_leak, float *cs_leak,
                            float *afr_leak)
{
    float s_sram, k_sram, s_core, k_core, s_cs, k_cs;
    float dk_core, dk_sram = 0, dk_cs = 0;
    float imax = 1000;

    u32 ndies = 1;
    u32 nclusters = 0;
    u32 ncores = 0;
    u32 core_count[MAX_CLUSTERS];

    bool simple_exps = false;
    bool adjust_leakages = true;
    bool has_cs = false;

    switch (chip_id) {
        case T6002:
            ndies = 2;
            nclusters += 4;
            load_fuses(core_leak + 4, 4, 0x22922bc1b8, 25, 13, 2, 2, true);
            load_fuses(sram_leak + 4, 4, 0x22922bc1cc, 4, 9, 1, 1, true);
            // fallthrough
        case T6001:
            nclusters += 2;
        case T6000:
            nclusters += 2;
            load_fuses(core_leak + 0, min(4, nclusters), 0x2922bc1b8, 25, 13, 2, 2, false);
            load_fuses(sram_leak + 0, min(4, nclusters), 0x2922bc1cc, 4, 9, 1, 1, false);

            s_sram = 4.3547606;
            k_sram = 0.024927923;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.48461742;
            k_core = 0.39013552;
            dk_core = 1.06975;
            dk_sram = 0.00625;

            ncores = 8;
            adjust_leakages = true;
            imax = 26.0;
            break;
        case T8112:
            nclusters = 1;
            load_fuses(core_leak, 1, 0x23d2c84dc, 30, 13, 2, 2, false);
            load_fuses(sram_leak, 1, 0x23d2c84b0, 15, 9, 1, 1, false);

            s_sram = 3.61619841;
            k_sram = 0.0529281;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.21356187;
            k_core = 0.43328839;
            dk_core = 0.983196;
            dk_sram = 0.007828;

            simple_exps = true;
            ncores = 10;
            adjust_leakages = false; // pre-adjusted?
            imax = 24.0;
            break;
        case T6022:
            ndies = 2;
            nclusters += 4;
            load_fuses(core_leak + 4, min(4, nclusters), 0x229e2cc1f8, 4, 13, 2, 2, true);
            load_fuses(sram_leak + 4, min(4, nclusters), 0x229e2cc208, 19, 9, 1, 1, true);
            load_fuses(cs_leak + 1, 1, 0x229e2cc204, 8, 12, 1, 1, false);
            load_fuses(afr_leak + 1, 1, 0x229e2cc210, 0, 12, 1, 1, false);

            // For some reason, this one is different on T6022...
            dk_cs = 6.7;
            // fallthrough
        case T6021:
            if (!dk_cs)
                dk_cs = 4.492;

            nclusters += 4;
            s_sram = 5.808;
            k_sram = 0.00707;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.24554153;
            k_core = 0.56203084;

            s_cs = 1.87;
            k_cs = 0.162;

            goto t602x;

        case T6020:
            nclusters = 2;
            s_sram = 5.02191218;
            k_sram = 0.0145621013;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.21006932;
            k_core = 0.52776378;

            s_cs = 1.8;
            k_cs = 0.162;
            dk_cs = 1.889;

        t602x:
            dk_core = 1.00075;
            dk_sram = 0.00785;
            load_fuses(core_leak + 0, min(4, nclusters), 0x29e2cc1f8, 4, 13, 2, 2, false);
            load_fuses(sram_leak + 0, min(4, nclusters), 0x29e2cc208, 19, 9, 1, 1, false);
            load_fuses(cs_leak + 0, 1, 0x29e2cc204, 8, 12, 1, 1, false);
            load_fuses(afr_leak + 0, 1, 0x29e2cc210, 0, 12, 1, 1, false);

            simple_exps = true;
            ncores = 10;
            adjust_leakages = false; // pre-adjusted?
            imax = 33.0;
            has_cs = true;
            break;

        default:
            bail("ADT: GPU: Unsupported chip\n");
    }

    if (get_core_counts(core_count, nclusters, ncores))
        return -1;

    printf("FDT: GPU: Core counts: ");
    for (u32 i = 0; i < nclusters; i++) {
        printf("%d ", core_count[i]);
    }
    printf("\n");

    if (adjust_leakages) {
        adjust_leakage(core_leak, nclusters, core_count, ncores, 0.0825);
        adjust_leakage(sram_leak, nclusters, core_count, ncores, 0.2247);
    }

    if (table_count != nclusters)
        bail("ADT: GPU: expected %d perf state tables but got %d\n", nclusters, table_count);

    if (has_cs && (!cs || !cs_leak)) {
        bail("ADT: GPU: expected CS perf table, but not found\n");
    }

    max_pwr[0] = 0;

    for (u32 i = 1; i < count; i++) {
        u32 total_mw = 0;

        for (u32 j = 0; j < nclusters; j++) {
            // macOS difference: macOS truncates Hz to integer MHz before doing this math.
            // That's probably wrong, so let's not do that.

            float mw = 0;
            size_t idx = j * count + i;

            mw += sram[idx].volt / 1000.f * sram_leak[j] * k_sram *
                  expf(sram[idx].volt / 1000.f * s_sram);
            mw += core[idx].volt / 1000.f * core_leak[j] * k_core *
                  expf(core[idx].volt / 1000.f * s_core);

            float sbase = sram[idx].volt / 750.f;
            float sram_v_p;
            if (simple_exps)
                sram_v_p = sbase * sbase; // v ^ 2
            else
                sram_v_p = sbase * sbase * sbase; // v ^ 3
            mw += dk_sram * core_count[j] * (sram[idx].freq / 1000000.f) * sram_v_p;

            float cbase = core[idx].volt / 750.f;
            float core_v_p;
            if (simple_exps || core[idx].volt < 750)
                core_v_p = cbase * cbase; // v ^ 2
            else
                core_v_p = cbase * cbase * cbase; // v ^ 3
            mw += dk_core * core_count[j] * (core[idx].freq / 1000000.f) * core_v_p;

            if (mw > imax * core[idx].volt)
                mw = imax * core[idx].volt;

            total_mw += mw;
        }

        // CS gets added after the imax limit

        if (has_cs) {
            for (u32 j = 0; j < ndies; j++) {
                float mw = 0;

                int csi = j * cs->count + min(i, cs->count - 1);
                u32 cs_mv = cs->states[csi].volt / 1000;
                u32 cs_hz = cs->states[csi].freq;

                mw += cs_mv / 1000.f * cs_leak[j] * k_cs * expf(cs_mv / 1000.f * s_cs);
                float csbase = cs_mv / 750.f;
                float cs_v_p = powf(csbase, 1.8);
                mw += dk_cs * (cs_hz / 1000000.f) * cs_v_p;

                total_mw += mw;
            }
        }

        max_pwr[i] = total_mw * 1000;
    }

    return 0;
}

static int dt_set_resvmem(void *dt, const char *path, u64 base, u64 size)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        bail("FDT: GPU: failed to find %s node\n", path);

    fdt64_t reg[2];

    fdt64_st(&reg[0], base);
    fdt64_st(&reg[1], size);

    if (fdt_setprop_string(dt, node, "status", "okay"))
        bail("FDT: GPU: failed to un-disable memory region");

    if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
        bail("FDT: GPU: failed to set reg prop for %s\n", path);

    return 0;
}

static int dt_set_region(void *dt, int sgx, const char *name, const char *path)
{
    u64 base, size;
    char prop[64];

    snprintf(prop, sizeof(prop), "%s-base", name);
    if (ADT_GETPROP(adt, sgx, prop, &base) < 0 || !base)
        bail("ADT: GPU: failed to find %s property\n", prop);

    snprintf(prop, sizeof(prop), "%s-size", name);
    if (ADT_GETPROP(adt, sgx, prop, &size) < 0 || !size)
        bail("ADT: GPU: failed to find %s property\n", prop);

    return dt_set_resvmem(dt, path, base, size);
}

int fdt_set_float_array(void *dt, int node, const char *name, float *val, int count)
{
    fdt32_t data[MAX_CLUSTERS];

    if (count > MAX_CLUSTERS)
        bail("FDT: GPU: fdt_set_float_array() with too many values\n");

    memcpy(data, val, sizeof(float) * count);
    for (int i = 0; i < count; i++) {
        data[i] = cpu_to_fdt32(data[i]);
    }

    if (fdt_setprop_inplace(dt, node, name, data, sizeof(u32) * count))
        bail("FDT: GPU: Failed to set %s\n", name);

    return 0;
}

static int fdt_set_aux_opp(void *dt, int gpu, const char *prop, const struct aux_perf_states *ps,
                           u32 dies)
{
    int len;
    const fdt32_t *opps_ph = fdt_getprop(dt, gpu, prop, &len);
    if (!opps_ph || len != 4)
        bail("FDT: GPU: %s not found\n", prop);

    int opps = fdt_node_offset_by_phandle(dt, fdt32_ld(opps_ph));
    if (opps < 0)
        bail("FDT: GPU: node for phandle %u not found\n", fdt32_ld(opps_ph));

    u32 count = ps->count;

    u32 i = 0;
    int opp;
    fdt_for_each_subnode(opp, dt, opps)
    {
        fdt32_t volts[MAX_DIES];

        for (u32 j = 0; j < dies; j++) {
            volts[j] = cpu_to_fdt32(ps->states[i + j * ps->count].volt);
        }

        if (i >= count)
            bail("FDT: GPU: Expected %d operating points, but found more\n", count);

        if (fdt_setprop_inplace(dt, opp, "opp-microvolt", &volts, sizeof(u32) * dies))
            bail("FDT: GPU: Failed to set opp-microvolt for aux PS %d\n", i);

        if (fdt_setprop_inplace_u64(dt, opp, "opp-hz", ps->states[i].freq))
            bail("FDT: GPU: Failed to set opp-hz for PS %d\n", i);

        i++;
    }

    return 0;
}

/*
 * T6030 (M3 Pro, AGX G15S) support.
 *
 * This targets the 14.8.3 GPU firmware. Values marked TODO are not confirmed on
 * hardware yet.
 *
 * Boot safety comes first: this boot.bin boots every kernel on the machine, so nothing in here may
 * fail kboot. dt_set_gpu_g15() therefore
 * - marks the gpu node disabled before it does anything else,
 * - positively identifies the loaded GPU firmware as the 14.8.3 ABI before it touches any GPU
 *   register (drm/asahi only accepts apple,firmware-compat = <14 8 3> for G15),
 * - length/range checks every ADT input (no ADT_GETPROP: it copies before checking the length),
 * - touches no GPU hardware and leaves the node disabled unless boot.bin carries the opt-in config
 *   line chosen.asahi,t6030-gpu=1 (aura cannot tell which kernel will boot, so this is per
 *   boot.bin); without it, it only runs and logs the firmware/ADT/DT checks (dry run),
 * - runs the ID/core-mask register and leakage fuse reads under an exception guard that also
 *   catches asynchronous SErrors (the PMGR power-on is unguarded, as everywhere else in aura; a
 *   read that stalls the bus without faulting cannot be caught by any guard),
 * - checks the uat-* regions against everything dt_set_memory() later bails on,
 * - adds the description of the loaded firmware segments (fw-text/fw-data, see g15_get_fw_segs())
 *   completely or not at all; the GPU setup does not depend on it,
 * - enables the uat-* regions and then the gpu node only after every step has succeeded,
 * - on any failure removes the firmware segment description, disables the node again, zeroes
 *   apple,firmware-compat, powers the GPU back off if it was powered on here, and returns 0.
 *
 * apple,firmware-version is the version of the loaded GPU firmware image (14.8.3), not os_firmware
 * as on the other SoCs: iBoot reports 14.8.3 as 14.7. drm/asahi only puts it in crash dumps.
 *
 * Differences from the t602x path:
 * - No CS/AFR clock domains: no apple,cs-opp / apple,afr-opp.
 * - One 15-bit leakage fuse instead of per-cluster fuse fields, and a banded max-power formula
 *   instead of calc_power_t600x().
 * - The OPP table is rebuilt from the runtime ADT, because the pstate count is not known statically
 *   (iBoot fills in the perf-state properties at boot).
 * - hw-cal-a/hw-cal-b/globals stay disabled: aura's Rust initdata has no G15 layouts, and drm/asahi
 *   only uses these blobs as an optional debug cross-check (gpu.rs load_hwdata_blob(), which falls
 *   back to an empty blob).
 */
#define G15S_ID_GEN            7        // ID_VERSION[31:24]
#define G15S_ID_VARIANT        3        // ID_VERSION[23:16], 3 = G15S
#define G15S_CORES_PER_CLUSTER 10       // G15S core mask layout: 10 slots per MGPU
#define G15S_MAX_CORES         20       // max total cores for G15S, max-power divisor
#define T6030_MAX_CLUSTERS     2        // 20 core slots / 10 per MGPU (drm/asahi max_num_clusters)
#define G15_SGX_ID_VERSION     0xd04000 // = Fender+0x4000
#define G15_SGX_CORE_MASK      0xe01500 // 3 words, third masked to 16 bits
#define G15_SGX_ID_COUNTS_1    0xd04010 // [7:0] cores/cluster, [15:8] clusters/die, [19:16] dies
#define G15_SGX_ID_COUNTS_2    0xd04014 // further topology counts, as drm/asahi reads them
#define G15_MAX_CLUSTERS       2        // the most clusters an ID_COUNTS_1 topology may report
#define G15_ARRAY_CLUSTERS     4        // array bound: the most clusters of a supported SoC (G15C)
#define T6030_LEAK_FUSE        0x3642c81a8 // efuse page 0x3642c8000 + 0x1a8
#define G15S_PWR_CLAMP         70000.0f    // I_static + I_dynamic clamp

/* Sanity bounds for ADT perf states. Anything outside them disables the GPU. */
#define G15_PS_MIN_MV 400
#define G15_PS_MAX_MV 1500
#define G15_PS_MIN_HZ 100000000u
#define G15_PS_MAX_HZ 3000000000u

/*
 * Per-SoC parameters of the G15 GPU setup; the tables are below calc_power_t6030(). Everything
 * else (pstates, UAT regions, firmware segments, register windows) is read from the ADT at boot.
 */
struct g15_soc {
    u32 chip;               /* selects the known firmware images, see firmware_gfx_version() */
    const char *name;       /* SoC, for the log */
    const char *gpu_name;   /* GPU variant, for the log */
    const char *compatible; /* the gpu node must have it */
    const char *optin;      /* /chosen property that arms the setup when it is "1" */
    u8 id_gen;              /* expected ID_VERSION[31:24] */
    u8 id_variant;          /* expected ID_VERSION[23:16] */
    /* Core topology: fixed here, or 0 to read it from ID_COUNTS_1 (then `dies` is required) */
    u32 cores_per_cluster;
    u32 clusters;
    u32 dies;
    u32 mask2_bits; /* core mask bits in the third mask word; the bits above it are ignored */
    /*
     * The GPU power closure must be off before the setup powers it on, and a failed setup releases
     * it recursively and checks that it is off again.
     */
    bool power_closure;
    /* Also accept the firmware images firmware.c lists as experimental */
    bool fw_experimental;
    /* Add the ADT's power-management values to the gpu node, see g15_power_props[] */
    bool power_props;
    /*
     * T6031: the firmware is admitted by its family (dt_t6031_gpu_image()); the identity and core
     * masks come from the bounded probe that ran before, so the setup itself accesses no GPU
     * hardware; the node takes the CS and AFR states; the ADT's firmware nub is fw_nub (NULL:
     * /arm-io/gfx-asc/iop-gfx-nub).
     */
    bool family_fw;
    bool probed;
    bool csafr;
    const char *fw_nub;
    /* NULL while not known: the setup then stops before it touches the GPU */
    int (*read_leak)(const struct g15_soc *soc, float *leak);
    void (*calc_power)(u32 count, const struct perf_state *core, float leak, u32 enabled_cores,
                       u32 *max_pwr);
};

struct g15_region {
    const char *adt_name;
    const char *dt_path;
    u64 base;
    u64 size;
};

/*
 * The t602x UAT regions plus gfx-shared-l2-region. On G15 iBoot puts the second-level table of the
 * firmware's TTBR1 tree there (j516s: 0x108fff74000, right below gfx-shared-region), so it has to
 * be in the kernel's linear map like the other page-table pages (first armed boot oopsed in
 * UatPageTable::map_pages reading 0x108fff76000).
 */
#define G15_NUM_REGIONS 4
static const struct g15_region g15_region_names[G15_NUM_REGIONS] = {
    {"gfx-handoff", "/reserved-memory/uat-handoff", 0, 0},
    {"gfx-shared-region", "/reserved-memory/uat-pagetables", 0, 0},
    {"gpu-region", "/reserved-memory/uat-ttbs", 0, 0},
    {"gfx-shared-l2-region", "/reserved-memory/uat-pagetables-l2", 0, 0},
};

static int g15_guard_begin(void)
{
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    return before;
}

/* Returns -1 if a synchronous abort or an SError was taken since g15_guard_begin() */
static int g15_guard_end(int before)
{
    /* Let any asynchronous SError from the guarded accesses arrive while the guard is armed */
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before ? 0 : -1;
}

static int g15_adt_u32(int node, const char *name, u32 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int g15_adt_u64(int node, const char *name, u64 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

/*
 * Perf states: T tables x N+1 entries of {u32 Hz, u32 mV}.
 *
 * The pstate count and the table count must come from the ADT and agree with each other: a stale
 * count could otherwise reshape the table into a different one that still passes every value check.
 * - perf-state-count (= N + 1, what upstream uses) and gpu-num-perf-states (= N) must agree
 *   when both are set.
 * - If only gpu-num-perf-states is set, perf-state-table-count must be present too (nothing is
 *   derived from a fallback count).
 * - A missing perf-state-table-count is derived from the length only when the count came from
 *   perf-state-count.
 */
static int g15_get_pstates(const struct g15_soc *soc, int sgx, u32 *count_out, u32 *tables_out,
                           const struct perf_state **out)
{
    /* With a topology read from the hardware, the tables are checked against it later */
    u32 max_tables = soc->clusters ? soc->clusters : G15_MAX_CLUSTERS;
    u32 count = 0, n = 0, tables = 0, len = 0;
    bool have_count = !g15_adt_u32(sgx, "perf-state-count", &count) && count;
    bool have_n = !g15_adt_u32(sgx, "gpu-num-perf-states", &n) && n;
    bool have_tables = !g15_adt_u32(sgx, "perf-state-table-count", &tables) && tables;

    if (have_count && have_n && count != n + 1)
        bail("ADT: GPU: perf-state-count %u disagrees with gpu-num-perf-states %u\n", count, n);
    if (!have_count) {
        if (!have_n || !have_tables)
            bail("ADT: GPU: no usable perf-state-count (gpu-num-perf-states %u, tables %u)\n", n,
                 tables);
        if (n >= MAX_PSTATES)
            bail("ADT: GPU: gpu-num-perf-states %u too large\n", n);
        count = n + 1; // N pstates plus the off state
    }
    if (count < 2 || count > MAX_PSTATES)
        bail("ADT: GPU: invalid perf-state-count (%u)\n", count);

    const struct perf_state *ps = adt_getprop(adt, sgx, "perf-states", &len);
    if (!ps || !len || len % (sizeof(*ps) * count))
        bail("ADT: GPU: invalid or missing perf-states (len %u, count %u)\n", len, count);

    if (!have_tables)
        tables = len / (sizeof(*ps) * count);
    if (!tables || tables > max_tables || len != sizeof(*ps) * count * tables)
        bail("ADT: GPU: unexpected perf-state tables (count %u, tables %u, len %u)\n", count,
             tables, len);

    for (u32 t = 0; t < tables; t++) {
        for (u32 i = 0; i < count; i++) {
            u32 hz = ps[t * count + i].freq;
            u32 mv = ps[t * count + i].volt;

            /*
             * pstate 0 is the off state (j516s runtime ADT: 0 Hz / 125 mV), so it is not checked.
             * Zero voltages in later tables are back-filled.
             */
            if (i == 0)
                continue;
            if (mv && (mv < G15_PS_MIN_MV || mv > G15_PS_MAX_MV))
                bail("ADT: GPU: perf state %u/%u has bad voltage %u mV\n", t, i, mv);
            if (t != 0)
                continue;
            if (!mv || hz < G15_PS_MIN_HZ || hz > G15_PS_MAX_HZ)
                bail("ADT: GPU: perf state %u has bad values (%u Hz, %u mV)\n", i, hz, mv);
        }
    }

    /*
     * The ADT order is not strictly increasing in frequency (j516s: 1182 MHz twice, 1312 MHz
     * before 1242 MHz) and must be kept as-is, since the firmware indexes pstates by position.
     * drm/asahi only relies on the last pstate being the fastest (perf_max_pstate).
     */
    for (u32 i = 1; i < count - 1; i++)
        if (ps[i].freq > ps[count - 1].freq)
            bail("ADT: GPU: perf state %u (%u Hz) is above the last one (%u Hz)\n", i, ps[i].freq,
                 ps[count - 1].freq);

    /*
     * A perf-states-sram table is not passed on: the kernel sets SRAM to max(core voltage,
     * apple,min-sram-microvolt). With power_props that floor is taken from it, see
     * g15_get_power_props().
     */
    if (!soc->power_props && adt_getprop(adt, sgx, "perf-states-sram", NULL))
        printf("ADT: GPU: note: perf-states-sram present but unused on %s\n", soc->gpu_name);

    *count_out = count;
    *tables_out = tables;
    *out = ps;
    return 0;
}

static int g15_get_region(void *dt, int sgx, struct g15_region *r)
{
    char prop[64];

    snprintf(prop, sizeof(prop), "%s-base", r->adt_name);
    if (g15_adt_u64(sgx, prop, &r->base) || !r->base)
        bail("ADT: GPU: missing or invalid %s\n", prop);

    snprintf(prop, sizeof(prop), "%s-size", r->adt_name);
    if (g15_adt_u64(sgx, prop, &r->size) || !r->size)
        bail("ADT: GPU: missing or invalid %s\n", prop);

    /*
     * The UAT maps these with 16K pages and the TTBAT base must be 16K aligned; the
     * kernel needs at least one 16K page for each (mmu.rs). dt_set_memory() also refuses to boot
     * (any kernel) on an enabled reserved-memory region that is not page aligned, see
     * g15_check_regions().
     */
    if ((r->base & (SZ_16K - 1)) || (r->size & (SZ_16K - 1)) || r->size < SZ_16K ||
        (r->base & (get_page_size() - 1)) || (r->size & (get_page_size() - 1)) ||
        r->base + r->size < r->base)
        bail("ADT: GPU: bad %s region 0x%lx+0x%lx\n", r->adt_name, r->base, r->size);

    /* The node must already have a 2+2 cell reg so that setting it later cannot fail */
    int node = fdt_path_offset(dt, r->dt_path);
    int len;
    if (node < 0 || !fdt_getprop(dt, node, "reg", &len) || len != 2 * sizeof(fdt64_t))
        bail("FDT: GPU: %s missing or has an unexpected reg\n", r->dt_path);

    printf("ADT: GPU: %s 0x%lx+0x%lx -> %s\n", r->adt_name, r->base, r->size, r->dt_path);
    return 0;
}

/*
 * Upper bound on the reserved-memory regions dt_set_memory() may add to /memory (MAX_MEM_REGIONS in
 * kboot.c, including the usable-RAM region), with headroom for the nodes that later kboot steps
 * add.
 */
#define G15_MEM_REGIONS_LIMIT    32
#define G15_MEM_REGIONS_HEADROOM 8

/*
 * The uat-* regions are enabled reserved-memory nodes without no-map, so dt_set_memory() runs later
 * in the same kboot and adds them to /memory. It fails kboot (for every kernel, after this code has
 * already returned) if such a region straddles the usable-RAM boundary, is not page aligned, or if
 * it runs out of memory regions. Everything that could make it fail is checked here, so a bad
 * region only disables the GPU:
 * - each region lies entirely outside usable RAM [phys_base, phys_base + mem_size). Later kboot
 *   steps only shrink usable RAM from the top, which keeps this true. iBoot carves the GPU regions
 *   out of the top of DRAM, so inside usable RAM would be unexpected anyway (and could overlap aura
 *   or the kernel).
 * - each region lies inside DRAM, if the ADT gives the DRAM range.
 * - the regions do not overlap each other.
 * - dt_set_memory() will not run out of regions.
 */
static int g15_check_regions(void *dt, const struct g15_region *regions)
{
    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();
    u64 dram_base = 0, dram_size = 0;

    int chosen = adt_path_offset(adt, "/chosen");
    bool have_dram = chosen >= 0 && !g15_adt_u64(chosen, "dram-base", &dram_base) &&
                     !g15_adt_u64(chosen, "dram-size", &dram_size) && dram_size &&
                     dram_base + dram_size > dram_base;

    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        const struct g15_region *r = &regions[i];
        u64 end = r->base + r->size;

        if (!(end <= ram_min || r->base >= ram_max))
            bail("ADT: GPU: %s 0x%lx..0x%lx is not outside usable RAM 0x%lx..0x%lx\n", r->adt_name,
                 r->base, end, ram_min, ram_max);
        if (have_dram && (r->base < dram_base || end > dram_base + dram_size))
            bail("ADT: GPU: %s 0x%lx..0x%lx is outside DRAM 0x%lx+0x%lx\n", r->adt_name, r->base,
                 end, dram_base, dram_size);

        for (int j = 0; j < i; j++) {
            if (r->base < regions[j].base + regions[j].size && regions[j].base < end)
                bail("ADT: GPU: %s overlaps %s\n", r->adt_name, regions[j].adt_name);
        }
    }

    /* Same selection as dt_set_memory(): enabled, mapped, and outside usable RAM */
    int used = 1 + G15_NUM_REGIONS;
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv >= 0) {
        int node;
        fdt_for_each_subnode(node, dt, resv)
        {
            int len;
            const char *status = fdt_getprop(dt, node, "status", NULL);
            if (status && !strcmp(status, "disabled"))
                continue;
            if (fdt_getprop(dt, node, "no-map", NULL))
                continue;
            const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
            if (!reg || len < (int)(2 * sizeof(fdt64_t)))
                continue;
            u64 start = fdt64_ld(&reg[0]);
            if (start < ram_min || start >= ram_max)
                used++;
        }
    }
    if (used + G15_MEM_REGIONS_HEADROOM > G15_MEM_REGIONS_LIMIT)
        bail("FDT: GPU: too many reserved-memory regions (%d) to add the UAT regions\n", used);

    return 0;
}

/* Address of the reg entry called `name` of the gpu node (2+2 cells, identity-mapped /soc) */
static int g15_dt_reg_by_name(void *dt, int gpu, const char *name, u64 *addr)
{
    int parent = fdt_parent_offset(dt, gpu);
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2)
        bail("FDT: GPU: unexpected gpu parent cells\n");

    int idx = fdt_stringlist_search(dt, gpu, "reg-names", name);
    if (idx < 0)
        bail("FDT: GPU: no %s in reg-names\n", name);

    int len;
    const fdt64_t *reg = fdt_getprop(dt, gpu, "reg", &len);
    if (!reg || len < 0 || (size_t)len < (idx + 1) * 2 * sizeof(fdt64_t))
        bail("FDT: GPU: gpu reg too short\n");

    *addr = fdt64_ld(&reg[idx * 2]);
    return 0;
}

/*
 * Read the GPU ID, the core masks and the leakage fuse. The DT sgx/asc bases must match the ADT, so
 * that we probe the same block the kernel will use.
 */
/* Set once dt_set_gpu_g15_inner() has started to power on the GPU */
static bool g15_powered;

static int t8122_identity_read(void *context, u32 *id, u32 masks[3], u32 *counts, u32 *counts2);

static int g15_read_hw(void *dt, int gpu, const struct g15_soc *soc, u32 *core_count,
                       u32 *clusters_out, u32 *enabled_cores, float *leak,
                       struct t8122_powered_identity *identity)
{
    int sgx_path[8], asc_path[8];
    u64 sgx_base, sgx_size, asc_base, dt_sgx, dt_asc;

    if (adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path) < 0)
        bail("ADT: GPU: Failed to get sgx\n");
    if (adt_get_reg(adt, sgx_path, "reg", 0, &sgx_base, &sgx_size) < 0)
        bail("ADT: GPU: Failed to get sgx reg 0\n");
    if (adt_path_offset_trace(adt, "/arm-io/gfx-asc", asc_path) < 0)
        bail("ADT: GPU: Failed to get gfx-asc\n");
    if (adt_get_reg(adt, asc_path, "reg", 0, &asc_base, NULL) < 0)
        bail("ADT: GPU: Failed to get gfx-asc reg 0\n");

    if (g15_dt_reg_by_name(dt, gpu, "sgx", &dt_sgx) || g15_dt_reg_by_name(dt, gpu, "asc", &dt_asc))
        return -1;
    if (dt_sgx != sgx_base || dt_asc != asc_base)
        bail("FDT: GPU: DT sgx/asc 0x%lx/0x%lx do not match ADT 0x%lx/0x%lx\n", dt_sgx, dt_asc,
             sgx_base, asc_base);
    if (!sgx_base || sgx_size < G15_SGX_CORE_MASK + 12)
        bail("ADT: GPU: sgx reg 0 too small (0x%lx)\n", sgx_size);

    /*
     * Only a closure that is off now can be put back exactly as it was if the setup fails later
     * (a plain power-off of the sgx devices can leave their parents on).
     */
    if (soc->power_closure && pmgr_adt_power_is_off("/arm-io/sgx") != 1)
        bail("FDT: GPU: the GPU power closure is not off, not powering it on\n");

    /*
     * PMGR power-on, unguarded like every other pmgr_adt_power_enable() caller in aura: these are
     * the regular PMGR PS registers (the polls are bounded), and a GUARD_MARK fault in the middle
     * of the read-modify-write would corrupt registers instead of recovering. The caller powers the
     * GPU back off if anything after this point fails.
     */
    g15_powered = true;
    if (pmgr_adt_power_enable("/arm-io/sgx") < 0)
        bail("FDT: GPU: failed to power on sgx\n");

    bool hw_topology = !soc->cores_per_cluster;
    u32 counts = 0, id_version, mask0, mask1, mask2;
    if (identity) {
        if (t8122_full_identity_read(t8122_identity_read, &sgx_base, identity))
            bail("FDT: GPU: full-start T8122 powered identity rejected\n");
        id_version = identity->id_version;
        counts = identity->id_counts_1;
        mask0 = identity->core_mask[0];
        mask1 = identity->core_mask[1];
        mask2 = identity->core_mask[2];
    } else {
        int before = g15_guard_begin();
        id_version = read32(sgx_base + G15_SGX_ID_VERSION);
        if (hw_topology)
            counts = read32(sgx_base + G15_SGX_ID_COUNTS_1);
        mask0 = read32(sgx_base + G15_SGX_CORE_MASK);
        mask1 = read32(sgx_base + G15_SGX_CORE_MASK + 4);
        mask2 = read32(sgx_base + G15_SGX_CORE_MASK + 8);
        if (g15_guard_end(before))
            bail("FDT: GPU: fault reading the GPU ID registers\n");
    }

    printf("FDT: GPU: ID_VERSION 0x%08x (gen %d variant %d rev %d.%d)\n", id_version,
           id_version >> 24, (id_version >> 16) & 0xff, (id_version >> 12) & 0xf,
           (id_version >> 8) & 0xf);
    if ((id_version >> 24) != soc->id_gen || ((id_version >> 16) & 0xff) != soc->id_variant)
        bail("FDT: GPU: not a %s (expected gen %d variant %d)\n", soc->gpu_name, soc->id_gen,
             soc->id_variant);

    /* Cores per cluster and clusters: fixed for the SoC, or as drm/asahi reads them */
    u32 cores_per_cluster = soc->cores_per_cluster, clusters = soc->clusters;
    if (hw_topology) {
        u32 dies = (counts >> 16) & 0xf;

        cores_per_cluster = counts & 0xff;
        clusters = ((counts >> 8) & 0xff) * dies;
        printf("FDT: GPU: ID_COUNTS_1 0x%08x: %u die(s), %u cluster(s) x %u core slots\n", counts,
               dies, clusters, cores_per_cluster);
        if (dies != soc->dies)
            bail("FDT: GPU: %u dies, expected %u\n", dies, soc->dies);
        if (!clusters || clusters > G15_MAX_CLUSTERS || !cores_per_cluster ||
            cores_per_cluster > 32)
            bail("FDT: GPU: unsupported core topology\n");
    }
    u32 slots = clusters * cores_per_cluster;

    /* Up to 80 (G15S) or 96 mask bits; at most 64 slots */
    u64 mask = mask0 | ((u64)mask1 << 32);
    u32 mask2_valid = (u32)MASK(soc->mask2_bits);
    printf("FDT: GPU: core mask 0x%0*x%016lx\n", (int)(soc->mask2_bits / 4), mask2 & mask2_valid,
           mask);
    if ((mask2 & mask2_valid) || (slots < 64 && (mask >> slots)))
        bail("FDT: GPU: core mask has bits beyond the %d %s core slots\n", slots, soc->gpu_name);

    *enabled_cores = 0;
    printf("FDT: GPU: Core counts: ");
    for (u32 i = 0; i < clusters; i++) {
        core_count[i] =
            __builtin_popcountll((mask >> (i * cores_per_cluster)) & MASK(cores_per_cluster));
        *enabled_cores += core_count[i];
        printf("%d ", core_count[i]);
    }
    printf("\n");
    if (!*enabled_cores)
        bail("FDT: GPU: no enabled cores\n");
    *clusters_out = clusters;

    return soc->read_leak(soc, leak);
}

/* G15S: one leakage fuse */
static int g15s_read_leak(const struct g15_soc *soc, float *leak)
{
    UNUSED(soc);

    /* First aura access to this fuse page (AON region, inside the arm-io MMIO mapping) */
    int before = g15_guard_begin();
    u32 fuse = read32(T6030_LEAK_FUSE);
    if (g15_guard_end(before))
        bail("FDT: GPU: fault reading leakage fuse at 0x%lx\n", T6030_LEAK_FUSE);

    /* v = fuse[29:15], leak = 2.0 * (v + 1). Only core_leak[0] exists on G15S. */
    u32 v = (fuse >> 15) & 0x7fff;
    *leak = 2.0f * (v + 1);

    return 0;
}

/*
 * G15S maximum performance power model: per pstate, a banded static current plus a dynamic
 * current, clamped, times the voltage. Units: I in mA, P in mW, so max_pwr[] is P * 1000
 * (microwatts), as for the other SoCs. Only table 0 voltages are used. Inputs are
 * bounded by g15_get_pstates() (mV <= 1500) and the clamp, so the result always fits a u32.
 */
static void calc_power_t6030(u32 count, const struct perf_state *core, float leak,
                             u32 enabled_cores, u32 *max_pwr)
{
    max_pwr[0] = 0;

    for (u32 i = 1; i < count; i++) {
        float mv = core[i].volt;
        float v = mv / 1000.f;
        u32 f_mhz = core[i].freq / 1000000; // truncated to integer MHz
        float k, b, a;

        if (mv < 700) {
            k = 0.0095196338f;
            b = 0.0017332509f;
            a = 3.8552403f;
        } else if (mv < 900) {
            k = 0.0083248606f;
            b = 0.0020220736f;
            a = 3.6032584f;
        } else {
            k = 0.0033452217f;
            b = 0.0038701950f;
            a = 2.7668998f;
        }

        float i_static = leak * k * expf(a + b * mv);
        float i_dynamic = 32.0f * powf(v, 1.2f) * f_mhz * enabled_cores / (float)G15S_MAX_CORES;
        float i_total = i_static + i_dynamic;

        if (!(i_total >= 0.0f)) // also catches NaN
            i_total = 0.0f;
        if (i_total > G15S_PWR_CLAMP)
            i_total = G15S_PWR_CLAMP;

        max_pwr[i] = v * i_total * 1000;
    }
}

/*
 * T8122 (G15G) stand-in power model, for experiments only: G15G's power model is not known, and its
 * decoded leakage fuse (t8122_leak_fuse_decode()) is used only on request, so these numbers are not
 * measured.
 * - Scaling: calc_power_t6030() for the Air's enabled cores (8 or 10). Its dynamic term is per
 *   G15S core, so it comes to enabled_cores / G15S_MAX_CORES of a G15S's.
 * - Why it overstates: the static term is not scaled down, although G15G has one cluster of 10
 *   core slots where G15S has two, and the leakage is twice the value fused into the J516S M3
 *   Pro's G15S (3206): about four times the J516S's static power per core slot.
 * It is not a power limit and does not lower clocks: drm/asahi sets the firmware's power targets
 * to the largest OPP power, so they rise with this table. The kernel's performance-state cap is
 * what limits the clocks. To be replaced by measured numbers.
 */
#define G15G_STANDIN_LEAK 6412.0f

/*
 * T8122 (G15G) leakage fuse. On either M3 Air, every boot reads the two fuse words
 * (T8122_LEAK_FUSE_WORD0/1, t8122_leak_fuse_export() below) and decodes two fields:
 * - field A, 14 bits: bits 31:29 of word 0 are its low 3 bits, bits 10:0 of word 1 its high 11;
 * - field B, 10 bits: bits 20:11 of word 1.
 * The other bits are not used. The values are 2 x (A + 1) and B + 1. Both are logged and
 * exported to the kernel as /chosen/asahi,t8122-gpu-leak-fuse (two IEEE-754 single cells, in that
 * order). The GPU setup keeps G15G_STANDIN_LEAK unless chosen.asahi,t8122-gpu-fuse-leakage=1
 * asks it to use the first value instead, as g15s_read_leak() uses the T6030 fuse.
 */
#define T8122_LEAK_FUSE_WORD0 0x2e42c8128UL
#define T8122_LEAK_FUSE_WORD1 0x2e42c812cUL
#define T8122_LEAK_FUSE_PROP  "asahi,t8122-gpu-leak-fuse"
#define T8122_LEAK_FUSE_OPTIN "asahi,t8122-gpu-fuse-leakage"

struct t8122_leak_fuse {
    u32 field_a, field_b;
    float value[2];
};

/* This boot's decoded fuse (valid), and whether the GPU setup is to use it (use) */
static struct t8122_leak_fuse t8122_fuse;
static bool t8122_fuse_valid, t8122_fuse_use;

static void t8122_leak_fuse_decode(u32 word0, u32 word1, struct t8122_leak_fuse *fuse)
{
    fuse->field_a = (word0 >> 29) | ((word1 & 0x7ff) << 3);
    fuse->field_b = (word1 >> 11) & 0x3ff;
    fuse->value[0] = 2.0f * (float)(fuse->field_a + 1);
    fuse->value[1] = (float)(fuse->field_b + 1);
}

static int g15g_standin_read_leak(const struct g15_soc *soc, float *leak)
{
    UNUSED(soc);

    if (!t8122_fuse_use) {
        printf("FDT: GPU: T8122 leakage: the stand-in%s\n",
               t8122_fuse_valid ? " (the fuse values are exported only)" : "");
        *leak = G15G_STANDIN_LEAK;
        return 0;
    }
    if (!t8122_fuse_valid)
        bail("FDT: GPU: chosen.%s=1, but this boot has no T8122 leakage fuse values\n",
             T8122_LEAK_FUSE_OPTIN);
    *leak = t8122_fuse.value[0];
    printf("FDT: GPU: T8122 leakage: %u, from this Mac's fuse (chosen.%s=1)\n",
           (u32)t8122_fuse.value[0], T8122_LEAK_FUSE_OPTIN);
    return 0;
}

static void calc_power_t8122_standin(u32 count, const struct perf_state *core, float leak,
                                     u32 enabled_cores, u32 *max_pwr)
{
    printf("FDT: GPU: T8122 power model: stand-in (not measured; does not limit clocks)\n");
    printf("FDT: GPU: T8122 stand-in: G15S model, dynamic power for %u of %u G15S cores, static "
           "power unscaled\n",
           enabled_cores, G15S_MAX_CORES);
    calc_power_t6030(count, core, leak, enabled_cores, max_pwr);
}

/* Replace all children of the operating-points-v2 table with one OPP per ADT pstate. */
static int g15_set_opps(void *dt, u32 opps_ph, u32 count, u32 table_count, u32 clusters,
                        const struct perf_state *perf_states, const u32 *max_pwr)
{
    int opps = fdt_node_offset_by_phandle(dt, opps_ph);
    if (opps < 0)
        bail("FDT: GPU: node for phandle %u not found\n", opps_ph);

    int child;
    while ((child = fdt_first_subnode(dt, opps)) >= 0) {
        if (fdt_del_node(dt, child))
            bail("FDT: GPU: failed to delete placeholder OPP\n");
        opps = fdt_node_offset_by_phandle(dt, opps_ph);
        if (opps < 0)
            bail("FDT: GPU: OPP table lost\n");
    }

    /* fdt_add_subnode() inserts at the head, so add in reverse to keep opp00 first */
    for (int i = count - 1; i >= 0; i--) {
        char name[16];
        fdt32_t volts[G15_ARRAY_CLUSTERS];

        for (u32 j = 0; j < clusters; j++) {
            /*
             * One voltage per MGPU (drm/asahi expects max_num_clusters entries). Missing tables and
             * zero entries are back-filled from table 0.
             */
            u32 t = j < table_count ? j : 0;
            u32 mv = perf_states[i + t * count].volt;
            if (!mv)
                mv = perf_states[i].volt;
            volts[j] = cpu_to_fdt32(mv * 1000);
        }

        snprintf(name, sizeof(name), "opp%02d", i);
        int opp = fdt_add_subnode(dt, opps, name);
        if (opp < 0)
            bail("FDT: GPU: failed to add %s\n", name);
        if (fdt_setprop_u64(dt, opp, "opp-hz", perf_states[i].freq))
            bail("FDT: GPU: Failed to set opp-hz for PS %d\n", i);
        if (fdt_setprop(dt, opp, "opp-microvolt", volts, sizeof(fdt32_t) * clusters))
            bail("FDT: GPU: Failed to set opp-microvolt for PS %d\n", i);
        if (fdt_setprop_u32(dt, opp, "opp-microwatt", max_pwr[i]))
            bail("FDT: GPU: Failed to set opp-microwatt for PS %d\n", i);

        opps = fdt_node_offset_by_phandle(dt, opps_ph);
        if (opps < 0)
            bail("FDT: GPU: OPP table lost\n");
    }

    return 0;
}

/*
 * G15C CS and AFR clock domains (T6031). ADT cs-perf-states and afr-perf-states are each
 * {u64 dies, u64 count}, then dies x count {u64 microvolts, u64 Hz}, and on T6031 one more u64 in
 * microvolts, equal to the GPU's SRAM floor, taken as the CS/AFR SRAM floor. Only one die is
 * supported. Each becomes an OPP table (opp-hz, one opp-microvolt cell per die, no power) behind
 * the gpu node's apple,cs-opp or apple,afr-opp phandle, rebuilt like g15_set_opps() does the main
 * one.
 */
#define G15_AUX_MAX    16
#define G15_AUX_MIN_HZ 10000000u /* CS and AFR idle at 24 MHz */

struct g15_aux {
    const char *adt; /* ADT property of the sgx node */
    const char *dt;  /* gpu node property naming the OPP table */
    u32 count;
    u32 phandle;
    u64 uv[G15_AUX_MAX];
    u64 hz[G15_AUX_MAX];
    u64 sram_uv; /* the trailing value, 0 if the ADT has none */
};

static int g15_get_aux(void *dt, int gpu, int sgx, u32 dies, struct g15_aux *a)
{
    u64 hdr[2];
    u32 len = 0;
    const u8 *p = adt_getprop(adt, sgx, a->adt, &len);

    if (!p || len < sizeof(hdr))
        bail("ADT: GPU: missing or short %s\n", a->adt);
    memcpy(hdr, p, sizeof(hdr));
    if (dies != 1 || hdr[0] != dies || !hdr[1] || hdr[1] > G15_AUX_MAX)
        bail("ADT: GPU: %s has %lu die(s) and %lu states, not 1 die and 1 to %d\n", a->adt, hdr[0],
             hdr[1], G15_AUX_MAX);
    u32 body = sizeof(hdr) + hdr[1] * 2 * sizeof(u64);
    if (len != body && len != body + sizeof(u64))
        bail("ADT: GPU: %s is %u bytes, not %u or %u\n", a->adt, len, body,
             body + (u32)sizeof(u64));

    a->count = hdr[1];
    a->sram_uv = 0;
    for (u32 i = 0; i < a->count; i++) {
        memcpy(&a->uv[i], p + sizeof(hdr) + 16 * i, sizeof(u64));
        memcpy(&a->hz[i], p + sizeof(hdr) + 16 * i + 8, sizeof(u64));
        if (a->uv[i] < G15_PS_MIN_MV * 1000 || a->uv[i] > G15_PS_MAX_MV * 1000 ||
            a->hz[i] < G15_AUX_MIN_HZ || a->hz[i] > G15_PS_MAX_HZ)
            bail("ADT: GPU: %s state %u has bad values (%lu Hz, %lu uV)\n", a->adt, i, a->hz[i],
                 a->uv[i]);
    }
    if (len == body + sizeof(u64)) {
        memcpy(&a->sram_uv, p + body, sizeof(u64));
        if (a->sram_uv < G15_PS_MIN_MV * 1000 || a->sram_uv > G15_PS_MAX_MV * 1000)
            bail("ADT: GPU: %s SRAM floor %lu uV is out of range\n", a->adt, a->sram_uv);
    }

    int plen;
    const fdt32_t *ph = fdt_getprop(dt, gpu, a->dt, &plen);
    if (!ph || plen != sizeof(*ph) || fdt_node_offset_by_phandle(dt, fdt32_ld(ph)) < 0)
        bail("FDT: GPU: %s missing or not an OPP table\n", a->dt);
    a->phandle = fdt32_ld(ph);
    return 0;
}

/* The CS and AFR SRAM floor: the one both ADT tables end with, else the GPU's SRAM floor */
static int g15_csafr_sram(const struct g15_aux aux[2], u32 gpu_floor_uv, u32 *out)
{
    if (aux[0].sram_uv && aux[1].sram_uv) {
        if (aux[0].sram_uv != aux[1].sram_uv)
            bail("ADT: GPU: the CS and AFR SRAM floors disagree (%lu, %lu uV)\n", aux[0].sram_uv,
                 aux[1].sram_uv);
        *out = aux[0].sram_uv;
    } else if (!aux[0].sram_uv && !aux[1].sram_uv) {
        *out = gpu_floor_uv;
    } else {
        bail("ADT: GPU: only one of the CS and AFR tables has an SRAM floor\n");
    }
    printf("ADT: GPU: CS %u and AFR %u states, CS/AFR SRAM floor %u uV (%s)\n", aux[0].count,
           aux[1].count, *out, aux[0].sram_uv ? "the tables' own" : "the GPU's");
    return 0;
}

static int g15_set_aux_opps(void *dt, const struct g15_aux *a)
{
    int opps = fdt_node_offset_by_phandle(dt, a->phandle), child;
    if (opps < 0)
        bail("FDT: GPU: %s table lost\n", a->dt);
    while ((child = fdt_first_subnode(dt, opps)) >= 0) {
        if (fdt_del_node(dt, child))
            bail("FDT: GPU: failed to delete a placeholder of %s\n", a->dt);
        opps = fdt_node_offset_by_phandle(dt, a->phandle);
        if (opps < 0)
            bail("FDT: GPU: %s table lost\n", a->dt);
    }
    /* fdt_add_subnode() inserts at the head, so add in reverse to keep opp00 first */
    for (int i = a->count - 1; i >= 0; i--) {
        char name[16];
        snprintf(name, sizeof(name), "opp%02d", i);
        int opp = fdt_add_subnode(dt, opps, name);
        if (opp < 0 || fdt_setprop_u64(dt, opp, "opp-hz", a->hz[i]) ||
            fdt_setprop_u32(dt, opp, "opp-microvolt", a->uv[i]))
            bail("FDT: GPU: failed to add %s %s\n", a->dt, name);
        opps = fdt_node_offset_by_phandle(dt, a->phandle);
        if (opps < 0)
            bail("FDT: GPU: %s table lost\n", a->dt);
    }
    return 0;
}

static int g15_set_float_array(void *dt, const char *name, float val, int count);

/*
 * The CS and AFR leakage have no G15C source: 0 stands in, as for the SRAM leakage, and is listed
 * in asahi,t6031-gpu-standin with the other stand-ins.
 */
#define G15C_STANDIN_AUX_LEAK 0.0f

static int g15_set_csafr(void *dt, const struct g15_aux aux[2], u32 sram_uv, u32 dies)
{
    if (g15_set_aux_opps(dt, &aux[0]) || g15_set_aux_opps(dt, &aux[1]) ||
        g15_set_float_array(dt, "apple,cs-leak-coef", G15C_STANDIN_AUX_LEAK, dies) ||
        g15_set_float_array(dt, "apple,afr-leak-coef", G15C_STANDIN_AUX_LEAK, dies))
        return -1;
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0 || fdt_setprop_u32(dt, gpu, "apple,csafr-min-sram-microvolt", sram_uv))
        bail("FDT: GPU: failed to set apple,csafr-min-sram-microvolt\n");
    return 0;
}

/* The gpu node properties that hold stand-ins, not this Mac's values (T6031) */
static int g15_set_standins(void *dt)
{
    static const char list[] = "apple,core-leak-coef\0apple,sram-leak-coef\0apple,cs-leak-coef\0"
                               "apple,afr-leak-coef\0opp-microwatt";
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0 || fdt_setprop(dt, gpu, "asahi,t6031-gpu-standin", list, sizeof(list)))
        bail("FDT: GPU: failed to set asahi,t6031-gpu-standin\n");
    return 0;
}

static int g15_set_float_array(void *dt, const char *name, float val, int count)
{
    fdt32_t data[MAX_CLUSTERS];

    if (count > MAX_CLUSTERS)
        bail("FDT: GPU: g15_set_float_array() with too many values\n");

    for (int i = 0; i < count; i++) {
        u32 raw;
        memcpy(&raw, &val, sizeof(raw));
        data[i] = cpu_to_fdt32(raw);
    }

    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");

    /* fdt_setprop (not _inplace): the placeholder may be missing or have another length */
    if (fdt_setprop(dt, gpu, name, data, sizeof(u32) * count))
        bail("FDT: GPU: Failed to set %s\n", name);

    return 0;
}

static int g15_set_status(void *dt, const char *path, const char *status)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        bail("FDT: GPU: %s not found\n", path);
    if (fdt_setprop_string(dt, node, "status", status))
        bail("FDT: GPU: failed to set %s status to %s\n", path, status);
    return 0;
}

/*
 * Power-management values for a SoC whose device tree does not carry them (T8122: drm/asahi then
 * takes them from the boot loader). Each is one 32-bit cell, apple,X from the ADT sgx node's gpu-X
 * (apple,se-target from gpu-se-tgt), copied as it is: the float ones are IEEE-754 bit patterns in
 * both. g15_get_power_props() also adds
 * - apple,fast-die0-sensor-mask, the 32-bit gpu-fast-die0-sensor-mask widened to 64 bits,
 * - apple,min-sram-microvolt, the lowest SRAM voltage of the ADT pstates,
 * - apple,idleoff-standby-timer, only when the ADT has gpu-idleoff-standby-timer.
 */
static const struct {
    const char *adt;
    const char *dt;
} g15_power_props[] = {
    {"gpu-perf-base-pstate", "apple,perf-base-pstate"},
    {"gpu-avg-power-filter-tc-ms", "apple,avg-power-filter-tc-ms"},
    {"gpu-avg-power-ki-only", "apple,avg-power-ki-only"},
    {"gpu-avg-power-kp", "apple,avg-power-kp"},
    {"gpu-avg-power-min-duty-cycle", "apple,avg-power-min-duty-cycle"},
    {"gpu-avg-power-target-filter-tc", "apple,avg-power-target-filter-tc"},
    {"gpu-fast-die0-integral-gain", "apple,fast-die0-integral-gain"},
    {"gpu-fast-die0-proportional-gain", "apple,fast-die0-proportional-gain"},
    {"gpu-perf-boost-ce-step", "apple,perf-boost-ce-step"},
    {"gpu-perf-boost-min-util", "apple,perf-boost-min-util"},
    {"gpu-perf-filter-drop-threshold", "apple,perf-filter-drop-threshold"},
    {"gpu-perf-filter-time-constant", "apple,perf-filter-time-constant"},
    {"gpu-perf-filter-time-constant2", "apple,perf-filter-time-constant2"},
    {"gpu-perf-integral-gain", "apple,perf-integral-gain"},
    {"gpu-perf-integral-gain2", "apple,perf-integral-gain2"},
    {"gpu-perf-integral-min-clamp", "apple,perf-integral-min-clamp"},
    {"gpu-perf-proportional-gain", "apple,perf-proportional-gain"},
    {"gpu-perf-proportional-gain2", "apple,perf-proportional-gain2"},
    {"gpu-perf-tgt-utilization", "apple,perf-tgt-utilization"},
    {"gpu-power-sample-period", "apple,power-sample-period"},
    {"gpu-ppm-filter-time-constant-ms", "apple,ppm-filter-time-constant-ms"},
    {"gpu-ppm-ki", "apple,ppm-ki"},
    {"gpu-ppm-kp", "apple,ppm-kp"},
    {"gpu-pwr-filter-time-constant", "apple,pwr-filter-time-constant"},
    {"gpu-pwr-integral-gain", "apple,pwr-integral-gain"},
    {"gpu-pwr-integral-min-clamp", "apple,pwr-integral-min-clamp"},
    {"gpu-pwr-min-duty-cycle", "apple,pwr-min-duty-cycle"},
    {"gpu-pwr-proportional-gain", "apple,pwr-proportional-gain"},
    {"gpu-pwr-sample-period-aic-clks", "apple,pwr-sample-period-aic-clks"},
    {"gpu-se-engagement-criteria", "apple,se-engagement-criteria"},
    {"gpu-se-filter-time-constant", "apple,se-filter-time-constant"},
    {"gpu-se-filter-time-constant-1", "apple,se-filter-time-constant-1"},
    {"gpu-se-inactive-threshold", "apple,se-inactive-threshold"},
    {"gpu-se-ki", "apple,se-ki"},
    {"gpu-se-ki-1", "apple,se-ki-1"},
    {"gpu-se-kp", "apple,se-kp"},
    {"gpu-se-kp-1", "apple,se-kp-1"},
    {"gpu-se-reset-criteria", "apple,se-reset-criteria"},
    {"gpu-se-tgt", "apple,se-target"},
};

struct g15_power_values {
    u32 val[ARRAY_SIZE(g15_power_props)];
    u32 fast_die_mask;
    u32 min_sram_uv;
    bool have_idleoff;
    u32 idleoff;
};

/*
 * Read and check every value g15_set_power_props() adds, before anything else changes. `core` is
 * the perf-states table g15_get_pstates() checked.
 */
static int g15_get_power_props(int sgx, u32 count, u32 table_count, const struct perf_state *core,
                               struct g15_power_values *pv)
{
    for (size_t i = 0; i < ARRAY_SIZE(g15_power_props); i++)
        if (g15_adt_u32(sgx, g15_power_props[i].adt, &pv->val[i]))
            bail("ADT: GPU: missing or invalid %s\n", g15_power_props[i].adt);

    if (g15_adt_u32(sgx, "gpu-fast-die0-sensor-mask", &pv->fast_die_mask))
        bail("ADT: GPU: missing or invalid gpu-fast-die0-sensor-mask\n");

    /* Optional, but one cell when present */
    u32 len = 0;
    pv->have_idleoff = adt_getprop(adt, sgx, "gpu-idleoff-standby-timer", &len) != NULL;
    if (pv->have_idleoff && g15_adt_u32(sgx, "gpu-idleoff-standby-timer", &pv->idleoff))
        bail("ADT: GPU: malformed gpu-idleoff-standby-timer (len %u)\n", len);

    /* The SRAM floor: the lowest SRAM voltage of the active pstates, from the first table */
    u32 min_mv = 0;
    len = 0;
    const struct perf_state *sram = adt_getprop(adt, sgx, "perf-states-sram", &len);
    if (!sram || len != sizeof(*sram) * count * table_count)
        bail("ADT: GPU: invalid or missing perf-states-sram (len %u)\n", len);
    for (u32 i = 1; i < count; i++) {
        u32 mv = sram[i].volt;
        if (mv < G15_PS_MIN_MV || mv > G15_PS_MAX_MV)
            bail("ADT: GPU: perf state %u has bad SRAM voltage %u mV\n", i, mv);
        if (!min_mv || mv < min_mv)
            min_mv = mv;
    }
    pv->min_sram_uv = min_mv * 1000;

    /*
     * The kernel gives every pstate SRAM at max(core voltage, floor). Refuse an ADT that asks for
     * more SRAM voltage than that anywhere. Zero entries of later tables stand for table 0's, as
     * in g15_set_opps().
     */
    for (u32 t = 0; t < table_count; t++) {
        for (u32 i = 1; i < count; i++) {
            u32 core_mv = core[t * count + i].volt, sram_mv = sram[t * count + i].volt;
            if (!core_mv)
                core_mv = core[i].volt;
            if (!sram_mv)
                sram_mv = sram[i].volt;
            if (sram_mv > max(core_mv, min_mv))
                bail("ADT: GPU: perf state %u/%u SRAM %u mV is above max(core %u mV, floor %u mV); "
                     "not supported\n",
                     t, i, sram_mv, core_mv, min_mv);
        }
    }

    printf("ADT: GPU: %u power-management values, fast-die sensor mask 0x%x, SRAM floor %u mV, "
           "idle-off standby timer %s\n",
           (u32)ARRAY_SIZE(g15_power_props), pv->fast_die_mask, min_mv,
           pv->have_idleoff ? "present" : "absent (kernel default)");
    return 0;
}

static int g15_set_power_props(void *dt, const struct g15_power_values *pv)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");

    int err = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g15_power_props) && !err; i++)
        err = fdt_setprop_u32(dt, gpu, g15_power_props[i].dt, pv->val[i]);
    if (!err)
        err = fdt_setprop_u64(dt, gpu, "apple,fast-die0-sensor-mask", pv->fast_die_mask);
    if (!err)
        err = fdt_setprop_u32(dt, gpu, "apple,min-sram-microvolt", pv->min_sram_uv);
    if (!err && pv->have_idleoff)
        err = fdt_setprop_u32(dt, gpu, "apple,idleoff-standby-timer", pv->idleoff);
    if (err)
        bail("FDT: GPU: failed to add the power-management properties (%d)\n", err);

    printf("FDT: GPU: %u power-management properties added from the ADT\n",
           (u32)ARRAY_SIZE(g15_power_props) + 2 + pv->have_idleoff);
    return 0;
}

/*
 * GPU firmware segments (fw-text, fw-data).
 *
 * iBoot loads the GPU coprocessor firmware before aura runs. ADT /arm-io/gfx-asc "segment-ranges"
 * says where: one {u64 phys, u64 firmware VA, u64 remap, u32 size, u32 flags} entry per segment,
 * in the order of "segment-names" (text, then data). A kernel that runs this firmware in place
 * needs the two carveouts and their firmware VAs, so the gpu node gets:
 * - two no-map reserved-memory nodes, appended to its memory-region as "fw-text" and "fw-data".
 *   The existing entries and names keep their order, so kernels that look up the other regions
 *   are not affected.
 * - apple,firmware-segment-vas = <TEXT VA, DATA VA> (two u64),
 * - apple,firmware-segment-flags = <1 0> (the ADT flags of the two segments),
 * - apple,m3-handoff-version = <1>.
 * All of these are added, or none: a missing or unexpected ADT or DT input, or a failed DT write,
 * leaves the DT as it was and is logged. The GPU setup does not depend on them. Only the ADT and
 * the DT are read for this, never the GPU or its memory.
 *
 * The carveouts lie outside usable RAM (checked), and no-map keeps them out of the kernel's linear
 * map: dt_set_memory() does not add no-map regions to /memory.
 */
#define G15_FW_SEGS         2
#define G15_FW_DATA_SEG     "__DATA"     // name of the second (last) segment
#define G15_FW_SEG_MAX_SIZE (16 * SZ_1M) // sanity bound; the 14.8.3 image uses 0x5c000 + 0x114000
#define G15_HANDOFF_VERSION 1
/* Bounds for the gpu node's memory-region and memory-region-names, which are copied for the undo */
#define G15_MAX_MEM_REGIONS      16
#define G15_MAX_MEM_REGION_NAMES 256
#define G15_FW_NODE_NAME_LEN     40

struct g15_fw_seg {
    const char *name;        /* memory-region-names entry */
    const char *node_prefix; /* reserved-memory node name, before the unit address */
    u32 flags;               /* ADT segment flags this segment must have */
    u64 phys;
    u64 va;
    u64 size;
    char node[G15_FW_NODE_NAME_LEN];
};

static const struct g15_fw_seg g15_fw_seg_defs[G15_FW_SEGS] = {
    {"fw-text", "gpu-fw-text", 1, 0, 0, 0, ""},
    {"fw-data", "gpu-fw-data", 0, 0, 0, 0, ""},
};

static const char *const g15_fw_seg_props[] = {
    "apple,firmware-segment-vas",
    "apple,firmware-segment-flags",
    "apple,m3-handoff-version",
};

/* What g15_unset_fw_segs() needs to restore the DT. Reset by dt_set_gpu_g15(). */
static struct {
    bool active; /* the DT may hold (part of) the firmware segment description */
    int mr_len;
    int names_len;
    fdt32_t mr[G15_MAX_MEM_REGIONS];
    char names[G15_MAX_MEM_REGION_NAMES];
    char node[G15_FW_SEGS][G15_FW_NODE_NAME_LEN]; /* added nodes, "" if not added */
} g15_fw_undo;

/* Whether [a, a + a_size) and [b, b + b_size) overlap; a range that wraps extends to the end */
static bool g15_overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
    u64 a_end = a + a_size < a ? ~0UL : a + a_size;
    u64 b_end = b + b_size < b ? ~0UL : b + b_size;

    return a_size && b_size && a < b_end && b < a_end;
}

/*
 * Read and check the firmware segments from the ADT and the gpu node inputs from the DT. Changes
 * nothing. `regions` are the checked UAT regions. Returns 0 if the description can be added.
 */
static int g15_get_fw_segs_nub(void *dt, int gpu, const struct g15_region *regions,
                               struct g15_fw_seg *segs, const char *nub_path);

static int g15_get_fw_segs(void *dt, int gpu, const struct g15_region *regions,
                           struct g15_fw_seg *segs)
{
    return g15_get_fw_segs_nub(dt, gpu, regions, segs, "/arm-io/gfx-asc/iop-gfx-nub");
}

/* As g15_get_fw_segs(), with the firmware nub at `nub_path` (T6031: iop-gfx-nub0) */
static int g15_get_fw_segs_nub(void *dt, int gpu, const struct g15_region *regions,
                               struct g15_fw_seg *segs, const char *nub_path)
{
    struct adt_segment_ranges ranges[G15_FW_SEGS];
    u32 len;

    int asc = adt_path_offset(adt, "/arm-io/gfx-asc");
    if (asc < 0)
        bail("ADT: GPU: /arm-io/gfx-asc not found\n");

    len = 0;
    const char *seg_names = adt_getprop(adt, asc, "segment-names", &len);
    /* Exactly two ";"-separated names, the second being G15_FW_DATA_SEG */
    const char *sep = seg_names && len ? memchr(seg_names, ';', len) : NULL;
    u32 tail = sep ? len - (u32)(sep + 1 - seg_names) : 0;
    if (!sep || sep == seg_names || tail < sizeof(G15_FW_DATA_SEG) ||
        memcmp(sep + 1, G15_FW_DATA_SEG, sizeof(G15_FW_DATA_SEG)))
        bail("ADT: GPU: gfx-asc segment-names is not two segments ending in " G15_FW_DATA_SEG "\n");

    len = 0;
    const void *seg_ranges = adt_getprop(adt, asc, "segment-ranges", &len);
    if (!seg_ranges || len != sizeof(ranges))
        bail("ADT: GPU: gfx-asc segment-ranges missing or not %d entries (len %u)\n", G15_FW_SEGS,
             len);
    memcpy(ranges, seg_ranges, sizeof(ranges));

    /* The firmware nub below gfx-asc carries a copy of the segments; if present it must agree */
    int nub = adt_path_offset(adt, nub_path);
    if (nub >= 0) {
        u32 preloaded;

        len = 0;
        const void *nub_ranges = adt_getprop(adt, nub, "segment-ranges", &len);
        if (nub_ranges && (len != sizeof(ranges) || memcmp(nub_ranges, ranges, sizeof(ranges))))
            bail("ADT: GPU: iop-gfx-nub segment-ranges differ from gfx-asc\n");
        if (!g15_adt_u32(nub, "pre-loaded", &preloaded) && preloaded != 1)
            bail("ADT: GPU: iop-gfx-nub pre-loaded is %u, not 1\n", preloaded);
    }

    u64 dram_base, dram_size;
    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen < 0 || g15_adt_u64(chosen, "dram-base", &dram_base) ||
        g15_adt_u64(chosen, "dram-size", &dram_size) || !dram_size ||
        dram_base + dram_size <= dram_base)
        bail("ADT: GPU: no usable /chosen dram-base/dram-size\n");

    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();

    for (int i = 0; i < G15_FW_SEGS; i++) {
        const struct adt_segment_ranges *r = &ranges[i];
        struct g15_fw_seg *s = &segs[i];

        *s = g15_fw_seg_defs[i];
        s->phys = r->phys;
        s->va = r->iova;
        s->size = r->size;
        u64 end = s->phys + s->size;

        if (r->unk != s->flags)
            bail("ADT: GPU: firmware segment %d flags 0x%x, expected 0x%x\n", i, r->unk, s->flags);
        /* remap repeats the physical address for these segments */
        if (r->remap != r->phys)
            bail("ADT: GPU: firmware segment %d remap 0x%lx differs from phys 0x%lx\n", i, r->remap,
                 r->phys);
        if (!s->phys || !s->size || s->size > G15_FW_SEG_MAX_SIZE ||
            ((s->phys | s->size) & (SZ_16K - 1)) || ((s->phys | s->size) & (get_page_size() - 1)) ||
            end < s->phys)
            bail("ADT: GPU: bad firmware segment %d 0x%lx+0x%lx\n", i, s->phys, s->size);
        if (!s->va || (s->va & (SZ_16K - 1)) || s->va + s->size < s->va)
            bail("ADT: GPU: bad firmware segment %d VA 0x%lx+0x%lx\n", i, s->va, s->size);

        if (!(end <= ram_min || s->phys >= ram_max))
            bail("ADT: GPU: firmware %s 0x%lx..0x%lx is not outside usable RAM 0x%lx..0x%lx\n",
                 s->name, s->phys, end, ram_min, ram_max);
        if (s->phys < dram_base || end > dram_base + dram_size)
            bail("ADT: GPU: firmware %s 0x%lx..0x%lx is outside DRAM 0x%lx+0x%lx\n", s->name,
                 s->phys, end, dram_base, dram_size);
        for (size_t j = 0; j < mcc_carveout_count; j++)
            if (g15_overlaps(s->phys, s->size, mcc_carveouts[j].base, mcc_carveouts[j].size))
                bail("ADT: GPU: firmware %s overlaps memory carveout 0x%lx+0x%lx\n", s->name,
                     mcc_carveouts[j].base, mcc_carveouts[j].size);
        for (int j = 0; j < G15_NUM_REGIONS; j++)
            if (g15_overlaps(s->phys, s->size, regions[j].base, regions[j].size))
                bail("ADT: GPU: firmware %s overlaps %s\n", s->name, regions[j].adt_name);
        for (int j = 0; j < i; j++) {
            if (g15_overlaps(s->phys, s->size, segs[j].phys, segs[j].size))
                bail("ADT: GPU: firmware %s overlaps %s\n", s->name, segs[j].name);
            if (g15_overlaps(s->va, s->size, segs[j].va, segs[j].size))
                bail("ADT: GPU: firmware %s VA range overlaps %s\n", s->name, segs[j].name);
        }

        snprintf(s->node, sizeof(s->node), "%s@%lx", s->node_prefix, s->phys);
    }

    /* Reserved memory: 2+2 cells, no node of that name yet, no overlap with an enabled region */
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0 || fdt_address_cells(dt, resv) != 2 || fdt_size_cells(dt, resv) != 2)
        bail("FDT: GPU: /reserved-memory missing or not 2+2 cells\n");

    int node;
    fdt_for_each_subnode(node, dt, resv)
    {
        const char *node_name = fdt_get_name(dt, node, NULL);
        for (int i = 0; i < G15_FW_SEGS; i++)
            if (node_name && !strcmp(node_name, segs[i].node))
                bail("FDT: GPU: /reserved-memory/%s already exists\n", node_name);

        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (status && !strcmp(status, "disabled"))
            continue;

        int reg_len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &reg_len);
        if (!reg || reg_len <= 0)
            continue;
        for (int k = 0; (k + 2) * (int)sizeof(fdt64_t) <= reg_len; k += 2) {
            u64 base = fdt64_ld(&reg[k]), size = fdt64_ld(&reg[k + 1]);
            for (int i = 0; i < G15_FW_SEGS; i++)
                if (size && g15_overlaps(segs[i].phys, segs[i].size, base, size))
                    bail("FDT: GPU: firmware %s overlaps /reserved-memory/%s\n", segs[i].name,
                         node_name ? node_name : "?");
        }
    }

    /*
     * The gpu node: memory-region and memory-region-names present, with one name per entry (so
     * that the appended names line up), no fw-text/fw-data yet, none of the properties yet.
     */
    int mr_len = 0, names_len = 0;
    const void *mr = fdt_getprop(dt, gpu, "memory-region", &mr_len);
    const void *mr_names = fdt_getprop(dt, gpu, "memory-region-names", &names_len);
    int count = fdt_stringlist_count(dt, gpu, "memory-region-names");
    if (!mr || !mr_names || mr_len <= 0 || mr_len % sizeof(fdt32_t) ||
        mr_len > (int)sizeof(g15_fw_undo.mr) || names_len <= 0 ||
        names_len > (int)sizeof(g15_fw_undo.names) || count != mr_len / (int)sizeof(fdt32_t))
        bail("FDT: GPU: gpu memory-region (len %d) and memory-region-names (%d names) missing, "
             "too long or inconsistent\n",
             mr_len, count);
    for (int i = 0; i < G15_FW_SEGS; i++)
        if (fdt_stringlist_search(dt, gpu, "memory-region-names", segs[i].name) >= 0)
            bail("FDT: GPU: gpu memory-region-names already has %s\n", segs[i].name);
    for (size_t i = 0; i < ARRAY_SIZE(g15_fw_seg_props); i++)
        if (fdt_getprop(dt, gpu, g15_fw_seg_props[i], NULL))
            bail("FDT: GPU: gpu node already has %s\n", g15_fw_seg_props[i]);

    return 0;
}

/*
 * Undo g15_set_fw_segs(), also after a partial one. The gpu node's memory-region lists are only
 * ever restored to their original (same or shorter) values and the rest is deleted, so this needs
 * no DT space. Returns -1 if the DT could not be restored.
 */
static int g15_unset_fw_segs(void *dt, const char *gpu_path)
{
    if (!g15_fw_undo.active)
        return 0;

    int gpu = fdt_path_offset(dt, gpu_path);
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");
    if (fdt_setprop(dt, gpu, "memory-region", g15_fw_undo.mr, g15_fw_undo.mr_len))
        bail("FDT: GPU: failed to restore memory-region\n");
    gpu = fdt_path_offset(dt, gpu_path);
    if (gpu < 0 ||
        fdt_setprop(dt, gpu, "memory-region-names", g15_fw_undo.names, g15_fw_undo.names_len))
        bail("FDT: GPU: failed to restore memory-region-names\n");
    for (size_t i = 0; i < ARRAY_SIZE(g15_fw_seg_props); i++) {
        gpu = fdt_path_offset(dt, gpu_path);
        if (gpu < 0)
            bail("FDT: GPU: gpu alias lost\n");
        int err = fdt_delprop(dt, gpu, g15_fw_seg_props[i]);
        if (err && err != -FDT_ERR_NOTFOUND)
            bail("FDT: GPU: failed to delete %s\n", g15_fw_seg_props[i]);
    }

    /* Nothing refers to the nodes any more */
    for (int i = 0; i < G15_FW_SEGS; i++) {
        if (!g15_fw_undo.node[i][0])
            continue;
        int resv = fdt_path_offset(dt, "/reserved-memory");
        int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, g15_fw_undo.node[i]);
        if (node >= 0 && fdt_del_node(dt, node))
            bail("FDT: GPU: failed to delete /reserved-memory/%s\n", g15_fw_undo.node[i]);
        if (node < 0 && node != -FDT_ERR_NOTFOUND)
            bail("FDT: GPU: cannot look up /reserved-memory/%s\n", g15_fw_undo.node[i]);
        g15_fw_undo.node[i][0] = 0;
    }

    g15_fw_undo.active = false;
    return 0;
}

static int g15_set_fw_segs_inner(void *dt, const char *gpu_path, const struct g15_fw_seg *segs)
{
    fdt32_t mr[G15_MAX_MEM_REGIONS + G15_FW_SEGS];
    char names[G15_MAX_MEM_REGION_NAMES + sizeof("fw-text") + sizeof("fw-data")];
    fdt64_t vas[G15_FW_SEGS];
    fdt32_t flags[G15_FW_SEGS];
    fdt32_t version = cpu_to_fdt32(G15_HANDOFF_VERSION);
    int len;

    /* Copy what gets appended to, for the undo (bounds checked by g15_get_fw_segs()) */
    int gpu = fdt_path_offset(dt, gpu_path);
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");
    const void *p = fdt_getprop(dt, gpu, "memory-region", &len);
    if (!p || len <= 0 || len > (int)sizeof(g15_fw_undo.mr))
        bail("FDT: GPU: gpu memory-region changed\n");
    memcpy(g15_fw_undo.mr, p, len);
    g15_fw_undo.mr_len = len;
    p = fdt_getprop(dt, gpu, "memory-region-names", &len);
    if (!p || len <= 0 || len > (int)sizeof(g15_fw_undo.names))
        bail("FDT: GPU: gpu memory-region-names changed\n");
    memcpy(g15_fw_undo.names, p, len);
    g15_fw_undo.names_len = len;
    g15_fw_undo.active = true;

    int mr_len = g15_fw_undo.mr_len, names_len = g15_fw_undo.names_len;
    memcpy(mr, g15_fw_undo.mr, mr_len);
    memcpy(names, g15_fw_undo.names, names_len);

    for (int i = 0; i < G15_FW_SEGS; i++) {
        const struct g15_fw_seg *s = &segs[i];
        fdt64_t reg[2];
        u32 phandle;

        int resv = fdt_path_offset(dt, "/reserved-memory");
        if (resv < 0)
            bail("FDT: GPU: /reserved-memory lost\n");
        int node = fdt_add_subnode(dt, resv, s->node);
        if (node < 0)
            bail("FDT: GPU: failed to add /reserved-memory/%s (%d)\n", s->node, node);
        memcpy(g15_fw_undo.node[i], s->node, sizeof(g15_fw_undo.node[i]));

        fdt64_st(&reg[0], s->phys);
        fdt64_st(&reg[1], s->size);
        int err = fdt_generate_phandle(dt, &phandle);
        if (!err)
            err = fdt_setprop_u32(dt, node, "phandle", phandle);
        if (!err)
            err = fdt_setprop(dt, node, "reg", reg, sizeof(reg));
        if (!err)
            err = fdt_setprop_empty(dt, node, "no-map");
        if (err)
            bail("FDT: GPU: failed to fill /reserved-memory/%s (%d)\n", s->node, err);

        size_t name_len = strlen(s->name) + 1;
        if ((size_t)names_len + name_len > sizeof(names))
            bail("FDT: GPU: memory-region-names too long\n");
        mr[mr_len / sizeof(fdt32_t)] = cpu_to_fdt32(phandle);
        mr_len += sizeof(fdt32_t);
        memcpy(names + names_len, s->name, name_len);
        names_len += name_len;
        fdt64_st(&vas[i], s->va);
        flags[i] = cpu_to_fdt32(s->flags);
    }

    gpu = fdt_path_offset(dt, gpu_path);
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");
    int err = fdt_setprop(dt, gpu, "memory-region", mr, mr_len);
    if (!err)
        err = fdt_setprop(dt, gpu, "memory-region-names", names, names_len);
    if (!err)
        err = fdt_setprop(dt, gpu, "apple,firmware-segment-vas", vas, sizeof(vas));
    if (!err)
        err = fdt_setprop(dt, gpu, "apple,firmware-segment-flags", flags, sizeof(flags));
    if (!err)
        err = fdt_setprop(dt, gpu, "apple,m3-handoff-version", &version, sizeof(version));
    if (err)
        bail("FDT: GPU: failed to set the gpu firmware segment properties (%d)\n", err);

    return 0;
}

/*
 * Add the firmware segment description checked by g15_get_fw_segs(). Returns 0 when it was added,
 * 1 when it was not and the DT is unchanged, -1 when a partial description could not be removed.
 */
static int g15_set_fw_segs(void *dt, const char *gpu_path, const struct g15_fw_seg *segs)
{
    if (!g15_set_fw_segs_inner(dt, gpu_path, segs)) {
        printf("FDT: GPU: fw-text 0x%lx+0x%lx (VA 0x%lx), fw-data 0x%lx+0x%lx (VA 0x%lx), "
               "m3-handoff-version %d\n",
               segs[0].phys, segs[0].size, segs[0].va, segs[1].phys, segs[1].size, segs[1].va,
               G15_HANDOFF_VERSION);
        return 0;
    }

    if (g15_unset_fw_segs(dt, gpu_path)) {
        printf("FDT: GPU: could not remove a partial firmware segment description\n");
        return -1;
    }
    printf("FDT: GPU: firmware segment description not added (DT write failed, undone)\n");
    return 1;
}

/*
 * The gpu node's mailbox, its first "mboxes" phandle. A DT may keep it disabled until the GPU is
 * usable (the T8122 DTs do); the setup then enables it right before the gpu node, and g15_disable()
 * disables it again. A mailbox that is not disabled is left alone (T6030).
 */
static u32 g15_mbox_enabled; /* phandle of the mailbox this setup enabled, 0 if none */

/* *phandle: the mailbox if it is disabled and has to be enabled with the gpu node, else 0 */
static int g15_get_mbox(void *dt, int gpu, u32 *phandle)
{
    int len;
    const fdt32_t *mboxes = fdt_getprop(dt, gpu, "mboxes", &len);

    *phandle = 0;
    if (!mboxes)
        return 0;
    if (len < (int)sizeof(fdt32_t))
        bail("FDT: GPU: gpu mboxes is malformed\n");

    u32 ph = fdt32_ld(mboxes);
    int mbox = fdt_node_offset_by_phandle(dt, ph);
    if (mbox < 0)
        bail("FDT: GPU: gpu mailbox (phandle %u) not found\n", ph);
    const char *status = fdt_getprop(dt, mbox, "status", NULL);
    if (status && !strcmp(status, "disabled")) {
        printf("FDT: GPU: gpu mailbox %s is disabled, it is enabled with the gpu node\n",
               fdt_get_name(dt, mbox, NULL));
        *phandle = ph;
    }
    return 0;
}

static int g15_set_mbox_status(void *dt, u32 phandle, const char *status)
{
    int mbox = fdt_node_offset_by_phandle(dt, phandle);
    if (mbox < 0 || fdt_setprop_string(dt, mbox, "status", status))
        bail("FDT: GPU: failed to set the gpu mailbox status to %s\n", status);
    return 0;
}

/* Put the gpu node and its UAT regions back into the "not usable" state. Best effort. */
static void g15_disable(void *dt)
{
    static const fdt32_t zero[3] = {0, 0, 0};

    if (g15_unset_fw_segs(dt, "gpu"))
        printf("FDT: GPU: failed to remove the firmware segment description\n");

    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        int node = fdt_path_offset(dt, g15_region_names[i].dt_path);
        if (node >= 0)
            fdt_setprop_string(dt, node, "status", "disabled");
    }

    if (g15_mbox_enabled) {
        g15_set_mbox_status(dt, g15_mbox_enabled, "disabled");
        g15_mbox_enabled = 0;
    }

    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        return;

    if (fdt_setprop_string(dt, gpu, "status", "disabled"))
        printf("FDT: GPU: failed to disable the gpu node\n");

    /*
     * Belt and braces: drm/asahi refuses G15 before any MMIO unless firmware-compat is <14 8 3>.
     * Zeroing it in place needs no DT space; deleting it (the kernel then refuses too) neither.
     */
    int len;
    if (fdt_getprop(dt, gpu, "apple,firmware-compat", &len) && len == sizeof(zero)) {
        if (fdt_setprop_inplace(dt, gpu, "apple,firmware-compat", zero, sizeof(zero)))
            printf("FDT: GPU: failed to clear apple,firmware-compat\n");
    } else {
        fdt_delprop(dt, gpu, "apple,firmware-compat");
    }
}

/*
 * T8122 (M3, AGX G15G). Same setup as T6030, with these differences:
 * - the core topology (cores per cluster, clusters) is read from ID_COUNTS_1, the way drm/asahi
 *   reads it for G15G, and the pstate tables must not outnumber the clusters it reports;
 * - the GPU power closure must be off before the setup powers it on, and a failed setup releases it
 *   recursively and checks that it is off again (as the T8122 powered identity probe does);
 * - the gpu node's mailbox is enabled with it when the DT keeps it disabled (see g15_get_mbox()).
 * Not known yet, so an armed T8122 setup stops before it touches the GPU: the G15G firmware images
 * (firmware_gfx_version() has none for T8122) and the power model; the leakage fuse is decoded but
 * not used by default. The stand-in experiment (g15_soc_t8122_standin) fills these in.
 */
static const struct g15_soc g15_soc_t6030 = {
    .chip = T6030,
    .name = "T6030",
    .gpu_name = "G15S",
    .compatible = "apple,agx-t6030",
    .optin = "asahi,t6030-gpu",
    .id_gen = G15S_ID_GEN,
    .id_variant = G15S_ID_VARIANT,
    .cores_per_cluster = G15S_CORES_PER_CLUSTER,
    .clusters = T6030_MAX_CLUSTERS,
    .mask2_bits = 16,
    .read_leak = g15s_read_leak,
    .calc_power = calc_power_t6030,
};

static const struct g15_soc g15_soc_t8122 = {
    .chip = T8122,
    .name = "T8122",
    .gpu_name = "G15G",
    .compatible = "apple,agx-t8122",
    .optin = "asahi,t8122-gpu",
    .id_gen = 7,     // drm/asahi hw/agx3.rs T8122: family 7
    .id_variant = 2, // variant 2 = G
    .dies = 1,
    .mask2_bits = 32,
    .power_closure = true,
};

/*
 * The T8122 experiment, armed by chosen.asahi,t8122-gpu=1 together with
 * chosen.asahi,t8122-gpu-power-standin=1: g15_soc_t8122, plus the experimental G15G firmware
 * image, the stand-in leakage and power model, and the ADT's power-management values on the gpu
 * node (drm/asahi takes them from the boot loader on T8122).
 */
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
    .fw_experimental = true,
    .power_props = true,
    .read_leak = g15g_standin_read_leak,
    .calc_power = calc_power_t8122_standin,
};

/*
 * T6031 (M3 Max, G15C), J514C/J516C only, requires chosen.asahi,t6031-gpu=1 (on a J516C,
 * dt_set_m3_defaults() sets it unless a config line set it): the J613 14.x setup with T6031
 * values, with these differences.
 * - The identity, topology and core masks come from the bounded powered identity read that runs
 *   first (t6031_gpu_probe()), which puts the GPU power closure back exactly as iBoot left it. The
 *   setup itself touches no GPU hardware and leaves the GPU unpowered: the kernel's power domain
 *   powers it when the GPU driver binds.
 * - The firmware is admitted by its family (dt_t6031_gpu_image()), not by a pinned image: the
 *   kernel identifies the image itself and starts it only when told to.
 * - Four clusters of ten core slots, one voltage per cluster in each OPP, and the CS and AFR
 *   domains (g15_set_csafr()).
 * - No G15C leakage source and no G15C power model: the T8122 stand-in leakage for every cluster,
 *   0 for the SRAM, CS and AFR leakage, and the G15S model for the OPP powers, all listed in
 *   asahi,t6031-gpu-standin. They do not limit clocks; the kernel's performance-state cap does.
 */
#define G15C_STANDIN_LEAK G15G_STANDIN_LEAK

static int g15c_standin_read_leak(const struct g15_soc *soc, float *leak)
{
    UNUSED(soc);
    printf("FDT: GPU: T6031 leakage: stand-in %u per cluster (no G15C leakage source); SRAM, CS "
           "and AFR 0\n",
           (u32)G15C_STANDIN_LEAK);
    *leak = G15C_STANDIN_LEAK;
    return 0;
}

static void calc_power_t6031_standin(u32 count, const struct perf_state *core, float leak,
                                     u32 enabled_cores, u32 *max_pwr)
{
    printf("FDT: GPU: T6031 power model: stand-in, the G15S model for %u cores (not measured; "
           "does not limit clocks)\n",
           enabled_cores);
    calc_power_t6030(count, core, leak, enabled_cores, max_pwr);
}

static const struct g15_soc g15_soc_t6031 = {
    .chip = T6031,
    .name = "T6031",
    .gpu_name = "G15C",
    .compatible = "apple,agx-t6031",
    .optin = "asahi,t6031-gpu",
    .id_gen = 7,
    .id_variant = 4, // drm/asahi hw/agx3.rs T6031: family 7, variant 4 = C
    .cores_per_cluster = T6031_CORES_PER_CLUSTER,
    .clusters = T6031_GPU_CLUSTERS,
    .dies = 1,
    .mask2_bits = 32,
    .power_props = true,
    .family_fw = true,
    .probed = true,
    .csafr = true,
    .fw_nub = "/arm-io/gfx-asc/iop-gfx-nub0",
    .read_leak = g15c_standin_read_leak,
    .calc_power = calc_power_t6031_standin,
};

/* This boot's T6031 firmware family (NULL: not admitted) and powered identity, set before the setup
 */
static const struct fw_version_info *g15_t6031_fw;
static struct t8122_powered_identity g15_t6031_id;

/*
 * T6031: the identity of the bounded probe that ran before this setup, instead of g15_read_hw()'s
 * power-on and reads. The device tree's GPU windows must still be the ADT's.
 */
static int g15_probed_hw(void *dt, int gpu, const struct g15_soc *soc, u32 *core_count,
                         u32 *clusters_out, u32 *enabled_cores, float *leak)
{
    int sgx_path[8], asc_path[8];
    u64 sgx_base, asc_base, dt_sgx, dt_asc;

    if (adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path) < 0 ||
        adt_get_reg(adt, sgx_path, "reg", 0, &sgx_base, NULL) < 0 ||
        adt_path_offset_trace(adt, "/arm-io/gfx-asc", asc_path) < 0 ||
        adt_get_reg(adt, asc_path, "reg", 0, &asc_base, NULL) < 0)
        bail("ADT: GPU: no sgx or gfx-asc reg 0\n");
    if (g15_dt_reg_by_name(dt, gpu, "sgx", &dt_sgx) || g15_dt_reg_by_name(dt, gpu, "asc", &dt_asc))
        return -1;
    if (dt_sgx != sgx_base || dt_asc != asc_base)
        bail("FDT: GPU: DT sgx/asc 0x%lx/0x%lx do not match ADT 0x%lx/0x%lx\n", dt_sgx, dt_asc,
             sgx_base, asc_base);
    if (g15_t6031_id.result != T8122_ID_PASS)
        bail("FDT: GPU: %s powered identity result %u, not 1 (pass): GPU node not filled\n",
             soc->name, g15_t6031_id.result);

    *clusters_out = soc->clusters;
    *enabled_cores = t6031_core_counts(&g15_t6031_id, core_count);
    printf("FDT: GPU: Core counts: ");
    for (u32 i = 0; i < soc->clusters; i++)
        printf("%d ", core_count[i]);
    printf("\n");
    return soc->read_leak(soc, leak);
}

/* 25G83 calibration on the scratch DT: this Mac's ADT and fuse, no unit tables.
 * The G15S-derived power model remains experimental. Release every power vote
 * after the bounded identity read; Linux owns firmware admission and startup.
 */
int dt_gpu_t8122_26_calibrate(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu"), sgx = adt_path_offset(adt, "/arm-io/sgx");
    u32 count, tables, clusters, enabled, cores[G15_MAX_CLUSTERS], power[MAX_PSTATES];
    const struct perf_state *ps;
    struct g15_power_values pv;
    if (gpu < 0 || sgx < 0 || !t8122_fuse_valid ||
        g15_get_pstates(&g15_soc_t8122_standin, sgx, &count, &tables, &ps) ||
        g15_get_power_props(sgx, count, tables, ps, &pv))
        return -1;
    int len;
    const fdt32_t *ref = fdt_getprop(dt, gpu, "operating-points-v2", &len);
    if (!ref || len != 4)
        return -1;
    u32 opps = fdt32_ld(ref);
    bool previous = t8122_fuse_use;
    t8122_fuse_use = true;
    g15_powered = false;
    float leak;
    int ret = g15_read_hw(dt, gpu, &g15_soc_t8122_standin, cores, &clusters, &enabled, &leak, NULL);
    t8122_fuse_use = previous;
    if (g15_powered && (pmgr_adt_power_disable_recursive("/arm-io/sgx") ||
                        pmgr_adt_power_is_off("/arm-io/sgx") != 1))
        ret = -1;
    g15_powered = false;
    if (ret || tables > clusters)
        return -1;
    calc_power_t8122_standin(count, ps, leak, enabled, power);
    if (power[count - 1] < 1000 || g15_set_opps(dt, opps, count, tables, clusters, ps, power) ||
        g15_set_power_props(dt, &pv) ||
        g15_set_float_array(dt, "apple,core-leak-coef", leak, clusters) ||
        g15_set_float_array(dt, "apple,sram-leak-coef", 0.0f, clusters))
        return -1;
    u32 sram_len;
    const struct perf_state *sram = adt_getprop(adt, sgx, "perf-states-sram", &sram_len);
    if (!sram || sram_len != sizeof(*sram) * count * tables)
        return -1;
    int table = fdt_node_offset_by_phandle(dt, opps), node, i = 0;
    fdt_for_each_subnode(node, dt, table)
    {
        if (i >= (int)count)
            return -1;
        fdt32_t voltage[G15_MAX_CLUSTERS];
        for (u32 t = 0; t < clusters; t++) {
            u32 mv = t < tables ? sram[t * count + i].volt : 0;
            voltage[t] = cpu_to_fdt32((mv ? mv : sram[i].volt) * 1000);
        }
        if (fdt_setprop(dt, node, "opp-microvolt-sram", voltage, clusters * sizeof(fdt32_t)))
            return -1;
        i++;
    }
    gpu = fdt_path_offset(dt, "gpu");
    if (i != (int)count || gpu < 0 || fdt_setprop_u32(dt, gpu, "apple,gpu-power-model", 1))
        return -1;
    return fdt_setprop_string(dt, gpu, "apple,j613-gpu-power-model",
                              "g15s-fuse-scaled-experimental");
}

/* Whether /chosen has `name` = "1" */
static bool g15_optin(void *dt, const char *name)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *optin = chosen >= 0 ? fdt_getprop(dt, chosen, name, &len) : NULL;
    return optin && len == 2 && !strcmp(optin, "1");
}

/*
 * Returns 0 when the GPU was set up and enabled, 1 after a successful dry run (not armed: every
 * firmware/ADT/DT check ran, no GPU hardware was touched and nothing was enabled), -1 on failure.
 */
static int dt_set_gpu_g15_inner(void *dt, const struct g15_soc *soc, bool armed)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        bail("ADT: GPU: /arm-io/sgx node not found\n");

    /* Informational: the chip revision (A0/B0/B1) selects the firmware image. */
    u32 chip_rev;
    int arm_io = adt_path_offset(adt, "/arm-io");
    if (arm_io >= 0 && !g15_adt_u32(arm_io, "chip-revision", &chip_rev))
        printf("ADT: GPU: arm-io chip-revision 0x%x\n", chip_rev);

    /*
     * 1. Firmware ABI. The GPU firmware ABI is identified from the image iBoot loaded, because
     * 14.8.3 and 14.7 share one iBoot string. Nothing else (in particular no GPU
     * register) is touched unless it is positively the 14.8.3 G15 ABI drm/asahi supports.
     */
    const struct fw_version_info *gfx_fw =
        soc->family_fw ? g15_t6031_fw : firmware_gfx_version_ex(soc->chip, soc->fw_experimental);
    bool have_model = soc->read_leak && soc->calc_power;
    if (!have_model)
        printf("FDT: GPU: the %s leakage fuse and power model are not known yet\n", soc->gpu_name);
    if (!gfx_fw)
        bail("FDT: GPU: loaded GPU firmware is not a known ABI\n");
    if (gfx_fw->version != V14_8_3 || gfx_fw->num_length != 3 || gfx_fw->num[0] != 14 ||
        gfx_fw->num[1] != 8 || gfx_fw->num[2] != 3)
        bail("FDT: GPU: GPU firmware ABI %s is not supported on %s\n", gfx_fw->string, soc->name);
    if (!have_model)
        return -1;

    /* 2. ADT and DT inputs (no DT changes yet) */
    u32 perf_state_count, perf_state_table_count;
    const struct perf_state *perf_states;
    if (g15_get_pstates(soc, sgx, &perf_state_count, &perf_state_table_count, &perf_states))
        return -1;

    struct g15_power_values power_values;
    if (soc->power_props && g15_get_power_props(sgx, perf_state_count, perf_state_table_count,
                                                perf_states, &power_values))
        return -1;

    struct g15_aux aux[2] = {{.adt = "cs-perf-states", .dt = "apple,cs-opp"},
                             {.adt = "afr-perf-states", .dt = "apple,afr-opp"}};
    u32 csafr_sram_uv = 0;
    if (soc->csafr && (!soc->power_props || g15_get_aux(dt, gpu, sgx, soc->dies, &aux[0]) ||
                       g15_get_aux(dt, gpu, sgx, soc->dies, &aux[1]) ||
                       g15_csafr_sram(aux, power_values.min_sram_uv, &csafr_sram_uv)))
        return -1;

    struct g15_region regions[G15_NUM_REGIONS];
    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        regions[i] = g15_region_names[i];
        if (g15_get_region(dt, sgx, &regions[i]))
            return -1;
    }
    if (g15_check_regions(dt, regions))
        return -1;

    int len;
    const fdt32_t *opps_ph_p = fdt_getprop(dt, gpu, "operating-points-v2", &len);
    if (!opps_ph_p || len != 4)
        bail("FDT: GPU: operating-points-v2 not found\n");
    u32 opps_ph = fdt32_ld(opps_ph_p);
    if (fdt_node_offset_by_phandle(dt, opps_ph) < 0)
        bail("FDT: GPU: OPP table for phandle %u not found\n", opps_ph);

    u32 mbox_ph;
    if (g15_get_mbox(dt, gpu, &mbox_ph))
        return -1;

    if (adt_getprop(adt, sgx, "gptbat-ready", NULL))
        printf("ADT: GPU: gptbat-ready present\n");
    else
        printf("ADT: GPU: gptbat-ready absent\n");

    /* Optional: the firmware segment description. Without it the GPU is still set up. */
    struct g15_fw_seg fw_segs[G15_FW_SEGS];
    bool have_fw_segs = !g15_get_fw_segs_nub(
        dt, gpu, regions, fw_segs, soc->fw_nub ? soc->fw_nub : "/arm-io/gfx-asc/iop-gfx-nub");
    if (have_fw_segs)
        printf("ADT: GPU: firmware text 0x%lx+0x%lx VA 0x%lx, data 0x%lx+0x%lx VA 0x%lx\n",
               fw_segs[0].phys, fw_segs[0].size, fw_segs[0].va, fw_segs[1].phys, fw_segs[1].size,
               fw_segs[1].va);
    else
        printf("FDT: GPU: no firmware segment description: fw-text, fw-data, "
               "apple,firmware-segment-* and apple,m3-handoff-version are not added\n");

    if (!armed) {
        printf("FDT: GPU: dry run OK: firmware %s, %u pstates x %u tables, UAT regions valid, "
               "firmware segments %s\n",
               gfx_fw->string, perf_state_count, perf_state_table_count,
               have_fw_segs ? "valid" : "omitted");
        printf("FDT: GPU: not armed (no chosen.%s=1): GPU hardware not touched, gpu "
               "node left disabled\n",
               soc->optin);
        return 1;
    }

    /* Reserve the full14 witness before power-on; GPU26 uses its separate path. */
    bool publish_identity = soc->chip == T8122;
    struct t8122_powered_identity identity = {0};
    if (publish_identity && t8122_full_identity_prepare(dt) < 0)
        bail("FDT: GPU: full-start T8122 identity node unavailable\n");
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        return -1;

    /* 3. Hardware: power on, GPU ID, core masks, leakage fuse (reads guarded); T6031: the probe's
     */
    u32 core_count[G15_ARRAY_CLUSTERS], clusters, enabled_cores;
    float leak;
    if (soc->probed) {
        if (g15_probed_hw(dt, gpu, soc, core_count, &clusters, &enabled_cores, &leak))
            return -1;
    } else if (g15_read_hw(dt, gpu, soc, core_count, &clusters, &enabled_cores, &leak,
                           publish_identity ? &identity : NULL)) {
        return -1;
    }
    if (perf_state_table_count > clusters)
        bail("FDT: GPU: %u perf-state tables but %u cluster(s)\n", perf_state_table_count,
             clusters);

    /* 4. Power model */
    u32 max_pwr[MAX_PSTATES];
    soc->calc_power(perf_state_count, perf_states, leak, enabled_cores, max_pwr);

    printf("FDT: GPU: Leakage: %d.%03d\n", (int)leak, ((int)(leak * 1000) % 1000));
    printf("FDT: GPU: Max power table: ");
    for (u32 i = 0; i < perf_state_count; i++)
        printf("%d ", max_pwr[i]);
    printf("\n");

    /* drm/asahi divides by max(opp-microwatt) / 1000 (rel_max_powers), so it must be >= 1 mW */
    for (u32 i = 1; i < perf_state_count; i++)
        if (max_pwr[i] > max_pwr[perf_state_count - 1])
            bail("FDT: GPU: max power of pstate %u is above the top pstate\n", i);
    if (max_pwr[perf_state_count - 1] < 1000)
        bail("FDT: GPU: implausible max power %u uW\n", max_pwr[perf_state_count - 1]);

    /* 5. DT writes. The gpu node and the uat-* regions are still disabled. */
    if (g15_set_opps(dt, opps_ph, perf_state_count, perf_state_table_count, clusters, perf_states,
                     max_pwr))
        return -1;

    /*
     * TODO: G15S has a single leakage value and no SRAM leakage fuse. It is not known yet which
     * HwData fields the firmware reads it from; drm/asahi wants one value per cluster, so give
     * every cluster the fused core value and 0 for SRAM.
     */
    if (g15_set_float_array(dt, "apple,core-leak-coef", leak, clusters))
        return -1;
    if (g15_set_float_array(dt, "apple,sram-leak-coef", 0.0f, clusters))
        return -1;
    if (soc->power_props && g15_set_power_props(dt, &power_values))
        return -1;
    if (soc->csafr && g15_set_csafr(dt, aux, csafr_sram_uv, soc->dies))
        return -1;
    if (soc->probed && g15_set_standins(dt))
        return -1;

    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        fdt64_t reg[2];
        int node = fdt_path_offset(dt, regions[i].dt_path);

        fdt64_st(&reg[0], regions[i].base);
        fdt64_st(&reg[1], regions[i].size);
        if (node < 0 || fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
            bail("FDT: GPU: failed to set reg for %s\n", regions[i].dt_path);
    }

    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        bail("FDT: GPU: gpu alias lost\n");
    if (firmware_set_fdt(dt, gpu, "apple,firmware-version", gfx_fw))
        return -1;
    if (firmware_set_fdt(dt, gpu, "apple,firmware-abi", gfx_fw))
        return -1;
    if (firmware_set_fdt(dt, gpu, "apple,firmware-compat", gfx_fw))
        return -1;

    /* All or nothing; a description that was not added does not stop the GPU setup */
    if (have_fw_segs && g15_set_fw_segs(dt, "gpu", fw_segs) < 0)
        return -1;

    /* 6. Enable: the UAT regions first, then the mailbox if it was disabled, the gpu node last */
    for (int i = 0; i < G15_NUM_REGIONS; i++)
        if (g15_set_status(dt, regions[i].dt_path, "okay"))
            return -1;
    if (mbox_ph) {
        g15_mbox_enabled = mbox_ph;
        if (g15_set_mbox_status(dt, mbox_ph, "okay"))
            return -1;
    }
    if (publish_identity && t8122_full_identity_publish(dt, &identity))
        bail("FDT: GPU: full-start T8122 identity publication failed\n");
    if (g15_set_status(dt, "gpu", "okay"))
        return -1;

    printf("FDT: GPU: %s GPU enabled (firmware %s)\n", soc->name, gfx_fw->string);
    return 0;
}

/* Never fails: on any problem the gpu node is left disabled and the boot continues. */
static int dt_set_gpu_g15(void *dt, const struct g15_soc *soc)
{
    /* Per-kboot state: nothing of the firmware segment description is in this DT yet */
    memset(&g15_fw_undo, 0, sizeof(g15_fw_undo));
    g15_mbox_enabled = 0;

    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }

    if (fdt_node_check_compatible(dt, gpu, soc->compatible)) {
        printf("FDT: GPU: gpu node is not %s, leaving it alone\n", soc->compatible);
        return 0;
    }

    /* Disabled until everything below has succeeded */
    if (fdt_setprop_string(dt, gpu, "status", "disabled")) {
        printf("FDT: GPU: cannot mark the gpu node disabled, not setting up the %s GPU\n",
               soc->name);
        g15_disable(dt);
        return 0;
    }

    /*
     * Explicit opt-in before any GPU hardware access. aura cannot tell which kernel GRUB will boot,
     * so without it the power-on and GPU register reads would run for every kernel, the default one
     * included. The opt-in is a config line appended to boot.bin (update-aura copies chosen.* lines
     * from /etc/aura.conf):  chosen.asahi,t6030-gpu=1 (chosen.asahi,t8122-gpu=1 on T8122)
     * Without it this is a dry run: the firmware identification (DRAM reads only) and all ADT/DT
     * checks run and are logged, then the gpu node is left disabled. T6031 also
     * requires its switch before its powered identity probe. The M3 switches of the boards
     * dt_set_m3_defaults() covers are set by default, and a config line of 0 still turns one off.
     */
    bool armed = g15_optin(dt, soc->optin);

    g15_powered = false;
    int ret = dt_set_gpu_g15_inner(dt, soc, armed);
    if (ret) {
        if (ret < 0)
            printf("FDT: GPU: %s GPU setup failed, leaving the gpu node disabled\n", soc->name);
        if (soc->chip == T8122)
            t8122_full_identity_invalidate(dt);
        g15_disable(dt);
        if (g15_powered) {
            bool off;
            if (soc->power_closure)
                off = !pmgr_adt_power_disable_recursive("/arm-io/sgx") &&
                      pmgr_adt_power_is_off("/arm-io/sgx") == 1;
            else
                off = pmgr_adt_power_disable("/arm-io/sgx") >= 0;
            if (!off)
                printf("FDT: GPU: failed to power the GPU back off\n");
        }
    }

    return 0;
}

static int dt_set_gpu_t8122(void *dt);

/*
 * On a J613 or J615, read the same T8122 leakage fuse words under the
 * exception guard, before anything powers the GPU, then log and export the decoded values (see
 * t8122_leak_fuse_decode()). It changes nothing else: the GPU setup uses them only with
 * chosen.asahi,t8122-gpu-fuse-leakage=1. A fault or a full device tree is logged, and the boot
 * goes on without the property.
 */
static void t8122_leak_fuse_export(void *dt)
{
    t8122_fuse_valid = false;
    t8122_fuse_use = g15_optin(dt, T8122_LEAK_FUSE_OPTIN);
    if (chip_id != T8122 || (fdt_node_check_compatible(dt, 0, "apple,j613") &&
                             fdt_node_check_compatible(dt, 0, "apple,j615")))
        return;

    int before = g15_guard_begin();
    u32 word0 = read32(T8122_LEAK_FUSE_WORD0);
    u32 word1 = read32(T8122_LEAK_FUSE_WORD1);
    if (g15_guard_end(before)) {
        printf("FDT: GPU: T8122 leakage fuse: the read faulted; nothing exported\n");
        return;
    }

    t8122_leak_fuse_decode(word0, word1, &t8122_fuse);
    t8122_fuse_valid = true;
    printf("FDT: GPU: T8122 leakage fuse: field A 0x%04x, field B 0x%03x; values %u and %u\n",
           t8122_fuse.field_a, t8122_fuse.field_b, (u32)t8122_fuse.value[0],
           (u32)t8122_fuse.value[1]);

    u32 cells[2];
    memcpy(&cells[0], &t8122_fuse.value[0], sizeof(cells[0]));
    memcpy(&cells[1], &t8122_fuse.value[1], sizeof(cells[1]));
    fdt32_t prop[2] = {cpu_to_fdt32(cells[0]), cpu_to_fdt32(cells[1])};
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_setprop(dt, chosen, T8122_LEAK_FUSE_PROP, prop, sizeof(prop)))
        printf("FDT: GPU: T8122 leakage fuse: could not add /chosen/%s\n", T8122_LEAK_FUSE_PROP);
    else
        printf("FDT: GPU: T8122 leakage fuse values in /chosen/%s; %s\n", T8122_LEAK_FUSE_PROP,
               t8122_fuse_use ? "the GPU setup uses the first (chosen." T8122_LEAK_FUSE_OPTIN "=1)"
                              : "the GPU setup does not use them");
}

/*
 * T8122: the GPU setup runs only with chosen.asahi,t8122-gpu=1; there is no dry run on a default
 * boot. Without it, the reservation-only diagnostic (dt_set_gpu_t8122()) runs if it is opted in.
 * With chosen.asahi,t8122-gpu-power-standin=1 as well, the setup is the stand-in experiment
 * (g15_soc_t8122_standin); on its own that line is only logged as ignored.
 */
static int dt_set_gpu_t8122_select(void *dt)
{
    t8122_leak_fuse_export(dt);

    bool standin = g15_optin(dt, "asahi,t8122-gpu-power-standin");

    if (!g15_optin(dt, g15_soc_t8122.optin)) {
        if (standin)
            printf("FDT: GPU: asahi,t8122-gpu-power-standin ignored: it needs "
                   "chosen.asahi,t8122-gpu=1\n");
        return dt_set_gpu_t8122(dt);
    }

    if (g15_optin(dt, "asahi,t8122-gpu-handoff-diag"))
        printf("FDT: GPU: asahi,t8122-gpu-handoff-diag ignored: chosen.asahi,t8122-gpu=1 runs "
               "the GPU setup\n");
    if (standin) {
        printf("FDT: GPU: T8122 experiment armed (chosen.asahi,t8122-gpu-power-standin=1): "
               "experimental G15G firmware image, stand-in power model\n");
        return dt_set_gpu_g15(dt, &g15_soc_t8122_standin);
    }
    return dt_set_gpu_g15(dt, &g15_soc_t8122);
}

/* A separate, exact opt-in reads only powered SGX identity registers. */

static int t8122_identity_power_enable(void *context)
{
    UNUSED(context);
    return pmgr_adt_power_enable("/arm-io/sgx");
}

static int t8122_identity_read(void *context, u32 *id, u32 masks[3], u32 *counts, u32 *counts2)
{
    u64 sgx_base = *(u64 *)context;
    int before = g15_guard_begin();
    *id = read32(sgx_base + G15_SGX_ID_VERSION);
    /* Core topology, as drm/asahi reads it; published, not checked */
    *counts = read32(sgx_base + G15_SGX_ID_COUNTS_1);
    *counts2 = read32(sgx_base + G15_SGX_ID_COUNTS_2);
    masks[0] = read32(sgx_base + G15_SGX_CORE_MASK);
    masks[1] = read32(sgx_base + G15_SGX_CORE_MASK + 4);
    masks[2] = read32(sgx_base + G15_SGX_CORE_MASK + 8);
    return g15_guard_end(before);
}

static int t8122_identity_power_disable(void *context)
{
    UNUSED(context);
    if (pmgr_adt_power_disable_recursive("/arm-io/sgx"))
        return -1;
    return pmgr_adt_power_is_off("/arm-io/sgx") == 1 ? 0 : -1;
}

/*
 * T6031: the bounded powered identity read, on a J514C or J516C booted with
 * chosen.asahi,t6031-gpu=1. Unlike the T8122 probe it cannot require the GPU power closure to be
 * off (GFX's parent AFR is always on), so it records every power state register of the closure,
 * powers the GPU on through PMGR (each step a bounded poll), checks that every device of the
 * closure is on before any GPU register is read, reads the ID, topology and core mask registers
 * under the exception guard, and then puts each register back as it was, children first, and
 * checks that it is. Every result, a failure included, is published with the values read (none
 * after a faulted read) in /chosen/asahi,t6031-gpu-powered-identity.
 */
#define T6031_SGX_BASE 0x408000000UL

struct t6031_probe {
    u64 sgx_base;
    struct t6031_closure closure;
};

static int t6031_probe_enable(void *ctx)
{
    UNUSED(ctx);
    return pmgr_adt_power_enable("/arm-io/sgx");
}

static int t6031_probe_active(void *ctx)
{
    return t6031_closure_active(&((struct t6031_probe *)ctx)->closure);
}

static int t6031_probe_read(void *ctx, u32 *id, u32 masks[3], u32 *counts, u32 *counts2)
{
    return t8122_identity_read(&((struct t6031_probe *)ctx)->sgx_base, id, masks, counts, counts2);
}

static int t6031_probe_restore(void *ctx)
{
    return t6031_closure_restore(&((struct t6031_probe *)ctx)->closure);
}

static void t6031_log_closure(const char *when, const struct t6031_closure *c, bool after)
{
    printf("FDT: GPU: T6031 power %s:", when);
    for (int i = 0; i < c->n; i++)
        printf(" %.16s 0x%lx=0x%x", c->ps[i].name ? c->ps[i].name : "?", c->ps[i].addr,
               after ? c->ps[i].after : c->ps[i].before);
    printf("\n");
}

static void t6031_gpu_probe(void *dt)
{
    static const struct t6031_probe_ops ops = {
        .enable = t6031_probe_enable,
        .active = t6031_probe_active,
        .read = t6031_probe_read,
        .restore = t6031_probe_restore,
    };
    struct t6031_probe p;
    int sgx_path[8];
    u64 sgx_size;

    g15_t6031_id = (struct t8122_powered_identity){.result = T8122_ID_NOT_RUN};
    if (adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path) < 0 ||
        adt_get_reg(adt, sgx_path, "reg", 0, &p.sgx_base, &sgx_size) < 0 ||
        p.sgx_base != T6031_SGX_BASE || sgx_size < G15_SGX_CORE_MASK + 12) {
        printf("FDT: GPU: T6031 identity not read: the ADT sgx window is not 0x%lx\n",
               T6031_SGX_BASE);
        return;
    }
    if (t6031_closure_get("/arm-io/sgx", &p.closure)) {
        printf("FDT: GPU: T6031 identity not read: the sgx power closure does not resolve\n");
        return;
    }
    /* Every result property is reserved before anything is powered */
    if (t6031_identity_node(dt, p.closure.n) < 0) {
        printf("FDT: GPU: T6031 identity not read: no room for /chosen/" T6031_GPU_IDENTITY "\n");
        return;
    }
    t6031_log_closure("before", &p.closure, false);
    g15_t6031_id = t6031_probe_run(&ops, &p);
    t6031_log_closure("after", &p.closure, true);
    if (g15_t6031_id.result == T8122_ID_POWER_OFF_FAILED)
        printf("FDT: GPU: T6031 WARNING: the power closure is not as iBoot left it\n");
    if (t6031_identity_publish(dt, &g15_t6031_id, &p.closure))
        printf("FDT: GPU: T6031 could not publish the powered identity\n");
    printf("FDT: GPU: T6031 powered identity result %u: ID_VERSION 0x%08x ID_COUNTS_1 0x%08x "
           "ID_COUNTS_2 0x%08x core masks 0x%x 0x%x 0x%x\n",
           g15_t6031_id.result, g15_t6031_id.id_version, g15_t6031_id.id_counts_1,
           g15_t6031_id.id_counts_2, g15_t6031_id.core_mask[0], g15_t6031_id.core_mask[1],
           g15_t6031_id.core_mask[2]);
}

/*
 * T6031 (J514C/J516C): the firmware image facts, the bounded powered identity read, then the GPU
 * setup (which itself touches no GPU hardware). Each part is fail-soft; all three
 * require chosen.asahi,t6031-gpu=1 because this profile uses unmeasured power stand-ins. On a
 * J516C dt_set_m3_defaults() sets it unless a config line set it (=0 keeps all three off); a
 * J514C still needs the line.
 */
static int dt_set_gpu_t6031(void *dt)
{
    g15_t6031_fw = NULL;
    g15_t6031_id = (struct t8122_powered_identity){.result = T8122_ID_NOT_RUN};
    if (!t6031_board(dt)) {
        printf("FDT: GPU: T6031: not a J514C/J516C device tree, nothing done\n");
        return 0;
    }
    if (!g15_optin(dt, g15_soc_t6031.optin)) {
        g15_disable(dt);
        printf("FDT: GPU: T6031: requires chosen.%s=1: no identity read, no GPU handoff\n",
               g15_soc_t6031.optin);
        return 0;
    }
    g15_t6031_fw = dt_t6031_gpu_image(dt);
    t6031_gpu_probe(dt);
    return dt_set_gpu_g15(dt, &g15_soc_t6031);
}

/*
 * Called only after every UAT and firmware reservation passes the existing
 * T8122 handoff checks. Power is voted on solely for bounded ID/core-mask
 * reads, then voted off before Linux starts. Neither GPU nor mailbox is enabled.
 */
static void t8122_read_powered_identity(void *dt, int gpu)
{
    if (!t8122_power_probe_opted_in(dt))
        return;

    int sgx_path[8];
    u64 sgx_base, sgx_size, dt_sgx;
    if (adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path) < 0 ||
        adt_get_reg(adt, sgx_path, "reg", 0, &sgx_base, &sgx_size) < 0 ||
        g15_dt_reg_by_name(dt, gpu, "sgx", &dt_sgx) || !sgx_base || sgx_base != dt_sgx ||
        sgx_size < G15_SGX_CORE_MASK + 12) {
        printf("FDT: GPU: T8122 powered identity preflight rejected SGX window\n");
        return;
    }

    /* The SGX gates are virtual on T8122; their real GFX parent must be off
     * at entry so recursive cleanup can restore that exact state.
     */
    if (pmgr_adt_power_is_off("/arm-io/sgx") != 1) {
        printf("FDT: GPU: T8122 SGX power closure was not off; skipping probe\n");
        return;
    }

    /* Reserve all result properties before powering hardware. */
    int node = t8122_identity_node(dt);
    if (node < 0) {
        printf("FDT: GPU: T8122 powered identity result node unavailable\n");
        return;
    }

    static const struct t8122_power_probe_ops ops = {
        .enable = t8122_identity_power_enable,
        .read = t8122_identity_read,
        .disable = t8122_identity_power_disable,
    };
    struct t8122_powered_identity identity = t8122_power_probe_run(&ops, &sgx_base);
    if (identity.result == T8122_ID_POWER_OFF_FAILED)
        printf("FDT: GPU: T8122 failed to release SGX power vote\n");

    if (t8122_identity_set_u32(dt, node, "id-version", identity.id_version) ||
        t8122_identity_set_u32(dt, node, "core-mask-0", identity.core_mask[0]) ||
        t8122_identity_set_u32(dt, node, "core-mask-1", identity.core_mask[1]) ||
        t8122_identity_set_u32(dt, node, "core-mask-2", identity.core_mask[2]) ||
        t8122_identity_set_u32(dt, node, "id-counts-1", identity.id_counts_1) ||
        t8122_identity_set_u32(dt, node, "id-counts-2", identity.id_counts_2) ||
        t8122_identity_set_u32(dt, node, "probe-result", identity.result))
        printf("FDT: GPU: T8122 could not publish powered identity result\n");
    printf("FDT: GPU: T8122 bounded powered identity result %u; GPU and ASC remain disabled\n",
           identity.result);
    printf("FDT: GPU: T8122 powered identity: ID_VERSION 0x%08x ID_COUNTS_1 0x%08x ID_COUNTS_2 "
           "0x%08x core masks 0x%x 0x%x 0x%x\n",
           identity.id_version, identity.id_counts_1, identity.id_counts_2, identity.core_mask[0],
           identity.core_mask[1], identity.core_mask[2]);
}

static bool t8122_identity_matches_j613(void *dt)
{
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    static const struct {
        const char *name;
        u32 value;
    } expected[] = {
        {"schema-version", 1},       {"probe-result", T8122_ID_PASS},
        {"id-version", 0x07022000},  {"id-counts-1", 0x0011010a},
        {"id-counts-2", 0x00040404}, {"core-mask-0", 0x37e},
        {"core-mask-1", 0},          {"core-mask-2", 0},
    };
    if (node < 0)
        return false;
    for (u32 i = 0; i < ARRAY_SIZE(expected); i++) {
        int len = 0;
        const fdt32_t *raw = fdt_getprop(dt, node, expected[i].name, &len);
        if (!raw || len != sizeof(*raw) || fdt32_ld(raw) != expected[i].value)
            return false;
    }
    return true;
}

/* Publish an ABI claim only for this boot's exact owner-approved image and
 * measured J613 identity. GPU and mailbox status remain disabled.
 */
static void t8122_admit_image_metadata(void *dt, const struct gfx_fw_image_info *image)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int gpu = fdt_path_offset(dt, "/soc/gpu@292400000");
    if (chosen < 0 || gpu < 0 || fdt_node_check_compatible(dt, 0, "apple,j613") ||
        !t8122_identity_matches_j613(dt))
        return;

    int len = 0;
    const char *ack = fdt_getprop(dt, chosen, "asahi,t8122-gpu-abi-ack", &len);
    if (!ack || len != sizeof("14.8.3") || memcmp(ack, "14.8.3", sizeof("14.8.3")))
        return;
    const char *uuid_text = fdt_getprop(dt, chosen, "asahi,t8122-gpu-expected-uuid", &len);
    u8 expected_uuid[16];
    if (!gfx_fw_parse_uuid_hex(uuid_text, len, expected_uuid) ||
        memcmp(expected_uuid, image->uuid, sizeof(expected_uuid)))
        return;
    const char *expected_rtkit = fdt_getprop(dt, chosen, "asahi,t8122-gpu-expected-rtkit", &len);
    char observed_rtkit[GFX_FW_VERSION_MAX];
    if (!expected_rtkit || len < 8 || len > GFX_FW_VERSION_MAX || expected_rtkit[len - 1] != '\0' ||
        strlen(expected_rtkit) + 1 != (size_t)len || !firmware_gfx_rtkit_version(observed_rtkit) ||
        strcmp(expected_rtkit, observed_rtkit))
        return;

    const fdt32_t compat[] = {cpu_to_fdt32(14), cpu_to_fdt32(8), cpu_to_fdt32(3)};
    if (fdt_setprop(dt, gpu, "apple,firmware-compat", compat, sizeof(compat))) {
        printf("FDT: GPU: T8122 exact image metadata matched but ABI property could not be set\n");
        return;
    }
    printf("FDT: GPU: T8122 J613 exact image metadata admitted; GPU and ASC remain disabled\n");
}

/* Separate one-boot ASC register preflight. Publish both status changes only
 * after all J613 identity, image, reservation and opt-in checks have passed.
 * Linux's diagnostic driver still returns ENODEV without RTKit or DRM setup.
 */
static void t8122_enable_asc_preflight(void *dt)
{
    /* When the full GPU setup is opted in (chosen.asahi,t8122-gpu=1) it owns the
     * gpu and mailbox enable; the lighter preflight must not also touch them.
     * The reservation-only diagnostic that hosts this preflight does not run
     * under that opt-in, so this is defence in depth, logged when it fires.
     */
    if (g15_optin(dt, "asahi,t8122-gpu")) {
        printf("FDT: GPU: T8122 ASC preflight skipped: chosen.asahi,t8122-gpu=1 owns the "
               "node enable\n");
        return;
    }

    int chosen = fdt_path_offset(dt, "/chosen");
    int gpu = fdt_path_offset(dt, "/soc/gpu@292400000");
    int mailbox = fdt_path_offset(dt, "/soc/mbox@292408000");
    if (chosen < 0 || gpu < 0 || mailbox < 0 || !t8122_identity_matches_j613(dt))
        return;
    int len = 0;
    const char *optin = fdt_getprop(dt, chosen, "asahi,t8122-gpu-asc-preflight", &len);
    if (!optin || len != sizeof("1") || memcmp(optin, "1", sizeof("1")))
        return;
    const fdt32_t *compat = fdt_getprop(dt, gpu, "apple,firmware-compat", &len);
    if (!compat || len != 3 * sizeof(*compat) || fdt32_ld(&compat[0]) != 14 ||
        fdt32_ld(&compat[1]) != 8 || fdt32_ld(&compat[2]) != 3)
        return;

    size_t size = fdt_totalsize(dt);
    void *copy = malloc(size);
    if (!copy)
        return;
    memcpy(copy, dt, size);
    if (!g15_set_status(copy, "/soc/mbox@292408000", "okay") &&
        !g15_set_status(copy, "/soc/gpu@292400000", "okay")) {
        memcpy(dt, copy, size);
        printf("FDT: GPU: T8122 J613 one-boot ASC register preflight enabled\n");
    } else {
        printf("FDT: GPU: T8122 ASC preflight status change failed; GPU and ASC stay disabled\n");
    }
    free(copy);
}

/*
 * Export the four T8122 UAT carveouts from this boot's ADT. Their physical
 * addresses depend on the installed RAM size: a J615's fixed addresses are
 * not valid on this J613. Normal diagnostics do not power or enable the GPU.
 */
static int dt_set_gpu_t8122(void *dt)
{
    static const char *const gpu_path = "/soc/gpu@292400000";
    int gpu = fdt_path_offset(dt, gpu_path);
    if (gpu < 0 || fdt_node_check_compatible(dt, gpu, "apple,agx-t8122"))
        return 0;

    const char *gpu_status = fdt_getprop(dt, gpu, "status", NULL);
    int mailbox = fdt_path_offset(dt, "/soc/mbox@292408000");
    const char *mailbox_status = mailbox >= 0 ? fdt_getprop(dt, mailbox, "status", NULL) : NULL;
    if (!gpu_status || strcmp(gpu_status, "disabled") || !mailbox_status ||
        strcmp(mailbox_status, "disabled")) {
        printf("FDT: GPU: T8122 diagnostic requires disabled GPU and mailbox nodes\n");
        return 0;
    }

    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *optin =
        chosen >= 0 ? fdt_getprop(dt, chosen, "asahi,t8122-gpu-handoff-diag", &len) : NULL;
    if (!optin || len != 2 || strcmp(optin, "1"))
        return 0;

    /* Never carry a compatibility claim from an input DT into a diagnostic boot.
     * Done only once the diagnostic is opted in, so a default boot is untouched. */
    int compat_err = fdt_delprop(dt, gpu, "apple,firmware-compat");
    if (compat_err && compat_err != -FDT_ERR_NOTFOUND) {
        printf("FDT: GPU: T8122 could not clear input firmware compatibility claim\n");
        return 0;
    }

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        return 0;

    struct g15_region regions[G15_NUM_REGIONS];
    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        regions[i] = g15_region_names[i];
        if (g15_get_region(dt, sgx, &regions[i]))
            return 0;
        int node = fdt_path_offset(dt, regions[i].dt_path);
        const char *status = node >= 0 ? fdt_getprop(dt, node, "status", NULL) : NULL;
        if (!status || strcmp(status, "disabled")) {
            printf("FDT: GPU: T8122 expected a disabled %s\n", regions[i].dt_path);
            return 0;
        }
    }
    if (g15_check_regions(dt, regions))
        return 0;

    struct g15_fw_seg fw_segs[G15_FW_SEGS];
    if (g15_get_fw_segs(dt, gpu, regions, fw_segs))
        return 0;
    struct gfx_fw_image_info image;
    if (!firmware_gfx_image_info(&image) || image.text_size != fw_segs[0].size) {
        printf("FDT: GPU: T8122 firmware header and ADT text extent disagree\n");
        return 0;
    }

    /* Fixed-size reg writes cannot change the DT structure. */
    for (int i = 0; i < G15_NUM_REGIONS; i++) {
        fdt64_t reg[2];
        fdt64_st(&reg[0], regions[i].base);
        fdt64_st(&reg[1], regions[i].size);
        int node = fdt_path_offset(dt, regions[i].dt_path);
        if (node < 0 || fdt_setprop_inplace(dt, node, "reg", reg, sizeof(reg))) {
            printf("FDT: GPU: T8122 cannot fill %s; all UAT nodes remain disabled\n",
                   regions[i].dt_path);
            return 0;
        }
    }

    /* Reuse the guarded, rollback-capable G15 segment description. */
    memset(&g15_fw_undo, 0, sizeof(g15_fw_undo));
    if (g15_set_fw_segs(dt, gpu_path, fw_segs))
        return 0;

    int enabled = 0;
    for (; enabled < G15_NUM_REGIONS; enabled++)
        if (g15_set_status(dt, regions[enabled].dt_path, "okay"))
            break;
    if (enabled != G15_NUM_REGIONS) {
        for (int i = 0; i < enabled; i++)
            if (g15_set_status(dt, regions[i].dt_path, "disabled"))
                printf("FDT: GPU: T8122 failed to restore %s disabled\n", regions[i].dt_path);
        if (g15_unset_fw_segs(dt, gpu_path))
            printf("FDT: GPU: T8122 failed to remove firmware segment description\n");
        return 0;
    }

    printf("FDT: GPU: T8122 UAT and firmware reservations supplied from ADT; "
           "GPU and ASC remain disabled\n");
    t8122_read_powered_identity(dt, gpu);
    t8122_admit_image_metadata(dt, &image);
    t8122_enable_asc_preflight(dt);
    return 0;
}

/* G17 SoCs whose firmware iBoot loads and whose UAT it sets up for a raw boot object */
struct g17_soc {
    const char *compat;           /* Linux compatible */
    const char *adt_compat;       /* the ADT's own compatible, also accepted */
    const char *name;             /* for the log */
    u32 count;                    /* perf-state-count, also the number of OPP entries */
    u32 num_states;               /* gpu-num-perf-states */
    bool firmware_data;           /* also hand over gfx-data/gfx1-data (the firmware __DATA) */
    bool (*firmware_known)(void); /* when true, /chosen gets firmware_profile_prop */
    const char *firmware_profile_prop;
    const char *firmware_profile;
};

static const struct g17_soc g17_soc_t8140 = {
    .compat = "apple,agx-t8140",
    .adt_compat = "gpu,t8140",
    .name = "T8140",
    .count = 16,
    .num_states = 15,
};

/*
 * The M5 GPU firmware build Linux's M5 runtime is written for. iBoot fills a
 * TLV block in each __TEXT with per-boot values (GKTS, ECAP, and addresses
 * such as BPTP = gfx-shared-region), so the image is matched by its build
 * string, its __TEXT size and the TLV layout, not by a hash.
 */
#define T8142_GPU_FIRMWARE_BUILD   "RTKit-3255.160.4.release"
#define T8142_GPU_FIRMWARE_PROFILE "M5-" T8142_GPU_FIRMWARE_BUILD

static bool t8142_gpu_firmware_role_known(const char *path, u32 text_size, const u32 tlv[2])
{
    int node = adt_path_offset(adt, path);
    if (node < 0)
        return false;

    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len != 2 * sizeof(*seg) || seg[0].size != text_size || !seg[0].phys)
        return false;
    const char *names = adt_getprop(adt, node, "segment-names", &len);
    if (!names || len < sizeof("__TEXT;__DATA") - 1 ||
        memcmp(names, "__TEXT;__DATA", sizeof("__TEXT;__DATA") - 1))
        return false;

    const u8 *text = (const u8 *)seg[0].phys;
    static const char tags[2][4] = {"GKTS", "ECAP"};
    enum exc_guard_t guard = exc_guard;
    int faults = exc_count;
    bool ok = true;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    for (int i = 0; ok && i < 2; i++)
        ok = !memcmp(text + tlv[i], tags[i], 4) && read32((u64)text + tlv[i] + 4) == 8;
    bool found = false;
    size_t build_len = sizeof(T8142_GPU_FIRMWARE_BUILD) - 1;
    for (u32 off = 0; ok && !found && off + build_len <= text_size; off++)
        found = text[off] == 'R' && !memcmp(text + off, T8142_GPU_FIRMWARE_BUILD, build_len);
    exc_guard = guard;

    return ok && found && exc_count == faults;
}

static bool t8142_gpu_firmware_known(void)
{
    static const u32 gfx_tlv[2] = {0x45d09, 0x45dec};
    static const u32 gfx1_tlv[2] = {0x45841, 0x45924};

    return t8142_gpu_firmware_role_known("/arm-io/gfx-asc", 0x4c000, gfx_tlv) &&
           t8142_gpu_firmware_role_known("/arm-io/gfx1-asc", 0x48000, gfx1_tlv);
}

static const struct g17_soc g17_soc_t8142 = {
    .compat = "apple,agx-t8142",
    .adt_compat = "gpu,t8142",
    .name = "T8142",
    .count = 13,
    .num_states = 12,
    .firmware_data = true,
    .firmware_known = t8142_gpu_firmware_known,
    .firmware_profile_prop = "aurora,m5-gpu-firmware-profile",
    .firmware_profile = T8142_GPU_FIRMWARE_PROFILE,
};

static int dt_set_gpu_g17(void *dt, const struct g17_soc *soc)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        return 0;
    if (fdt_node_check_compatible(dt, gpu, soc->compat) &&
        fdt_node_check_compatible(dt, gpu, soc->adt_compat))
        return 0;

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    u32 count, num_states, tables, len;
    const struct perf_state *states, *sram;
    if (sgx < 0 || ADT_GETPROP(adt, sgx, "perf-state-count", &count) != sizeof(count) ||
        ADT_GETPROP(adt, sgx, "gpu-num-perf-states", &num_states) != sizeof(num_states) ||
        ADT_GETPROP(adt, sgx, "perf-state-table-count", &tables) != sizeof(tables) ||
        count != soc->count || num_states != soc->num_states || tables != 1)
        goto disable;
    states = adt_getprop(adt, sgx, "perf-states", &len);
    if (!states || len != count * sizeof(*states))
        goto disable;
    sram = adt_getprop(adt, sgx, "perf-states-sram", &len);
    if (!sram || len != count * sizeof(*sram))
        goto disable;
    for (u32 i = 0; i < count; i++)
        if (states[i].volt > UINT32_MAX / 1000)
            goto disable;

    int ph_len;
    const fdt32_t *opp_ph = fdt_getprop(dt, gpu, "operating-points-v2", &ph_len);
    if (!opp_ph || ph_len != sizeof(*opp_ph))
        goto disable;
    int table = fdt_node_offset_by_phandle(dt, fdt32_ld(opp_ph));
    if (table < 0)
        goto disable;
    int opp, opp_count = 0;
    fdt_for_each_subnode(opp, dt, table)
    {
        if ((u32)++opp_count > soc->count || !fdt_getprop(dt, opp, "opp-hz", &ph_len) ||
            ph_len != sizeof(u64) || !fdt_getprop(dt, opp, "opp-microvolt", &ph_len) ||
            ph_len != sizeof(u32))
            goto disable;
    }
    if ((u32)opp_count != soc->count)
        goto disable;

    static const struct {
        const char *name;
        const char *path;
        bool firmware_data;
    } regions[] = {
        {"gfx-handoff", "/reserved-memory/uat-handoff"},
        {"gfx-shared-l2-region", "/reserved-memory/uat-l2-pagetables"},
        {"gfx-shared-region", "/reserved-memory/uat-pagetables"},
        {"gpu-region", "/reserved-memory/uat-ttbs"},
        {"gfx-data", "/reserved-memory/gfx-data", true},
        {"gfx1-data", "/reserved-memory/gfx1-data", true},
    };
    size_t nregions = ARRAY_SIZE(regions) - (soc->firmware_data ? 0 : 2);
    u64 base[ARRAY_SIZE(regions)], size[ARRAY_SIZE(regions)];
    for (size_t i = 0; i < nregions; i++) {
        char prop[64];
        snprintf(prop, sizeof(prop), "%s-base", regions[i].name);
        if (ADT_GETPROP(adt, sgx, prop, &base[i]) != sizeof(u64))
            goto disable;
        snprintf(prop, sizeof(prop), "%s-size", regions[i].name);
        if (ADT_GETPROP(adt, sgx, prop, &size[i]) != sizeof(u64) || !base[i] || !size[i] ||
            (base[i] & (SZ_16K - 1)) || (size[i] & (SZ_16K - 1)) ||
            base[i] > UINT64_MAX - size[i] || fdt_path_offset(dt, regions[i].path) < 0)
            goto disable;
        for (size_t j = 0; j < i; j++)
            if (base[i] < base[j] + size[j] && base[j] < base[i] + size[i])
                goto disable;
    }

    opp_count = 0;
    fdt_for_each_subnode(opp, dt, table)
    {
        if (fdt_setprop_inplace_u64(dt, opp, "opp-hz", states[opp_count].freq) ||
            fdt_setprop_inplace_u32(dt, opp, "opp-microvolt", states[opp_count].volt * 1000))
            goto disable;
        opp_count++;
    }
    for (size_t i = 0; i < nregions; i++)
        if (dt_set_resvmem(dt, regions[i].path, base[i], size[i]))
            goto disable;

    if (soc->firmware_known) {
        int chosen = fdt_path_offset(dt, "/chosen");
        if (!soc->firmware_known())
            printf("FDT: GPU: unknown %s firmware build, Linux will not start it\n", soc->name);
        else if (chosen < 0 ||
                 fdt_setprop_string(dt, chosen, soc->firmware_profile_prop, soc->firmware_profile))
            goto disable;
    }
    return 0;

disable:
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu >= 0 && fdt_setprop_string(dt, gpu, "status", "disabled"))
        return -1;
    printf("FDT: GPU: invalid %s ADT or DT data, disabling GPU\n", soc->name);
    return 0;
}

/*
 * T6040 (M4 Pro).
 *
 * Only part of the GPU's per-unit data is in the ADT. The operating points (one voltage per
 * cluster), the CS and AFR tables and the UAT/firmware regions are; the leakage coefficients
 * come from an efuse page, the core mask and the two performance-state maps (A and B) from
 * SGX registers, and the maximum power and the relative tables are computed from all of it.
 *
 * The fill runs only when the device tree enables the GPU, so a tree that keeps the GPU off
 * leaves the hardware untouched. Anything unexpected disables the GPU node again.
 */
#define T6040_OPPS       16
#define T6040_CLUSTERS   2
#define T6040_CORES      10
#define T6040_MAP_STATES 11
#define T6040_EFUSE      0x508374000UL /* efuse page; the leakage fields are at 0xc10..0xc2b */

/* Rounding barrier: keeps the compiler from fusing a multiply into a following add. */
#define T6040_ROUND(x) __asm__("" : "+w"(x))

static float t6040_f32(u32 w)
{
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static double t6040_f64(u64 w)
{
    double d;
    memcpy(&d, &w, sizeof(d));
    return d;
}

/*
 * Numeric model provenance: Jonathan Goyvaerts' public T6040 GPU work,
 * https://github.com/aurora-silicon/linux/pull/408, based on Gravity Linux
 * work by Cody Ho and Niklas Sheth. This C implementation reproduces those
 * numerical tables with the running chip's calibration.
 *
 * The power model below, with its constants and the efuse fields read in dt_set_gpu_t6040(),
 * reproduces the J616s device tree's maximum power (69116 mW), its power ladder and both
 * relative tables exactly.
 *
 * Binary32 words (A, B, C) of the GPU cluster power model, by voltage band (< 701, < 901, more).
 */
static const u32 t6040_gpu_coef[3][3] = {
    {0x3c44f3b6, 0x3aa07f54, 0x406dcd8c},
    {0x3c367d9a, 0x3abd2472, 0x40671cc1},
    {0x3b9b602e, 0x3b46b49e, 0x4037a220},
};

/* Binary32 words (A, B, T) of the CS power model, by the same voltage bands. */
static const u32 t6040_cs_coef[3][3] = {
    {0x3c21eaa8, 0x3a97a9b9, 0x3d121894},
    {0x3c116cb8, 0x3abe814d, 0x3d0d151a},
    {0x3b4d1eb1, 0x3b66dc36, 0x3ccd6ba8},
};

#define T6040_W_BASE     0x402df854 /* static exponent base */
#define T6040_W_GPU_DYN  0x41769ba6
#define T6040_W_EXPONENT 0x3f99999a
#define T6040_W_CAP_MA   0x472be000 /* 44000 mA */
#define T6040_W_110      0x42dc0000
#define T6040_D_CS_DYN   0x401bc7ae20000000ULL
#define T6040_D_EXPONENT 0x3ff3333340000000ULL

/* ln(x) for 0.25 < x < 4, by the atanh series. */
static double t6040_log(double x)
{
    double t = (x - 1.0) / (x + 1.0), t2 = t * t, term = t, sum = 0.0;

    for (int k = 1; k < 200; k += 2) {
        double add = term / k;
        sum += add;
        if (__builtin_fabs(add) <= __builtin_fabs(sum) * 1e-20)
            break;
        term *= t2;
    }
    return 2.0 * sum;
}

/* exp(y) for |y| < 40: y = k ln2 + r, Taylor series for exp(r), then scale by 2^k. */
static double t6040_exp(double y)
{
    const double ln2 = 0.69314718055994530942;
    int k = (int)(y / ln2 + (y >= 0 ? 0.5 : -0.5));
    double r = y - k * ln2, term = 1.0, sum = 1.0;

    for (int n = 1; n < 40; n++) {
        term *= r / n;
        sum += term;
        if (__builtin_fabs(term) <= 1e-20 * sum)
            break;
    }
    for (; k > 0; k--)
        sum *= 2.0;
    for (; k < 0; k++)
        sum *= 0.5;
    return sum;
}

static int t6040_band(s32 mv)
{
    return mv < 701 ? 0 : (mv < 901 ? 1 : 2);
}

/* Volts as binary32 and the integer mV that the piecewise tests and the exponent use. */
static float t6040_volts(u32 mv, s32 *m)
{
    float v = (float)mv / 1000.0f;
    T6040_ROUND(v);
    float k = v * 1000.0f;
    T6040_ROUND(k);
    *m = (s32)k;
    return v;
}

static u32 t6040_cluster_mw(u32 mv, u32 f_mhz, float leak, float active)
{
    s32 m;
    float v = t6040_volts(mv, &m);
    const u32 *k = t6040_gpu_coef[t6040_band(m)];

    float la = leak * t6040_f32(k[0]);
    T6040_ROUND(la);
    float bm = t6040_f32(k[1]) * (float)m;
    T6040_ROUND(bm);
    float x = bm + t6040_f32(k[2]);
    T6040_ROUND(x);
    float i_static = la * powf(t6040_f32(T6040_W_BASE), x);
    T6040_ROUND(i_static);

    float d = powf(v, t6040_f32(T6040_W_EXPONENT)) * t6040_f32(T6040_W_GPU_DYN);
    T6040_ROUND(d);
    d = d * (float)f_mhz;
    T6040_ROUND(d);
    d = d * active;
    T6040_ROUND(d);

    float i = i_static + d;
    T6040_ROUND(i);
    if (i > t6040_f32(T6040_W_CAP_MA))
        i = t6040_f32(T6040_W_CAP_MA);
    float p = v * i;
    T6040_ROUND(p);
    return (u32)p;
}

static u32 t6040_cs_mw(u32 mv, u32 f_mhz, float leak)
{
    s32 m;
    float v = t6040_volts(mv, &m);
    const u32 *k = t6040_cs_coef[t6040_band(m)];

    float la = leak * t6040_f32(k[0]);
    T6040_ROUND(la);
    float bm = t6040_f32(k[1]) * (float)m;
    T6040_ROUND(bm);
    float x = __builtin_fmaf(t6040_f32(k[2]), t6040_f32(T6040_W_110), bm);
    float i_static = la * powf(t6040_f32(T6040_W_BASE), x);
    T6040_ROUND(i_static);

    double d = t6040_exp(t6040_f64(T6040_D_EXPONENT) * t6040_log((double)v));
    d = d * t6040_f64(T6040_D_CS_DYN);
    T6040_ROUND(d);
    d = d * (double)f_mhz;
    T6040_ROUND(d);
    float i_dynamic = (float)d;

    float i = i_static + i_dynamic;
    T6040_ROUND(i);
    if (i > t6040_f32(T6040_W_CAP_MA))
        i = t6040_f32(T6040_W_CAP_MA);
    float p = v * i;
    T6040_ROUND(p);
    return (u32)p;
}

static int t6040_read32(u64 addr, u32 *val)
{
    int before = exc_count;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    *val = read32(addr);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before ? 0 : -1;
}

/*
 * Performance-state map: two 4-bit operating-point indices per 32-bit word. Logical state 0 is
 * off and selects operating point 0; every later state selects a strictly higher operating point,
 * so no map entry past state 0 can reach the off state.
 */
static int t6040_perf_map(u64 base, u32 *index)
{
    for (u32 i = 0; i < 16; i += 2) {
        u32 w;
        if (t6040_read32(base + 2 * i, &w))
            return -1;
        index[i] = w & 0xf;
        index[i + 1] = (w >> 16) & 0xf;
    }
    /* The supported firmware map has 11 entries; refuse nonzero unused fields. */
    u32 n = T6040_OPPS;
    while (n > 1 && !index[n - 1])
        n--;
    if (n != T6040_MAP_STATES)
        return -1;
    if (index[0] != 0)
        return -1;
    for (u32 i = 1; i < n; i++)
        if (index[i] <= index[i - 1])
            return -1;
    if (index[n - 1] != T6040_OPPS - 1)
        return -1;
    for (u32 i = n; i < T6040_OPPS; i++)
        if (index[i])
            return -1;
    return n;
}

static int t6040_set_u32s(void *dt, int node, const char *name, const u32 *val, u32 count)
{
    fdt32_t data[16];

    if (count > ARRAY_SIZE(data))
        return -1;
    for (u32 i = 0; i < count; i++)
        data[i] = cpu_to_fdt32(val[i]);
    return fdt_setprop(dt, node, name, data, count * sizeof(u32));
}

static int t6040_set_floats(void *dt, int node, const char *name, const float *val, u32 count)
{
    u32 words[16];

    if (count > ARRAY_SIZE(words))
        return -1;
    memcpy(words, val, count * sizeof(float));
    return t6040_set_u32s(dt, node, name, words, count);
}

/* An OPP table that the in-place writes below can fill: `count` entries of the expected sizes. */
static int t6040_opp_table(void *dt, int gpu, const char *prop, u32 count, int volt_len)
{
    int len;
    const fdt32_t *ph = fdt_getprop(dt, gpu, prop, &len);
    if (!ph || len != sizeof(*ph))
        return -1;
    int table = fdt_node_offset_by_phandle(dt, fdt32_ld(ph));
    if (table < 0)
        return -1;
    int opp;
    u32 n = 0;
    fdt_for_each_subnode(opp, dt, table)
    {
        if (++n > count || !fdt_getprop(dt, opp, "opp-hz", &len) || len != sizeof(u64) ||
            !fdt_getprop(dt, opp, "opp-microvolt", &len) || len != volt_len)
            return -1;
    }
    return n == count ? table : -1;
}

static const struct {
    const char *name;
    const char *path;
} t6040_regions[] = {
    {"gfx-handoff", "/reserved-memory/uat-handoff"},
    {"gfx-shared-l2-region", "/reserved-memory/uat-l2-pagetables"},
    {"gfx-shared-region", "/reserved-memory/uat-pagetables"},
    {"gpu-region", "/reserved-memory/uat-ttbs"},
    {"gfx-data", "/reserved-memory/gpu-fw-data"},
    {NULL, "/reserved-memory/gpu-fw-text"},
};

static int dt_set_gpu_t6040(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        return 0;
    if (fdt_node_check_compatible(dt, gpu, "apple,agx-t6040"))
        return 0;

    const char *status = fdt_getprop(dt, gpu, "status", NULL);
    if (status && strcmp(status, "okay") && strcmp(status, "ok")) {
        printf("FDT: GPU: T6040 GPU is disabled in the device tree, not filling it\n");
        return 0;
    }

    /* Written last, so an older loader or an interrupted fill cannot enable the GPU. */
    int ret = fdt_delprop(dt, gpu, "apple,t6040-handoff-version");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return -1;
    size_t touched = 0;
    int sgx_path[8];
    int sgx = adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path);
    /* ADT counts raw operating points, independently of the SGX selector maps. */
    u32 count, tables, num_states, len;
    u64 sgx_base;
    if (sgx < 0 || adt_get_reg(adt, sgx_path, "reg", 0, &sgx_base, NULL) < 0 ||
        ADT_GETPROP(adt, sgx, "perf-state-count", &count) != sizeof(count) ||
        ADT_GETPROP(adt, sgx, "perf-state-table-count", &tables) != sizeof(tables) ||
        ADT_GETPROP(adt, sgx, "gpu-num-perf-states", &num_states) != sizeof(num_states) ||
        count != T6040_OPPS || num_states != count - 1 || tables != T6040_CLUSTERS) {
        printf("FDT: GPU: invalid T6040 raw OPP counts\n");
        goto disable;
    }

    /* Operating point 0 is the off state; every other one is checked here before any use. */
    const struct perf_state *states = adt_getprop(adt, sgx, "perf-states", &len);
    if (!states || len != count * tables * sizeof(*states))
        goto disable;
    for (u32 j = 0; j < tables; j++)
        if (states[j * count].freq)
            goto disable;
    for (u32 j = 0; j < tables; j++)
        for (u32 i = 1; i < count; i++)
            if (!states[j * count + i].freq || states[j * count + i].freq != states[i].freq ||
                states[j * count + i].volt < 400 || states[j * count + i].volt > 1300)
                goto disable;

    const struct aux_perf_states *aux[2];
    const char *aux_name[2] = {"cs-perf-states", "afr-perf-states"};
    for (int j = 0; j < 2; j++) {
        aux[j] = adt_getprop(adt, sgx, aux_name[j], &len);
        if (!aux[j] || len < sizeof(*aux[j]) || aux[j]->dies != 1 || aux[j]->count < 2 ||
            aux[j]->count > MAX_PSTATES ||
            len < sizeof(*aux[j]) + aux[j]->count * sizeof(aux[j]->states[0]))
            goto disable;
        for (u32 i = 0; i < aux[j]->count; i++)
            if (!aux[j]->states[i].freq || aux[j]->states[i].volt < 400000 ||
                aux[j]->states[i].volt > 1300000)
                goto disable;
    }

    /* Base policy defaults to the first active state, as in the T6040 power policy.
     * Absence is valid; a malformed present value must not fall back to that policy. */
    u32 base_pstate = 1;
    const u32 *base_prop = adt_getprop(adt, sgx, "gpu-perf-base-pstate", &len);
    if (base_prop) {
        if (len != sizeof(base_pstate))
            goto disable;
        memcpy(&base_pstate, base_prop, sizeof(base_pstate));
    }
    if (base_pstate != 1) {
        printf("FDT: GPU: invalid T6040 gpu-perf-base-pstate\n");
        goto disable;
    }

    int asc = adt_path_offset(adt, "/arm-io/gfx-asc");
    if (asc < 0)
        goto disable;
    const struct adt_segment_ranges *segments = adt_getprop(adt, asc, "segment-ranges", &len);
    if (!segments || len != 2 * sizeof(*segments))
        goto disable;

    /* The reserved regions, the OPP tables and the GPU node must all take the fill. */
    u64 region_base[ARRAY_SIZE(t6040_regions)], region_size[ARRAY_SIZE(t6040_regions)];
    for (size_t i = 0; i < ARRAY_SIZE(t6040_regions); i++) {
        char prop[64];
        if (t6040_regions[i].name) {
            snprintf(prop, sizeof(prop), "%s-base", t6040_regions[i].name);
            if (ADT_GETPROP(adt, sgx, prop, &region_base[i]) != sizeof(u64))
                goto disable;
            snprintf(prop, sizeof(prop), "%s-size", t6040_regions[i].name);
            if (ADT_GETPROP(adt, sgx, prop, &region_size[i]) != sizeof(u64))
                goto disable;
        } else {
            region_base[i] = segments[0].phys;
            region_size[i] = segments[0].size;
        }
        if (!region_base[i] || !region_size[i] || (region_base[i] & (SZ_16K - 1)) ||
            (region_size[i] & (SZ_16K - 1)) || region_base[i] > UINT64_MAX - region_size[i] ||
            region_base[i] + region_size[i] > (1ULL << 42) ||
            fdt_path_offset(dt, t6040_regions[i].path) < 0)
            goto disable;
        for (size_t j = 0; j < i; j++)
            if (region_base[i] < region_base[j] + region_size[j] &&
                region_base[j] < region_base[i] + region_size[i])
                goto disable;
    }
    if (segments[1].phys != region_base[4] || segments[1].size != region_size[4])
        goto disable;
    int table =
        t6040_opp_table(dt, gpu, "operating-points-v2", count, T6040_CLUSTERS * sizeof(u32));
    if (table < 0 || t6040_opp_table(dt, gpu, "apple,cs-opp", aux[0]->count, sizeof(u32)) < 0 ||
        t6040_opp_table(dt, gpu, "apple,afr-opp", aux[1]->count, sizeof(u32)) < 0)
        goto disable;

    /* Leakage from the efuse page: two cluster cores, the shared SRAM, CS and AFR. */
    u32 ef[5];
    const u32 ef_off[5] = {0xc10, 0xc1c, 0xc20, 0xc24, 0xc28};
    for (int i = 0; i < 5; i++)
        if (t6040_read32(T6040_EFUSE + ef_off[i], &ef[i]))
            goto disable;
    u64 sram_word = ((u64)ef[3] << 32) | ef[2];
    const u32 raw[5] = {(ef[0] >> 1) & 0x3fff, (ef[0] >> 15) & 0x3fff, (sram_word >> 24) & 0x3ff,
                        (ef[1] >> 9) & 0x1fff, (ef[4] >> 4) & 0xfff};
    const u32 raw_max[5] = {0x3fff, 0x3fff, 0x3ff, 0x1fff, 0xfff};
    for (int i = 0; i < 5; i++)
        if (raw[i] == 0 || raw[i] == raw_max[i])
            goto disable;

    float core_leak[2] = {(float)(raw[0] + 1) * 1.5f, (float)(raw[1] + 1) * 1.5f};
    float sram = (float)(raw[2] + 1) * 3.0f;
    float sram_leak[2] = {sram, sram};
    float cs_leak = (float)(raw[3] + 1);
    float afr_leak = (float)(raw[4] + 1) * 1.5f;

    /*
     * The core mask and the A/B maps are SGX registers, so the GPU must be powered to read
     * them. Put every power state the enable changed back as it was once they are read.
     */
    struct pmgr_saved_modes sgx_power;
    if (pmgr_adt_power_enable_traced_saved("/arm-io/sgx", &sgx_power) < 0)
        goto disable;
    u32 masks[3], index_a[16], index_b[16];
    int read_failed = 0;
    for (int i = 0; i < 3; i++)
        read_failed |= t6040_read32(sgx_base + 0xe01500 + 4 * i, &masks[i]);
    int n = read_failed ? -1 : t6040_perf_map(sgx_base + 0xe01400, index_a);
    int n_b = n < 0 ? -1 : t6040_perf_map(sgx_base + 0xe01480, index_b);
    pmgr_restore_modes(&sgx_power);
    if (n < 0 || n_b != n)
        goto disable;

    if (masks[1] || masks[2] || (masks[0] >> (T6040_CLUSTERS * T6040_CORES)))
        goto disable;
    u32 active[T6040_CLUSTERS];
    for (int c = 0; c < T6040_CLUSTERS; c++) {
        active[c] = __builtin_popcount((masks[0] >> (T6040_CORES * c)) & MASK(T6040_CORES));
        if (!active[c])
            goto disable;
    }

    /* Both maps must climb in frequency, so the B ladder below cannot wrap. */
    for (int i = 2; i < n; i++)
        if (states[index_a[i]].freq <= states[index_a[i - 1]].freq ||
            states[index_b[i]].freq <= states[index_b[i - 1]].freq)
            goto disable;

    /* Maximum power of each logical A state, and the relative tables. State 0 stays at 0. */
    u32 power[T6040_MAP_STATES] = {0}, freq[T6040_MAP_STATES] = {0};
    for (int i = 1; i < n; i++)
        freq[i] = states[index_a[i]].freq / 1000000;
    for (int i = 1; i < n; i++) {
        const struct perf_state *s = &states[index_a[i]];
        u32 c = min((u32)i, (u32)aux[0]->count - 1);
        u32 cs =
            t6040_cs_mw(aux[0]->states[c].volt / 1000, aux[0]->states[c].freq / 1000000, cs_leak);
        for (int j = 0; j < T6040_CLUSTERS; j++) {
            float r = (float)active[j] / 10.0f;
            /* Half the CS power per cluster, truncated as in the reference tables. */
            power[i] += t6040_cluster_mw(s[j * count].volt, s->freq / 1000000, core_leak[j], r) +
                        cs / T6040_CLUSTERS;
        }
        /* A power ladder that falls would put the relative-A table above 100. */
        if (power[i] < power[i - 1])
            goto disable;
    }

    /* Keep the unpopulated selector slots explicit and zero in the handoff. */
    u32 rel_a[T6040_OPPS] = {0}, rel_b[T6040_OPPS] = {0};
    u64 span_p = power[n - 1] - power[0], span_f = freq[n - 1] - freq[base_pstate];
    if (!span_p || !span_f)
        goto disable;
    for (int i = 0; i < n; i++) {
        rel_a[i] = (u32)(100ULL * (power[i] - power[0]) / span_p);
        rel_b[i] =
            i < (int)base_pstate ? 0 : (u32)(100ULL * (freq[i] - freq[base_pstate]) / span_f);
    }

    /* Everything is validated. Fill the GPU node, then publish the reserved regions last. */
    int opp;
    u32 i = 0;
    fdt_for_each_subnode(opp, dt, table)
    {
        fdt32_t volts[T6040_CLUSTERS];
        for (u32 j = 0; j < T6040_CLUSTERS; j++)
            volts[j] = cpu_to_fdt32(states[i + j * count].volt * 1000);
        if (fdt_setprop_inplace(dt, opp, "opp-microvolt", volts, sizeof(volts)) ||
            fdt_setprop_inplace_u64(dt, opp, "opp-hz", states[i].freq))
            goto disable;
        i++;
    }
    if (fdt_set_aux_opp(dt, gpu, "apple,cs-opp", aux[0], 1) ||
        fdt_set_aux_opp(dt, gpu, "apple,afr-opp", aux[1], 1))
        goto disable;

    if (firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware) ||
        t6040_set_floats(dt, gpu, "apple,core-leak-coef", core_leak, T6040_CLUSTERS) ||
        t6040_set_floats(dt, gpu, "apple,sram-leak-coef", sram_leak, T6040_CLUSTERS) ||
        t6040_set_floats(dt, gpu, "apple,cs-leak-coef", &cs_leak, 1) ||
        t6040_set_floats(dt, gpu, "apple,afr-leak-coef", &afr_leak, 1) ||
        t6040_set_u32s(dt, gpu, "apple,perf-map-a", index_a, count) ||
        t6040_set_u32s(dt, gpu, "apple,perf-map-b", index_b, count) ||
        t6040_set_u32s(dt, gpu, "apple,perf-relative-a", rel_a, count) ||
        t6040_set_u32s(dt, gpu, "apple,perf-relative-b", rel_b, count) ||
        fdt_setprop_u32(dt, gpu, "apple,perf-base-pstate", base_pstate) ||
        fdt_setprop_u32(dt, gpu, "apple,max-power-mw", power[n - 1]) ||
        fdt_setprop_u32(dt, gpu, "apple,core-mask", masks[0]))
        goto disable;

    while (touched < ARRAY_SIZE(t6040_regions)) {
        size_t r = touched++;
        if (dt_set_resvmem(dt, t6040_regions[r].path, region_base[r], region_size[r]))
            goto disable;
    }

    gpu = fdt_path_offset(dt, "gpu");
    if (fdt_setprop_u32(dt, gpu, "apple,t6040-handoff-version", 2))
        goto disable;

    printf("FDT: GPU: T6040 cores %u+%u, max power %u mW, OS firmware %s\n", active[0], active[1],
           power[n - 1], os_firmware.string);
    return 0;

disable:
    /* A region published before a failure would stay reserved for a disabled GPU. */
    for (size_t r = 0; r < touched; r++) {
        int node = fdt_path_offset(dt, t6040_regions[r].path);
        if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled"))
            return -1;
    }
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu >= 0 && fdt_setprop_string(dt, gpu, "status", "disabled"))
        return -1;
    printf("FDT: GPU: invalid T6040 ADT, fuse or SGX data, disabling GPU\n");
    return 0;
}

int dt_set_gpu_m4(void *dt);

int dt_set_gpu(void *dt)
{
    if (chip_id == T8132)
        return dt_set_gpu_m4(dt);

    if (chip_id == T8140)
        return dt_set_gpu_g17(dt, &g17_soc_t8140);
    if (chip_id == T8142)
        return dt_set_gpu_g17(dt, &g17_soc_t8142);

    if (chip_id == T6040)
        return dt_set_gpu_t6040(dt);

    bool has_cs_afr = false;
    int (*calc_power)(u32 count, u32 table_count, const struct perf_state *core,
                      const struct perf_state *sram, const struct aux_perf_states *cs, u32 *max_pwr,
                      float *core_leak, float *sram_leak, float *cs_leak, float *afr_leak);

    u32 dies = 1;

    printf("FDT: GPU: Initializing GPU info\n");

    switch (chip_id) {
        case T8103:
            calc_power = calc_power_t8103;
            break;
        case T6022:
            dies = 2;
            // fallthrough
        case T6021:
        case T6020:
            has_cs_afr = true;
            calc_power = calc_power_t600x;
            break;
        case T6002:
            dies = 2;
            // fallthrough
        case T6001:
        case T6000:
        case T8112:
            calc_power = calc_power_t600x;
            break;
        case T6030:
            return dt_set_gpu_g15(dt, &g15_soc_t6030);
        case T6031:
            return dt_set_gpu_t6031(dt);
        case T6032:
            /*
             * T6032 (J575D): refused until the firmware ABI exists. Missing pieces, each one a
             * flip-plus-table-row after this is in place: the loaded G15-family image identity
             * (a T6032 row in firmware.c's gfx_fw_images[]) and a GPU setup with the T6032
             * topology (the 27.0.1 ADT names gpu,t6032 in /arm-io/sgx) armed by its opt-in.
             */
            printf("FDT: GPU: T6032: handoff refused: no known T6032 gfx firmware image/ABI for "
                   "OS firmware %s exists yet; add its identity to gfx_fw_images and a T6032 GPU "
                   "setup, then opt in\n",
                   os_firmware.string);
            return 0;
        case T8122: {
            int dcp = fdt_path_offset(dt, "dcp"), len;
            const fdt32_t *profile =
                dcp < 0 ? NULL : fdt_getprop(dt, dcp, "apple,j613-25g83-profile", &len);
            if (profile && len == 4 && fdt32_ld(profile) == 1) {
                t8122_leak_fuse_export(dt);
                return dt_set_gpu_t8122_26(dt);
            }
            return dt_set_gpu_t8122_select(dt);
        }
        default:
            printf("ADT: GPU: unsupported chip!\n");
            return 0;
    }

    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }

    /* check for old-style downstream compatibles */
    bool downstream_dtb = false;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t8103") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t8112") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6000") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6001") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6002") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6020") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6021") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6022") == 0;

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        bail("ADT: GPU: /arm-io/sgx node not found\n");

    u32 perf_state_count;
    if (ADT_GETPROP(adt, sgx, "perf-state-count", &perf_state_count) < 0 || !perf_state_count)
        bail("ADT: GPU: missing perf-state-count\n");

    u32 perf_state_table_count;
    if (ADT_GETPROP(adt, sgx, "perf-state-table-count", &perf_state_table_count) < 0 ||
        !perf_state_table_count)
        bail("ADT: GPU: missing perf-state-table-count\n");

    if (perf_state_count > MAX_PSTATES)
        bail("ADT: GPU: perf-state-count too large\n");

    if (perf_state_table_count > MAX_CLUSTERS)
        bail("ADT: GPU: perf-state-table-count too large\n");

    u32 perf_states_len;
    const struct perf_state *perf_states, *perf_states_sram;
    const struct aux_perf_states *perf_states_afr, *perf_states_cs;

    perf_states = adt_getprop(adt, sgx, "perf-states", &perf_states_len);
    if (!perf_states ||
        perf_states_len != sizeof(*perf_states) * perf_state_count * perf_state_table_count)
        bail("ADT: GPU: invalid perf-states length\n");

    perf_states_sram = adt_getprop(adt, sgx, "perf-states-sram", &perf_states_len);
    if (perf_states_sram &&
        perf_states_len != sizeof(*perf_states) * perf_state_count * perf_state_table_count)
        bail("ADT: GPU: invalid perf-states-sram length\n");

    perf_states_afr = adt_getprop(adt, sgx, "afr-perf-states", NULL);
    perf_states_cs = adt_getprop(adt, sgx, "cs-perf-states", NULL);

    if (has_cs_afr && !perf_states_cs)
        bail("ADT: GPU: cs-perf-states not found\n");

    if (has_cs_afr && !perf_states_afr)
        bail("ADT: GPU: afr-perf-states not found\n");

    u32 max_pwr[MAX_PSTATES];
    float core_leak[MAX_CLUSTERS];
    float sram_leak[MAX_CLUSTERS];
    float cs_leak[MAX_DIES];
    float afr_leak[MAX_DIES];

    if (calc_power(perf_state_count, perf_state_table_count, perf_states, perf_states_sram,
                   perf_states_cs, max_pwr, core_leak, sram_leak, cs_leak, afr_leak))
        return -1;

    printf("FDT: GPU: Max power table: ");
    for (u32 i = 0; i < perf_state_count; i++) {
        printf("%d ", max_pwr[i]);
    }
    printf("\nFDT: GPU: Core leakage table: ");
    for (u32 i = 0; i < perf_state_table_count; i++) {
        printf("%d.%03d ", (int)core_leak[i], ((int)(core_leak[i] * 1000) % 1000));
    }
    printf("\nFDT: GPU: SRAM leakage table: ");
    for (u32 i = 0; i < perf_state_table_count; i++) {
        printf("%d.%03d ", (int)sram_leak[i], ((int)(sram_leak[i] * 1000) % 1000));
    }
    printf("\n");

    if (has_cs_afr) {
        printf("FDT: GPU: CS leakage table: ");
        for (u32 i = 0; i < dies; i++) {
            printf("%d.%03d ", (int)cs_leak[i], ((int)(cs_leak[i] * 1000) % 1000));
        }
        printf("\n");

        printf("FDT: GPU: AFR leakage table: ");
        for (u32 i = 0; i < dies; i++) {
            printf("%d.%03d ", (int)afr_leak[i], ((int)(afr_leak[i] * 1000) % 1000));
        }
        printf("\n");
    }

    if (downstream_dtb) {
        if (fdt_set_float_array(dt, gpu, "apple,core-leak-coef", core_leak, perf_state_table_count))
            return -1;

        if (fdt_set_float_array(dt, gpu, "apple,sram-leak-coef", sram_leak, perf_state_table_count))
            return -1;

        int len;
        const fdt32_t *opps_ph = fdt_getprop(dt, gpu, "operating-points-v2", &len);
        if (!opps_ph || len != 4)
            bail("FDT: GPU: operating-points-v2 not found\n");

        int opps = fdt_node_offset_by_phandle(dt, fdt32_ld(opps_ph));
        if (opps < 0)
            bail("FDT: GPU: node for phandle %u not found\n", fdt32_ld(opps_ph));

        u32 i = 0;
        int opp;
        fdt_for_each_subnode(opp, dt, opps)
        {
            fdt32_t volts[MAX_CLUSTERS];

            for (u32 j = 0; j < perf_state_table_count; j++) {
                volts[j] = cpu_to_fdt32(perf_states[i + j * perf_state_count].volt * 1000);
            }

            if (i >= perf_state_count)
                bail("FDT: GPU: Expected %d operating points, but found more\n", perf_state_count);

            if (fdt_setprop_inplace(dt, opp, "opp-microvolt", &volts,
                                    sizeof(u32) * perf_state_table_count))
                bail("FDT: GPU: Failed to set opp-microvolt for PS %d\n", i);

            if (fdt_setprop_inplace_u64(dt, opp, "opp-hz", perf_states[i].freq))
                bail("FDT: GPU: Failed to set opp-hz for PS %d\n", i);

            if (fdt_setprop_inplace_u32(dt, opp, "opp-microwatt", max_pwr[i]))
                bail("FDT: GPU: Failed to set opp-microwatt for PS %d\n", i);

            i++;
        }

        if (i != perf_state_count)
            bail("FDT: GPU: Expected %d operating points, but found %d\n", perf_state_count, i);

        if (has_cs_afr) {
            int ret = fdt_set_aux_opp(dt, gpu, "apple,cs-opp", perf_states_cs, dies);
            if (ret)
                return ret;

            if (fdt_set_float_array(dt, gpu, "apple,cs-leak-coef", cs_leak, dies))
                return -1;
        }

        if (has_cs_afr) {
            int ret = fdt_set_aux_opp(dt, gpu, "apple,afr-opp", perf_states_afr, dies);
            if (ret)
                return ret;
            if (fdt_set_float_array(dt, gpu, "apple,afr-leak-coef", afr_leak, dies))
                return -1;
        }
    }

    if (dt_set_region(dt, sgx, "gfx-handoff", "/reserved-memory/uat-handoff"))
        return -1;
    if (dt_set_region(dt, sgx, "gfx-shared-region", "/reserved-memory/uat-pagetables"))
        return -1;
    if (dt_set_region(dt, sgx, "gpu-region", "/reserved-memory/uat-ttbs"))
        return -1;

    // refresh gpu dt node offset after modifying the dt in dt_set_region()
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }

    const struct fw_version_info *compat;

    switch (os_firmware.version) {
        case V12_3_1:
            compat = &fw_versions[V12_3];
            break;
        case V13_5B4:
        case V13_6_1:
            compat = &fw_versions[V13_5];
            break;
        default:
            compat = &os_firmware;
            break;
    }

    if (downstream_dtb) {
        if (firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware))
            return -1;
        if (firmware_set_fdt(dt, gpu, "apple,firmware-compat", compat))
            return -1;
    }
    // Ignoring errors for old dts compat
    firmware_set_fdt(dt, gpu, "apple,firmware-abi", compat);

    size_t data_a_size, data_b_size, globals_size;
    if (rust_gpu_initdata_size(compat->num[0], compat->num[1], &data_a_size, &data_b_size,
                               &globals_size) == -1)
        return -1;

    void *data_a = (void *)top_of_memory_alloc(ALIGN_UP(data_a_size, SZ_16K));
    void *data_b = (void *)top_of_memory_alloc(ALIGN_UP(data_b_size, SZ_16K));
    void *globals = (void *)top_of_memory_alloc(ALIGN_UP(globals_size, SZ_16K));
    memset(data_a, 0, data_a_size);
    memset(data_b, 0, data_b_size);
    memset(globals, 0, globals_size);

    size_t n_perf_states_cs = 0, n_perf_states_afr = 0;
    const struct aux_perf_state *pstate_cs_raw = NULL, *pstate_afr_raw = NULL;

    if (has_cs_afr) {
        n_perf_states_cs = perf_states_cs->count;
        n_perf_states_afr = perf_states_afr->count;
        pstate_cs_raw = perf_states_cs->states;
        pstate_afr_raw = perf_states_afr->states;
    }
    struct initdata_inputs ins = {
        .perf_state_table_count = perf_state_table_count,
        .perf_state_count = perf_state_count,
        .c_perf_states = perf_states,
        .max_pwr = max_pwr,
        .core_leak = core_leak,
        .sram_leak = sram_leak,
        .cs_leak = cs_leak,
        .afr_leak = afr_leak,
        .n_perf_states_cs = n_perf_states_cs,
        .pstates_cs = pstate_cs_raw,
        .n_perf_states_afr = n_perf_states_afr,
        .pstates_afr = pstate_afr_raw,
        .compat_maj = compat->num[0],
        .compat_min = compat->num[1],
    };
    if (rust_fill_gpu_initdata(&ins, data_a, data_b, globals) == -1)
        return -1;

    if (dt_set_resvmem(dt, "/reserved-memory/hw-cal-a", (u64)data_a, ALIGN_UP(data_a_size, SZ_16K)))
        return 0; // Old dts.
    if (dt_set_resvmem(dt, "/reserved-memory/hw-cal-b", (u64)data_b, ALIGN_UP(data_b_size, SZ_16K)))
        return -1;
    if (dt_set_resvmem(dt, "/reserved-memory/globals", (u64)globals,
                       ALIGN_UP(globals_size, SZ_16K)))
        return -1;

    /* Save init data sizes for debugging */
    // refresh gpu dt node offset after modifying the dt in dt_set_region()
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }
    fdt_setprop_u32(dt, gpu, "debug,hw-cal-a-size", data_a_size);
    fdt_setprop_u32(dt, gpu, "debug,hw-cal-b-size", data_b_size);
    fdt_setprop_u32(dt, gpu, "debug,globals-size", globals_size);

    return 0;
}
