/* SPDX-License-Identifier: GPL-2.0-only */
/* T8152 ATC common/AUX initialization, qualified incrementally on J873g.
 * DP lanes and crossbar clocks are separate stages; common initialization
 * alone does not establish a link. */
#include "atc_phy.h"
#include "dptx_phy.h"
#include "malloc.h"
#include "string.h"

#include "../ace3.h"
#include "../adt.h"
#include "../soc.h"
#include "../tps6598x.h"
#include "../utils.h"

#define ATC_CFG              0x60000
#define ATC_AON              0x1e00
#define ATC_XBAR             0x74000
#define ATC_TIMEOUT          10000
#define ATC_ALT_MODE_TIMEOUT 2000000
#define ATC_ALT_MODE_POLL_MS 50
#define ATC_SAVED_MAX        192
#define ATC_PSD58            0x303a13a00ULL

/* Gap between consecutive PHY control steps that have no status bit: lane
 * clamp, power and isolation fields in AON 0x1e18/0x1e1c, the PMA clock and
 * AUX sleep requests in AON 0x24, the CFG clock, reset and sleep controls,
 * and the crossbar enable writes ahead of their acknowledgement polls. Steps
 * that do report completion are polled instead (CFG status, crossbar acks,
 * PSD). */
#define ATC_STEP_DELAY_US 1

/* Settle time after the DPTX enable in 0x2040 is withdrawn, before the
 * transmitter control bits in 0x2000 are cleared. No status bit is known. */
#define ATC_DPTX_DISABLE_SETTLE_US 5

/* Wait after the DCP0 -> DFP0 route is configured, before the DCP continues
 * link training. The route start already polls every crossbar acknowledgement;
 * what this additionally waits for is not known. Kept as qualified on J873g
 * (tracking: aura#344). */
#define ATC_ROUTE_SETTLE_MS 5

struct atc_saved {
    u32 offset;
    u32 value;
    u32 mask;
};

struct atc_phy {
    int node;
    u64 base;
    bool swap;
    bool started;
    bool aux_started;
    bool active;
    bool dp_started;
    bool clock_owned;
    bool route_started;
    u64 dp_clock;
    u32 saved_clock;
    u32 link_rate;
    unsigned int gates;
    unsigned int saved_count;
    struct atc_saved saved[ATC_SAVED_MAX];
};

struct atc_tunable {
    const char *name;
    u32 offset, size;
    bool optional;
};

/* The ADT tunables applied by the common initialization, in order. */
static const struct atc_tunable tunables[] = {
    {"tunable_ACIOPHY_CMN_DFLT", 0x800, 0x6c, false},
    {"tunable_CIOPLL_CORE_DFLT", 0x1600, 0x280, false},
    {"tunable_ATC_FABRIC", 0x6c000, 0x198, false},
    {"tunable_ATC_COMMON_CFG", 0x78000, 0x74, true},
    {"tunable_DPPLL_CORE_DFLT", 0x1000, 0x280, false},
    {"tunable_DPPLL_TOP_DFLT", 0xe00, 0x200, false},
    {"tunable_ASDC_AUX_SHM_DFLT", 0x2300, 0x14, false},
};

static const u64 gate_addr[] = {0x3082901c0ULL, ATC_PSD58, 0x3082900c0ULL, 0x3082900e0ULL};
static const u32 gate_idle[] = {0x300, 6, 0x244, 0x244};

static int atc_tunable_check(atc_phy_t *phy, unsigned int index, bool apply);

static bool atc_crossbar_check(void)
{
    int trace[8];
    int node = adt_path_offset_trace(adt, "/arm-io/atc3-dpxbar", trace);
    u64 base, size;
    if (node < 0 || !adt_is_compatible(adt, node, "atc-dpxbar,t8142") ||
        adt_get_reg(adt, trace, "reg", 0, &base, &size) || base != 0x413074000ULL || size != 0x4000)
        return false;
    u32 len;
    const u32 *handle = adt_getprop(adt, node, "AAPL,phandle", &len);
    if (!handle || len != 4)
        return false;
    node = adt_path_offset(adt, "/arm-io/atc3-dpphy");
    if (node < 0 || !adt_is_compatible(adt, node, "atc-dpphy,t8152"))
        return false;
    const u32 *parent = adt_getprop(adt, node, "dpxbar-parent", &len);
    if (!parent || len != 4 || *parent != *handle)
        return false;
    const u32 *port = adt_getprop(adt, node, "dp-switch-dfp-port", &len);
    if (!port || len != 4 || *port != 0)
        return false;
    const u32 *endpoint = adt_getprop(adt, node, "dp-switch-dfp-endpoint", &len);
    return endpoint && len == 4 && *endpoint == 3;
}

