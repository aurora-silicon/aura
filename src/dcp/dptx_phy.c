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
    DPTX_PHY_T8132,
    DPTX_PHY_T8152_ATC,
    DPTX_PHY_T8152_HDMI,
};

enum dptx_t8132_cal {
    T8132_DCOCFG,
    T8132_DTCVREG,
    T8132_DTCCAL,
    T8132_DCDADJ,
    T8132_DLFCFG,
    T8132_RODCOROLEAKCOMP,
    T8132_NR_CAL,
};

static const char *const dptx_t8132_cal_props[T8132_NR_CAL] = {
    [T8132_DCOCFG] = "DCOCFG", [T8132_DTCVREG] = "DTCVREG",
    [T8132_DTCCAL] = "DTCCAL", [T8132_DCDADJ] = "DCDADJ",
    [T8132_DLFCFG] = "DLFCFG", [T8132_RODCOROLEAKCOMP] = "RODCOROLEAKCOMP",
};

typedef struct dptx_phy {
    u64 regs[2];
    enum dptx_type type;
    u32 dcp_index;
    u32 active_lanes;
    u32 rate;
    u32 dfp_output;
    bool active;
    bool pclkdrv_select;
    u32 cal[T8132_NR_CAL];
    bool cal_set[T8132_NR_CAL];
    const char *path;
    atc_phy_t *atc;
} dptx_phy_t;

/*
 * T8132 (M4) dedicated HDMI dptx-phy (dptx-phy,t8132).
 * regs[0] is the dptx block (PLL/PHY/lanes/AUX) and regs[1] is the core block
 * (source select at +0x10, block power at +0x48, shadow latch at +0xa0).
 */
static void dptx_t8132_rmw(u64 base, u32 off, u32 shift, u32 mask, u32 val)
{
    mask32(base + off, mask, (val << shift) & mask);
}

#define DPTX_T8132_D(phy, off, shift, mask, val)                                                   \
    dptx_t8132_rmw((phy)->regs[0], off, shift, mask, val)
#define DPTX_T8132_C(phy, off, shift, mask, val)                                                   \
    dptx_t8132_rmw((phy)->regs[1], off, shift, mask, val)

/* core+0x48: PLL (bits 6:4), AUX (10:8) and PHY (2:0) power, 1 us apart. */
static void dptx_t8132_block_power(dptx_phy_t *phy, u32 lsb, bool on)
{
    if (on) {
        DPTX_T8132_C(phy, 0x48, lsb, BIT(lsb), 1);
        udelay(1);
        DPTX_T8132_C(phy, 0x48, lsb + 1, BIT(lsb + 1), 1);
        udelay(1);
        DPTX_T8132_C(phy, 0x48, lsb + 2, BIT(lsb + 2), 0);
        udelay(1);
    } else {
        DPTX_T8132_C(phy, 0x48, lsb + 2, BIT(lsb + 2), 1);
        udelay(1);
        DPTX_T8132_C(phy, 0x48, lsb + 1, BIT(lsb + 1), 0);
        udelay(1);
        DPTX_T8132_C(phy, 0x48, lsb, BIT(lsb), 0);
        udelay(1);
    }
}

static void dptx_t8132_device_power(dptx_phy_t *phy, bool on)
{
    DPTX_T8132_C(phy, 0x10, 0, 0x7, phy->dcp_index);
    dptx_t8132_block_power(phy, 4, on); /* PLL */
    dptx_t8132_block_power(phy, 8, on); /* AUX */
    dptx_t8132_block_power(phy, 0, on); /* PHY */
}

