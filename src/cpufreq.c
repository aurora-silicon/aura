/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "cpufreq.h"
#include "adt.h"
#include "firmware.h"
#include "pmgr.h"
#include "soc.h"
#include "utils.h"

#define CLUSTER_PSTATE 0x20020

#define CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK BIT(42)
#define CLUSTER_PSTATE_BUSY                   BIT(31)
#define CLUSTER_PSTATE_SET                    BIT(25)
#define CLUSTER_PSTATE_M2_APSC_DIS            BIT(23)
#define CLUSTER_PSTATE_M1_APSC_DIS            BIT(22)
#define CLUSTER_PSTATE_UNK_M2                 BIT(22)
#define CLUSTER_PSTATE_UNK_M1                 BIT(20)
#define CLUSTER_PSTATE_DESIRED2               GENMASK(15, 12)
#define CLUSTER_PSTATE_APSC_BUSY              BIT(7)
#define CLUSTER_PSTATE_DESIRED1               GENMASK(4, 0)
#define CLUSTER_PSTATE_DESIRED1_S5L8960X      GENMASK(24, 22)

#define PMGR_VOLTAGE_CTL_OFF_S5L8960X 0x20c00
#define PMGR_VOLTAGE_CTL_OFF_T7000    0x23000
#define PMGR_VOLTAGE_CTL_OFF_S8000    0xa0000

#define CLUSTER_SWITCH_TIMEOUT 400

struct cluster_t {
    const char *name;
    u64 base;
    bool pcluster;
    uint32_t apsc_pstate;
    uint32_t default_pstate;
    uint32_t voltage_ctl;
};

struct feat_t {
    const char *name;
    u64 offset;
    u64 clear;
    u64 set;
    u64 wait;
    bool pcluster_only;
};

static u32 pstate_reg_to_pstate(u64 val)
{
    switch (chip_id) {
        case S5L8960X:
        case T7000:
        case T7001:
            return FIELD_GET(CLUSTER_PSTATE_DESIRED1_S5L8960X, val);
        case S8000:
        case S8001:
        case S8003:
        case T8010:
        case T8011:
        case T8012:
        case T8015:
        case T8103:
        case T6000:
        case T6001:
        case T6002:
        case T8112:
        case T6020:
        case T6021:
        case T6022:
        case T6030 ... T6034:
        case T8122:
        case T8142:
        case T8140:
            return FIELD_GET(CLUSTER_PSTATE_DESIRED1, val);
        default:
            printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
            return 0;
    }
}

static int set_pstate(const struct cluster_t *cluster, uint32_t pstate)
{
    if (chip_id == T8142 && poll64(cluster->base + CLUSTER_PSTATE, CLUSTER_PSTATE_BUSY, 0,
                                   CLUSTER_SWITCH_TIMEOUT) < 0) {
        printf("cpufreq: Cluster %s is busy before P-State request\n", cluster->name);
        return -1;
    }

    u64 val = read64(cluster->base + CLUSTER_PSTATE);

    if (pstate_reg_to_pstate(val) != pstate) {
        switch (chip_id) {
            case S5L8960X:
            case T7000:
            case T7001:
                val &= ~CLUSTER_PSTATE_DESIRED1_S5L8960X;
                val |= CLUSTER_PSTATE_SET | FIELD_PREP(CLUSTER_PSTATE_DESIRED1_S5L8960X, pstate);
                break;
            case S8000:
            case S8001:
            case S8003:
            case T8010:
            case T8011:
            case T8012:
            case T8015:
            case T8103:
            case T6000:
            case T6001:
            case T6002:
                val &= ~CLUSTER_PSTATE_DESIRED2;
                val |= CLUSTER_PSTATE_SET | FIELD_PREP(CLUSTER_PSTATE_DESIRED2, pstate);
                /* fallthrough */
            case T8112:
            case T6020:
            case T6021:
            case T6022:
            case T6030 ... T6034:
            case T8122:
            case T8142:
            case T8140:
                val &= ~CLUSTER_PSTATE_DESIRED1;
                val |= CLUSTER_PSTATE_SET | FIELD_PREP(CLUSTER_PSTATE_DESIRED1, pstate);
                break;
            default:
                printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
                return -1;
        }
        write64(cluster->base + CLUSTER_PSTATE, val);
        if (poll64(cluster->base + CLUSTER_PSTATE, CLUSTER_PSTATE_BUSY, 0, CLUSTER_SWITCH_TIMEOUT) <
            0) {
            printf("cpufreq: Timed out waiting for cluster %s P-State switch\n", cluster->name);
            return -1;
        }
    }

    if (chip_id == T8142 &&
        pstate_reg_to_pstate(read64(cluster->base + CLUSTER_PSTATE)) != pstate) {
        printf("cpufreq: Cluster %s did not accept P-State %u\n", cluster->name, pstate);
        return -1;
    }

    return 0;
}