/* Resolve ATC3/DP0 through PMGR2's map and group tables before any write. */
static u64 atc_dp_clock(int node)
{
    const u32 expected_link[] = {0x00020004, 0x44, 8, 4};
    u32 len = 0;
    const void *link = adt_getprop(adt, node, "atc-dp-link", &len);
    if (!link || len != sizeof(expected_link) || memcmp(link, expected_link, len))
        return 0;
    struct group {
        u16 id, map;
        u32 offset;
    };
    const struct group *groups = adt_getprop(adt, node, "reg-groups", &len);
    if (!groups || len % sizeof(*groups))
        return 0;
    u32 offset = 0;
    int map = -1;
    for (u32 i = 0; i < len / sizeof(*groups); i++) {
        if (groups[i].id == 0x20) {
            map = groups[i].map;
            offset = groups[i].offset;
            break;
        }
    }
    struct mapping {
        u16 reg, id;
    };
    const struct mapping *maps = adt_getprop(adt, node, "reg-maps", &len);
    if (map < 0 || !maps || len % sizeof(*maps))
        return 0;
    int reg = -1;
    for (u32 i = 0; i < len / sizeof(*maps); i++)
        if (maps[i].id == map) {
            reg = maps[i].reg;
            break;
        }
    int trace[8];
    u64 base, size;
    if (reg < 0 || adt_path_offset_trace(adt, "/arm-io/pmgr-child", trace) != node ||
        adt_get_reg(adt, trace, "reg", reg, &base, &size) || (u64)offset + 0x44 + 3 * 8 + 4 > size)
        return 0;
    u64 addr = base + offset + 0x44 + 3 * 8;
    return addr == 0x30002005cULL ? addr : 0;
}

static int atc_update(atc_phy_t *phy, u32 offset, u32 mask, u32 value)
{
    if ((offset & 3) || offset >= 0x84000 || (value & ~mask))
        return -1;

    u32 before = read32(phy->base + offset);
    unsigned int i;
    for (i = 0; i < phy->saved_count; i++)
        if (phy->saved[i].offset == offset)
            break;
    if (i == phy->saved_count) {
        if (i == ATC_SAVED_MAX)
            return -1;
        phy->saved[i] = (struct atc_saved){offset, before, 0};
        phy->saved_count++;
    }
    phy->saved[i].mask |= mask;
    write32(phy->base + offset, (before & ~mask) | value);
    if ((read32(phy->base + offset) & mask) != value) {
        printf("ATC3: register readback failed at %#x\n", offset);
        return -1;
    }
    return 0;
}

static int atc_tunable_check(atc_phy_t *phy, unsigned int index, bool apply)
{
    const struct atc_tunable *t = &tunables[index];
    u32 len = 0;
    const u32 *p = adt_getprop(adt, phy->node, t->name, &len);
    if (!p)
        return t->optional ? 0 : -1;
    if (!len || len % 12)
        return -1;
    for (u32 i = 0; i < len / 4; i += 3) {
        u32 offset = p[i] & 0x7ffffff;
        /* The live tables use only the qualified 32-bit masked-write opcode. */
        if (p[i] >> 27 != 4 || (offset & 3) || offset + 4 > t->size || (p[i + 2] & ~p[i + 1]))
            return -1;
        if (apply && atc_update(phy, t->offset + offset, p[i + 1], p[i + 2]))
            return -1;
    }
    return 0;
}

static int atc_psd_wait(u32 state)
{
    u64 deadline = timeout_calculate(ATC_TIMEOUT);
    do {
        if ((read32(ATC_PSD58 + 0xbc) & 1) && (read32(ATC_PSD58 + 0x80) & 7) == state)
            return 0;
        udelay(1);
    } while (!timeout_expired(deadline));
    return -1;
}

static int atc_gates_restore(atc_phy_t *phy)
{
    int ret = 0;
    while (phy->gates) {
        unsigned int i = --phy->gates;
        if (i == 1) {
            write32(ATC_PSD58 + 0x90, gate_idle[i]);
            if (atc_psd_wait(gate_idle[i])) {
                printf("ATC3: PSD restore status=%#x ack=%#x\n", read32(ATC_PSD58 + 0x80),
                       read32(ATC_PSD58 + 0xbc));
                ret = -1;
            }
        } else {
            write32(gate_addr[i], gate_idle[i]);
            int waited = poll32(gate_addr[i], 0xf0, (gate_idle[i] & 15) << 4, ATC_TIMEOUT);
            /* WAS_CLKGATED can settle after ACTUAL reaches the requested mode.
             * ATC3 AON reports 0x100 before reaching the saved 0x300 state. */
            if (!waited)
                waited = poll32(gate_addr[i], 0xffffffff, gate_idle[i], ATC_TIMEOUT);
            u32 actual = read32(gate_addr[i]);
            if (waited || actual != gate_idle[i]) {
                printf("ATC3: gate %u restore wait=%d expected=%#x actual=%#x\n", i, waited,
                       gate_idle[i], actual);
                ret = -1;
            }
        }
    }
    return ret;
}

