/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "dptx_phy.h"
#include "atc_phy.h"
#include "malloc.h"

#include "../adt.h"
#include "../exception.h"
#include "../utils.h"

#define DPTX_MAX_LANES    4
#define DPTX_LANE0_OFFSET 0x5000
#define DPTX_LANE_STRIDE  0x1000
#define DPTX_LANE_END     (DPTX_LANE0_OFFSET + DPTX_MAX_LANES * DPTX_LANE_STRIDE)

#define DPTX_T8152_LANE0_OFFSET 0x4500
#define DPTX_T8152_LANE_STRIDE  0x2800

enum dptx_type {
    DPTX_PHY_T8112,
    DPTX_PHY_T602X,
    DPTX_PHY_T8152_ATC,
    DPTX_PHY_T8152_HDMI,
};

typedef struct dptx_phy {
    u64 regs[2];
    enum dptx_type type;
    u32 dcp_index;
    u32 active_lanes;
    const char *path;
    atc_phy_t *atc;
} dptx_phy_t;

/* AppleT8152DPTXPort::setBitsInReg */
static void dptx_t8152_set_bits(dptx_phy_t *phy, u32 offset, u32 shift, u32 mask, u32 value)
{
    u32 reg = read32(phy->regs[0] + offset);
    write32(phy->regs[0] + offset, (reg & ~mask) | (value << shift));
}

struct dptx_t8152_tunable {
    const char *name;
    u32 offset;
};

/* Table used by AppleT8152DPTXPort::handleSetTunables; all offsets are on the
 * core register block (regs[0]). */
static const struct dptx_t8152_tunable dptx_t8152_tunables[] = {
    {"asdc-txd-cfg-0-tunables", 0x5500},   {"asdc-txd-cfg-1-tunables", 0x7d00},
    {"asdc-txd-cfg-2-tunables", 0xa500},   {"asdc-txd-cfg-3-tunables", 0xcd00},
    {"asdc-txspl-cfg-0-tunables", 0x5180}, {"asdc-txspl-cfg-1-tunables", 0x7980},
    {"asdc-txspl-cfg-2-tunables", 0xa180}, {"asdc-txspl-cfg-3-tunables", 0xc980},
    {"lane-shm-regs-0-tunables", 0x4500},  {"lane-shm-regs-1-tunables", 0x6d00},
    {"lane-shm-regs-2-tunables", 0x9500},  {"lane-shm-regs-3-tunables", 0xbd00},
    {"asdc-dppll-core-tunables", 0x0800},  {"asdc-dppll-top-tunables", 0x0600},
    {"asdc-aux-shm-tunables", 0x1400},     {"asdc-cmn-tunables", 0x1c00},
};

/* AppleDPTXPort::writeTunables: every record is {offset, width, mask, pad,
 * value, pad} and applied as an unshifted read-modify-write. */
static void dptx_t8152_apply_tunables(dptx_phy_t *phy)
{
    int node = adt_path_offset(adt, phy->path);
    if (node < 0) {
        printf("DPTX-PHY: failed to find %s for tunables\n", phy->path);
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(dptx_t8152_tunables); i++) {
        const struct dptx_t8152_tunable *tunable = &dptx_t8152_tunables[i];
        u32 len;
        const u32 *records = adt_getprop(adt, node, tunable->name, &len);
        if (!records)
            continue;
        for (u32 off = 0; off + 24 <= len; off += 24) {
            const u32 *rec = &records[off / 4];
            if (rec[1] != 4)
                continue;
            u32 reg = read32(phy->regs[0] + tunable->offset + rec[0]);
            write32(phy->regs[0] + tunable->offset + rec[0], (reg & ~rec[2]) | rec[4]);
        }
    }
}

/* AppleT8152DPTXPort::phySetActiveLaneCount: per-lane configuration on the
 * lane shm blocks; the trailing argument of each register write is the
 * lane-enabled bit. */
static void dptx_t8152_set_active_lane_count(dptx_phy_t *phy, u32 num_lanes)
{
    static const u32 offsets[] = {0x64, 0x24, 0x24, 0x48, 0x48, 0x5c, 0x5c, 0x48};
    static const u32 shifts[] = {4, 4, 1, 0, 1, 2, 3, 4};
    static const u32 masks[] = {0x10, 0x10, 0x2, 0x1, 0x2, 0x4, 0x8, 0x10};

    dptx_t8152_set_bits(phy, 0x2400, 0, 0x1, 0);

    for (u32 lane = 0; lane < DPTX_MAX_LANES; lane++) {
        u32 base = DPTX_T8152_LANE0_OFFSET + lane * DPTX_T8152_LANE_STRIDE;
        for (size_t i = 0; i < ARRAY_SIZE(offsets); i++)
            dptx_t8152_set_bits(phy, base + offsets[i], shifts[i], masks[i], lane < num_lanes);
    }

    dptx_t8152_set_bits(phy, 0x2420, 0, 0xf, (1 << num_lanes) - 1);
    dptx_t8152_set_bits(phy, 0x2400, 0, 0x1, 1);
}

