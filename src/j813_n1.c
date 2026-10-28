/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * J813 (T8142, MacBook Air M5): train the PCIe link to the N1 Wi-Fi/Bluetooth
 * module on apcie port 0, so Linux can use the root port as a generic ECAM
 * host. The sequence follows macOS 25G76 AppleT8142PCIe / APCIECoreRCGen4:
 * AUS5 PHY power-up, root complex, N1 reset, port reset, refclk buffer,
 * PERST, root complex init and LTSSM.
 *
 * ADT /arm-io/apcie reg map (AppleT8142PCIe::start): 0 config (ECAM),
 * 1 APCIe common (RC), 2 PHY (common block at +0x4000), 3 PHY-PHY, 4 PHY IP,
 * 5 AXI2AF, 8.. port 0 (8 config, 10 PHY glue).
 */

#include "j813_n1.h"
#include "adt.h"
#include "libfdt/libfdt.h"
#include "pmgr.h"
#include "smc.h"
#include "tunables.h"
#include "utils.h"

#define APCIE      "/arm-io/apcie"
#define APCIE_PORT "/arm-io/apcie/pci-bridge0"

#define PHY_COMMON_OFF 0x4000

#define PORT_LTSSMCTL 0x80
#define PORT_PIPE_STS 0xa8
#define PORT_LINKSTS  0x208
#define PORT_APPCLK   0x800
#define PORT_STATUS   0x804
#define PORT_PERST    0x82c

#define DBI_RO_WR 0x8bc

/* SMC key gP13: ADT pci-bridge0 function-reset = pKW4('gP13', 0x800000). */
#define SMC_KEY_GP13 0x67503133
#define N1_RESET_LO  0x800000

/* AP GPIO 192 (ADT pci-bridge0 function-perst), driven high as output. */
#define GPIO_N1_PERST 0x39a000300

static int j813_reg(int *path, int idx, u64 *addr)
{
    if (adt_get_reg(adt, path, "reg", idx, addr, NULL) < 0) {
        printf("j813-n1: missing %s reg[%d]\n", APCIE, idx);
        return -1;
    }
    return 0;
}

static int j813_wait(u64 addr, u32 mask, u32 target, u32 timeout_us, const char *what)
{
    if (poll32(addr, mask, target, timeout_us)) {
        printf("j813-n1: timeout waiting for %s (0x%x)\n", what, read32(addr));
        return -1;
    }
    return 0;
}

static int j813_tunables(const char *path, const char *prop, u64 base)
{
    if (tunables_apply_local_addr(path, prop, base)) {
        printf("j813-n1: applying %s failed\n", prop);
        return -1;
    }
    return 0;
}

static int j813_n1_reset(void)
{
    smc_dev_t *smc = smc_init();
    int ret = 0;

    if (!smc)
        return -1;
    for (u32 level = 0; level < 2 && !ret; level++) {
        ret = smc_write_u32(smc, SMC_KEY_GP13, N1_RESET_LO | level);
        mdelay(50);
    }
    smc_shutdown(smc);
    return ret;
}