static void dptx_t8132_pll_core_cfg(dptx_phy_t *phy)
{
    const u32 *v = phy->cal;
    const bool *set = phy->cal_set;

    if (set[T8132_DCOCFG]) {
        DPTX_T8132_D(phy, 0x2240, 1, 0x6, (v[T8132_DCOCFG] >> 3) & 3);
        DPTX_T8132_D(phy, 0x2240, 4, 0x70, v[T8132_DCOCFG] & 7);
    }
    if (set[T8132_DTCVREG])
        DPTX_T8132_D(phy, 0x2230, 0, 0x1f, v[T8132_DTCVREG]);
    if (set[T8132_DTCCAL])
        DPTX_T8132_D(phy, 0x22e8, 17, 0x60000, v[T8132_DTCCAL]);
    if (set[T8132_DCDADJ])
        DPTX_T8132_D(phy, 0x2240, 22, 0x7c00000, v[T8132_DCDADJ]);
    if (set[T8132_DLFCFG])
        DPTX_T8132_D(phy, 0x22b0, 20, 0x700000, v[T8132_DLFCFG]);
    if (set[T8132_RODCOROLEAKCOMP])
        DPTX_T8132_D(phy, 0x2240, 19, 0x180000, v[T8132_RODCOROLEAKCOMP]);

    /* Fixed PLL core tunables from Linux drivers/phy/apple/dptx.c
     * t6050_pll_core_cfg() (commit c0e42a1852a6, captured from macOS
     * AppleDPTXPhy on T8122/T8132/T6050). */
    DPTX_T8132_D(phy, 0x22e8, 0, 0x1, 1);
    DPTX_T8132_D(phy, 0x2240, 3, 0x8, 1);
    DPTX_T8132_D(phy, 0x2240, 0, 0x1, 1);
    DPTX_T8132_D(phy, 0x2224, 10, 0xc00, 2);
    DPTX_T8132_D(phy, 0x22b0, 27, 0x08000000, 0);
    DPTX_T8132_D(phy, 0x2338, 23, 0x0f800000, 0xf);
}

static bool dptx_t8132_pclk(dptx_phy_t *phy, bool on)
{
    u32 bit = phy->pclkdrv_select ? 3 : 1;

    if (!on && !(read32(phy->regs[0] + 0x2200) & BIT(bit)))
        return false;
    DPTX_T8132_D(phy, 0x2200, bit, BIT(bit), on);
    return true;
}

static void dptx_t8132_configure_pll(dptx_phy_t *phy)
{
    DPTX_T8132_D(phy, 0x2218, 0, 0x1, 1);
    DPTX_T8132_D(phy, 0x2234, 0, 0x3, 0);
    dptx_t8132_pclk(phy, true);
    DPTX_T8132_D(phy, 0x2320, 1, 0x6, 1); /* single-ended clock output */
}

static int dptx_t8132_impedance_calibration(dptx_phy_t *phy)
{
    u32 done, code;

    DPTX_T8132_D(phy, 0x4224, 0, 0x1, 0);
    mdelay(1);
    DPTX_T8132_D(phy, 0x4224, 0, 0x1, 1);
    udelay(1);
    /* AUX impedance calibration target/enable mask (0x3c in bits 9:4, matching
     * T8152's 0x1c00[9:4]=0x3c and Linux t6050_impedance_calibration()). */
    DPTX_T8132_D(phy, 0x4200, 4, 0x3f0, 0x3c);
    udelay(1);
    DPTX_T8132_D(phy, 0x4200, 0, 0x1, 1);
    udelay(125);
    done = read32(phy->regs[0] + 0x4204) & 1;
    udelay(10);
    DPTX_T8132_D(phy, 0x4200, 0, 0x1, 0);
    code = (read32(phy->regs[0] + 0x4210) >> 18) & 0x7f;
    if (!done) {
        printf("DPTX-PHY: T8132 impedance calibration did not complete (0x4204 %#x)\n",
               read32(phy->regs[0] + 0x4204));
        return -1;
    }
    DPTX_T8132_D(phy, 0x4228, 16, 0x7f0000, code);
    DPTX_T8132_D(phy, 0x4228, 15, 0x8000, 1);
    dprintf("DPTX-PHY: T8132 impedance calibration code %#x\n", code);
    return 0;
}