/* AppleT8152DPTXPort::getTunableConfig: the DPPLLConfig words for each link
 * rate (constant tables in AppleDisplayCrossbar, 26A434). */
static const struct {
    u32 rate; /* MHz */
    u32 w[22];
} dptx_t8152_pll_configs[] = {
    {1620, {1, 0x21c, 0, 0, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x3c, 2, 1, 0, 0, 1, 0, 0, 0, 1, 3}},
    {2700,
     {1, 0x1c2, 0x1fff, 0x3ffe, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x32, 2, 1, 0, 0, 1, 0, 0, 0, 1, 3}},
    {5400, {0, 0x1c2, 0x1fff, 0x3ffe, 1, 9, 1, 0, 0, 0x1a, 1, 0x32, 1, 0, 0, 1, 0, 0, 0, 1, 1, 3}},
    {8100, {0, 0x2a3, 0x2ffd, 0x3ffc, 1, 9, 1, 0, 0, 0x1a, 1, 0x5a, 1, 0, 0, 1, 0, 0, 0, 1, 1, 3}},
};

/* AppleT8152DPTXPort::pllPowerUp/pllPowerDown: command in 0x408[2:0],
 * request bit 3, acknowledge bit 19. */
static int dptx_t8152_pll_command(dptx_phy_t *phy, u32 command)
{
    int i;

    dptx_t8152_set_bits(phy, 0x408, 6, 0x40, 1);
    dptx_t8152_set_bits(phy, 0x408, 0, 0x7, command);
    dptx_t8152_set_bits(phy, 0x408, 3, 0x8, 1);
    for (i = 0; i < 10; i++) {
        udelay(16);
        if (read32(phy->regs[0] + 0x408) & BIT(19))
            break;
    }
    dptx_t8152_set_bits(phy, 0x408, 3, 0x8, 0);
    dptx_t8152_set_bits(phy, 0x408, 6, 0x40, 0);
    if (i == 10) {
        printf("DPTX-PHY: no ACK to PLL command %u\n", command);
        return -1;
    }
    return 0;
}

/* AppleDPTXPort::setLinkRate with the T8152 hooks: phyPowerDown,
 * pllPowerDown, pllConfigureFrequency, phy/pllSetHalfLinkRate, pllPowerUp. */
static int dptx_t8152_set_link_rate(dptx_phy_t *phy, u32 link_rate)
{
    const u32 *w = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(dptx_t8152_pll_configs); i++)
        if (dptx_t8152_pll_configs[i].rate == link_rate)
            w = dptx_t8152_pll_configs[i].w;
    if (!w)
        return -1;

    dptx_t8152_set_bits(phy, 0x2400, 0, 0x1, 0);
    if (dptx_t8152_pll_command(phy, 0))
        return -1;

    /* pllConfigureFrequency, downspread off */
    dptx_t8152_set_bits(phy, 0x6c0, 0, 0x7ff, w[1]);
    dptx_t8152_set_bits(phy, 0x6c0, 11, 0x800, w[6]);
    dptx_t8152_set_bits(phy, 0x6c0, 13, 0x2000, w[10]);
    dptx_t8152_set_bits(phy, 0x6c0, 18, 0x3c0000, w[21]);
    dptx_t8152_set_bits(phy, 0x6c4, 14, 0xfffc000, w[2]);
    dptx_t8152_set_bits(phy, 0x6c4, 0, 0x3fff, (!(w[7] & 1) && w[2] < 2) ? 0 : w[3]);
    dptx_t8152_set_bits(phy, 0x6c8, 14, 0xc000, w[0]);
    dptx_t8152_set_bits(phy, 0x6c8, 8, 0x100, 0);
    dptx_t8152_set_bits(phy, 0x6c8, 0, 0xff, w[8]);
    dptx_t8152_set_bits(phy, 0x6c8, 9, 0x3e00, w[5]);
    dptx_t8152_set_bits(phy, 0x6c8, 16, 0x10000, w[17]);
    dptx_t8152_set_bits(phy, 0x6c8, 26, 0xfc000000, w[9]);
    dptx_t8152_set_bits(phy, 0x6cc, 0, 0x7f, w[11]);
    dptx_t8152_set_bits(phy, 0x6cc, 9, 0x600, w[13]);
    dptx_t8152_set_bits(phy, 0x6cc, 11, 0x7800, w[14]);
    dptx_t8152_set_bits(phy, 0x6cc, 15, 0x38000, w[15]);
    dptx_t8152_set_bits(phy, 0x6cc, 20, 0x100000, w[16]);
    dptx_t8152_set_bits(phy, 0x6cc, 21, 0x200000, w[18]);
    dptx_t8152_set_bits(phy, 0x6cc, 22, 0xc00000, w[19]);
    dptx_t8152_set_bits(phy, 0x6cc, 24, 0x3000000, w[4]);
    dptx_t8152_set_bits(phy, 0x6cc, 26, 0xc000000, w[20]);

    /* AppleT8112DPTXPort::updateLinkRateDependentShadowRegisters */
    set32(phy->regs[1] + 0xa0, BIT(3));
    set32(phy->regs[1] + 0xa0, BIT(2));
    set32(phy->regs[1] + 0xa0, BIT(18));
    clear32(phy->regs[1] + 0xa0, BIT(18));
    set32(phy->regs[1] + 0xa0, BIT(19));
    clear32(phy->regs[1] + 0xa0, BIT(19));
    clear32(phy->regs[1] + 0xa0, BIT(2));
    clear32(phy->regs[1] + 0xa0, BIT(3));

    /* No UHBR rates; half link rate from 2.5 Gbps up. */
    bool half = link_rate >= 2500;
    dptx_t8152_set_bits(phy, 0x2400, 2, 0x4, 0);
    dptx_t8152_set_bits(phy, 0x2400, 3, 0x8, !half);
    dptx_t8152_set_bits(phy, 0x0000, 17, 0x20000, !half);

    if (dptx_t8152_pll_command(phy, 7))
        return -1;
    if (!(read32(phy->regs[0] + 0x438) & BIT(30))) {
        printf("DPTX-PHY: PLL failed to lock at %u MHz\n", link_rate);
        return -1;
    }
    return 0;
}