int cpufreq_init_cluster(const struct cluster_t *cluster, const struct feat_t *features)
{
    /* Reset P-State to the APSC p-state */

    if (cluster->apsc_pstate && set_pstate(cluster, cluster->apsc_pstate))
        return -1;

    /* CPU complex features */

    for (; features->name; features++) {
        if (features->pcluster_only && !cluster->pcluster)
            continue;

        u64 reg = cluster->base + features->offset;

        if (pmgr_get_feature(features->name))
            mask64(reg, features->clear, features->set);
        else
            mask64(reg, features->set, features->clear);

        if (features->wait && poll32(reg, features->wait, 0, CLUSTER_SWITCH_TIMEOUT) < 0) {
            printf("cpufreq: Timed out waiting for feature %s on cluster %s\n", features->name,
                   cluster->name);
            return -1;
        }
    }

    /* Pre M1 voltage control handling in pmgr.reg[0]. */
    if (cluster->voltage_ctl)
        pmgr_set_voltage_ctl(cluster->voltage_ctl);

    switch (chip_id) {
        case S5L8960X:
        case T7000 ... T7001:
        case S8000 ... S8003:
        case T8010 ... T8015: /* This covers a gap but T8013 and T8014 will not randomly appear. */
        case T8142:           /* Keep firmware voltage, PLL and throttling configuration. */
            /* Do nothing */
            break;
        case T8103:
        case T6000:
        case T6001:
        case T6002:
        case T8112:
        case T6020:
        case T6021:
        case T6022:
            /* Unknown */
            write64(cluster->base + 0x440f8, 1);

            /* APSC Snooze */
            set64(cluster->base + 0x200f8, BIT(40));
            break;
        case T6030 ... T6034:
        case T8122:
            /* Unknown */
            write64(cluster->base + 0x440f8, 1);
            break;
        default:
            printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
            break;
    }

    switch (chip_id) {
        case T8103: {
            u64 lo = read64(cluster->base + 0x70000 + cluster->apsc_pstate * 0x20);
            u64 hi = read64(cluster->base + 0x70008 + cluster->apsc_pstate * 0x20);
            write64(cluster->base + 0x70210, lo);
            write64(cluster->base + 0x70218, hi);
            break;
        }
        case T8112: {
            u64 lo = read64(cluster->base + 0x78000 + cluster->apsc_pstate * 0x40);
            u64 hi = read64(cluster->base + 0x78008 + cluster->apsc_pstate * 0x40);
            write64(cluster->base + 0x7ffe8, lo);
            write64(cluster->base + 0x7fff0, hi);
            break;
        }
    }

    /* Default P-State */
    if (cluster->default_pstate && set_pstate(cluster, cluster->default_pstate))
        return -1;

    return 0;
}

