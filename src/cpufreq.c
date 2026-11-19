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
/* Bound for the generations that verify each request (2 ms, as Linux's T8152/T6050 bound). */
#define CLUSTER_CHECKED_SWITCH_TIMEOUT 2000

/* The ADT's per-CPU cpm-impl-reg window sits at this offset in its cluster's register block. */
#define CLUSTER_CPM_IMPL_OFFSET 0x40000

#define CLUSTER_MAX 8

struct cluster_t {
    const char *name;
    u64 base;
    bool pcluster;
    uint32_t apsc_pstate;
    uint32_t default_pstate;
    uint32_t voltage_ctl;
    /* PMGR ADT property with this cluster's [frequency, mV] ladder, where the ADT has one. */
    const char *ladder;
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
        case T8152:
        case T6040:
        case T6041:
            return FIELD_GET(CLUSTER_PSTATE_DESIRED1, val);
        default:
            printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
            return 0;
    }
}

static bool has_ps2(void)
{
    switch (chip_id) {
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
            return true;
        default:
            return false;
    }
}

/*
 * The command register of these generations reads back the request it accepted (Linux's
 * apple-soc-cpufreq verifies each transition on them), so a request is only made once the
 * previous one is done, and checked after.
 */
static bool requests_checked(void)
{
    switch (chip_id) {
        case T8140:
        case T8142:
        case T8152:
        case T6040:
        case T6041:
            return true;
        default:
            return false;
    }
}

/*
 * Request pstate. On chips with a second request field it gets ps2; with ps2 == pstate the
 * register is left alone when the first field already holds pstate.
 */
static int set_pstate_ps2(const struct cluster_t *cluster, uint32_t pstate, uint32_t ps2)
{
    bool checked = requests_checked();
    u32 timeout = checked ? CLUSTER_CHECKED_SWITCH_TIMEOUT : CLUSTER_SWITCH_TIMEOUT;

    if (checked && poll64(cluster->base + CLUSTER_PSTATE, CLUSTER_PSTATE_BUSY, 0, timeout) < 0) {
        printf("cpufreq: Cluster %s is busy before P-State request\n", cluster->name);
        return -1;
    }

    u64 val = read64(cluster->base + CLUSTER_PSTATE);

    if (pstate_reg_to_pstate(val) != pstate ||
        (ps2 != pstate && has_ps2() && FIELD_GET(CLUSTER_PSTATE_DESIRED2, val) != ps2)) {
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
                val |= CLUSTER_PSTATE_SET | FIELD_PREP(CLUSTER_PSTATE_DESIRED2, ps2);
                /* fallthrough */
            case T8112:
            case T6020:
            case T6021:
            case T6022:
            case T6030 ... T6034:
            case T8122:
            case T8142:
            case T8140:
            case T8152:
            case T6040:
            case T6041:
                val &= ~CLUSTER_PSTATE_DESIRED1;
                val |= CLUSTER_PSTATE_SET | FIELD_PREP(CLUSTER_PSTATE_DESIRED1, pstate);
                break;
            default:
                printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
                return -1;
        }
        write64(cluster->base + CLUSTER_PSTATE, val);
        if (poll64(cluster->base + CLUSTER_PSTATE, CLUSTER_PSTATE_BUSY, 0, timeout) < 0) {
            printf("cpufreq: Timed out waiting for cluster %s P-State switch\n", cluster->name);
            return -1;
        }
    }

    if (checked && pstate_reg_to_pstate(read64(cluster->base + CLUSTER_PSTATE)) != pstate) {
        printf("cpufreq: Cluster %s did not accept P-State %u\n", cluster->name, pstate);
        return -1;
    }

    return 0;
}