/* AppleT8152DPTXPort::performImpedanceCalibration: adapter reports completion
 * in bit 0 of 0x1c04 and the calibration result in 0x1c24. */
static void dptx_t8152_impedance_calibration(dptx_phy_t *phy)
{
    u32 i;

    for (i = 0; i < 100000; i++) {
        if (read32(phy->regs[0] + 0x1c04) & 1)
            break;
        udelay(10);
    }
    if (i == 100000)
        printf("DPTX-PHY: impedance calibration timed out\n");

    dptx_t8152_set_bits(phy, 0x1c00, 4, 0x3f0, 0x3f);
    u32 value = read32(phy->regs[0] + 0x1c24);
    dptx_t8152_set_bits(phy, 0x1c18, 0xf, 0x3f8000, (value >> 6) & 0x7f);
    dptx_t8152_set_bits(phy, 0x1c18, 0xe, 0x4000, 1);
}

/* AppleDPTXPort::handleActivate for T8152: power up, apply the adapter
 * tunables from the devicetree, then run the PLL/PHY/AUX activation hooks. */
static int dptx_t8152_activate(dptx_phy_t *phy)
{
    /* AppleT8112DPTXPort::setDPTXSourceSelect: only the low nibble */
    mask32(phy->regs[1] + 0x10, 0xf, phy->dcp_index);

    /* pllSetPowerEnabled, auxSetPowerEnabled, phySetPowerEnabled */
    set32(phy->regs[1] + 0x48, 0x010);
    mdelay(1);
    set32(phy->regs[1] + 0x48, 0x020);
    clear32(phy->regs[1] + 0x48, 0x040);
    set32(phy->regs[1] + 0x48, 0x100);
    mdelay(1);
    set32(phy->regs[1] + 0x48, 0x200);
    clear32(phy->regs[1] + 0x48, 0x400);
    set32(phy->regs[1] + 0x48, 0x001);
    mdelay(1);
    set32(phy->regs[1] + 0x48, 0x002);
    clear32(phy->regs[1] + 0x48, 0x004);

    dptx_t8152_apply_tunables(phy);

    /* configurePll: single-ended clock output, no pclkdrv-select property */
    dptx_t8152_set_bits(phy, 0x08d0, 4, 0x10, 1);
    dptx_t8152_set_bits(phy, 0x0000, 1, 0x02, 1);     /* activatePll */
    dptx_t8152_set_bits(phy, 0x1400, 0, 0x01, 0);     /* auxSetupCmnControl */
    dptx_t8152_set_bits(phy, 0x1c00, 4, 0x3f0, 0x3c); /* activatePhy */
    dptx_t8152_set_bits(phy, 0x0000, 0, 0x01, 1);
    mdelay(1);

    int ret = dptx_phy_set_active_lane_count(phy, DPTX_MAX_LANES);
    if (ret)
        return ret;

    /* auxPowerUp and auxSetTerminationOverride are no-ops on T8152 */
    dptx_t8152_impedance_calibration(phy);

    return 0;
}