void cpufreq_fixup_cluster(const struct cluster_t *cluster)
{
    u64 val = read64(cluster->base + CLUSTER_PSTATE);

    // Older versions of m1n1 stage 1 erroneously cleared CLUSTER_PSTATE_UNK_Mx, so put it back for
    // firmwares it supported (don't touch anything newer, which includes newer devices).
    // Also clear the CLUSTER_PSTATE_DESIRED2 field since it doesn't seem to do anything, and isn't
    // used on newer chips.
    if (os_firmware.version != V_UNKNOWN && os_firmware.version <= V13_3) {
        u64 bits = 0;
        switch (chip_id) {
            case T8103:
            case T6000:
            case T6001:
            case T6002:
                bits = CLUSTER_PSTATE_UNK_M1;
                break;
            case T8112:
            case T6020:
            case T6021:
            case T6022:
                bits = CLUSTER_PSTATE_UNK_M2;
                break;
            default:
                return;
        }
        if (!(val & bits) || (val & CLUSTER_PSTATE_DESIRED2)) {
            val |= bits;
            val &= ~CLUSTER_PSTATE_DESIRED2;
            printf("cpufreq: Correcting setting for cluster %s\n", cluster->name);
            write64(cluster->base + CLUSTER_PSTATE, val);
        }
    }
}

static const struct cluster_t s5l8960x_clusters[] = {
    {"CPU", 0x202200000, false, 2, 6, PMGR_VOLTAGE_CTL_OFF_S5L8960X},
    {},
};

static const struct cluster_t t7000_clusters[] = {
    {"CPU", 0x202200000, false, 2, 5, PMGR_VOLTAGE_CTL_OFF_T7000},
    {},
};

static const struct cluster_t t7001_clusters[] = {
    {"CPU", 0x202200000, false, 2, 7, PMGR_VOLTAGE_CTL_OFF_T7000},
    {},
};

static const struct cluster_t s8000_clusters[] = {
    {"CPU", 0x202200000, false, 2, 7, PMGR_VOLTAGE_CTL_OFF_S8000},
    {},
};

static const struct cluster_t t8010_clusters[] = {
    /* Fused cluster, kernel expects E-core entry */
    {"CPU", 0x202f00000, false, 2, 4, PMGR_VOLTAGE_CTL_OFF_S8000},
    {},
};

static const struct cluster_t t8012_clusters[] = {
    /* Fused cluster, kernel expects P-core entry */
    {"CPU", 0x202f00000, false, 6, 10, PMGR_VOLTAGE_CTL_OFF_S8000},
    {},
};

static const struct cluster_t t8015_clusters[] = {
    {"ECPU", 0x208e00000, false, 2, 6, PMGR_VOLTAGE_CTL_OFF_S8000},
    {"PCPU", 0x208e80000, true, 2, 7, 0 /* already written for the first cluster */},
    {},
};

static const struct cluster_t t8103_clusters[] = {
    {"ECPU", 0x210e00000, false, 1, 5},
    {"PCPU", 0x211e00000, true, 1, 7},
    {},
};

static const struct cluster_t t6000_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 5},
    {"PCPU0", 0x211e00000, true, 1, 7},
    {"PCPU1", 0x212e00000, true, 1, 7},
    {},
};

static const struct cluster_t t6002_clusters[] = {
    {"ECPU0", 0x0210e00000, false, 1, 5},
    {"PCPU0", 0x0211e00000, true, 1, 7},
    {"PCPU1", 0x0212e00000, true, 1, 7},
    {"ECPU1", 0x2210e00000, false, 1, 5},
    {"PCPU2", 0x2211e00000, true, 1, 7},
    {"PCPU3", 0x2212e00000, true, 1, 7},
    {},
};

static const struct cluster_t t8112_clusters[] = {
    {"ECPU", 0x210e00000, false, 1, 7},
    {"PCPU", 0x211e00000, true, 1, 6},
    {},
};

static const struct cluster_t t6020_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 5},
    {"PCPU0", 0x211e00000, true, 1, 6},
    {"PCPU1", 0x212e00000, true, 1, 6},
    {},
};

static const struct cluster_t t6022_clusters[] = {
    {"ECPU0", 0x0210e00000, false, 1, 5},
    {"PCPU0", 0x0211e00000, true, 1, 6},
    {"PCPU1", 0x0212e00000, true, 1, 6},
    {"ECPU1", 0x2210e00000, false, 1, 5},
    {"PCPU2", 0x2211e00000, true, 1, 6},
    {"PCPU3", 0x2212e00000, true, 1, 6},
    {},
};