static int j813_n1_link(void)
{
    int path[8];
    u64 ecam, rc, phy, phyphy, phyip, axi, port, lane;

    if (adt_path_offset_trace(adt, APCIE, path) < 0)
        return -1;
    if (j813_reg(path, 0, &ecam) || j813_reg(path, 1, &rc) || j813_reg(path, 2, &phy) ||
        j813_reg(path, 3, &phyphy) || j813_reg(path, 4, &phyip) || j813_reg(path, 5, &axi) ||
        j813_reg(path, 8, &port) || j813_reg(path, 10, &lane))
        return -1;
    u64 phycommon = phy + PHY_COMMON_OFF;

    if (pmgr_adt_power_enable(APCIE))
        return -1;

    /* _apcieGPInit: PHY tunables, reference PLL. */
    if (j813_tunables(APCIE, "apcie-phy-tunables", phy) ||
        j813_wait(phycommon, BIT(31), BIT(31), 1000000, "refpll_clk_good"))
        return -1;

    /* AppleT8142PCIe::_initializeGen4Phy */
    set32(phyphy, BIT(0));
    if (j813_wait(phyphy, BIT(2), BIT(2), 1000000, "sleep_b_sml_out"))
        return -1;
    udelay(1000);
    set32(phyphy, BIT(1));
    if (j813_wait(phyphy, BIT(3), BIT(3), 1000000, "sleep_b_big_out"))
        return -1;
    clear32(phyphy, BIT(4));
    udelay(1000);
    set32(phyphy + 4, BIT(0));
    if (j813_tunables(APCIE, "apcie-phy-ip-pll-tunables", phyip) ||
        j813_tunables(APCIE, "apcie-phy-ip-auspma-tunables", phyip))
        return -1;
    clear32(phyip + 0x90, BIT(16));
    set32(phyphy + 4, BIT(4));
    if (j813_wait(phyphy + 8, BIT(0), BIT(0), 1000000, "max_pclk_good"))
        return -1;

    set32(phycommon, BIT(0));
    set32(phyphy, BIT(27));
    if (j813_tunables(APCIE, "apcie-axi2af-tunables", axi))
        return -1;

    /* _enableRootComplex */
    write32(rc + 0x54, 0x140);
    set32(rc + 0x50, BIT(0));
    if (j813_wait(rc + 0x58, BIT(0), BIT(0), 250000, "gtb_initialized"))
        return -1;

    if (j813_n1_reset()) {
        printf("j813-n1: SMC N1 reset release failed\n");
        return -1;
    }
    mdelay(100);

    /* APCIECoreRCGen4Port::_resetPortHardware */
    static const u32 port_reset[][2] = {
        {0x88, 0x110},      {0x148, 0xffffffff}, {0x210, 0xffffffff}, {0x84, 0},
        {0x104, 0xfffffff0}, {0x124, 0x100},      {0x16c, 0},          {0x13c, 0},
        {0x800, 0x100100},  {0x808, 0x1001ff},   {0x82c, 0x10000},    {0x130, 0x3020000},
        {0x140, 0x10},      {0x144, 0x253770},   {0x834, 0},          {0x83c, 0},
    };
    for (size_t i = 0; i < ARRAY_SIZE(port_reset); i++)
        write32(port + port_reset[i][0], port_reset[i][1]);
    if (j813_tunables(APCIE_PORT, "apcie-config-tunables", port))
        return -1;
    set32(port + PORT_APPCLK, BIT(0));

    /* APCIECoreRCGen4Port::_initializeRefclkBuffer (port PHY lane reg 0) */
    set32(lane, BIT(0));
    if (j813_wait(lane, BIT(2), BIT(2), 100000, "refclk buffer (1)"))
        return -1;
    set32(lane, BIT(1));
    if (j813_wait(lane, BIT(3), BIT(3), 100000, "refclk buffer (2)"))
        return -1;
    clear32(lane, BIT(4));
    udelay(1000);
    set32(lane, BIT(9));
    set32(lane, BIT(10));
    udelay(1000); /* t-refclk-to-perst */

    set32(port + PORT_PERST, BIT(0));
    mask32(GPIO_N1_PERST, 0xf, 0x3);
    if (j813_wait(port + PORT_PIPE_STS, BIT(0), BIT(0), 100000, "lane pipe reset") ||
        j813_wait(port + PORT_STATUS, BIT(0), BIT(0), 250000, "port RUN") ||
        j813_wait(port + PORT_LINKSTS, BIT(2), 0, 500000, "port idle"))
        return -1;

    /* _initializeRootComplex: root port DBI tunables, then the LTSSM. */
    set32(ecam + DBI_RO_WR, BIT(0));
    if (j813_tunables(APCIE_PORT, "pcie-rc-tunables", ecam) ||
        j813_tunables(APCIE_PORT, "pcie-rc-gen3-shadow-tunables", ecam) ||
        j813_tunables(APCIE_PORT, "pcie-rc-gen4-shadow-tunables", ecam))
        return -1;
    clear32(ecam + DBI_RO_WR, BIT(0));
    write32(port + PORT_LTSSMCTL, 1);
    if (j813_wait(port + PORT_LINKSTS, BIT(0), BIT(0), 1000000, "link up"))
        return -1;

    /* Root port bus numbers, then check the endpoint. */
    write32(ecam + 0x18, 0x00010100);
    mdelay(100);
    u32 ident = read32(ecam + 0x100000);
    printf("j813-n1: link up (0x%x), endpoint %04x:%04x\n", read32(port + PORT_LINKSTS),
           ident & 0xffff, ident >> 16);
    return (ident & 0xffff) == 0x106b ? 0 : -1;
}

int j813_n1_prepare(void *dt)
{
    if (chip_id != T8142)
        return 0;

    int node = fdt_node_offset_by_compatible(dt, -1, "pci-host-ecam-generic");
    if (node < 0)
        return 0;

    int len;
    const char *status = fdt_getprop(dt, node, "status", &len);
    if (status && strcmp(status, "okay") && strcmp(status, "ok"))
        return 0;

    if (!j813_n1_link())
        return 0;

    /* An untrained link would hang Linux's first config access. */
    printf("j813-n1: N1 PCIe bring-up failed; disabling the ECAM host\n");
    return fdt_setprop_string(dt, node, "status", "disabled") ? -1 : 0;
}