int dptx_phy_activate(dptx_phy_t *phy)
{
    if (phy->type == DPTX_PHY_T8152_ATC) {
        if (atc_phy_activate(phy->atc) || atc_phy_set_link_rate(phy->atc, 0x0a))
            return -1;
        phy->active_lanes = 0;
        return 0;
    }
    if (phy->type == DPTX_PHY_T8152_HDMI)
        return dptx_t8152_activate(phy);
    // MMIO: R.4   0x23c500010 (dptx-phy[1], offset 0x10) = 0x0
    // MMIO: W.4   0x23c500010 (dptx-phy[1], offset 0x10) = 0x0
    read32(phy->regs[1] + 0x10);
    write32(phy->regs[1] + 0x10, phy->dcp_index);

    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x444
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x454
    set32(phy->regs[1] + 0x48, 0x010);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x454
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x474
    set32(phy->regs[1] + 0x48, 0x020);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x474
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x434
    clear32(phy->regs[1] + 0x48, 0x040);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x434
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x534
    set32(phy->regs[1] + 0x48, 0x100);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x534
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x734
    set32(phy->regs[1] + 0x48, 0x200);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x734
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x334
    clear32(phy->regs[1] + 0x48, 0x400);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x334
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x335
    set32(phy->regs[1] + 0x48, 0x001);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x335
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x337
    set32(phy->regs[1] + 0x48, 0x002);
    // MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x337
    // MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x333
    clear32(phy->regs[1] + 0x48, 0x004);

    // MMIO: R.4   0x23c542014 (dptx-phy[0], offset 0x2014) = 0x80a0c
    u32 val_2014 = read32(phy->regs[0] + 0x2014);
    // MMIO: W.4   0x23c542014 (dptx-phy[0], offset 0x2014) = 0x300a0c
    write32(phy->regs[0] + 0x2014, (0x30 << 16) | (val_2014 & 0xffff));

    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x644800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    set32(phy->regs[0] + 0x20b8, 0x010000);

    // MMIO: R.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a2
    // MMIO: W.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a0
    clear32(phy->regs[0] + 0x2220, 0x0000002);

    // MMIO: R.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103003
    // MMIO: W.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103803
    set32(phy->regs[0] + 0x222c, 0x000800);
    // MMIO: R.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103803
    // MMIO: W.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103903
    set32(phy->regs[0] + 0x222c, 0x000100);

    // MMIO: R.4   0x23c542230 (dptx-phy[0], offset 0x2230) = 0x2308804
    // MMIO: W.4   0x23c542230 (dptx-phy[0], offset 0x2230) = 0x2208804
    clear32(phy->regs[0] + 0x2230, 0x0100000);

    // MMIO: R.4   0x23c542278 (dptx-phy[0], offset 0x2278) = 0x18300811
    // MMIO: W.4   0x23c542278 (dptx-phy[0], offset 0x2278) = 0x10300811
    clear32(phy->regs[0] + 0x2278, 0x08000000);

    // MMIO: R.4   0x23c5422a4 (dptx-phy[0], offset 0x22a4) = 0x1044200
    // MMIO: W.4   0x23c5422a4 (dptx-phy[0], offset 0x22a4) = 0x1044201
    set32(phy->regs[0] + 0x22a4, 0x0000001);

    // MMIO: R.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x18030
    u32 val_4008 = read32(phy->regs[0] + 0x4008);
    // MMIO: W.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30030
    write32(phy->regs[0] + 0x4008, (0x6 << 15) | (val_4008 & 0x7fff));
    // MMIO: R.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30030
    // MMIO: W.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30010
    clear32(phy->regs[0] + 0x4008, 0x00020);

    // MMIO: R.4   0x23c54420c (dptx-phy[0], offset 0x420c) = 0x88e3
    // MMIO: W.4   0x23c54420c (dptx-phy[0], offset 0x420c) = 0x88c3
    clear32(phy->regs[0] + 0x420c, 0x0020);

    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x0
    // MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
    set32(phy->regs[0] + 0x4600, 0x8000000);

    // MMIO: R.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x21780
    // MMIO: W.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x221780
    // MMIO: R.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x21780
    // MMIO: W.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x221780
    // MMIO: R.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x21780
    // MMIO: W.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x221780
    // MMIO: R.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x21780
    // MMIO: W.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x221780
    for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END; loff += DPTX_LANE_STRIDE)
        set32(phy->regs[0] + loff + 0x40, 0x200000);

    // MMIO: R.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x221780
    // MMIO: W.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x2a1780
    // MMIO: R.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x221780
    // MMIO: W.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x2a1780
    // MMIO: R.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x221780
    // MMIO: W.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x2a1780
    // MMIO: R.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x221780
    // MMIO: W.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x2a1780
    for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END; loff += DPTX_LANE_STRIDE)
        set32(phy->regs[0] + loff + 0x40, 0x080000);

    // MMIO: R.4   0x23c545244 (dptx-phy[0], offset 0x5244) = 0x18
    // MMIO: W.4   0x23c545244 (dptx-phy[0], offset 0x5244) = 0x8
    // MMIO: R.4   0x23c546244 (dptx-phy[0], offset 0x6244) = 0x18
    // MMIO: W.4   0x23c546244 (dptx-phy[0], offset 0x6244) = 0x8
    // MMIO: R.4   0x23c547244 (dptx-phy[0], offset 0x7244) = 0x18
    // MMIO: W.4   0x23c547244 (dptx-phy[0], offset 0x7244) = 0x8
    // MMIO: R.4   0x23c548244 (dptx-phy[0], offset 0x8244) = 0x18
    // MMIO: W.4   0x23c548244 (dptx-phy[0], offset 0x8244) = 0x8
    for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END; loff += DPTX_LANE_STRIDE)
        clear32(phy->regs[0] + loff + 0x244, 0x10);

    // MMIO: R.4   0x23c542214 (dptx-phy[0], offset 0x2214) = 0x1e0
    // MMIO: W.4   0x23c542214 (dptx-phy[0], offset 0x2214) = 0x1e1
    set32(phy->regs[0] + 0x2214, 0x001);

    // MMIO: R.4   0x23c542224 (dptx-phy[0], offset 0x2224) = 0x20086001
    // MMIO: W.4   0x23c542224 (dptx-phy[0], offset 0x2224) = 0x20086000
    clear32(phy->regs[0] + 0x2224, 0x00000001);

    // MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
    // MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
    set32(phy->regs[0] + 0x2200, 0x0002);

    // MMIO: R.4   0x23c541000 (dptx-phy[0], offset 0x1000) = 0xe0000003
    // MMIO: W.4   0x23c541000 (dptx-phy[0], offset 0x1000) = 0xe0000001
    clear32(phy->regs[0] + 0x1000, 0x00000002);

    // MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
    // MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    set32(phy->regs[0] + 0x4004, 0x08);

    /* TODO: no idea what happens here, supposedly setting/clearing some bits */
    // MMIO: R.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
    read32(phy->regs[0] + 0x4404);
    // MMIO: W.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
    write32(phy->regs[0] + 0x4404, 0x555d444);
    // MMIO: R.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
    read32(phy->regs[0] + 0x4404);
    // MMIO: W.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
    write32(phy->regs[0] + 0x4404, 0x555d444);

    dptx_phy_set_active_lane_count(phy, 0);

    // MMIO: R.4   0x23c544200 (dptx-phy[0], offset 0x4200) = 0x4002430
    // MMIO: W.4   0x23c544200 (dptx-phy[0], offset 0x4200) = 0x4002420
    clear32(phy->regs[0] + 0x4200, 0x0000010);

    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
    // MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
    clear32(phy->regs[0] + 0x4600, 0x0000001);
    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
    // MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000001
    set32(phy->regs[0] + 0x4600, 0x0000001);
    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000001
    // MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000003
    set32(phy->regs[0] + 0x4600, 0x0000002);
    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000043
    // MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000043
    // MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000041
    /* TODO: read first to check if the previous set(...,0x2) sticked? */
    read32(phy->regs[0] + 0x4600);
    clear32(phy->regs[0] + 0x4600, 0x0000001);

    // MMIO: R.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
    // MMIO: W.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
    /* TODO: probably a set32 of an already set bit */
    u32 val_4408 = read32(phy->regs[0] + 0x4408);
    if (val_4408 != 0x482 && val_4408 != 0x483)
        printf("DPTX-PHY: unexpected initial value at regs[0] offset 0x4408: 0x%03x\n", val_4408);
    write32(phy->regs[0] + 0x4408, val_4408);
    // MMIO: R.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
    // MMIO: W.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x483
    set32(phy->regs[0] + 0x4408, 0x001);

    return 0;
}