static const struct cluster_t t6031_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 5},
    {"PCPU0", 0x211e00000, true, 1, 6},
    {"PCPU1", 0x212e00000, true, 1, 6},
    {},
};

static const struct cluster_t t6032_clusters[] = {
    {"ECPU0", 0x0210e00000, false, 1, 5},
    {"PCPU0", 0x0211e00000, true, 1, 6},
    {"PCPU1", 0x0212e00000, true, 1, 6},
    {"ECPU1", 0x2210e00000, false, 1, 5},
    {"PCPU2", 0x2211e00000, true, 1, 6},
    {"PCPU3", 0x2212e00000, true, 1, 6},
    {},
};

static const struct cluster_t t6030_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 5},
    {"PCPU0", 0x211e00000, true, 1, 6},
    {},
};

/* J813 firmware handoff states: ECPU 972 MHz, PCPU 3516 MHz. */
static const struct cluster_t t8142_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 2},
    {"PCPU0", 0x211e00000, true, 1, 10},
    {},
};

const struct cluster_t *cpufreq_get_clusters(void)
{
    switch (chip_id) {
        case S5L8960X:
            return s5l8960x_clusters;
        case T7000:
            return t7000_clusters;
        case T7001:
            return t7001_clusters;
        case S8000:
        case S8001:
        case S8003:
            return s8000_clusters;
        case T8010:
        case T8011:
            return t8010_clusters;
        case T8012:
            return t8012_clusters;
        case T8015:
            return t8015_clusters;
        case T8103:
            return t8103_clusters;
        case T6000:
        case T6001:
            return t6000_clusters;
        case T6002:
            return t6002_clusters;
        case T8112:
            return t8112_clusters;
        case T6020:
        case T6021:
            return t6020_clusters;
        case T6022:
            return t6022_clusters;
        case T6030:
        case T8122:
            return t6030_clusters;
        case T8142:
            return t8142_clusters;
        case T6031:
        case T6034:
            return t6031_clusters;
        case T6032:
            return t6032_clusters;
        default:
            printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
            return NULL;
    }
}

static const struct feat_t s5l8960x_features[] = {
    {},
};

static const struct feat_t s8000_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M1_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {},
};

static const struct feat_t t8010_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M1_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {},
};

static const struct feat_t t8015_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M1_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