static int atc_gates_enable(atc_phy_t *phy)
{
    /* Qualified cold state only. Never take ownership of an active port. */
    for (unsigned int i = 0; i < 4; i++) {
        u32 value = i == 1 ? read32(ATC_PSD58 + 0x80) & 7 : read32(gate_addr[i]);
        if (value != gate_idle[i]) {
            printf("ATC3: power dependency %u is not idle (%#x)\n", i, value);
            return -1;
        }
    }
    for (unsigned int i = 0; i < 4; i++) {
        phy->gates++;
        if (i == 1) {
            write32(ATC_PSD58 + 0x90, 0);
            if (atc_psd_wait(0))
                goto fail;
        } else {
            write32(gate_addr[i], (gate_idle[i] & 0xfffffcf0) | 15);
            if (poll32(gate_addr[i], 0xf0, 0xf0, ATC_TIMEOUT))
                goto fail;
        }
    }
    return 0;
fail:
    atc_gates_restore(phy);
    return -1;
}

atc_phy_t *atc_phy_init(const char *path)
{
    int trace[8];
    int node = adt_path_offset_trace(adt, path, trace);
    int pmgr = adt_path_offset(adt, "/arm-io/pmgr-child");
    u64 base, size;
    const u32 expected_gates[] = {173, 58, 188, 189};
    u32 len = 0;
    if (chip_id != T8152 || board_id != 0x24 || !adt_is_compatible(adt, 0, "J873gAP") || node < 0 ||
        pmgr < 0 || !adt_is_compatible(adt, node, "atc-phy,t8152") ||
        !adt_is_compatible(adt, pmgr, "pmgr2,t8152") ||
        adt_get_reg(adt, trace, "reg", 2, &base, &size) || base != 0x413000000ULL ||
        size != 0x84000)
        return NULL;
    const void *gates = adt_getprop(adt, node, "service-gates", &len);
    if (!gates || len != sizeof(expected_gates) || memcmp(gates, expected_gates, len))
        return NULL;

    int hpm = adt_path_offset(adt, "/arm-io/nub-spmi-a0/hpm3");
    const u32 *reg = hpm < 0 ? NULL : adt_getprop(adt, hpm, "reg", &len);
    if (!reg || len < 4 || reg[0] != 8)
        return NULL;
    spmi_dev_t *spmi = spmi_init("/arm-io/nub-spmi-a0");
    if (!spmi)
        return NULL;
    /* iBoot hands the port controllers over asleep: their registers read back as
     * unimplemented or zero until an SPMI wakeup, and they sit in power state 7.
     * Wake hpm3 and request S0 as USB initialization does, then wait: on a fresh
     * boot the controller reports the attached partner only after S0, and a
     * DisplayPort partner also needs to finish alternate-mode entry. Release the
     * controller the same way USB initialization does. */
    tps6598x_dev_t *tps = tps6598x_init_spmi("/arm-io/nub-spmi-a0/hpm3", spmi);
    if (!tps) {
        spmi_shutdown(spmi);
        printf("ATC3: back-right USB-C controller did not wake\n");
        return NULL;
    }
    if (tps6598x_powerup(tps) < 0)
        printf("ATC3: back-right USB-C controller did not enter S0\n");
    u8 mode[4], status[8], data[8];
    int m, s, d;
    u32 data_status = 0;
    u64 deadline = timeout_calculate(ATC_ALT_MODE_TIMEOUT);
    for (;;) {
        memset(status, 0, sizeof(status));
        memset(data, 0, sizeof(data));
        m = ace3_read(spmi, 8, ACE3_REG_MODE, mode, sizeof(mode));
        s = ace3_read(spmi, 8, ACE3_REG_STATUS, status, sizeof(status));
        d = ace3_read(spmi, 8, TPS6598X_REG_DATA_STATUS, data, sizeof(data));
        if (d >= 4)
            memcpy(&data_status, data, sizeof(data_status));
        if (m != 4 || memcmp(mode, "APP ", 4) || s < 4 || d < 4 ||
            ((status[0] & TPS6598X_STATUS_PLUG_PRESENT) &&
             (data_status & TPS6598X_DATA_DP_CONNECTION)) ||
            timeout_expired(deadline))
            break;
        mdelay(ATC_ALT_MODE_POLL_MS);
    }
    tps6598x_shutdown(tps);
    spmi_shutdown(spmi);
    if (m != 4 || memcmp(mode, "APP ", 4) || s < 4 || d < 4) {
        printf("ATC3: back-right USB-C controller state is unreadable\n");
        return NULL;
    }
    if (!(status[0] & TPS6598X_STATUS_PLUG_PRESENT)) {
        printf("ATC3: nothing is plugged into the back-right USB-C port\n");
        return NULL;
    }
    if (!(data_status & TPS6598X_DATA_DP_CONNECTION) ||
        data_status & (TPS6598X_DATA_USB3_CONNECTION | TPS6598X_DATA_USB4_CONNECTION |
                       TPS6598X_DATA_TBT_CONNECTION)) {
        printf("ATC3: back-right USB-C partner is not a DisplayPort-only sink (data status %#x)\n",
               data_status);
        return NULL;
    }

    atc_phy_t *phy = calloc(1, sizeof(*phy));
    if (!phy)
        return NULL;
    phy->node = node;
    phy->base = base;
    phy->swap = !!(status[0] & TPS6598X_STATUS_PLUG_UPSIDE_DOWN);
    phy->dp_clock = atc_dp_clock(pmgr);
    if (!phy->dp_clock || !atc_crossbar_check()) {
        free(phy);
        return NULL;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(tunables); i++) {
        if (atc_tunable_check(phy, i, false)) {
            printf("ATC3: unsupported tuning table %s\n", tunables[i].name);
            free(phy);
            return NULL;
        }
    }
    printf("ATC3: direct four-lane DP, reversed=%d\n", phy->swap);
    return phy;
}