int dptx_phy_set_active_lane_count(dptx_phy_t *phy, u32 num_lanes)
{
    if (phy->type == DPTX_PHY_T8152_ATC) {
        if (num_lanes == 3 || num_lanes > 4)
            return -1;
        /* Setting the active lane count does nothing to the PHY on macOS;
         * the ATC port records the logical count after success. The physical
         * lane mode was selected during the native four-lane initialization. */
        phy->active_lanes = num_lanes;
        return 0;
    }
    if (phy->type == DPTX_PHY_T8152_HDMI) {
        if (num_lanes == 3 || num_lanes > DPTX_MAX_LANES)
            return -1;
        if (!num_lanes) {
            printf("DPTX-PHY: T8152 does not accept a zero lane count\n");
            phy->active_lanes = 0;
            return 0;
        }
        dptx_t8152_set_active_lane_count(phy, num_lanes);
        phy->active_lanes = num_lanes;
        return 0;
    }
    u32 l;

    dprintf("DPTX-PHY: set_active_lane_count(%u) phy_regs = {0x%lx, 0x%lx}\n", num_lanes,
            phy->regs[0], phy->regs[1]);

    if (num_lanes == 3 || num_lanes > DPTX_MAX_LANES)
        return -1;

    u32 ctrl = read32(phy->regs[0] + 0x4000);
    write32(phy->regs[0] + 0x4000, ctrl);

    for (l = 0; l < num_lanes; l++) {
        u64 offset = 0x5000 + 0x1000 * l;
        read32(phy->regs[0] + offset);
        write32(phy->regs[0] + offset, 0x100);
    }
    for (; l < DPTX_MAX_LANES; l++) {
        u64 offset = 0x5000 + 0x1000 * l;
        read32(phy->regs[0] + offset);
        write32(phy->regs[0] + offset, 0x300);
    }
    for (l = 0; l < num_lanes; l++) {
        u64 offset = 0x5000 + 0x1000 * l;
        read32(phy->regs[0] + offset);
        write32(phy->regs[0] + offset, 0x0);
    }
    for (; l < DPTX_MAX_LANES; l++) {
        u64 offset = 0x5000 + 0x1000 * l;
        read32(phy->regs[0] + offset);
        write32(phy->regs[0] + offset, 0x300);
    }

    if (num_lanes > 0) {
        // clear32(phy->regs[0] + 0x4000, 0x4000000);
        ctrl = read32(phy->regs[0] + 0x4000);
        ctrl &= ~0x4000000;
        write32(phy->regs[0] + 0x4000, ctrl);
    }
    phy->active_lanes = num_lanes;

    return 0;
}