static const struct feat_t t8103_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M1_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {"ppt-thrtl", 0x48400, 0, BIT(63), 0, false},
    {"llc-thrtl", 0x40240, 0, BIT(63), 0, false},
    {"amx-thrtl", 0x40250, 0, BIT(63), 0, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

static const struct feat_t t8112_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M2_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {"ppt-thrtl", 0x40270, 0, BIT(63), 0, false},
    {"ppt-thrtl", 0x48408, 0, BIT(63), 0, false},
    {"ppt-thrtl", 0x48b30, 0, BIT(0), 0, true},
    {"ppt-thrtl", 0x20078, 0, BIT(0), 0, true},
    {"ppt-thrtl", 0x48400, 0, BIT(63), 0, false},
    {"amx-thrtl", 0x40250, 0, BIT(63), 0, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

static const struct feat_t t8122_features[] = {
    {"ppt-thrtl", 0x48400, 0, BIT(63), 0, false},
    {"ppt-thrtl", 0x48408, 0, BIT(63), 0, false},
    {"llc-thrtl", 0x40270, 0, BIT(63), 0, false},
    {"amx-thrtl", 0x40250, 0, BIT(63), 0, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

static const struct feat_t t6020_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M2_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {"ppt-thrtl", 0x48400, 0, BIT(63), 0, false},
    {"llc-thrtl", 0x40270, 0, BIT(63), 0, false},
    {"amx-thrtl", 0x40250, 0, BIT(63), 0, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

static const struct feat_t t6030_features[] = {
    {"cpu-apsc", CLUSTER_PSTATE, CLUSTER_PSTATE_M2_APSC_DIS, 0, CLUSTER_PSTATE_APSC_BUSY, false},
    {"ppt-thrtl", 0x48400, 0, BIT(63), 0, false},
    {"ppt-thrtl", 0x48408, 0, BIT(63), 0, false},
    {"llc-thrtl", 0x40270, 0, BIT(63), 0, false},
    {"amx-thrtl", 0x40250, 0, BIT(63), 0, false},
    {"cpu-fixed-freq-pll-relock", CLUSTER_PSTATE, 0, CLUSTER_PSTATE_FIXED_FREQ_PLL_RECLOCK, 0,
     false},
    {},
};

/* M5 uses CLPC/PMP; the earlier chips' feature registers are not applicable. */
static const struct feat_t t8142_features[] = {
    {},
};

const struct feat_t *cpufreq_get_features(void)
{
    switch (chip_id) {
        case S5L8960X:
        case T7000:
        case T7001:
            return s5l8960x_features;
        case S8000:
        case S8001:
        case S8003:
            return s8000_features;
        case T8010:
        case T8011:
        case T8012:
            return t8010_features;
        case T8015:
            return t8015_features;
        case T8103:
        case T6000:
        case T6001:
        case T6002:
            return t8103_features;
        case T8112:
            return t8112_features;
        case T8122:
            return t8122_features;
        case T8142:
            return t8142_features;
        case T6020:
        case T6021:
        case T6022:
            return t6020_features;
        case T6030 ... T6034:
            return t6030_features;
        default:
            printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
            return NULL;
    }
}

/* On T8152 (26A428 firmware) a perf-state request uses logical offset
 * 0xe20020. E/P ACC maps resolve that to RegMap 0x20/0x40 + 0x20. The ADT
 * supplies the physical apertures and the hardware-index bias (2 on J873g).
 * Do not apply earlier-generation voltage/PLL/chicken-bit initialization. */
struct t8152_clock {
    u64 reg;
    const u32 *opps;
    u32 count;
    bool failed;
};

static struct t8152_clock t8152_clocks[2];
static u32 t8152_state0;
static bool t8152_clocks_ready;

static int t8152_cpufreq_init(void)
{
    if (t8152_clocks_ready)
        return 0;
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/pmgr-child", path);
    if (chip_id != T8152 || board_id != 0x24 || !adt_is_compatible(adt, 0, "J873gAP") || node < 0 ||
        !adt_is_compatible(adt, node, "pmgr2,t8152"))
        return -1;
    u32 len;
    const u16 *maps = adt_getprop(adt, node, "reg-maps", &len);
    if (!maps || len % 4 || ADT_GETPROP(adt, node, "cpu-perf-state0-index", &t8152_state0) < 0 ||
        t8152_state0 != 2)
        return -1;
    const char *tables[] = {"voltage-states1-sram", "voltage-states5-sram"};
    for (unsigned int c = 0; c < 2; c++) {
        int idx = -1;
        for (u32 i = 0; i < len / 2; i += 2) {
            if (maps[i + 1] == (c ? 0x40 : 0x20)) {
                if (idx >= 0)
                    return -1;
                idx = maps[i];
            }
        }
        u64 base, size;
        if (idx < 0 || adt_get_reg(adt, path, "reg", idx, &base, &size) || size < 0x28 ||
            base != (c ? 0x211e20000ULL : 0x210e20000ULL))
            return -1;
        u32 table_len;
        const u32 *opps = adt_getprop(adt, node, tables[c], &table_len);
        if (!opps || !table_len || table_len % 8 || table_len / 8 + t8152_state0 > 32)
            return -1;
        for (u32 i = 0; i < table_len / 8; i++) {
            if (!opps[2 * i] || opps[2 * i] > 5000000 || (i && opps[2 * i] <= opps[2 * (i - 1)]))
                return -1;
        }
        t8152_clocks[c] =
            (struct t8152_clock){.reg = base + 0x20, .opps = opps, .count = table_len / 8};
    }
    t8152_clocks_ready = true;
    return 0;
}

__attribute__((used, retain, noinline)) u64 cpufreq_get_cluster_hz(unsigned int cluster)
{
    if (chip_id == T6040)
        return cpufreq_t6040_get_hz(cluster);
    if (cluster >= 2 || t8152_cpufreq_init())
        return 0;
    struct t8152_clock *c = &t8152_clocks[cluster];
    u64 raw = read64(c->reg);
    u32 state = raw & 31;
    if ((raw & CLUSTER_PSTATE_BUSY) || state < t8152_state0 || state - t8152_state0 >= c->count)
        return 0;
    return (u64)c->opps[2 * (state - t8152_state0)] * 1000;
}

__attribute__((used, retain, noinline)) int cpufreq_set_cluster_pstate(unsigned int cluster,
                                                                       unsigned int pstate)
{
    if (chip_id == T6040)
        return cpufreq_t6040_set_pstate(cluster, pstate);
    if (cluster >= 2 || t8152_cpufreq_init())
        return -1;
    struct t8152_clock *c = &t8152_clocks[cluster];
    if (c->failed || pstate < t8152_state0 || pstate - t8152_state0 >= c->count)
        return -1;
    /* Matching firmware waits before and after changing low 5 request bits. */
    if (poll64(c->reg, CLUSTER_PSTATE_BUSY, 0, 2000))
        goto failed;
    u64 raw = read64(c->reg);
    if ((raw & 31) == pstate)
        return 0;
    write64(c->reg, (raw & ~31ULL) | CLUSTER_PSTATE_SET | pstate);
    if (poll64(c->reg, CLUSTER_PSTATE_BUSY, 0, 2000))
        goto failed;
    if ((read64(c->reg) & 31) == pstate)
        return 0;
failed:
    c->failed = true;
    return -1;
}

/*
 * iBoot hands T8140 over with the P cluster at P1 (744 MHz), where inflating a
 * 45 MB kernel Image takes ~2.3 s. cpufreq_init() leaves T8140 alone, so raise
 * only the P cluster while payloads are loaded and put iBoot's P-state back
 * before the SMP bring-up and the kernel handoff.
 */
#define T8140_PAYLOAD_BOOST_PSTATE 8

static const struct cluster_t t8140_pcluster = {"PCPU", 0x211e00000, true, 0, 0, 0};
static u32 t8140_inherited_pstate;
static bool t8140_boosted;

void cpufreq_payload_boost(bool enable)
{
    if (chip_id != T8140 || enable == t8140_boosted)
        return;

    if (enable)
        t8140_inherited_pstate = pstate_reg_to_pstate(read64(t8140_pcluster.base + CLUSTER_PSTATE));

    u32 pstate = enable ? T8140_PAYLOAD_BOOST_PSTATE : t8140_inherited_pstate;
    if (set_pstate(&t8140_pcluster, pstate))
        printf("cpufreq: payload boost %d failed on %s\n", enable, t8140_pcluster.name);

    /* Even after a failed raise, try to restore the inherited state. */
    t8140_boosted = enable;
}

int cpufreq_init(void)
{
    if (chip_id == T6040)
        return cpufreq_t6040_init();
    if (chip_id == T8140) {
        printf("cpufreq: skipping T8140 P-state hold pending register evidence\n");
        return 0;
    }

    printf("cpufreq: Initializing clusters\n");

    const struct cluster_t *cluster = cpufreq_get_clusters();
    const struct feat_t *features = cpufreq_get_features();

    if (!cluster || !features)
        return -1;

    /* Without this, CLUSTER_PSTATE_BUSY gets stuck */
    if (chip_id == T8012 || chip_id == T8015)
        pmgr_power_on(0, "SPMI");

    bool err = false;
    while (cluster->base) {
        err |= cpufreq_init_cluster(cluster++, features);
    }

    return err ? -1 : 0;
}

void cpufreq_fixup(void)
{
    if (chip_id == T6040) {
        printf("cpufreq: T6040 retains firmware clock policy; low-state proxy tests available\n");
        return;
    }
    if (chip_id == T8140)
        return;

    const struct cluster_t *cluster = cpufreq_get_clusters();

    if (!cluster)
        return;

    while (cluster->base) {
        cpufreq_fixup_cluster(cluster++);
    }
}