static int set_pstate(const struct cluster_t *cluster, uint32_t pstate)
{
    return set_pstate_ps2(cluster, pstate, pstate);
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
        /*
         * Keep firmware voltage, PLL and throttling configuration: the 0x440f8/APSC snooze
         * writes below are not established on these generations.
         */
        case T8140:
        case T8142:
        case T8152:
        case T6040:
        case T6041:
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

    // Older versions of aura stage 1 erroneously cleared CLUSTER_PSTATE_UNK_Mx, so put it back for
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

/*
 * From T8132 on, every ADT CPU node's cpm-impl-reg is its cluster block + 0x40000, as on T8103:
 * J700 (T8140) and J813 (T8142) 0x210e40000/0x211e40000, J773s/J616c (T6040/T6041) also
 * 0x212e40000, J873 (T8152) 0x210e40000/0x211e40000. cluster_matches_adt() checks it. Ladders are
 * the PMGR voltage-statesN-sram properties those ADTs carry (none on T8142).
 */

/* J813 firmware handoff states: ECPU 972 MHz, PCPU 3516 MHz. */
static const struct cluster_t t8142_clusters[] = {
    {"ECPU0", 0x210e00000, false, 1, 2},
    {"PCPU0", 0x211e00000, true, 1, 10},
    {},
};

/*
 * iBoot hands the J700 P cluster over at P1, the bottom of its ladder (744 MHz). Hold P8, the
 * state J700 runs payload loads at, until Linux's cpufreq driver takes over; the E cluster keeps
 * the state firmware left. No APSC reset state: that sequence is not established on T8140.
 */
static const struct cluster_t t8140_clusters[] = {
    {"ECPU0", 0x210e00000, false, 0, 0, 0, "voltage-states1-sram"},
    {"PCPU0", 0x211e00000, true, 0, 8, 0, "voltage-states5-sram"},
    {},
};

/* Firmware's states are kept; the clusters are listed for the proxy and the payload code. */
static const struct cluster_t t6040_clusters[] = {
    {"ECPU0", 0x210e00000, false, 0, 0, 0, "voltage-states1-sram"},
    {"PCPU0", 0x211e00000, true, 0, 0, 0, "voltage-states5-sram"},
    {"PCPU1", 0x212e00000, true, 0, 0, 0, "voltage-states13-sram"},
    {},
};

static const struct cluster_t t8152_clusters[] = {
    {"ECPU0", 0x210e00000, false, 0, 0, 0, "voltage-states1-sram"},
    {"PCPU0", 0x211e00000, true, 0, 0, 0, "voltage-states5-sram"},
    {},
};

static const struct cluster_t *chip_clusters(void)
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
        case T8140:
            return t8140_clusters;
        case T6040:
        case T6041:
            return t6040_clusters;
        case T8152:
            return t8152_clusters;
        default:
            return NULL;
    }
}