static int dptx_t8132_activate(dptx_phy_t *phy)
{
    if (phy->active)
        return 0;
    dptx_t8132_device_power(phy, true);
    phy->active = true;
    dptx_t8132_pll_core_cfg(phy);
    dptx_t8132_configure_pll(phy);
    /* PLL activation */
    DPTX_T8132_D(phy, 0x1000, 1, 0x2, 0);
    DPTX_T8132_D(phy, 0x4004, 3, 0x8, 1);
    /* PHY activation: every lane, highest first */
    udelay(1);
    for (int l = DPTX_MAX_LANES - 1; l >= 0; l--)
        DPTX_T8132_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 9, 0x200, 0);
    /* AUX power-up */
    DPTX_T8132_D(phy, 0xd000, 0, 0x1, 0);
    return dptx_t8132_impedance_calibration(phy);
}

static int dptx_t8132_pll_command(dptx_phy_t *phy, u32 cmd)
{
    bool up = cmd == 7;
    u32 ack, sts;

    DPTX_T8132_D(phy, 0x100c, 0, 0x7, cmd);
    DPTX_T8132_D(phy, 0x100c, 3, 0x8, 1);
    udelay(100);
    ack = read32(phy->regs[0] + 0x1014);
    DPTX_T8132_D(phy, 0x100c, 3, 0x8, 0);
    sts = read32(phy->regs[0] + 0x1008);
    if (!(ack & 1)) {
        printf("DPTX-PHY: T8132 PLL power %s: no ACK (0x1014 %#x)\n", up ? "up" : "down", ack);
        return -1;
    }
    if (!!(sts & BIT(3)) != up) {
        printf("DPTX-PHY: T8132 PLL power %s: lock is %s (0x1008 %#x)\n", up ? "up" : "down",
               up ? "missing" : "still set", sts);
        return -1;
    }
    return 0;
}

static int dptx_t8132_pll_power_down(dptx_phy_t *phy)
{
    if (!dptx_t8132_pclk(phy, false))
        return 0;
    int ret = dptx_t8132_pll_command(phy, 0);
    udelay(1);
    return ret;
}

static int dptx_t8132_pll_power_up(dptx_phy_t *phy)
{
    int ret = dptx_t8132_pll_command(phy, 7);
    if (!ret)
        dptx_t8132_pclk(phy, true);
    return ret;
}

static void dptx_t8132_phy_power_down(dptx_phy_t *phy)
{
    DPTX_T8132_D(phy, 0x4004, 0, 0x1, 1);
    DPTX_T8132_D(phy, 0x4000, 6, 0x40, 0);
    DPTX_T8132_D(phy, 0x4004, 3, 0x8, 0);
    DPTX_T8132_D(phy, 0x4000, 25, 0x2000000, 0);
    DPTX_T8132_D(phy, 0x4000, 24, 0x1000000, 0);
}

static void dptx_t8132_phy_power_up(dptx_phy_t *phy)
{
    DPTX_T8132_D(phy, 0x4004, 3, 0x8, 1);
    for (int l = 0; l < DPTX_MAX_LANES; l++)
        DPTX_T8132_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l + 0x10, 27, 0x38000000, 4);
    DPTX_T8132_D(phy, 0x4030, 0, 0x1, 1); /* TXA LDO low-power mode */
    udelay(1);
    DPTX_T8132_D(phy, 0x4000, 24, 0x1000000, 1);
    udelay(1);
    DPTX_T8132_D(phy, 0x4000, 25, 0x2000000, 1);
    udelay(1);
    DPTX_T8132_D(phy, 0x4000, 6, 0x40, 1);
    udelay(1);
    DPTX_T8132_D(phy, 0x4004, 0, 0x1, 0);
}

