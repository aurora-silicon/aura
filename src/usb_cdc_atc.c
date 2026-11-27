/* SPDX-License-Identifier: GPL-2.0-only */

#include "usb_cdc_atc.h"
#include "adt.h"
#include "string.h"
#include "tunables.h"
#include "usb_cdc.h"
#include "utils.h"

/* T8130 ATC register facts from public Asahi Linux atc.c and the J700 ADT. */
#define USB2_CTL         0x04
#define USB2_SIG         0x08
#define USB2_MISCTUNE    0x1c
#define USB2_USBCTL      0x00
#define ATC_EVT_USB2_CTL 0x00
#define ATC_CFG0         0x08
#define ATC_LANE_MODE    0x60
#define ATC_CROSSBAR     0x64
#define ATC_RCAL         0x04
#define ATC_BIAS         0x00
#define ATC_POWER_CTRL   0x00
#define ATC_POWER_STAT   0x04
#define ATC_MISC         0x08

#define PIPE_OVERRIDE    0x00
#define PIPE_MUX         0x0c
#define PIPE_AON_GEN     0x1c
#define PIPE_NONSELECTED 0x20

struct atc_tuning_group {
    const char *name;
    u32 reg;
    u32 offset;
    u32 span;
    bool required;
};

static const struct atc_tuning_group common_groups[] = {
    {"tunable_ATC_FABRIC", 3, 0x44000, 0x4000, true},
    {"tunable_CIO3PLL_CORE", 12, 0x2a00, 0x200, true},
    {"tunable_CIO3PLL_TOP", 11, 0x2800, 0x200, true},
    {"tunable_ACIOPHY_LANE_USBC0", 14, 0x5000, 0x1000, true},
    {"tunable_ACIOPHY_PLL_TOP", 7, 0x1000, 0x4000, true},
    {"tunable_ACIOPHY_TOP", 4, 0x0, 0x4000, true},
    {"tunable_AUSCMN_DIG", 5, 0x800, 0x200, true},
    {"tunable_AUSPLL_CORE", 9, 0x2200, 0x4000, true},
    {"tunable_AUX_TOP", 26, 0x16000, 0x4000, true},
    {"tunable_AUSCMN_SHM", 6, 0xa00, 0x200, true},
    {"tunable_CLKMON_CFG", 10, 0x2600, 0x100, false},
};

static const struct atc_tuning_group lane_groups[2][5] = {
    {{"tunable_LN0_RX_TOP_USB_DFLT", 16, 0x9000, 0x1000, true},
     {"tunable_LN0_RX_EQ_USB_EQA", 17, 0xa000, 0x1000, true},
     {"tunable_LN0_RX_SHM_USB_DFLT", 18, 0xb000, 0x1000, true},
     {"tunable_LN0_TX_TOP_USB_DFLT", 19, 0xc000, 0x1000, true},
     {"tunable_LN0_TX_SHM_USB_DFLT", 20, 0xd000, 0x1000, true}},
    {{"tunable_LN1_RX_TOP_USB_DFLT", 21, 0x10000, 0x1000, true},
     {"tunable_LN1_RX_EQ_USB_EQA", 22, 0x11000, 0x1000, true},
     {"tunable_LN1_RX_SHM_USB_DFLT", 23, 0x12000, 0x1000, true},
     {"tunable_LN1_TX_TOP_USB_DFLT", 24, 0x13000, 0x1000, true},
     {"tunable_LN1_TX_SHM_USB_DFLT", 25, 0x14000, 0x1000, true}},
};

static uintptr_t core, pipe, power, rcal, bias;
static bool phy_ready;
static bool force_swapped;
static int atc_node;
static int atc_path[8];
static unsigned active_lane;

void usb_cdc_atc_force_swapped(bool enable)
{
    force_swapped = enable;
}

/* The lane the last bring-up used. A re-take asks for the same one, in case ATC_MISC's orientation
 * bit doesn't come back with the domain. */
bool usb_cdc_atc_lane(void)
{
    return active_lane;
}