static int atc_clock_write(atc_phy_t *phy, u32 value)
{
    mask32(phy->dp_clock, 0x0f000000, value & 0x0f000000);
    if (poll32(phy->dp_clock, BIT(30), 0, ATC_TIMEOUT) ||
        (read32(phy->dp_clock) & 0x0f000000) != (value & 0x0f000000))
        return -1;
    return 0;
}

static int atc_restore(atc_phy_t *phy, struct atc_saved *saved)
{
    if (!saved->mask)
        return 0;
    u64 addr = phy->base + saved->offset;
    mask32(addr, saved->mask, saved->value & saved->mask);
    u32 actual = read32(addr);
    if ((actual ^ saved->value) & saved->mask) {
        printf("ATC3: restore %#x mask %#x expected %#x got %#x\n", saved->offset, saved->mask,
               saved->value, actual);
        return -1;
    }
    saved->mask = 0;
    return 0;
}

/* DCP0/UFP0 -> ATC3/DFP0, PCLK1. The local DFP index is zero, not three. */
static int atc_route_stop(atc_phy_t *phy)
{
    if (!phy->route_started)
        return 0;
    if (atc_update(phy, ATC_XBAR + 0x3c, 1, 0) || atc_update(phy, ATC_XBAR, 1, 0) ||
        atc_update(phy, ATC_XBAR + 0xc, 1, 0) || atc_update(phy, ATC_XBAR + 0x24, 0x100, 0))
        return -1;
    udelay(ATC_STEP_DELAY_US);
    if (poll32(phy->base + ATC_XBAR + 0x800, 1, 0, ATC_TIMEOUT) ||
        poll32(phy->base + ATC_XBAR + 0x808, 1, 0, ATC_TIMEOUT) ||
        poll32(phy->base + ATC_XBAR + 0x81c, 0x100, 0, ATC_TIMEOUT)) {
        printf("ATC3: crossbar stop acknowledgement timeout\n");
        return -1;
    }
    if (atc_update(phy, ATC_XBAR + 8, 1, 0) || atc_update(phy, ATC_XBAR + 0x20, 7, 0) ||
        atc_update(phy, ATC_XBAR + 0x38, 0x700, 0) || atc_update(phy, ATC_XBAR + 4, 1, 1) ||
        atc_update(phy, ATC_XBAR + 0x1c, 1, 1) || atc_update(phy, ATC_XBAR + 0x34, 0x100, 0x100))
        return -1;
    for (unsigned int i = phy->saved_count; i > 0; i--) {
        struct atc_saved *s = &phy->saved[i - 1];
        /* This output latch ignores an ordinary clear and stays set after
         * teardown; it is not a reversible configuration field. */
        if (s->offset == ATC_XBAR + 0x4c) {
            s->mask = 0;
            continue;
        }
        if (s->offset >= ATC_XBAR && s->offset < ATC_XBAR + 0x50 && atc_restore(phy, s))
            return -1;
    }
    phy->route_started = false;
    printf("ATC3: display route disabled, configuration restored\n");
    return 0;
}