static int dptx_t8132_set_lanes(dptx_phy_t *phy, u32 n)
{
    if (n == 3 || n > DPTX_MAX_LANES)
        return -1;
    if (!phy->active)
        return 0;
    DPTX_T8132_D(phy, 0x4000, 26, 0x4000000, 1);
    udelay(1);
    for (u32 l = 0; l < DPTX_MAX_LANES; l++)
        DPTX_T8132_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 8, 0x100, n <= l);
    if (n)
        DPTX_T8132_D(phy, 0x4000, 26, 0x4000000, 0);
    phy->active_lanes = n;
    return 0;
}

/*
 * T8132/T6050 dedicated HDMI PHY PLL configuration (register map and per-rate
 * analog tunables match Linux drivers/phy/apple/dptx.c, commit c0e42a1852a6
 * "phy: apple: dptx: Add the T6050 variant", derived from macOS AppleDPTXPhy
 * register captures).
 *
 * Feedback dividers are computed from the 24 MHz reference clock:
 *   - RBR  (1620 MHz): full-rate, VCO = 2 * rate = 3240 MHz = 135 * 24
 *   - HBR  (2700 MHz): half-rate, VCO = rate     = 2700 MHz = 112.5 * 24
 *   - HBR2 (5400 MHz): half-rate + tx_div_sel=1, VCO = rate / 2 = 2700 MHz = 112.5 * 24
 *   - HBR3 (8100 MHz): half-rate + tx_div_sel=1, VCO = rate / 2 = 4050 MHz = 168.75 * 24
 * The symbol-clock divider pclk_div (0x20b8[13:9]) is (vco_mhz / (rate_mhz / 10)) - 1.
 * Remaining analog fields come from the macOS AppleDPTXPhy per-rate capture:
 *   - pre_div (0x20b8[21:20]), tx_div_sel (0x20b8[22]), post_div (0x20b8[15:14])
 *   - vco_band (0x20b8[19:16]), lpf_cfg (0x2234[31:23]), dco_cfg (0x2244[15:0])
 *   - bypass (0x222c[1:0], PLL output divider bypass for VCO <= 3240 MHz).
 */
#define DPTX_T8132_REF_MHZ 24U

struct dptx_t8132_pll_cfg {
    u32 rate_mhz;
    u32 vco_mhz;
    u8 pre_div;
    u8 tx_div_sel;
    u8 post_div;
    u8 vco_band;
    u16 lpf_cfg;
    u16 dco_cfg;
    bool bypass;
};

static const struct dptx_t8132_pll_cfg dptx_t8132_pll_cfgs[] = {
    {1620, 3240, 1, 0, 1, 5, 0x80, 0x8804, true},
    {2700, 2700, 1, 0, 1, 5, 0x80, 0x8804, true},
    {5400, 2700, 1, 1, 1, 5, 0x100, 0x8984, true},
    {8100, 4050, 1, 1, 1, 6, 0x100, 0x8984, false},
};

static void dptx_t8132_shadow_update(dptx_phy_t *phy)
{
    set32(phy->regs[1] + 0xa0, 0x8);
    set32(phy->regs[1] + 0xa0, 0x4);
    set32(phy->regs[1] + 0xa0, 0x40000);
    clear32(phy->regs[1] + 0xa0, 0x40000);
    set32(phy->regs[1] + 0xa0, 0x80000);
    clear32(phy->regs[1] + 0xa0, 0x80000);
    clear32(phy->regs[1] + 0xa0, 0x4);
    clear32(phy->regs[1] + 0xa0, 0x8);
}