const struct cluster_t *cpufreq_get_clusters(void)
{
    const struct cluster_t *clusters = chip_clusters();

    if (!clusters)
        printf("cpufreq: Chip 0x%x is unsupported\n", chip_id);
    return clusters;
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

/*
 * M5 uses CLPC/PMP; the earlier chips' feature registers are not applicable. T8140, T6040/T6041
 * and T8152 ADTs name the features (cpu-apsc, ppt-thrtl, llc-thrtl, amx-thrtl,
 * cpu-fixed-freq-pll-relock), but the cluster register offset and bit of each feature have not
 * been established on these generations, so firmware's settings are kept.
 */
static const struct feat_t firmware_features[] = {
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
        case T8140:
        case T8142:
        case T8152:
        case T6040:
        case T6041:
            return firmware_features;
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

/*
 * A cluster whose block the ADT does not place where the table says is not touched. Checked on
 * the generations whose ADTs were compared with the tables above (see there); earlier tables are
 * long established and their ADTs (pre-T8103, T6002's second die) were not compared.
 */
static bool cluster_matches_adt(const struct cluster_t *cluster)
{
    int node = adt_path_offset(adt, "/cpus");
    bool any = false;

    if (!requests_checked() || node < 0)
        return true;
    ADT_FOREACH_CHILD(adt, node)
    {
        u64 reg[2];
        if (ADT_GETPROP_ARRAY(adt, node, "cpm-impl-reg", reg) != sizeof(reg))
            continue;
        any = true;
        if (reg[0] == cluster->base + CLUSTER_CPM_IMPL_OFFSET)
            return true;
    }
    if (any)
        printf("cpufreq: ADT has no CPU at cluster %s 0x%lx\n", cluster->name, cluster->base);
    return !any;
}

struct cluster_ladder {
    const u32 *opps; /* ADT [frequency, mV] pairs */
    u32 count;
    u32 state0; /* request state of the first entry */
    bool hz;    /* frequencies in Hz; kHz otherwise */
};

/*
 * The cluster's ladder from the PMGR ADT node. The first entry is request state 1, or the PMGR's
 * cpu-perf-state0-index where it has one (2 on J873). J700 lists Hz, J616c/J773 kHz.
 */
static int cluster_ladder(const struct cluster_t *cluster, struct cluster_ladder *l)
{
    static const char *const pmgr_paths[] = {"/arm-io/pmgr", "/arm-io/pmgr-child"};
    const u32 *opps = NULL;
    u32 len = 0;
    int node = -1;

    if (!cluster->ladder)
        return -1;
    for (unsigned int i = 0; i < ARRAY_SIZE(pmgr_paths) && !opps; i++) {
        node = adt_path_offset(adt, pmgr_paths[i]);
        if (node >= 0)
            opps = adt_getprop(adt, node, cluster->ladder, &len);
    }
    if (!opps || !len || len % 8)
        return -1;

    u32 state0 = 1;
    if (adt_getprop(adt, node, "cpu-perf-state0-index", NULL) &&
        ADT_GETPROP(adt, node, "cpu-perf-state0-index", &state0) != sizeof(state0))
        return -1;

    *l = (struct cluster_ladder){.opps = opps, .count = len / 8, .state0 = state0};
    if (!state0 ||
        l->state0 + l->count - 1 > FIELD_GET(CLUSTER_PSTATE_DESIRED1, CLUSTER_PSTATE_DESIRED1))
        return -1;
    /* A CPU runs between 100 MHz and 10 GHz: as kHz that is below 10^7, as Hz above. */
    l->hz = opps[0] > 10000000;
    for (u32 i = 0; i < l->count; i++) {
        u32 f = opps[2 * i];
        if (!f || (l->hz ? f < 100000000 : f > 10000000) || (i && f <= opps[2 * (i - 1)]))
            return -1;
    }
    return 0;
}

static bool cluster_failed[CLUSTER_MAX];

/* A cluster of this chip, with its ladder, that the proxy may read and set. */
static const struct cluster_t *proxy_cluster(unsigned int index, struct cluster_ladder *l)
{
    const struct cluster_t *clusters = chip_clusters();

    if (!clusters || index >= CLUSTER_MAX)
        return NULL;
    for (unsigned int i = 0; i < index; i++)
        if (!clusters[i].base)
            return NULL;

    const struct cluster_t *cluster = &clusters[index];
    if (!cluster->base || cluster_failed[index] || !cluster_matches_adt(cluster) ||
        cluster_ladder(cluster, l))
        return NULL;
    return cluster;
}

/* ADT frequency for the accepted request state, not a measured frequency. */
__attribute__((used, retain, noinline)) u64 cpufreq_get_cluster_hz(unsigned int index)
{
    struct cluster_ladder l;
    const struct cluster_t *cluster = proxy_cluster(index, &l);

    if (!cluster)
        return 0;
    u64 raw = read64(cluster->base + CLUSTER_PSTATE);
    u32 state = pstate_reg_to_pstate(raw);
    if ((raw & CLUSTER_PSTATE_BUSY) || state < l.state0 || state - l.state0 >= l.count)
        return 0;
    return (u64)l.opps[2 * (state - l.state0)] * (l.hz ? 1 : 1000);
}

/* A failed request locks the cluster out until the next boot: its state is then unknown. */
__attribute__((used, retain, noinline)) int cpufreq_set_cluster_pstate(unsigned int index,
                                                                       unsigned int pstate)
{
    struct cluster_ladder l;
    const struct cluster_t *cluster = proxy_cluster(index, &l);

    if (!cluster || pstate < l.state0 || pstate - l.state0 >= l.count)
        return -1;
    if (set_pstate(cluster, pstate)) {
        cluster_failed[index] = true;
        return -1;
    }
    return 0;
}

/*
 * Loading payloads (inflating a kernel Image, reading boot= storage) is CPU bound on one core. A
 * P cluster that firmware left below the state cpufreq_init() will hold is raised to that state
 * while payloads load, and put back before SMP bring-up and the handoff (or a chainload, whose
 * next stage expects firmware's state). On J700, where iBoot leaves the P cluster at P1, a 45 MB
 * Image took 2.34 s at P1 and 0.60 s at P8. A second request field (PS2) is left as found.
 *
 * Not on chips whose table programs PMGR voltage control (before T8103): there the cluster's
 * voltage is only set up by cpufreq_init_cluster(), so a raised state is not covered before it.
 */
static struct {
    u32 inherited, ps2;
    /* Set from saving firmware's state until it has been put back: a restore is owed. */
    bool owed;
} boost[CLUSTER_MAX];

static bool boost_supported(const struct cluster_t *clusters)
{
    if (!clusters)
        return false;
    for (const struct cluster_t *c = clusters; c->base; c++)
        if (c->voltage_ctl)
            return false;
    return true;
}

static void cluster_boost(const struct cluster_t *cluster, unsigned int index)
{
    /* Raised already, or a restore is still owed: keep the saved state as it is. */
    if (!cluster->pcluster || !cluster->default_pstate || boost[index].owed)
        return;

    struct cluster_ladder l;
    u32 target = cluster->default_pstate;
    bool ladder = !cluster_ladder(cluster, &l);
    if (cluster->ladder && (!ladder || target < l.state0 || target - l.state0 >= l.count)) {
        printf("cpufreq: %s has no ADT ladder up to P%u, not raising it for payloads\n",
               cluster->name, target);
        return;
    }
    if (!cluster_matches_adt(cluster))
        return;

    if (requests_checked() && poll64(cluster->base + CLUSTER_PSTATE, CLUSTER_PSTATE_BUSY, 0,
                                     CLUSTER_CHECKED_SWITCH_TIMEOUT) < 0) {
        printf("cpufreq: %s is busy, not raising it for payloads\n", cluster->name);
        return;
    }
    u64 raw = read64(cluster->base + CLUSTER_PSTATE);
    u32 inherited = pstate_reg_to_pstate(raw);
    if (!inherited || (ladder && (inherited < l.state0 || inherited - l.state0 >= l.count))) {
        printf("cpufreq: %s inherited P-state %u is not on its ladder, not raising it\n",
               cluster->name, inherited);
        return;
    }
    if (inherited >= target)
        return;

    /* From here a restore is owed, even if the raise below fails. */
    boost[index].inherited = inherited;
    boost[index].ps2 = FIELD_GET(CLUSTER_PSTATE_DESIRED2, raw);
    boost[index].owed = true;
    if (set_pstate_ps2(cluster, target, has_ps2() ? boost[index].ps2 : target))
        printf("cpufreq: raising %s to P%u for payloads failed\n", cluster->name, target);
    else
        printf("cpufreq: %s raised P%u -> P%u for payloads\n", cluster->name, inherited, target);
}

static void cluster_unboost(const struct cluster_t *cluster, unsigned int index)
{
    if (!boost[index].owed)
        return;
    u32 ps2 = has_ps2() ? boost[index].ps2 : boost[index].inherited;
    /*
     * Keep the saved state until it is back: a later raise must not take the raised state for
     * firmware's, and a later restore tries again. A cluster left raised does not stop the boot,
     * since cpufreq_init() and Linux's cpufreq driver set its P-state later.
     */
    if (set_pstate_ps2(cluster, boost[index].inherited, ps2)) {
        printf("cpufreq: putting %s back to P%u failed; it may run raised until Linux sets it\n",
               cluster->name, boost[index].inherited);
        return;
    }
    boost[index].owed = false;
}

void cpufreq_payload_boost(bool enable)
{
    const struct cluster_t *clusters = chip_clusters();

    if (!boost_supported(clusters))
        return;
    for (unsigned int i = 0; i < CLUSTER_MAX && clusters[i].base; i++) {
        if (enable)
            cluster_boost(&clusters[i], i);
        else
            cluster_unboost(&clusters[i], i);
    }
}

int cpufreq_init(void)
{
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
        if (!cluster_matches_adt(cluster))
            err = true;
        else
            err |= cpufreq_init_cluster(cluster, features);
        cluster++;
    }

    return err ? -1 : 0;
}

void cpufreq_fixup(void)
{
    const struct cluster_t *cluster = cpufreq_get_clusters();

    if (!cluster)
        return;

    while (cluster->base) {
        cpufreq_fixup_cluster(cluster++);
    }
}