static int validate_group(int node, const struct atc_tuning_group *group)
{
    usb_cdc_wait_us(0);
    u32 length;
    const u8 *records = adt_getprop(adt, node, group->name, &length);
    if (!records)
        return group->required ? -1 : 0;
    /* J700 describes each tuning bank as its own reg entry, not one core span. */
    u64 window, window_size;
    if (!length || length % 12 || group->span < 4 ||
        adt_get_reg(adt, atc_path, "reg", group->reg, &window, &window_size) < 0 ||
        window != core + group->offset || window_size < group->span)
        return -1;
    return tunables_validate_compact("/arm-io/atc-phy0", group->name, group->span);
}

static int apply_group(int node, const struct atc_tuning_group *group)
{
    usb_cdc_wait_us(0);
    if (!adt_getprop(adt, node, group->name, NULL))
        return group->required ? -1 : 0;
    if (validate_group(node, group))
        return -1;
    u64 window;
    if (adt_get_reg(adt, atc_path, "reg", group->reg, &window, NULL) < 0)
        return -1;
    return tunables_apply_compact_addr("/arm-io/atc-phy0", group->name, window, group->span);
}

int usb_cdc_atc_power_on(uintptr_t pipehandler)
{
    int node = adt_path_offset_trace(adt, "/arm-io/atc-phy0", atc_path);
    u64 usb2, usb2_size, core_addr, core_size, axi, axi_size;
    u64 power_addr, power_size, rcal_addr, rcal_size, bias_addr, bias_size;
    if (node < 0 || adt_get_reg(adt, atc_path, "reg", 0, &usb2, &usb2_size) < 0 ||
        adt_get_reg(adt, atc_path, "reg", 4, &core_addr, &core_size) < 0 ||
        adt_get_reg(adt, atc_path, "reg", 5, &rcal_addr, &rcal_size) < 0 ||
        adt_get_reg(adt, atc_path, "reg", 6, &bias_addr, &bias_size) < 0 ||
        adt_get_reg(adt, atc_path, "reg", 30, &power_addr, &power_size) < 0 ||
        adt_get_reg(adt, atc_path, "reg", 31, &axi, &axi_size) < 0 || usb2_size < 0x20 ||
        core_size < ATC_CROSSBAR + 4 || rcal_size < ATC_RCAL + 4 || bias_size < 4 ||
        power_size < ATC_MISC + 4 || axi_size < 0x8000 || rcal_addr != core_addr + 0x800 ||
        bias_addr != core_addr + 0xa00 || power_addr != core_addr + 0x20000)
        return -1;

    core = core_addr;
    power = power_addr;
    rcal = rcal_addr;
    bias = bias_addr;
    pipe = pipehandler;
    phy_ready = false;
    atc_node = node;

    if (tunables_validate_compact("/arm-io/atc-phy0", "tunable_ATC0AXI2AF", 0x8000))
        return -1;
    for (size_t i = 0; i < ARRAY_SIZE(common_groups); i++)
        if (validate_group(node, &common_groups[i]))
            return -1;
    for (size_t lane = 0; lane < ARRAY_SIZE(lane_groups); lane++)
        for (size_t i = 0; i < ARRAY_SIZE(lane_groups[lane]); i++)
            if (validate_group(node, &lane_groups[lane][i]))
                return -1;

    set32(usb2 + USB2_SIG, 0xf);
    clear32(usb2 + USB2_SIG, 7 << 12); /* device role */
    udelay(10);
    clear32(usb2 + USB2_CTL, BIT(3)); /* SIDDQ */
    udelay(10);
    clear32(usb2 + USB2_CTL, BIT(0)); /* RESET */
    udelay(10);
    clear32(usb2 + USB2_CTL, BIT(1)); /* PORT_RESET */
    udelay(10);
    set32(core + ATC_EVT_USB2_CTL, BIT(0) | BIT(3));
    udelay(10);
    set32(usb2 + USB2_CTL, BIT(2)); /* APB_RESET_N */
    udelay(10);
    clear32(usb2 + USB2_MISCTUNE, BIT(29) | BIT(30));
    /* T8140's eUSB2 repeater needs to settle after reset release (Linux atc.c). */
    usb_cdc_wait_us(5000);
    write32(usb2 + USB2_USBCTL, 2); /* RUN */

    /* Park the upstream DWC3 PIPE on the dummy PHY until DWC3 is ready. */
    write32(pipe + PIPE_MUX, 0x22);
    write32(pipe + PIPE_AON_GEN, BIT(0));
    write32(pipe + PIPE_NONSELECTED, 0x9332);

    set32(power + ATC_MISC, BIT(0));
    set32(power + ATC_POWER_CTRL, BIT(0));
    if (usb_cdc_poll32(power + ATC_POWER_STAT, BIT(0), BIT(0), 100000))
        return -1;
    set32(power + ATC_POWER_CTRL, BIT(1));
    if (usb_cdc_poll32(power + ATC_POWER_STAT, BIT(1), BIT(1), 100000))
        return -1;
    clear32(power + ATC_POWER_CTRL, BIT(2));
    set32(power + ATC_POWER_CTRL, BIT(3));

    if (tunables_apply_compact_addr("/arm-io/atc-phy0", "tunable_ATC0AXI2AF", axi, 0x8000))
        return -1;
    for (size_t i = 0; i < ARRAY_SIZE(common_groups); i++) {
        if (apply_group(node, &common_groups[i]))
            return -1;
    }

    if (force_swapped)
        set32(power + ATC_MISC, BIT(2));
    unsigned lane = !!(read32(power + ATC_MISC) & BIT(2));
    active_lane = lane;
    for (size_t i = 0; i < ARRAY_SIZE(lane_groups[0]); i++) {
        if (apply_group(node, &lane_groups[lane][i]))
            return -1;
    }

    const u32 cfg_bits[] = {BIT(2), BIT(3), BIT(0), BIT(1)};
    for (size_t i = 0; i < ARRAY_SIZE(cfg_bits); i++) {
        set32(core + ATC_CFG0, cfg_bits[i]);
        udelay(10);
    }
    clear32(core + ATC_CFG0, BIT(4));
    udelay(10);
    set32(core + ATC_CFG0, BIT(5));
    udelay(10);
    set32(bias + ATC_BIAS, BIT(1));
    udelay(10);

    u32 lane_mode = lane ? 0x252 : 0x489;
    u32 crossbar = 0x110 | lane;
    write32(core + ATC_LANE_MODE, lane_mode);
    write32(core + ATC_CROSSBAR, crossbar);
    set32(power + ATC_POWER_CTRL, BIT(4));
    if (usb_cdc_poll32(rcal + ATC_RCAL, BIT(0), BIT(0), 100000))
        return -1;

    phy_ready = true;
    printf("CDC ATC: lane=%u mode=%x crossbar=%x rcal=%x\n", lane, lane_mode, crossbar,
           read32(rcal + ATC_RCAL));
    return 0;
}

