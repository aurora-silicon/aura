/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * J873g (Mac mini M6) hardware that iBoot leaves half set up and that Linux
 * cannot finish yet (no T8152 PMGR, eUSB2 PHY or Type-C driver).
 *
 * USB: usb-drd1 (back-left USB-C, HPM hpm1) and usb-drd2 (VL822 hub behind
 * the front USB-C ports) are powered and their eUSB2 PHYs put into host mode
 * with both DARTs still disabled; Linux describes them as plain DWC3 hosts.
 * Every step checks its precondition first, so a controller is either left
 * as iBoot left it or fully set up.
 */

#include "j873.h"
#include "adt.h"
#include "malloc.h"
#include "smc.h"
#include "soc.h"
#include "spmi.h"
#include "string.h"
#include "tps6598x.h"
#include "tunables.h"
#include "utils.h"

#define POLL_US 1000000

struct j873_usb {
    u64 aon, bank, gate, dart0, dart1, pipe, phy, evt, dwc;
    const char *hpm;
    u64 hub_reset;
};

static const struct j873_usb j873_usb[] = {
    {
        .aon = 0x308290140,
        .bank = 0x303a14800,
        .gate = 0x308290160,
        .dart0 = 0x40b020000,
        .dart1 = 0x40b040000,
        .pipe = 0x40a414000,
        .phy = 0x40a418000,
        .evt = 0x40a448000,
        .dwc = 0x40a430000,
        .hpm = "/arm-io/nub-spmi-a0/hpm1",
    },
    {
        .aon = 0x308290180,
        .bank = 0x303a14900,
        .gate = 0x3082901a0,
        .dart0 = 0x383020000,
        .dart1 = 0x383040000,
        .pipe = 0x382414000,
        .phy = 0x382418000,
        .evt = 0x382448000,
        .dwc = 0x382430000,
        /* ADT usb3hub-fw function-usb3hub_rst = nub-gpio GPIO(0x27): iBoot
         * leaves the VL822 RESET# driven low. */
        .hub_reset = 0x3081f0000 + 0x27 * 4,
    },
};

static int expect(u64 addr, u32 want, u32 mask, const char *what)
{
    u32 val = read32(addr);
    if ((val & mask) == want)
        return 0;
    printf("j873: USB %s: 0x%lx = 0x%x, expected 0x%x (mask 0x%x)\n", what, addr, val, want, mask);
    return -1;
}

static int wait32(u64 addr, u32 mask, u32 want, const char *what)
{
    if (!poll32(addr, mask, want, POLL_US))
        return 0;
    printf("j873: USB %s: timed out at 0x%lx = 0x%x\n", what, addr, read32(addr));
    return -1;
}

/* iBoot leaves the port controller in power state 7, where it does not
 * detect a plug; wake it the way usb_init() does on other machines. */
static int j873_hpm_wake(const char *path)
{
    spmi_dev_t *spmi = spmi_init("/arm-io/nub-spmi-a0");
    if (!spmi)
        return -1;
    tps6598x_dev_t *tps = tps6598x_init_spmi(path, spmi);
    int ret = tps ? tps6598x_powerup(tps) : -1;
    /* No tps6598x_shutdown(): it would send the controller back to sleep. */
    free(tps);
    spmi_shutdown(spmi);
    if (ret)
        printf("j873: %s: SSPS 0 failed\n", path);
    return ret;
}

static int j873_usb_power(const struct j873_usb *u)
{
    u32 aon = read32(u->aon), psd = read32(u->bank + 0x80), gate = read32(u->gate);

    if (aon != 768 || (psd & 7) != 6 || gate != 768) {
        printf("j873: USB power state 0x%x 0x%x 0x%x is not iBoot's, not touching it\n", aon, psd,
               gate);
        return -1;
    }
    write32(u->aon, (aon & 0xfffffcf0) | 15);
    if (wait32(u->aon, 0xf0, 0xf0, "AON"))
        return -1;
    write32(u->bank + 0x90, 0);
    if (wait32(u->bank + 0xbc, 1, 1, "PSD ack") || wait32(u->bank + 0x80, 7, 0, "PSD state"))
        return -1;
    write32(u->gate, (gate & 0xfffffcf0) | 15);
    return wait32(u->gate, 0xf0, 0xf0, "gate");
}