static int dptx_t8132_program_rate(dptx_phy_t *phy, u32 rate)
{
    const struct dptx_t8132_pll_cfg *c = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(dptx_t8132_pll_cfgs); i++) {
        if (dptx_t8132_pll_cfgs[i].rate_mhz == rate)
            c = &dptx_t8132_pll_cfgs[i];
    }
    if (!c)
        return -1;

    /* Compute integer and 14-bit fractional feedback divider from 24 MHz refclk:
     * frac_den = floor(0x3fff / den) * den, matching the hardware sigma-delta scale. */
    u32 fb_int = c->vco_mhz / DPTX_T8132_REF_MHZ;
    u32 rem = c->vco_mhz % DPTX_T8132_REF_MHZ;
    u32 fb_frac_num = 0;
    u32 fb_frac_den = 0;
    if (rem) {
        u32 a = rem, b = DPTX_T8132_REF_MHZ;
        while (b) {
            u32 t = a % b;
            a = b;
            b = t;
        }
        u32 num = rem / a;
        u32 den = DPTX_T8132_REF_MHZ / a;
        u32 scale = 0x3fff / den;
        fb_frac_num = num * scale;
        fb_frac_den = den * scale;
    }
    u32 fb_quad = 4 * fb_int + (fb_frac_den ? (4 * fb_frac_num) / fb_frac_den : 0);
    u32 frac_den_reg = fb_frac_num > 1 ? fb_frac_den : 0;
    u32 pclk_div = (c->vco_mhz / (c->rate_mhz / 10)) - 1;
    bool half = rate >= 2000;

    DPTX_T8132_D(phy, 0x222c, 1, 0x2, c->bypass);
    DPTX_T8132_D(phy, 0x222c, 0, 0x1, !c->bypass);
    DPTX_T8132_D(phy, 0x2244, 0, 0xffff, c->dco_cfg);
    DPTX_T8132_D(phy, 0x2234, 23, 0xff800000, c->lpf_cfg);
    DPTX_T8132_D(phy, 0x20b0, 0, 0x3ff, fb_quad);
    DPTX_T8132_D(phy, 0x20b0, 10, 0x400, 0);
    DPTX_T8132_D(phy, 0x20b4, 14, 0x0fffc000, fb_frac_num);
    DPTX_T8132_D(phy, 0x20b4, 0, 0x3fff, frac_den_reg);
    DPTX_T8132_D(phy, 0x20b8, 9, 0x3e00, pclk_div);
    DPTX_T8132_D(phy, 0x20b8, 14, 0xc000, c->post_div);
    DPTX_T8132_D(phy, 0x20b8, 16, 0xf0000, c->vco_band);
    DPTX_T8132_D(phy, 0x20b8, 20, 0x300000, c->pre_div);
    DPTX_T8132_D(phy, 0x20b8, 22, 0x400000, c->tx_div_sel);
    dptx_t8132_shadow_update(phy);

    /* half-link-rate: PHY, then PLL */
    DPTX_T8132_D(phy, 0x4004, 5, 0x20, !half);
    DPTX_T8132_D(phy, 0x4004, 6, 0x40, half);
    DPTX_T8132_D(phy, 0x2000, 29, 0x20000000, !half);
    DPTX_T8132_D(phy, 0x201c, 5, 0x20, !half);
    phy->rate = rate;
    return 0;
}

static int dptx_t8132_reconfigure(dptx_phy_t *phy, bool set_lanes, u32 lanes, bool set_rate,
                                  u32 rate)
{
    if (!phy->active)
        return -1;
    dptx_t8132_phy_power_down(phy);
    int ret = dptx_t8132_pll_power_down(phy);
    if (ret)
        return ret;
    if (set_lanes) {
        ret = dptx_t8132_set_lanes(phy, lanes);
        if (ret)
            return ret;
    }
    if (set_rate) {
        if (!rate) {
            phy->rate = 0;
            return 0;
        }
        ret = dptx_t8132_program_rate(phy, rate);
        if (ret)
            return ret;
    }
    if (!phy->rate)
        return 0;
    ret = dptx_t8132_pll_power_up(phy);
    if (ret)
        return ret;
    dptx_t8132_phy_power_up(phy);
    return 0;
}

static int dptx_t8132_deactivate(dptx_phy_t *phy)
{
    if (!phy->active)
        return 0;
    dptx_t8132_set_lanes(phy, 0);
    dptx_t8132_phy_power_down(phy);
    for (int l = 0; l < DPTX_MAX_LANES; l++)
        DPTX_T8132_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 9, 0x200, 1);
    int ret = dptx_t8132_pll_power_down(phy);
    DPTX_T8132_D(phy, 0xd000, 0, 0x1, 1); /* AUX power-down */
    phy->active = false;
    phy->rate = 0;
    dptx_t8132_device_power(phy, false);
    return ret;
}