int usb_cdc_atc_switch_pipe(void)
{
    if (!phy_ready || (read32(power + ATC_POWER_STAT) & 3) != 3 ||
        (read32(power + ATC_POWER_CTRL) & (BIT(3) | BIT(4))) != (BIT(3) | BIT(4)) ||
        !(read32(power + ATC_MISC) & BIT(0)) || !(read32(rcal + ATC_RCAL) & BIT(0)))
        return -1;

    if (force_swapped)
        set32(power + ATC_MISC, BIT(2));
    unsigned lane = !!(read32(power + ATC_MISC) & BIT(2));
    if (lane != active_lane) {
        /* The HPM may have changed orientation during a cable reconnect. */
        clear32(power + ATC_POWER_CTRL, BIT(4));
        for (size_t i = 0; i < ARRAY_SIZE(lane_groups[0]); i++) {
            if (apply_group(atc_node, &lane_groups[lane][i]))
                return -1;
        }
        write32(core + ATC_LANE_MODE, lane ? 0x252 : 0x489);
        write32(core + ATC_CROSSBAR, 0x110 | lane);
        set32(power + ATC_POWER_CTRL, BIT(4));
        if (poll32(rcal + ATC_RCAL, BIT(0), BIT(0), 100000))
            return -1;
        active_lane = lane;
        printf("CDC ATC: cable orientation changed, lane=%u\n", lane);
    }

    mask32(pipe + PIPE_MUX, 7 << 3, 0);
    udelay(10);
    mask32(pipe + PIPE_MUX, 7, 0);
    udelay(10);
    mask32(pipe + PIPE_MUX, 7 << 3, BIT(3));
    udelay(10);
    clear32(pipe + PIPE_OVERRIDE, BIT(0) | BIT(2));
    return 0;
}