static int atc_route_start(atc_phy_t *phy)
{
    if (phy->route_started)
        return 0;
    u64 base = phy->base + ATC_XBAR;
    /* Reject an existing owner of this source or destination. */
    if ((read32(base) | read32(base + 0xc) | read32(base + 0x3c)) & 1 ||
        read32(base + 0x24) & 0x100 || read32(base + 0x20) & 7 || read32(base + 0x38) & 0x700) {
        printf("ATC3: display route is already in use\n");
        return -1;
    }
    phy->route_started = true;
    /* Both DFP0 mux fields select DCP0. T8142 needs no cycle-slip workaround. */
    if (atc_update(phy, ATC_XBAR + 0x44, 0x00f00000, 0) ||
        atc_update(phy, ATC_XBAR + 0x44, 0x00000f00, 0) || atc_update(phy, ATC_XBAR + 4, 1, 0) ||
        atc_update(phy, ATC_XBAR + 0x1c, 1, 0) || atc_update(phy, ATC_XBAR + 0x34, 0x100, 0))
        goto fail;
    udelay(ATC_STEP_DELAY_US);
    if (poll32(base + 0x804, 1, 0, ATC_TIMEOUT) || poll32(base + 0x818, 1, 0, ATC_TIMEOUT) ||
        poll32(base + 0x82c, 0x100, 0, ATC_TIMEOUT)) {
        printf("ATC3: crossbar reset acknowledgement timeout\n");
        goto fail;
    }
    if (atc_update(phy, ATC_XBAR + 8, 1, 1) || atc_update(phy, ATC_XBAR + 0x20, 7, 1) ||
        atc_update(phy, ATC_XBAR + 0x38, 0x700, 0x100) || atc_update(phy, ATC_XBAR + 0x48, 1, 0) ||
        atc_update(phy, ATC_XBAR, 1, 1) || atc_update(phy, ATC_XBAR + 0xc, 1, 1) ||
        atc_update(phy, ATC_XBAR + 0x24, 0x100, 0x100) ||
        atc_update(phy, ATC_XBAR + 0x4c, 0x100, 0x100) || atc_update(phy, ATC_XBAR + 0x3c, 1, 1))
        goto fail;
    printf("ATC3: DCP0 to local DFP0 route configured\n");
    return 0;
fail:
    atc_route_stop(phy);
    return -1;
}

static int atc_dp_stop(atc_phy_t *phy)
{
    if (!phy->dp_started)
        return 0;
    if (atc_route_stop(phy))
        return -1;
    /* Stop the shim clock before withdrawing the PMGR source. */
    if (atc_update(phy, ATC_CFG + 0x14, 7, 7) || atc_update(phy, ATC_CFG + 0x18, 1, 0) ||
        poll32(phy->base + ATC_CFG + 0x1c, 1, 0, ATC_TIMEOUT)) {
        printf("ATC3: DPTX clock stop failed\n");
        return -1;
    }
    if (atc_update(phy, 0x2040, BIT(16), 0))
        return -1;
    udelay(ATC_DPTX_DISABLE_SETTLE_US);
    if (atc_update(phy, 0x2000, BIT(15), 0) || atc_update(phy, 0x2000, BIT(13), 0) ||
        atc_update(phy, 0x2000, BIT(14), 0) || atc_update(phy, 0x2000, BIT(2), 0) ||
        atc_update(phy, 0x2000, BIT(3), BIT(3)) || atc_update(phy, 0xe10, BIT(2), BIT(2)) ||
        atc_update(phy, 0xe08, BIT(29), BIT(29)))
        return -1;
    if (phy->clock_owned) {
        if (atc_clock_write(phy, phy->saved_clock))
            return -1;
        phy->clock_owned = false;
    }
    /* Lane register banks disappear in deep sleep. Restore them while the
     * PMAs remain powered, with the DPTX clock already stopped. */
    for (unsigned int i = phy->saved_count; i > 0; i--) {
        struct atc_saved *s = &phy->saved[i - 1];
        if (s->offset >= 0x3000 && s->offset < 0x20000 && atc_restore(phy, s))
            return -1;
    }
    /* Remove both PMA clock requests, then clamp and power down both lanes. */
    const u32 requests[] = {BIT(12), BIT(16), BIT(14), BIT(18)};
    for (unsigned int i = 0; i < ARRAY_SIZE(requests); i++)
        if (atc_update(phy, ATC_AON + 0x24, requests[i], 0))
            return -1;
    udelay(ATC_STEP_DELAY_US);
    for (u32 offset = 0x1e18; offset <= 0x1e1c; offset += 4) {
        if (atc_update(phy, offset, BIT(5), 0))
            return -1;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(0), BIT(0)))
            return -1;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(2), 0))
            return -1;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(1), 0))
            return -1;
        udelay(ATC_STEP_DELAY_US);
    }
    phy->dp_started = false;
    phy->link_rate = 0;
    printf("ATC3: DPTX stopped and clock restored\n");
    return 0;
}