static int j873_usb_phy_host(const struct j873_usb *u)
{
    if (expect(u->aon, 1023, ~0, "AON") || expect(u->bank + 0x80, 0, 7, "PSD") ||
        expect(u->gate, 1023, ~0, "gate") || expect(u->dart0 + 0xc00, 0, ~0, "DART0") ||
        expect(u->dart1 + 0xc00, 0, ~0, "DART1") || expect(u->pipe + 0xc, 66, ~0, "PIPE") ||
        expect(u->pipe + 0x1c, 0, ~0, "PIPE") || expect(u->phy, 4, ~0, "PHY") ||
        expect(u->phy + 4, 11, ~0, "PHY"))
        return -1;

    /* USB2-only dummy PIPE path, already selected by firmware. Keep USB3 mux. */
    mask32(u->dwc + 0xc200, 0x40, 0);
    mask32(u->dwc + 0xc2c0, 0x20000, 0);
    write32(u->pipe + 0x1c, 1);
    if (expect(u->dwc + 0xc120, 0x33313130, ~0, "DWC3 ID") ||
        expect(u->dwc + 0xc704, 0, 0x80000000, "DWC3 DCTL"))
        return -1;
    write32(u->pipe + 0x1c, 0);

    /* eUSB2 PHY init in host mode with the USB1/USB2 DFLT/HOST tunables; the
     * device-only VBUS force bits are omitted. */
    mask32(u->phy + 8, 0x7003, 3);
    mask32(u->phy + 0x10, 3, 1);
    mask32(u->phy + 0x14, 0x7f000000, 0x23000000);
    mask32(u->phy + 0x18, 0x703, 0x103);
    mask32(u->phy + 8, 0x7003, 0x7003);
    mdelay(10);
    mask32(u->phy + 4, 8, 0);
    udelay(1000);
    mask32(u->phy + 4, 1, 0);
    mask32(u->phy + 4, 2, 0);
    mask32(u->evt, 0, 9);
    mask32(u->phy + 4, 0, 4);
    mask32(u->phy + 0x1c, 0x20000000, 0);
    mask32(u->phy + 0x1c, 0x40000000, 0);
    udelay(1000);
    if (!read32(u->evt + 0x20)) {
        printf("j873: USB: no UTMI clock events, PHY did not start\n");
        return -1;
    }
    mdelay(5);
    mask32(u->phy, 7, 2);
    write32(u->pipe + 0x1c, 1);
    mask32(u->pipe + 0x20, 0, 0x8000);
    /* Host personality (GCTL.PRTCAPDIR = host) while both DARTs stay disabled. */
    mask32(u->dwc + 0xc110, 0x3000, 0x1000);
    return 0;
}

static bool j873_usb_is_up(const struct j873_usb *u)
{
    return read32(u->aon) == 1023 && !(read32(u->bank + 0x80) & 7) && read32(u->gate) == 1023 &&
           (read32(u->dwc + 0xc110) & 0x3000) == 0x1000;
}

int j873_usb_prep(void)
{
    int ret = 0;

    if (chip_id != T8152 || board_id != 0x24)
        return -1;

    for (size_t i = 0; i < ARRAY_SIZE(j873_usb); i++) {
        const struct j873_usb *u = &j873_usb[i];

        if (j873_usb_is_up(u)) {
            printf("j873: usb-drd%zu already in host mode\n", i + 1);
            continue;
        }
        if (u->hpm && j873_hpm_wake(u->hpm))
            ret = -1;
        if (j873_usb_power(u) || j873_usb_phy_host(u)) {
            printf("j873: usb-drd%zu setup failed\n", i + 1);
            ret = -1;
            continue;
        }
        if (u->hub_reset) {
            /* Output mode (bits 3:1 = 1); set the data bit to release RESET#. */
            if (expect(u->hub_reset, 2, 0xf, "hub reset GPIO")) {
                ret = -1;
                continue;
            }
            set32(u->hub_reset, 1);
        }
        printf("j873: usb-drd%zu in host mode\n", i + 1);
    }
    return ret;
}