u32 dptx_phy_get_active_lane_count(dptx_phy_t *phy)
{
    return phy->active_lanes;
}

int dptx_phy_set_drive_settings(dptx_phy_t *phy, const dptx_drive_settings_t *settings, u32 count)
{
    /* AppleDPTXPort::setDriveSettings only records the request; the HDMI
     * phy is not reprogrammed. */
    if (phy && phy->type == DPTX_PHY_T8152_HDMI)
        return 0;
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    return atc_phy_set_drive_settings(phy->atc, settings, count);
}

int dptx_phy_set_link_rate(dptx_phy_t *phy, u32 link_rate)
{
    if (phy->type == DPTX_PHY_T8152_ATC)
        return link_rate % 270 ? -1 : atc_phy_set_link_rate(phy->atc, link_rate / 270);
    if (phy->type == DPTX_PHY_T8152_HDMI)
        return dptx_t8152_set_link_rate(phy, link_rate);
    UNUSED(link_rate);
    u32 sts_1008, sts_1014;

    // MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    // MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    set32(phy->regs[0] + 0x4004, 0x08);

    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    clear32(phy->regs[0] + 0x4000, 0x0000040);

    // MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    // MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
    clear32(phy->regs[0] + 0x4004, 0x08);

    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    clear32(phy->regs[0] + 0x4000, 0x2000000);
    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    set32(phy->regs[0] + 0x4000, 0x1000000);

    // MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
    // MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
    // MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
    /* TODO: what is this read checking for? */
    read32(phy->regs[0] + 0x2200);
    clear32(phy->regs[0] + 0x2200, 0x0002);

    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf008
    /* TODO: what is the setting/clearing? */
    u32 val_100c = read32(phy->regs[0] + 0x100c);
    write32(phy->regs[0] + 0x100c, val_100c);
    set32(phy->regs[0] + 0x100c, 0x0008);

    // MMIO: R.4   0x23c541014 (dptx-phy[0], offset 0x1014) = 0x1
    sts_1014 = read32(phy->regs[0] + 0x1014);
    UNUSED(sts_1014); /* TODO: assert(sts_1014 == 0x1); */

    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf008
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
    clear32(phy->regs[0] + 0x100c, 0x0008);

    // MMIO: R.4   0x23c541008 (dptx-phy[0], offset 0x1008) = 0x1
    sts_1008 = read32(phy->regs[0] + 0x1008);
    UNUSED(sts_1008); /* TODO: assert(sts_1008 == 0x1); */

    // MMIO: R.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a0
    // MMIO: W.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x1109020
    clear32(phy->regs[0] + 0x2220, 0x0000080);

    // MMIO: R.4   0x23c5420b0 (dptx-phy[0], offset 0x20b0) = 0x1e0e01c2
    // MMIO: W.4   0x23c5420b0 (dptx-phy[0], offset 0x20b0) = 0x1e0e01c2
    u32 val_20b0 = read32(phy->regs[0] + 0x20b0);
    /* TODO: what happens on dptx-phy */
    if (phy->type == DPTX_PHY_T602X)
        val_20b0 = (val_20b0 & ~0x3ff) | 0x2a3;
    write32(phy->regs[0] + 0x20b0, val_20b0);

    // MMIO: R.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
    // MMIO: W.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
    u32 val_20b4 = read32(phy->regs[0] + 0x20b4);
    /* TODO: what happens on dptx-phy */
    if (phy->type == DPTX_PHY_T602X)
        val_20b4 = (val_20b4 | 0x4000000) & ~0x0008000;
    write32(phy->regs[0] + 0x20b4, val_20b4);

    // MMIO: R.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
    // MMIO: W.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
    val_20b4 = read32(phy->regs[0] + 0x20b4);
    /* TODO: what happens on dptx-phy */
    if (phy->type == DPTX_PHY_T602X)
        val_20b4 = (val_20b4 | 0x0000001) & ~0x0000004;
    write32(phy->regs[0] + 0x20b4, val_20b4);

    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    /* TODO: unclear */
    set32(phy->regs[0] + 0x20b8, 0);
    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    /* TODO: unclear */
    set32(phy->regs[0] + 0x20b8, 0);
    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    /* TODO: unclear */
    if (phy->type == DPTX_PHY_T602X)
        set32(phy->regs[0] + 0x20b8, 0x010000);
    else
        set32(phy->regs[0] + 0x20b8, 0);
    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
    clear32(phy->regs[0] + 0x20b8, 0x200000);

    // MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
    // MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
    /* TODO: unclear */
    set32(phy->regs[0] + 0x20b8, 0);

    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x0
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x4000c
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x4000c
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8000c
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8000c
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
    // MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
    // MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x0
    set32(phy->regs[1] + 0xa0, 0x8);
    set32(phy->regs[1] + 0xa0, 0x4);
    set32(phy->regs[1] + 0xa0, 0x40000);
    clear32(phy->regs[1] + 0xa0, 0x40000);
    set32(phy->regs[1] + 0xa0, 0x80000);
    clear32(phy->regs[1] + 0xa0, 0x80000);
    clear32(phy->regs[1] + 0xa0, 0x4);
    clear32(phy->regs[1] + 0xa0, 0x8);

    // MMIO: R.4   0x23c542000 (dptx-phy[0], offset 0x2000) = 0x2
    // MMIO: W.4   0x23c542000 (dptx-phy[0], offset 0x2000) = 0x2
    /* TODO: unclear */
    set32(phy->regs[0] + 0x2000, 0x0);

    // MMIO: R.4   0x23c542018 (dptx-phy[0], offset 0x2018) = 0x0
    // MMIO: W.4   0x23c542018 (dptx-phy[0], offset 0x2018) = 0x0
    clear32(phy->regs[0] + 0x2018, 0x0);

    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
    set32(phy->regs[0] + 0x100c, 0x0007);
    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf00f
    set32(phy->regs[0] + 0x100c, 0x0008);

    // MMIO: R.4   0x23c541014 (dptx-phy[0], offset 0x1014) = 0x38f
    sts_1014 = read32(phy->regs[0] + 0x1014);
    /* TODO: assert(sts_1014 == 0x38f); */

    // MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf00f
    // MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
    clear32(phy->regs[0] + 0x100c, 0x0008);

    // MMIO: R.4   0x23c541008 (dptx-phy[0], offset 0x1008) = 0x9
    sts_1008 = read32(phy->regs[0] + 0x1008);
    /* TODO: assert(sts_1008 == 0x9); */

    // MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
    // MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
    set32(phy->regs[0] + 0x2200, 0x0002);

    // MMIO: R.4   0x23c545010 (dptx-phy[0], offset 0x5010) = 0x18003000
    // MMIO: W.4   0x23c545010 (dptx-phy[0], offset 0x5010) = 0x18003000
    // MMIO: R.4   0x23c546010 (dptx-phy[0], offset 0x6010) = 0x18003000
    // MMIO: W.4   0x23c546010 (dptx-phy[0], offset 0x6010) = 0x18003000
    // MMIO: R.4   0x23c547010 (dptx-phy[0], offset 0x7010) = 0x18003000
    // MMIO: W.4   0x23c547010 (dptx-phy[0], offset 0x7010) = 0x18003000
    // MMIO: R.4   0x23c548010 (dptx-phy[0], offset 0x8010) = 0x18003000
    // MMIO: W.4   0x23c548010 (dptx-phy[0], offset 0x8010) = 0x18003000
    write32(phy->regs[0] + 0x8010, 0x18003000);
    for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END; loff += DPTX_LANE_STRIDE) {
        u32 val_l010 = read32(phy->regs[0] + loff + 0x10);
        write32(phy->regs[0] + loff + 0x10, val_l010);
    }

    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x51021ac
    set32(phy->regs[0] + 0x4000, 0x1000000);
    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x51021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ac
    set32(phy->regs[0] + 0x4000, 0x2000000);

    // MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
    // MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    set32(phy->regs[0] + 0x4004, 0x08);

    // MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ac
    // MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ec
    set32(phy->regs[0] + 0x4000, 0x0000040);

    // MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
    // MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x48
    clear32(phy->regs[0] + 0x4004, 0x01);

    return 0;
}