int atc_phy_set_route(atc_phy_t *phy, bool enable)
{
    if (!phy)
        return -1;
    if (!enable)
        return atc_route_stop(phy);
    if (!phy->active || !phy->dp_started)
        return -1;
    int ret = atc_route_start(phy);
    if (!ret)
        mdelay(ATC_ROUTE_SETTLE_MS);
    return ret;
}

int atc_phy_deactivate(atc_phy_t *phy)
{
    int ret = 0;
    if (!phy)
        return -1;
    /* Keep power and ownership intact on a failed stop so callers can recover. */
    if (atc_dp_stop(phy))
        return -1;
    if (phy->started) {
        u64 cfg = phy->base + ATC_CFG;
        if (phy->aux_started) {
            u64 aux = phy->base + ATC_AON + 0x24;
            set32(aux, BIT(10));
            udelay(ATC_STEP_DELAY_US);
            clear32(aux, BIT(6));
            udelay(ATC_STEP_DELAY_US);
            clear32(aux, BIT(8));
            udelay(ATC_STEP_DELAY_US);
            phy->aux_started = false;
        }
        /* Assert main reset before restoring volatile setup. */
        clear32(cfg, BIT(4));
        udelay(ATC_STEP_DELAY_US);
        while (phy->saved_count) {
            struct atc_saved *s = &phy->saved[--phy->saved_count];
            if (atc_restore(phy, s))
                ret = -1;
        }
        clear32(cfg, BIT(3));
        set32(cfg, BIT(2));
        clear32(cfg, BIT(1));
        if (poll32(cfg + 4, BIT(1), 0, ATC_TIMEOUT)) {
            printf("ATC3: big-sleep shutdown timeout\n");
            ret = -1;
        }
        clear32(cfg, BIT(0));
        if (poll32(cfg + 4, BIT(0), 0, ATC_TIMEOUT)) {
            printf("ATC3: small-sleep shutdown timeout\n");
            ret = -1;
        }
        clear32(cfg + 0x30, BIT(0));
        if (read32(cfg) != 4 || read32(cfg + 4) & 3 || read32(cfg + 0x30) != 0) {
            printf("ATC3: final CFG control/status/clock %#x/%#x/%#x\n", read32(cfg),
                   read32(cfg + 4), read32(cfg + 0x30));
            ret = -1;
        }
        phy->started = false;
    }
    if (atc_gates_restore(phy))
        ret = -1;
    phy->active = false;
    printf("ATC3: common PHY shutdown status %d\n", ret);
    return ret;
}

int atc_phy_activate(atc_phy_t *phy)
{
    if (!phy)
        return -1;
    if (phy->active)
        return 0;
    if (atc_gates_enable(phy))
        return -1;
    u64 cfg = phy->base + ATC_CFG;
    if (read32(cfg) != 4 || (read32(cfg + 4) & 3) || read32(cfg + 0x30))
        goto fail;
    phy->started = true;
    set32(cfg + 0x30, BIT(0));
    udelay(ATC_STEP_DELAY_US);
    set32(cfg, BIT(0));
    if (poll32(cfg + 4, BIT(0), BIT(0), ATC_TIMEOUT))
        goto fail;
    set32(cfg, BIT(1));
    if (poll32(cfg + 4, BIT(1), BIT(1), ATC_TIMEOUT))
        goto fail;
    clear32(cfg, BIT(2));
    udelay(ATC_STEP_DELAY_US);
    set32(cfg, BIT(3));

    for (unsigned int i = 0; i < 2; i++)
        if (atc_tunable_check(phy, i, true))
            goto fail;
    /* PHY lane mapping: both PMAs carry DisplayPort. This is not the
     * DCP-to-ATC display crossbar. */
    if (atc_update(phy, 0x4c, 0x1f, 0x14) ||
        atc_update(phy, 0x4c, 0x3ffe0, phy->swap ? 0x100 : 0x22000))
        goto fail;
    for (unsigned int i = 2; i < ARRAY_SIZE(tunables); i++)
        if (atc_tunable_check(phy, i, true))
            goto fail;
    if (atc_update(phy, ATC_CFG + 8, BIT(1), phy->swap ? BIT(1) : 0))
        goto fail;
    set32(cfg, BIT(4));
    if ((read32(cfg) & 0x1f) != 0x1b)
        goto fail;
    /* AUX init: release small/big sleep, then AUX clamp. */
    phy->aux_started = true;
    if (atc_update(phy, ATC_AON + 0x24, BIT(8), BIT(8)))
        goto fail;
    udelay(ATC_STEP_DELAY_US);
    if (atc_update(phy, ATC_AON + 0x24, BIT(6), BIT(6)))
        goto fail;
    udelay(ATC_STEP_DELAY_US);
    if (atc_update(phy, ATC_AON + 0x24, BIT(10), 0))
        goto fail;
    udelay(ATC_STEP_DELAY_US);
    phy->active = true;
    printf("ATC3: common PHY and AUX initialized, %u registers saved\n", phy->saved_count);
    return 0;
fail:
    printf("ATC3: common PHY initialization failed\n");
    atc_phy_deactivate(phy);
    return -1;
}