/*
 * Internal PCIe (apcie): iBoot leaves the root complex enabled, but the AUS5
 * PHY gated and RC reg 4 (lane configuration) at 0x103, which gives every
 * lane to port 0. Apply the PHY tunables, ungate the PHY, enable PHY common,
 * write the ADT lane-cfg, and apply the port 0 (N1) and port 2 (10GbE) DART
 * tunables while those DARTs are still unused (writing them under live DMA
 * faults the DART). Port bring-up is left to Linux pcie-apple.
 */
int j873_pcie_prep(void)
{
    int path[8];
    u64 rc, phy, phyphy, size;
    u32 lane_cfg;

    if (chip_id != T8152 || board_id != 0x24)
        return -1;

    int node = adt_path_offset_trace(adt, "/arm-io/apcie", path);
    if (node < 0 || adt_get_reg(adt, path, "reg", 1, &rc, &size) < 0 ||
        adt_get_reg(adt, path, "reg", 2, &phy, &size) < 0 ||
        adt_get_reg(adt, path, "reg", 3, &phyphy, &size) < 0 ||
        ADT_GETPROP(adt, node, "lane-cfg", &lane_cfg) < 0) {
        printf("j873: apcie: ADT node incomplete\n");
        return -1;
    }
    if (rc != 0x38c000000 || phy != 0x38f000000 || phyphy != 0x38f020000) {
        printf("j873: apcie: unexpected layout\n");
        return -1;
    }
    if (!(read32(rc + 0x50) & 1)) {
        printf("j873: apcie: root complex not enabled by iBoot, not touching it\n");
        return -1;
    }
    if (read32(rc + 4) == lane_cfg && !(read32(phyphy + 4) & 0x20)) {
        printf("j873: apcie: PHY already up, lane-cfg 0x%x\n", lane_cfg);
        return 0;
    }

    if (tunables_apply_local("/arm-io/apcie", "apcie-phy-tunables", 2) < 0) {
        printf("j873: apcie: PHY tunables failed\n");
        return -1;
    }
    if (read32(phyphy + 4) & 0x20) {
        set32(phyphy + 4, 0x10);
        if (poll32(phyphy + 8, 0x10, 0x10, POLL_US)) {
            printf("j873: apcie: AUS5 PHY did not report ready\n");
            return -1;
        }
        clear32(phyphy + 4, 0x20);
        set32(phyphy, 0x200000);
    }
    set32(phy + 0x4000, 1);
    write32(rc + 4, lane_cfg);

    if (tunables_apply_local("/arm-io/dart-apcie0", "dart-tunables-instance-0", 0) < 0 ||
        tunables_apply_local("/arm-io/dart-apcie2", "dart-tunables-instance-0", 0) < 0) {
        printf("j873: apcie: DART tunables failed\n");
        return -1;
    }
    printf("j873: apcie: PHY up, lane-cfg 0x%x\n", lane_cfg);
    return 0;
}

/* ADT pKW8("key", lo, hi): an eight-byte SMC write of (hi << 32) | lo | level. */
struct j873_smc_pw8 {
    u32 phandle;
    char four_cc[4];
    u32 key;
    u32 lo;
    u32 hi;
};

/*
 * The SMC holds the N1 (Wi-Fi/Bluetooth on apcie port 0) in reset: ADT
 * pci-bridge0 function-reset = pKW8('pcIO', 0x800000, 0x130000). Write
 * level 0, then 1; without it the port 0 link stays in detect.
 */
int j873_n1_power(void)
{
    u32 len;

    if (chip_id != T8152 || board_id != 0x24)
        return -1;

    int node = adt_path_offset(adt, "/arm-io/apcie/pci-bridge0");
    const struct j873_smc_pw8 *fn =
        node < 0 ? NULL : adt_getprop(adt, node, "function-reset", &len);
    if (!fn || len != sizeof(*fn) || memcmp(fn->four_cc, "8WKp", 4)) {
        printf("j873: N1: no pKW8 function-reset in the ADT\n");
        return -1;
    }

    smc_dev_t *smc = smc_init();
    if (!smc)
        return -1;
    int ret = 0;
    for (u32 level = 0; level < 2 && !ret; level++) {
        ret = smc_write_u64(smc, fn->key, ((u64)fn->hi << 32) | fn->lo | level);
        mdelay(10);
    }
    smc_shutdown(smc);
    printf("j873: N1 reset released: %d\n", ret);
    return ret;
}