int dptx_phy_deactivate(dptx_phy_t *phy)
{
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    int ret = atc_phy_deactivate(phy->atc);
    if (!ret)
        phy->active_lanes = 0;
    return ret;
}

int dptx_phy_set_route(dptx_phy_t *phy, bool enable)
{
    /* The dedicated HDMI phy has no per-link route to switch. */
    if (phy && phy->type == DPTX_PHY_T8152_HDMI)
        return 0;
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    return atc_phy_set_route(phy->atc, enable);
}

/* Highest link rate in MHz: the HDMI phy has PLL configurations up to HBR3
 * (macOS trains the PS190 at HBR3); everything else is qualified at HBR. */
u32 dptx_phy_max_link_rate(dptx_phy_t *phy)
{
    return phy && phy->type == DPTX_PHY_T8152_HDMI ? 8100 : 2700;
}

u32 dptx_phy_dcp_output(dptx_phy_t *phy)
{
    switch (phy->type) {
        case DPTX_PHY_T8112:
            return 5;
        case DPTX_PHY_T602X:
            return 4;
        case DPTX_PHY_T8152_ATC:
            return 3;
        case DPTX_PHY_T8152_HDMI:
            /* ADT dptx-phy dp-switch-dfp-endpoint = 4: J873g has no panel
             * and wires this phy to display-crossbar0's lpdptx slot
             * (dfp-endpoints: atc0-3, lpdptx, dptx). */
            return 4;
        default:
            return 5;
    }
}