/* Common four-lane setup. Rate-dependent fields are applied separately below.
 * These offsets are specific to T8152 ATC, not the older standalone DPTX PHY. */
static const struct {
    u32 offset, mask, value;
} dp_setup[] = {
    {0x2044, 0xffff, 0x4b0},
    {0x4c, 0x3c0000, 0x3c0000},
    {0x4c80, 0x40, 0x40},
    {0xdc80, 0x40, 0x40},
    {0x4fd4, 0x2000000, 0x2000000},
    {0x4fd4, 0x1800000, 0x800000},
    {0xdfd4, 0x2000000, 0x2000000},
    {0xdfd4, 0x1800000, 0x800000},
    {0x4d84, 0x1c0000, 0},
    {0x4fd4, 1, 0},
    {0x4fd4, 8, 0},
    {0x4fd4, 2, 2},
    {0x4fd4, 0x10, 0x10},
    {0x8904, 0x1c0000, 0},
    {0x8800, 0x10000000, 0},
    {0x8800, 0x80000000, 0},
    {0x8800, 0x20000000, 0x20000000},
    {0x8804, 1, 1},
    {0xdd84, 0x1c0000, 0},
    {0xdfd4, 1, 0},
    {0xdfd4, 8, 0},
    {0xdfd4, 2, 2},
    {0xdfd4, 0x10, 0x10},
    {0x11904, 0x1c0000, 0},
    {0x11800, 0x10000000, 0},
    {0x11800, 0x80000000, 0},
    {0x11800, 0x20000000, 0x20000000},
    {0x11804, 1, 1},
    {0x3c04, 2, 2},
    {0x3c04, 1, 1},
    {0xcc04, 2, 2},
    {0xcc04, 1, 1},
};