/* Read-modify-write of a field in the core register block (regs[0]). */
static void dptx_t8152_set_bits(dptx_phy_t *phy, u32 offset, u32 shift, u32 mask, u32 value)
{
    u32 reg = read32(phy->regs[0] + offset);
    write32(phy->regs[0] + offset, (reg & ~mask) | (value << shift));
}

struct dptx_t8152_tunable {
    const char *name;
    u32 offset;
};

/* The T8152 DPTX tunable groups in the ADT and where each group starts in the
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

/* Every ADT tunable record is {offset, width, mask, pad, value, pad} and is
 * applied as an unshifted read-modify-write. */
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

/* Per-lane configuration on the lane shm blocks. The bits are lane-disable
 * bits: start from 0xf and clear one bit per active lane, so 0x2420 holds the
 * mask of unused lanes (0 with four lanes, as read back on J873g). */
static void dptx_t8152_set_active_lane_count(dptx_phy_t *phy, u32 num_lanes)
{
    static const u32 offsets[] = {0x64, 0x24, 0x24, 0x48, 0x48, 0x5c, 0x5c, 0x48};
    static const u32 shifts[] = {4, 4, 1, 0, 1, 2, 3, 4};
    static const u32 masks[] = {0x10, 0x10, 0x2, 0x1, 0x2, 0x4, 0x8, 0x10};

    dptx_t8152_set_bits(phy, 0x2400, 0, 0x1, 0);

    for (u32 lane = 0; lane < DPTX_MAX_LANES; lane++) {
        u32 base = DPTX_T8152_LANE0_OFFSET + lane * DPTX_T8152_LANE_STRIDE;
        for (size_t i = 0; i < ARRAY_SIZE(offsets); i++)
            dptx_t8152_set_bits(phy, base + offsets[i], shifts[i], masks[i], lane >= num_lanes);
    }

    dptx_t8152_set_bits(phy, 0x2420, 0, 0xf, 0xf & ~((1 << num_lanes) - 1));
    dptx_t8152_set_bits(phy, 0x2400, 0, 0x1, 1);
}

/* DP PLL configuration words for each supported link rate. */
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

/* PLL power command: command in 0x408[2:0], request bit 3, acknowledge
 * bit 19. */
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

/* Link rate change: PHY and PLL down, PLL frequency, half link rate for the
 * PHY and PLL, then PLL up. */
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

    /* PLL frequency, downspread off */
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

    /* Latch the link-rate dependent shadow registers */
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

/* Impedance calibration: the adapter reports completion in bit 0 of 0x1c04
 * and the calibration result in 0x1c24. */
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

/* T8152 activation: power up, apply the adapter tunables from the devicetree,
 * then activate the PLL, PHY and AUX. */
static int dptx_t8152_activate(dptx_phy_t *phy)
{
    /* DPTX source select: only the low nibble */
    mask32(phy->regs[1] + 0x10, 0xf, phy->dcp_index);

    /* PLL, AUX and PHY power enables */
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

    /* PLL: single-ended clock output, no pclkdrv-select property */
    dptx_t8152_set_bits(phy, 0x08d0, 4, 0x10, 1);
    dptx_t8152_set_bits(phy, 0x0000, 1, 0x02, 1);     /* PLL on */
    dptx_t8152_set_bits(phy, 0x1400, 0, 0x01, 0);     /* AUX common control */
    dptx_t8152_set_bits(phy, 0x1c00, 4, 0x3f0, 0x3c); /* PHY on */
    dptx_t8152_set_bits(phy, 0x0000, 0, 0x01, 1);
    mdelay(1);

    int ret = dptx_phy_set_active_lane_count(phy, DPTX_MAX_LANES);
    if (ret)
        return ret;

    /* T8152 needs no separate AUX power-up or termination override */
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
    if (phy->type == DPTX_PHY_T8132)
        return dptx_t8132_activate(phy);
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
    if (phy->type == DPTX_PHY_T8132)
        return dptx_t8132_reconfigure(phy, true, num_lanes, false, 0);
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
    /* The dedicated HDMI PHYs are not reprogrammed for drive settings; the
     * request is only accepted. */
    if (phy && (phy->type == DPTX_PHY_T8152_HDMI || phy->type == DPTX_PHY_T8132))
        return 0;
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    return atc_phy_set_drive_settings(phy->atc, settings, count);
}