/* The phy register block is power-gated until its PMGR device is enabled.
 * Probe it under the exception guard so a gated block fails the init instead
 * of aborting in the middle of the activation sequence and rebooting. */
static bool dptx_phy_accessible(dptx_phy_t *phy)
{
    enum exc_guard_t guard = exc_guard;
    int faults = exc_count;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    read32(phy->regs[1] + 0x10);
    exc_guard = guard;

    return exc_count == faults;
}

dptx_phy_t *dptx_phy_init(const char *phy_node, u32 dcp_index)
{
    enum dptx_type type;
    int adt_phy_path[8];

    int node = adt_path_offset_trace(adt, phy_node, adt_phy_path);
    if (node < 0) {
        printf("DPtx-phy: Error getting phy node %s\n", phy_node);
        return NULL;
    }

    if (adt_is_compatible(adt, node, "dptx-phy,t8112"))
        type = DPTX_PHY_T8112;
    else if (adt_is_compatible(adt, node, "dptx-phy,t602x"))
        type = DPTX_PHY_T602X;
    else if (adt_is_compatible(adt, node, "atc-phy,t8152"))
        type = DPTX_PHY_T8152_ATC;
    else if (adt_is_compatible(adt, node, "dptx-phy,t8152"))
        type = DPTX_PHY_T8152_HDMI;
    else {
        printf("DPtx-phy: dptx-phy node %s is not compatible\n", phy_node);
        return NULL;
    }

    dptx_phy_t *phy = calloc(sizeof *phy, 1);
    if (!phy)
        return NULL;

    phy->type = type;
    phy->dcp_index = dcp_index;
    phy->path = phy_node;

    if (type == DPTX_PHY_T8152_ATC) {
        if (dcp_index != 0)
            goto out_err;
        phy->atc = atc_phy_init(phy_node);
        if (!phy->atc)
            goto out_err;
        return phy;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 0, &phy->regs[0], NULL) < 0) {
        printf("DPtx-phy: failed to get %s.reg[0]\n", phy_node);
        goto out_err;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 1, &phy->regs[1], NULL) < 0) {
        printf("DPtx-phy: failed to get %s.reg[1]\n", phy_node);
        goto out_err;
    }

    if (!dptx_phy_accessible(phy)) {
        printf("DPtx-phy: %s is not responding (power gate off?)\n", phy_node);
        goto out_err;
    }

    return phy;

out_err:
    free(phy);
    return NULL;
}

void dptx_phy_shutdown(dptx_phy_t *phy)
{
    if (!phy)
        return;
    if (phy->type == DPTX_PHY_T8152_ATC)
        atc_phy_shutdown(phy->atc);
    free(phy);
}