int atc_phy_set_link_rate(atc_phy_t *phy, u32 rate)
{
    static const u32 rates[] = {0x06, 0x0a, 0x14, 0x1e};
    unsigned int r;
    if (!phy || !phy->active)
        return -1;
    for (r = 0; r < ARRAY_SIZE(rates); r++)
        if (rates[r] == rate)
            break;
    if (r == ARRAY_SIZE(rates))
        return -1;
    if (phy->dp_started)
        return phy->link_rate == rate ? 0 : -1;
    u32 clock = read32(phy->dp_clock);
    if (clock & BIT(30))
        return -1;
    phy->dp_started = true;
    if (atc_update(phy, ATC_CFG + 0x14, 7, 0) || atc_update(phy, ATC_CFG + 0x18, 1, 1))
        goto fail;
    if (!r && (atc_update(phy, 0xe10, BIT(2), BIT(2)) || atc_update(phy, 0xe08, BIT(29), BIT(29))))
        goto fail;

    for (u32 i = 0; i < 2; i++) {
        if (atc_update(phy, ATC_AON + 0x24, BIT(12 + 2 * i), BIT(12 + 2 * i)))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, ATC_AON + 0x24, BIT(16 + 2 * i), BIT(16 + 2 * i)))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
        u32 offset = 0x1e18 + 4 * i;
        if (atc_update(phy, offset, BIT(1), BIT(1)))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(2), BIT(2)))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(0), 0))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
        if (atc_update(phy, offset, BIT(5), BIT(5)))
            goto fail;
        udelay(ATC_STEP_DELAY_US);
    }
    if (atc_update(phy, 0x2000, 0x380000, r << 19))
        goto fail;
    for (unsigned int i = 0; i < ARRAY_SIZE(dp_setup); i++)
        if (atc_update(phy, dp_setup[i].offset, dp_setup[i].mask, dp_setup[i].value))
            goto fail;
    for (u32 i = 0; i < 2; i++) {
        if (atc_update(phy, 0x8684 + i * 0x9000, 3, r ? 0 : 1) ||
            atc_update(phy, 0x4c80 + i * 0x9000, 0x30, r ? 0 : 0x10))
            goto fail;
    }
    const u32 tx_ctrl[] = {0x4eb0, 0x8b10, 0xdeb0, 0x11b10};
    for (unsigned int i = 0; i < ARRAY_SIZE(tx_ctrl); i++) {
        if (atc_update(phy, tx_ctrl[i], 0x7e07ff, 1) ||
            atc_update(phy, tx_ctrl[i] - 0x14, 0x7f, r < 2 ? 0x65 : 0x40))
            goto fail;
    }
    static const u32 wake_count[] = {0xbb5, 0xbb7, 0xbb8, 0xbb9};
    if (atc_update(phy, 0x2000, 0x70, 0x10) || atc_update(phy, 0x2000, BIT(13), BIT(13)) ||
        atc_update(phy, 0x2044, 0xffff0000, wake_count[r] << 16) ||
        atc_update(phy, phy->swap ? 0x89f0 : 0x119f0, BIT(14), BIT(14)) ||
        atc_update(phy, 0x2000, BIT(2), BIT(2)) || atc_update(phy, 0x2000, BIT(3), BIT(3)) ||
        atc_update(phy, 0x2040, BIT(16), BIT(16)))
        goto fail;

    phy->saved_clock = clock;
    phy->clock_owned = true;
    if (atc_clock_write(phy, (clock & ~0x0f000000) | ((10 - r) << 24)))
        goto fail;
    if (atc_route_start(phy))
        goto fail;
    phy->link_rate = rate;
    printf("ATC3: four-lane DPTX setup rate %#x, %u registers saved\n", rate, phy->saved_count);
    return 0;
fail:
    printf("ATC3: DPTX setup failed\n");
    atc_dp_stop(phy);
    return -1;
}

int atc_phy_set_drive_settings(atc_phy_t *phy, const dptx_drive_settings_t *settings, u32 count)
{
    /* Matching T8152 fallback table, indexed by voltage * 4 + pre-emphasis.
     * A rate-specific live table takes precedence when present. */
    static const u32 defaults[16] = {
        0x025, 0x420, 0x717, 0x90f, 0x019, 0x397, 0x78f, 0x025,
        0x011, 0x38f, 0x025, 0x025, 0x000, 0x025, 0x025, 0x025,
    };
    if (!phy || count > 4 || (!settings && count))
        return -1;
    if (!count)
        return 0;
    if (!phy->active || !phy->dp_started || phy->link_rate != 0x0a)
        return -1;
    u32 len = 0;
    const u32 *table = adt_getprop(adt, phy->node, "dp-training-table-hbr", &len);
    if (!table)
        table = adt_getprop(adt, phy->node, "dp-training-table", &len);
    if (!table)
        table = defaults;
    else if (len != sizeof(defaults))
        return -1;
    u32 presets[4];
    /* Validate the entire request before changing any lane. */
    for (u32 i = 0; i < count; i++) {
        if (settings[i].format || settings[i].voltage > 3 || settings[i].pre_emphasis > 3 ||
            settings[i].voltage + settings[i].pre_emphasis > 3)
            return -1;
        presets[i] = table[settings[i].voltage * 4 + settings[i].pre_emphasis];
        if (presets[i] & ~0x7ffbf)
            return -1;
    }
    for (u32 i = 0; i < count; i++) {
        u32 pma = (i / 2) ^ !phy->swap;
        /* Even DP lanes use the PMA RX pair, odd lanes use its TX pair. */
        u32 offset = ((i & 1) ? 0x4eb0 : 0x8b10) + pma * 0x9000;
        u32 preset = presets[i];
        u32 control = 1 | (((preset >> 13) & 0x3f) << 17) | (((preset >> 7) & 0x3f) << 5);
        if (atc_update(phy, offset, 0x7e07ff, control) ||
            atc_update(phy, offset - 0x14, 0x7f, (preset & 0x3f) | 0x40))
            return -1;
        dprintf("ATC3: DP lane %u voltage %u emphasis %u preset %#x\n", i, settings[i].voltage,
                settings[i].pre_emphasis, preset);
    }
    return 0;
}

void atc_phy_shutdown(atc_phy_t *phy)
{
    if (!phy)
        return;
    if (!atc_phy_deactivate(phy))
        free(phy);
}