int dptx_phy_set_link_rate(dptx_phy_t *phy, u32 link_rate)
{
    if (phy->type == DPTX_PHY_T8152_ATC)
        return link_rate % 270 ? -1 : atc_phy_set_link_rate(phy->atc, link_rate / 270);
    if (phy->type == DPTX_PHY_T8132)
        return dptx_t8132_reconfigure(phy, false, 0, true, link_rate);
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
    if (phy && phy->type == DPTX_PHY_T8132)
        return dptx_t8132_deactivate(phy);
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    int ret = atc_phy_deactivate(phy->atc);
    if (!ret)
        phy->active_lanes = 0;
    return ret;
}

int dptx_phy_set_route(dptx_phy_t *phy, bool enable)
{
    /* The dedicated HDMI PHYs have no per-link route to switch. */
    if (phy && (phy->type == DPTX_PHY_T8152_HDMI || phy->type == DPTX_PHY_T8132))
        return 0;
    if (!phy || phy->type != DPTX_PHY_T8152_ATC)
        return -1;
    return atc_phy_set_route(phy->atc, enable);
}

/* Highest link rate in MHz: the HDMI PHYs have PLL configurations up to HBR3
 * (macOS trains the converter at HBR3); everything else is qualified at HBR. */
u32 dptx_phy_max_link_rate(dptx_phy_t *phy)
{
    return phy && (phy->type == DPTX_PHY_T8152_HDMI || phy->type == DPTX_PHY_T8132) ? 8100 : 2700;
}

bool dptx_phy_supports_hpd(dptx_phy_t *phy)
{
    return phy && phy->type != DPTX_PHY_T8132;
}

bool dptx_phy_needs_validate(dptx_phy_t *phy)
{
    return phy && phy->type == DPTX_PHY_T8132;
}

u32 dptx_phy_dcp_output(dptx_phy_t *phy)
{
    switch (phy->type) {
        case DPTX_PHY_T8112:
            return 5;
        case DPTX_PHY_T602X:
            return 4;
        case DPTX_PHY_T8132:
            return phy->dfp_output;
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

/* The T8152 HDMI phy register block is power-gated until its PMGR device is
 * enabled (see dcp_init). Probe it under the exception guard so a gated block
 * fails the init instead of aborting in the middle of the activation sequence
 * and rebooting. */
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
    else if (adt_is_compatible(adt, node, "dptx-phy,t8132"))
        type = DPTX_PHY_T8132;
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

    if (type == DPTX_PHY_T8132) {
        u32 val = 0;
        for (int i = 0; i < T8132_NR_CAL; i++) {
            if (ADT_GETPROP(adt, node, dptx_t8132_cal_props[i], &val) < 0 || val == 0xffffffff)
                continue;
            phy->cal[i] = val;
            phy->cal_set[i] = true;
        }
        if (ADT_GETPROP(adt, node, "pclkdrv-select", &val) >= 0 && val)
            phy->pclkdrv_select = true;
        if (ADT_GETPROP(adt, node, "dp-switch-dfp-endpoint", &val) >= 0)
            phy->dfp_output = val;
        else
            phy->dfp_output = 4;
    }

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

    if ((type == DPTX_PHY_T8152_HDMI || type == DPTX_PHY_T8132) && !dptx_phy_accessible(phy)) {
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
