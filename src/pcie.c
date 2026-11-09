/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "adt.h"
#include "pcie.h"
#include "pmgr.h"
#include "string.h"
#include "tunables.h"
#include "utils.h"

/*
 * The ADT uses 17 register sets:
 *
 * 0:  90000000 00000006 10000000 00000000  ECAM
 * 1:  80000000 00000006 00040000 00000000  RC
 * 2:  80080000 00000006 00090000 00000000  PHY
 * 3:  800c0000 00000006 00020000 00000000  PHY IP
 * 4:  8c000000 00000006 00004000 00000000  AXI
 * 5:  3d2bc000 00000000 00001000 00000000  fuses
 * 6:  81000000 00000006 00008000 00000000  port 0 config
 * 7:  81010000 00000006 00001000 00000000  port 0 LTSSM debug
 * 8:  80084000 00000006 00004000 00000000  port 0 PHY
 * 9:  800c8000 00000006 00016610 00000000  port 0 PHY IP
   <macOS 12.0 RC and later add a per-port Intr2AXI reg here>
 * 10: 82000000 00000006 00008000 00000000  port 1 config
 * 11: 82010000 00000006 00001000 00000000  port 1 LTSSM debug
 * 12: 80088000 00000006 00004000 00000000  port 1 PHY
 * 13: 800d0000 00000006 00006000 00000000  port 1 PHY IP
   <...>
 * 14: 83000000 00000006 00008000 00000000  port 2 config
 * 15: 83010000 00000006 00001000 00000000  port 2 LTSSM debug
 * 16: 8008c000 00000006 00004000 00000000  port 2 PHY
 * 17: 800d8000 00000006 00006000 00000000  port 2 PHY IP
   <...>
 */

/* PHY registers */

#define APCIE_PHY_CTRL             0x000
#define APCIE_PHY_CTRL_CLK0REQ     BIT(0)
#define APCIE_PHY_CTRL_CLK1REQ     BIT(1)
#define APCIE_PHY_CTRL_CLK0ACK     BIT(2)
#define APCIE_PHY_CTRL_CLK1ACK     BIT(3)
#define APCIE_PHY_CTRL_RESET_T8103 BIT(7)
#define APCIE_PHY_CTRL_RESET_T8132 BIT(4)

#define APCIE_PHYIF_CTRL     0x024
#define APCIE_PHYIF_CTRL_RUN BIT(0)

/* PHY common registers */
#define APCIE_PHYCMN_CLK         0x000
#define APCIE_PHYCMN_CLK_MODE    GENMASK(1, 0) /* Guesswork */
#define APCIE_PHYCMN_CLK_MODE_ON 1
#define APCIE_PHYCMN_CLK_100MHZ  BIT(31)

/* Port registers */

#define APCIE_PORT_LINKSTS      0x208
#define APCIE_PORT_LINKSTS_UP   BIT(0)
#define APCIE_PORT_LINKSTS_BUSY BIT(2)
#define APCIE_PORT_LINKSTS_L2   BIT(6)

#define APCIE_PORT_APPCLK    0x800
#define APCIE_PORT_APPCLK_EN BIT(0)

#define APCIE_PORT_STATUS     0x804
#define APCIE_PORT_STATUS_RUN BIT(0)

#define APCIE_PORT_RESET     0x814
#define APCIE_PORT_RESET_DIS BIT(0)

#define APCIE_T602X_PORT_RESET  0x82c
#define APCIE_T602X_PORT_MSIMAP 0x3800

/* PCIe capability registers */
#define PCIE_CAP_BASE    0x70
#define PCIE_LNKCAP      0x0c
#define PCIE_LNKCAP_SLS  GENMASK(3, 0)
#define PCIE_LNKCAP_MLW  GENMASK(9, 4)
#define PCIE_LNKCAP2     0x2c
#define PCIE_LNKCAP2_SLS GENMASK(6, 1)
#define PCIE_LNKCTL2     0x30
#define PCIE_LNKCTL2_TLS GENMASK(3, 0)

/* DesignWare PCIe Core registers */

#define DWC_DBI_RO_WR    0x8bc
#define DWC_DBI_RO_WR_EN BIT(0)

#define DWC_DBI_PORT_LINK_CONTROL        0x710
#define DWC_DBI_PORT_LINK_DLL_LINK_EN    BIT(5)
#define DWC_DBI_PORT_LINK_FAST_LINK_MODE BIT(7)
#define DWC_DBI_PORT_LINK_MODE           GENMASK(21, 16)
#define DWC_DBI_PORT_LINK_MODE_1_LANE    0x1
#define DWC_DBI_PORT_LINK_MODE_2_LANES   0x3
#define DWC_DBI_PORT_LINK_MODE_4_LANES   0x7
#define DWC_DBI_PORT_LINK_MODE_8_LANES   0xf
#define DWC_DBI_PORT_LINK_MODE_16_LANES  0x1f

#define DWC_DBI_LINK_WIDTH_SPEED_CONTROL 0x80c
#define DWC_DBI_LINK_WIDTH               GENMASK(12, 8)
#define DWC_DBI_SPEED_CHANGE             BIT(17)

#define PHY_STRIDE   0x4000
#define PHYIP_STRIDE 0x40000

struct fuse_bits {
    u16 src_reg;
    u16 tgt_reg;
    u8 src_bit;
    u8 tgt_bit;
    u8 width;
};

const struct fuse_bits pcie_fuse_bits_t8103[] = {
    {0x0084, 0x6238, 4, 0, 6},   {0x0084, 0x6220, 10, 14, 3}, {0x0084, 0x62a4, 13, 17, 2},
    {0x0418, 0x522c, 27, 9, 2},  {0x0418, 0x522c, 13, 12, 3}, {0x0418, 0x5220, 18, 14, 3},
    {0x0418, 0x52a4, 21, 17, 2}, {0x0418, 0x522c, 23, 16, 5}, {0x0418, 0x5278, 23, 20, 3},
    {0x0418, 0x5018, 31, 2, 1},  {0x041c, 0x1204, 0, 2, 5},   {},
};

const struct fuse_bits pcie_fuse_bits_t6000[] = {
    {0x004c, 0x1004, 3, 2, 5},   {0x0048, 0x522c, 26, 16, 5}, {0x0048, 0x522c, 29, 9, 2},
    {0x0048, 0x522c, 26, 12, 3}, {0x0048, 0x522c, 26, 16, 5}, {0x0048, 0x52a4, 24, 17, 2},
    {0x004c, 0x5018, 2, 3, 1},   {0x0048, 0x50a4, 14, 17, 2}, {0x0048, 0x62a4, 14, 17, 2},
    {0x0048, 0x6220, 8, 14, 3},  {0x0048, 0x6238, 2, 0, 6},   {},
};

/* clang-format off */
const struct fuse_bits pcie_fuse_bits_t8112[] = {
    {0x0490, 0x6238, 0, 0, 6},   {0x0490, 0x6220, 6, 14, 3},  {0x0490, 0x62a4, 12, 17, 2},
    {0x0490, 0x5018, 14, 2, 1},  {0x0490, 0x5220, 15, 14, 3}, {0x0490, 0x52a4, 18, 17, 2},
    {0x0490, 0x5278, 20, 20, 3}, {0x0490, 0x522c, 23, 12, 3}, {0x0490, 0x522c, 26, 9, 2},
    {0x0490, 0x522c, 28, 16, 4}, {0x0494, 0x522c, 0, 20, 1},  {0x0494, 0x1204, 5, 2, 5},
    {},
};
/* clang-format on */

enum apcie_type {
    APCIE_T81XX = 0,
    APCIE_T602X = 1,
    APCIE_T8122 = 2,
    APCIE_T6031 = 3,
    APCIE_T8132 = 4,
    APCIE_T8140 = 5,
};

struct reg_info {
    enum apcie_type type;
    enum apcie_type compat;
    int shared_reg_count;
    int config_idx;
    int rc_idx;
    int phy_common_idx;
    int phy_idx;
    u32 phy_ctrl_reset;
    int phy_ip_idx;
    int axi_idx;
    int fuse_idx;
    bool alt_phy_start;
};

static const struct reg_info regs_t8xxx_t600x = {
    .type = APCIE_T81XX,
    .compat = APCIE_T81XX,
    .shared_reg_count = 6,
    .config_idx = 0,
    .rc_idx = 1,
    .phy_common_idx = -1,
    .phy_idx = 2,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8103,
    .phy_ip_idx = 3,
    .axi_idx = 4,
    .fuse_idx = 5,
};

static const struct reg_info regs_t602x = {
    .type = APCIE_T602X,
    .compat = APCIE_T602X,
    .shared_reg_count = 8,
    .config_idx = 0,
    .rc_idx = 1,
    // 2 = phy unknown?
    .phy_common_idx = 3,
    .phy_idx = 4,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8103,
    .phy_ip_idx = 5,
    .axi_idx = 6,
    .fuse_idx = 7,
};

static const struct reg_info regs_t8122 = {
    .type = APCIE_T8122,
    .compat = APCIE_T8122,
    .shared_reg_count = 7,
    .config_idx = 0,
    .rc_idx = 1,
    .phy_common_idx = 2,
    .phy_idx = 2,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8103,
    .phy_ip_idx = 3,
    .axi_idx = 4,
    .fuse_idx = 5,
};

static const struct reg_info regs_t8132 = {
    .type = APCIE_T8132,
    .compat = APCIE_T8122,
    .shared_reg_count = 7,
    .config_idx = 0,
    .rc_idx = 1,
    .phy_common_idx = 2,
    .phy_idx = 2,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8132,
    .phy_ip_idx = 3,
    .axi_idx = 4,
    .fuse_idx = 5,
};

static const struct reg_info regs_t6031 = {
    .type = APCIE_T6031,
    .compat = APCIE_T8122,
    .shared_reg_count = 8,
    .config_idx = 0,
    .rc_idx = 1,
    .phy_common_idx = 2,
    .phy_idx = 2,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8103,
    .phy_ip_idx = 3,
    .axi_idx = 4,
};

static const struct reg_info regs_t8140 = {
    .type = APCIE_T8140,
    .compat = APCIE_T8140,
    .shared_reg_count = 7,
    .config_idx = 0,
    .rc_idx = 1,
    .phy_common_idx = 2,
    .phy_idx = 2,
    .phy_ctrl_reset = APCIE_PHY_CTRL_RESET_T8132,
    .phy_ip_idx = 3,
    .axi_idx = 4,
};

static bool pcie_initialized = false;

enum PCIE_CONTROLLERS {
    APCIE,
    APCIE_GE0,
    APCIE_GE1,
    NUM_CONTROLLERS,
};

#define MAX_PHYS 4

struct state {
    int num_phys;
    u64 rc_base;
    u64 axi_base;
    u64 phy_common_base;
    u64 phy_base[MAX_PHYS];
    u64 phy_ip_base[MAX_PHYS];
    u64 fuse_base;
    u32 port_count;
    u64 port_base[8];
    u64 port_ltssm_base[8];
    u64 port_phy_base[8];
    u64 port_intr2axi_base[8];
    const struct reg_info *pcie_regs;
    bool initialized;
    bool dirty;
    u32 active_ports;
};

static struct state controllers[NUM_CONTROLLERS];

/* Check every T8140 ADT window and tunable before PMGR or MMIO changes. */
static int pcie_t8140_preflight(const char *path, int *adt_path, u32 ports, u32 reg_len)
{
    static const char *const shared_props[] = {
        "apcie-axi2af-tunables",        "apcie-phy-tunables",         "apcie-phy-ip-pll-tunables",
        "apcie-phy-ip-auspma-tunables", "apcie-cio3pllcore-tunables", "apcie-pcieclkgen-tunables",
    };
    static const int shared_indices[] = {4, 2, 3, 3, 5, 6};
    u64 base, span;
    u32 present = 0;

    /* The observed ADT has seven shared windows followed by exactly
     * six windows for each port. Reject truncated or extended descriptions. */
    if (!ports || ports > 8 || reg_len != 16 * (7 + 6 * ports))
        return -1;

    for (int i = 0; i < 7; i++) {
        if (adt_get_reg(adt, adt_path, "reg", i, &base, &span) || !base || !span ||
            base > UINT64_MAX - span) {
            printf("pcie: invalid T8140 shared reg[%d]\n", i);
            return -1;
        }
        if (i == 0) {
            if (span < (u64)ports * (1 << 15))
                return -1;
        }
        if (i == 1 && span < 0x5c)
            return -1;
        if (i == 2 && span < 0xc000)
            return -1;
    }

    for (int i = 0; i < (int)ARRAY_SIZE(shared_props); i++) {
        if (adt_get_reg(adt, adt_path, "reg", shared_indices[i], &base, &span))
            return -1;
        int ret = tunables_validate_local(path, shared_props[i], span);
        if (ret) {
            printf("pcie: missing or invalid T8140 %s\n", shared_props[i]);
            return -1;
        }
    }

    int node = adt_path_offset(adt, path);
    if (adt_get_reg(adt, adt_path, "reg", 1, &base, &span))
        return -1;
    if (adt_getprop(adt, node, "apcie-common-tunables", NULL) &&
        tunables_validate_local(path, "apcie-common-tunables", span))
        return -1;

    for (u32 port = 0; port < ports; port++) {
        char bridge[64];
        snprintf(bridge, sizeof(bridge), "%s/pci-bridge%d", path, port);
        int bridge_node = adt_path_offset(adt, bridge);
        u64 port_config_span = 0;
        for (int reg = 0; reg < 6; reg++) {
            int idx = 7 + port * 6 + reg;
            if (adt_get_reg(adt, adt_path, "reg", idx, &base, &span) || !base || !span ||
                base > UINT64_MAX - span || (reg == 0 && span < 0x4000) ||
                (reg == 2 && span < 0x400)) {
                printf("pcie: invalid T8140 port %d reg[%d]\n", port, idx);
                return -1;
            }
            if (reg == 0)
                port_config_span = span;
        }
        if (bridge_node < 0)
            continue;
        if (!adt_is_compatible(adt, bridge_node, "apcie-bridge"))
            return -1;
        u32 max_speed;
        if (ADT_GETPROP(adt, bridge_node, "maximum-link-speed", &max_speed) < 0 || !max_speed ||
            max_speed > 4)
            return -1;
        if (tunables_validate_local(bridge, "apcie-config-tunables", port_config_span) ||
            tunables_validate_local(bridge, "pcie-rc-tunables", 1 << 15) ||
            tunables_validate_local(bridge, "pcie-rc-gen3-shadow-tunables", 1 << 15) ||
            tunables_validate_local(bridge, "pcie-rc-gen4-shadow-tunables", 1 << 15))
            return -1;
        present++;
        printf("pcie: T8140 ADT bridge %d validated\n", port);
    }
    if (!present)
        return -1;
    printf("pcie: T8140 ADT preflight passed (%d ports, %d bridges)\n", ports, present);
    return 0;
}

/* What PHY control gets once the PHY is ready. */
static u32 pcie_phy_up(const struct state *state)
{
    if (state->pcie_regs->type == APCIE_T602X)
        return 0x300;
    if (state->pcie_regs->compat == APCIE_T8122)
        return 0x200;
    return BIT(27);
}

/* With ans_off, only read the windows, stop the shared PHY and power ANS's gates off. */
static int pcie_init_controller(int controller, const char *path, bool ans_off)
{
    struct state *state = &controllers[controller];
    int adt_path[8];
    int adt_offset;
    u32 lane_mode = DWC_DBI_PORT_LINK_MODE_1_LANE;
    u32 link_width = 1;
    const struct fuse_bits *fuse_bits;

    if (state->dirty) {
        printf("pcie: refusing retry of partially initialized controller %d\n", controller);
        return -1;
    }
    state->initialized = false;
    state->active_ports = 0;
    state->num_phys = 1;

    adt_offset = adt_path_offset_trace(adt, path, adt_path);
    if (adt_offset < 0) {
        printf("pcie: Error getting node %s\n", path);
        return -1;
    }

    if (adt_is_compatible(adt, adt_offset, "apcie,t8103")) {
        fuse_bits = pcie_fuse_bits_t8103;
        state->pcie_regs = &regs_t8xxx_t600x;
        printf("pcie: Initializing t8103 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t6000")) {
        fuse_bits = pcie_fuse_bits_t6000;
        state->pcie_regs = &regs_t8xxx_t600x;
        printf("pcie: Initializing t6000 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t8112")) {
        fuse_bits = pcie_fuse_bits_t8112;
        state->pcie_regs = &regs_t8xxx_t600x;
        printf("pcie: Initializing t8112 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t8122")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t8122;
        printf("pcie: Initializing t8122 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t6030")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t8122;
        printf("pcie: Initializing t6030 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t6020")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t602x;
        printf("pcie: Initializing t6020 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t6031")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t6031;
        printf("pcie: Initializing t6031 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t8132")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t8132;
        printf("pcie: Initializing t8132 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t8140")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t8140;
        printf("pcie: Initializing t8140 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie,t6040")) {
        fuse_bits = NULL;
        state->pcie_regs = &regs_t8132;
        printf("pcie: Initializing t6040 PCIe controller\n");
    } else if (adt_is_compatible(adt, adt_offset, "apcie-ge,t6020")) {
        u32 lane_cfg;
        fuse_bits = NULL;
        state->pcie_regs = &regs_t602x;

        printf("pcie: Initializing t6020 PCIe GE controller\n");
        if (ADT_GETPROP(adt, adt_offset, "lane-cfg", &lane_cfg) < 0) {
            printf("pcie: Error getting lane_cfg for %s\n", path);
            return -1;
        }
        switch (lane_cfg) {
            case 0:
                state->num_phys = 4;
                lane_mode = DWC_DBI_PORT_LINK_MODE_16_LANES;
                link_width = 16;
                break;
            case 1:
                state->num_phys = 2;
                lane_mode = DWC_DBI_PORT_LINK_MODE_8_LANES;
                link_width = 8;
                break;
            default:
                printf("pcie: Unknown lane config %d for %s\n", lane_cfg, path);
                return -1;
        }
    } else {
        printf("pcie: Unsupported compatible\n");
        return -1;
    }

    if (ADT_GETPROP(adt, adt_offset, "#ports", &state->port_count) < 0) {
        printf("pcie: Error getting port count for %s\n", path);
        return -1;
    }

    u64 config_base;
    if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->config_idx, &config_base, NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->config_idx,
               path);
        return -1;
    }

    if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->rc_idx, &state->rc_base, NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->rc_idx, path);
        return -1;
    }

    if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->axi_idx, &state->axi_base, NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->axi_idx, path);
        return -1;
    }

    if (state->pcie_regs->phy_common_idx != -1) {
        if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->phy_common_idx,
                        &state->phy_common_base, NULL)) {
            printf("pcie: Error getting reg with index %d for %s\n",
                   state->pcie_regs->phy_common_idx, path);
            return -1;
        }
    } else {
        state->phy_common_base = 0;
    }

    if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->phy_idx, &state->phy_base[0], NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->phy_idx, path);
        return -1;
    }

    if (adt_get_reg(adt, adt_path, "reg", state->pcie_regs->phy_ip_idx, &state->phy_ip_base[0],
                    NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->phy_ip_idx,
               path);
        return -1;
    }

    if (state->pcie_regs->compat == APCIE_T8122 || state->pcie_regs->type == APCIE_T8140) {
        // The T8122 init seems very similar to the T602X, with different offsets,
        // and with reg[2], [3] and [4] coalesced to reg[2] in the ADT.
        state->phy_base[0] = state->phy_base[0] + 0x8000;
        state->phy_common_base += 0x4000;
    }

    for (int phy = 1; phy < state->num_phys; phy++) {
        state->phy_base[phy] = state->phy_base[0] + PHY_STRIDE * phy;
        state->phy_ip_base[phy] = state->phy_ip_base[0] + PHYIP_STRIDE * phy;
    }

    if (state->pcie_regs->type != APCIE_T8140 &&
        adt_get_reg(adt, adt_path, "reg", state->pcie_regs->fuse_idx, &state->fuse_base, NULL)) {
        printf("pcie: Error getting reg with index %d for %s\n", state->pcie_regs->fuse_idx, path);
        return -1;
    }

    u32 reg_len;
    if (!adt_getprop(adt, adt_offset, "reg", &reg_len)) {
        printf("pcie: Error getting reg length for %s\n", path);
        return -1;
    }

    int port_reg_cnt = -1;
    if (state->pcie_regs->type == APCIE_T8140) {
        port_reg_cnt = 6;
    } else if (state->port_count && state->port_count <= 8 && reg_len % 16 == 0) {
        u32 entries = reg_len / 16;
        u32 shared = state->pcie_regs->shared_reg_count;
        if (entries >= shared + 3 * state->port_count &&
            (entries - shared) % state->port_count == 0)
            port_reg_cnt = (entries - shared) / state->port_count;
    }
    if (port_reg_cnt < 0) {
        printf("pcie: invalid ADT port geometry (%d bytes, %d ports)\n", reg_len,
               state->port_count);
        return -1;
    }
    printf("pcie: ADT uses %d reg entries per port\n", port_reg_cnt);

    if (state->pcie_regs->type == APCIE_T8140 &&
        pcie_t8140_preflight(path, adt_path, state->port_count, reg_len)) {
        printf("pcie: T8140 ADT preflight failed; leaving PCIe disabled\n");
        return -1;
    }

    if (ans_off) {
        for (int phy = 0; phy < state->num_phys; phy++) {
            u64 ctrl = state->phy_base[phy] + APCIE_PHY_CTRL;
            set32(ctrl, state->pcie_regs->phy_ctrl_reset);
            clear32(ctrl, APCIE_PHY_CTRL_CLK1REQ);
            poll32(ctrl, APCIE_PHY_CTRL_CLK1ACK, 0, 50000);
            clear32(ctrl, APCIE_PHY_CTRL_CLK0REQ);
            poll32(ctrl, APCIE_PHY_CTRL_CLK0ACK, 0, 50000);
            /* PHY control keeps its value through the power cycle: take the PHY down whole. */
            clear32(ctrl, pcie_phy_up(state));
            clear32(state->phy_base[phy] + 4, 0x10);
            clear32(state->phy_base[phy] + 4, 0x01);
        }
        if (pmgr_adt_power_disable_under(path, "ANS"))
            return -1;
        /* Off at least as long as in the power cycle measured on the J700. */
        udelay(100000);
        return 0;
    }

    state->dirty = true;
    if (state->pcie_regs->type == APCIE_T8140)
        printf("pcie: T8140 enabling controller power\n");
    if (pmgr_adt_power_enable(path)) {
        printf("pcie: Error enabling power for %s\n", path);
        return -1;
    }
    printf("pcie: controller %d power enabled\n", controller);

    if (state->pcie_regs->type == APCIE_T8140)
        printf("pcie: T8140 AXI reg[4] tunables\n");
    if (!adt_getprop(adt, adt_offset, "apcie-axi2af-tunables", NULL)) {
        printf("pcie: No axi2af tunables\n");
    } else if (tunables_apply_local(path, "apcie-axi2af-tunables", state->pcie_regs->axi_idx)) {
        printf("pcie: Error applying %s for %s\n", "apcie-axi2af-tunables", path);
        return -1;
    }

    if (state->pcie_regs->type == APCIE_T8140) {
        printf("pcie: T8140 CIO3 PLL core reg[5] tunables\n");
        if (tunables_apply_local(path, "apcie-cio3pllcore-tunables", 5)) {
            printf("pcie: Error applying T8140 CIO3 PLL core tunables\n");
            return -1;
        }
        printf("pcie: T8140 PCIe clock generator reg[6] tunables\n");
        if (tunables_apply_local(path, "apcie-pcieclkgen-tunables", 6)) {
            printf("pcie: Error applying T8140 PCIe clock generator tunables\n");
            return -1;
        }
        printf("pcie: T8140 clock tunables applied\n");
    }

    /* ??? */
    if (state->pcie_regs->type == APCIE_T8140)
        printf("pcie: T8140 RC reg[1] setup\n");
    if (controller == APCIE)
        write32(state->rc_base + 0x4, 0);

    if (!adt_getprop(adt, adt_offset, "apcie-common-tunables", NULL)) {
        printf("pcie: No common tunables\n");
    } else if (tunables_apply_local(path, "apcie-common-tunables", state->pcie_regs->rc_idx)) {
        printf("pcie: Error applying %s for %s\n", "apcie-common-tunables", path);
        return -1;
    }

    /*
     * Initialize PHY.
     */

    if (state->pcie_regs->type == APCIE_T8140)
        printf("pcie: T8140 PHY reg[2] tunables (including port PHYs)\n");
    if (!adt_getprop(adt, adt_offset, "apcie-phy-tunables", NULL)) {
        printf("pcie: No PHY tunables\n");
    } else if (tunables_apply_local(path, "apcie-phy-tunables", state->pcie_regs->phy_idx)) {
        printf("pcie: Error applying %s for %s\n", "apcie-phy-tunables", path);
        return -1;
    }

    if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->type == APCIE_T6031) {
        if (poll32(state->phy_common_base + APCIE_PHYCMN_CLK, APCIE_PHYCMN_CLK_100MHZ,
                   APCIE_PHYCMN_CLK_100MHZ, 250000)) {
            printf("pcie: Reference clock not available\n");
            return -1;
        }
    }

    /* T8140's ANS runs on this PHY, so it is left as it is unless ANS was powered off. */
    bool cold = state->pcie_regs->type == APCIE_T8140 && !(read32(state->phy_base[0] + 4) & 0x10);
    for (int phy = 0; phy < state->num_phys; phy++) {
        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 PHY %d requesting CLK0\n", phy);
        set32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK0REQ);
        if (poll32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK0ACK,
                   APCIE_PHY_CTRL_CLK0ACK, 50000)) {
            printf("pcie: Timeout enabling PHY CLK0\n");
            return -1;
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 PHY %d CLK0 acknowledged; requesting CLK1\n", phy);
        set32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK1REQ);
        if (poll32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK1ACK,
                   APCIE_PHY_CTRL_CLK1ACK, 50000)) {
            printf("pcie: Timeout enabling PHY CLK1\n");
            return -1;
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 PHY %d CLK1 acknowledged; releasing reset\n", phy);
        if (state->pcie_regs->phy_ctrl_reset)
            clear32(state->phy_base[phy] + APCIE_PHY_CTRL, state->pcie_regs->phy_ctrl_reset);
        udelay(1);

        /* Confirm reset release before continuing with PHY control. */
        if (state->pcie_regs->type == APCIE_T8140 &&
            poll32(state->phy_base[phy] + APCIE_PHY_CTRL, state->pcie_regs->phy_ctrl_reset, 0,
                   50000)) {
            printf("pcie: Timeout releasing T8140 PHY reset\n");
            return -1;
        }

        /* ??? */
        if (state->pcie_regs->type == APCIE_T81XX) {
            set32(state->rc_base + APCIE_PHYIF_CTRL, APCIE_PHYIF_CTRL_RUN);
            udelay(1);
        } else if (state->pcie_regs->type == APCIE_T602X ||
                   state->pcie_regs->compat == APCIE_T8122 ||
                   state->pcie_regs->type == APCIE_T8140) {
            set32(state->phy_base[phy] + 4, 0x01);
        }

        /* Apply "fuses". */
        for (int i = 0; fuse_bits && fuse_bits[i].width; i++) {
            u32 fuse;
            fuse = (read32(state->fuse_base + fuse_bits[i].src_reg) >> fuse_bits[i].src_bit);
            fuse &= (1 << fuse_bits[i].width) - 1;
            mask32(state->phy_ip_base[phy] + fuse_bits[i].tgt_reg,
                   ((1 << fuse_bits[i].width) - 1) << fuse_bits[i].tgt_bit,
                   fuse << fuse_bits[i].tgt_bit);
        }

        if (state->pcie_regs->type == APCIE_T8140 && !cold) {
            printf("pcie: T8140 PHY %d up; skipping PHY IP PLL and AUSPMA tunables\n", phy);
        } else {
            char pll_prop[64];
            char auspma_prop[64];

            if (state->num_phys == 1) {
                strcpy(pll_prop, "apcie-phy-ip-pll-tunables");
                strcpy(auspma_prop, "apcie-phy-ip-auspma-tunables");
            } else {
                snprintf(pll_prop, sizeof(pll_prop), "apcie-phy-%d-ip-pll-tunables", phy);
                snprintf(auspma_prop, sizeof(auspma_prop), "apcie-phy-%d-ip-auspma-tunables", phy);
            }

            if (tunables_apply_local_addr(path, pll_prop, state->phy_ip_base[phy])) {
                printf("pcie: Error applying %s for %s\n", pll_prop, path);
                return -1;
            }
            if (tunables_apply_local_addr(path, auspma_prop, state->phy_ip_base[phy])) {
                printf("pcie: Error applying %s for %s\n", auspma_prop, path);
                return -1;
            }
        }

        if (cold)
            clear32(state->phy_ip_base[phy] + 0x90, BIT(16));
        if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 ||
            cold) {
            set32(state->phy_base[phy] + 4, 0x10);
        }
    }

    if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 ||
        state->pcie_regs->type == APCIE_T8140) {
        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 PHY common reg[2]+0x4000 clock mode\n");
        mask32(state->phy_common_base + APCIE_PHYCMN_CLK, APCIE_PHYCMN_CLK_MODE,
               FIELD_PREP(APCIE_PHYCMN_CLK_MODE, 1));
    }

    if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 || cold) {
        // Why always PHY 1 in this case?
        u32 off = state->num_phys > 1 ? PHY_STRIDE : 0;
        if (poll32(state->phy_base[0] + off + 0x8, 1, 1, 250000)) {
            printf("pcie: PHY clock enable timed out\n");
            return -1;
        }
        for (int phy = 0; phy < state->num_phys; phy++)
            set32(state->phy_base[phy] + APCIE_PHY_CTRL, pcie_phy_up(state));
    }

    if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 ||
        state->pcie_regs->type == APCIE_T8140) {
        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 RC readiness\n");
        write32(state->rc_base + 0x54, 0x140);
        write32(state->rc_base + 0x50, 0x1);
        if (poll32(state->rc_base + 0x58, 1, 1, 250000)) {
            printf("pcie: Failed to initialize RC thing\n");
            return -1;
        }
        if (state->pcie_regs->type == APCIE_T602X) {
            if (controller == APCIE)
                clear32(state->rc_base + 0x3c, 0x1);
            pmgr_adt_power_disable_index(path, 1);
        }
    }

    for (u32 port = 0; port < state->port_count; port++) {
        char bridge[64];
        int bridge_offset;

        /*
         * Initialize RC port.
         */

        switch (controller) {
            case APCIE:
                snprintf(bridge, sizeof(bridge), "%s/pci-bridge%d", path, port);
                break;
            case APCIE_GE0:
                strcpy(bridge, "/arm-io/apcie-ge0/pci-ge0-bridge");
                break;
            case APCIE_GE1:
                strcpy(bridge, "/arm-io/apcie-ge1/pci-ge1-bridge");
                break;
        }

        if ((bridge_offset = adt_path_offset(adt, bridge)) < 0)
            continue;

        printf("pcie: Initializing port %d\n", port);

        if (adt_get_reg(adt, adt_path, "reg",
                        port * port_reg_cnt + state->pcie_regs->shared_reg_count,
                        &state->port_base[port], NULL)) {
            printf("pcie: Error getting reg with index %d for %s\n",
                   port * port_reg_cnt + state->pcie_regs->shared_reg_count, path);
            return -1;
        }

        if (adt_get_reg(adt, adt_path, "reg",
                        port * port_reg_cnt + state->pcie_regs->shared_reg_count + 1,
                        &state->port_ltssm_base[port], NULL)) {
            printf("pcie: Error getting reg with index %d for %s\n",
                   port * port_reg_cnt + state->pcie_regs->shared_reg_count + 1, path);
            return -1;
        }

        if (adt_get_reg(adt, adt_path, "reg",
                        port * port_reg_cnt + state->pcie_regs->shared_reg_count + 2,
                        &state->port_phy_base[port], NULL)) {
            printf("pcie: Error getting reg with index %d for %s\n",
                   port * port_reg_cnt + state->pcie_regs->shared_reg_count + 2, path);
            return -1;
        }

        if (port_reg_cnt >= 5) {
            if (adt_get_reg(adt, adt_path, "reg",
                            port * port_reg_cnt + state->pcie_regs->shared_reg_count + 4,
                            &state->port_intr2axi_base[port], NULL)) {
                printf("pcie: Error getting reg with index %d for %s\n",
                       port * port_reg_cnt + state->pcie_regs->shared_reg_count + 4, path);
                return -1;
            }
        } else {
            state->port_intr2axi_base[port] = 0;
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d config reg[%d] setup\n", port,
                   port * port_reg_cnt + state->pcie_regs->shared_reg_count);
        if (state->pcie_regs->type == APCIE_T602X) {
            set32(state->rc_base + 0x3c, 0x1);

            // ??????
            if (controller == APCIE)
                write32(state->port_base[port] + 0x10, 0x2);
        }

        if (state->pcie_regs->type == APCIE_T6031) {
            clear32(state->axi_base + 0x600, BIT(16));
        }

        if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 ||
            state->pcie_regs->type == APCIE_T8140) {
            write32(state->port_base[port] + 0x88, 0x110);
            write32(state->port_base[port] + 0x100, 0xffffffff);
            write32(state->port_base[port] + 0x148, 0xffffffff);
            write32(state->port_base[port] + 0x210, 0xffffffff);
            write32(state->port_base[port] + 0x80, 0x0);
            write32(state->port_base[port] + 0x84, 0x0);
            if (state->pcie_regs->type == APCIE_T602X) {
                write32(state->port_base[port] + 0x104, 0x7fffffff);
            } else if (state->pcie_regs->compat == APCIE_T8122 ||
                       state->pcie_regs->type == APCIE_T8140) {
                write32(state->port_base[port] + 0x104, 0xfffffff0);
            }
            write32(state->port_base[port] + 0x124, 0x100);
            write32(state->port_base[port] + 0x16c, 0x0);
            write32(state->port_base[port] + 0x13c, 0x10);
            write32(state->port_base[port] + 0x800, 0x100100);
            write32(state->port_base[port] + 0x808, 0x1000ff);
            write32(state->port_base[port] + 0x82c, 0x0);
            if (state->pcie_regs->compat == APCIE_T8122 || state->pcie_regs->type == APCIE_T8140) {
                for (int i = 0; i < 16; i++) {
                    write32(state->port_base[port] + 0x3000 + 4 * i, 0);
                }
            }
            // It seems fine to write the full range (512) here.
            for (int i = 0; i < 512; i++) {
                write32(state->port_base[port] + APCIE_T602X_PORT_MSIMAP + 4 * i, 0);
            }
            if (state->pcie_regs->type == APCIE_T602X) {
                write32(state->port_base[port] + 0x397c, 0x0);
            }
            if (controller == APCIE)
                write32(state->port_base[port] + 0x130, 0x3000000);
            else
                write32(state->port_base[port] + 0x130, 0x3000008);
            write32(state->port_base[port] + 0x140, 0x10);
            write32(state->port_base[port] + 0x144, 0x253770);
            write32(state->port_base[port] + 0x21c, 0x0);
            write32(state->port_base[port] + 0x834, 0x0);
            if (controller != APCIE || state->pcie_regs->compat == APCIE_T8122 ||
                state->pcie_regs->type == APCIE_T8140)
                write32(state->port_base[port] + 0x83c, 0x0);
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d config tunables\n", port);
        if (tunables_apply_local_addr(bridge, "apcie-config-tunables", state->port_base[port])) {
            printf("pcie: Error applying %s for %s\n", "apcie-config-tunables", bridge);
            return -1;
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d enabling APPCLK\n", port);
        set32(state->port_base[port] + APCIE_PORT_APPCLK, APCIE_PORT_APPCLK_EN);

        if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->compat == APCIE_T8122 ||
            state->pcie_regs->type == APCIE_T8140) {
            if (state->pcie_regs->type == APCIE_T8140)
                printf("pcie: T8140 port %d PHY reg[%d] clock/reset sequence\n", port,
                       port * port_reg_cnt + state->pcie_regs->shared_reg_count + 2);
            clear32(state->port_phy_base[port] + APCIE_PHY_CTRL,
                    APCIE_PHY_CTRL_CLK0REQ | APCIE_PHY_CTRL_CLK1REQ);

            set32(state->port_phy_base[port] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK0REQ);
            if (poll32(state->port_phy_base[port] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK0ACK,
                       APCIE_PHY_CTRL_CLK0ACK, 50000)) {
                printf("pcie: Timeout enabling PHY CLK0\n");
                return -1;
            }

            set32(state->port_phy_base[port] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK1REQ);
            if (poll32(state->port_phy_base[port] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK1ACK,
                       APCIE_PHY_CTRL_CLK1ACK, 50000)) {
                printf("pcie: Timeout enabling PHY CLK1\n");
                return -1;
            }

            if (state->pcie_regs->type == APCIE_T602X) {
                clear32(state->port_phy_base[port] + APCIE_PHY_CTRL, 0x4000);
            } else if (state->pcie_regs->compat == APCIE_T8122 ||
                       state->pcie_regs->type == APCIE_T8140) {
                clear32(state->port_phy_base[port] + APCIE_PHY_CTRL, 0x10);
            }
            set32(state->port_phy_base[port] + APCIE_PHY_CTRL, 0x200);
            set32(state->port_phy_base[port] + APCIE_PHY_CTRL, 0x400);

            set32(state->port_base[port] + APCIE_T602X_PORT_RESET, APCIE_PORT_RESET_DIS);
        } else {
            /* PERSTN */
            set32(state->port_base[port] + APCIE_PORT_RESET, APCIE_PORT_RESET_DIS);
        }

        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d waiting for RUN\n", port);
        if (poll32(state->port_base[port] + APCIE_PORT_STATUS, APCIE_PORT_STATUS_RUN,
                   APCIE_PORT_STATUS_RUN, 250000)) {
            printf("pcie: Port failed to come up on %s\n", bridge);
            return -1;
        }

        if (state->pcie_regs->type == APCIE_T602X && controller != APCIE) {
            write32(state->port_ltssm_base[port] + 0x10, 0x2);
            write32(state->port_ltssm_base[port] + 0x1c, 0x4);
            set32(state->port_ltssm_base[port] + 0x20, 0x2);
            write32(state->port_ltssm_base[port] + 0x14, 0x1);

            clear32(state->port_base[port] + APCIE_PORT_APPCLK, 0x100);
        }

        if (poll32(state->port_base[port] + APCIE_PORT_LINKSTS, APCIE_PORT_LINKSTS_BUSY, 0,
                   250000)) {
            printf("pcie: Port failed to become idle on %s\n", bridge);
            return -1;
        }

        /* Do it again? */
        if (state->pcie_regs->type == APCIE_T602X && controller == APCIE) {
            clear32(state->port_base[port] + APCIE_T602X_PORT_RESET, APCIE_PORT_RESET_DIS);
            set32(state->port_base[port] + APCIE_T602X_PORT_RESET, APCIE_PORT_RESET_DIS);

            if (poll32(state->port_base[port] + APCIE_PORT_LINKSTS, APCIE_PORT_LINKSTS_BUSY, 0,
                       250000)) {
                printf("pcie: Port failed to become idle (2) on %s\n", bridge);
                return -1;
            }

            udelay(1000);

            write32(state->port_ltssm_base[port] + 0x10, 0x2);
            write32(state->port_ltssm_base[port] + 0x1c, 0x4);
            set32(state->port_ltssm_base[port] + 0x20, 0x2);
            write32(state->port_ltssm_base[port] + 0x14, 0x1);
        }

        /* Make Designware PCIe Core registers writable. */
        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d ECAM reg[0] DBI and tunables\n", port);
        set32(config_base + DWC_DBI_RO_WR, DWC_DBI_RO_WR_EN);

        if (tunables_apply_local_addr(bridge, "pcie-rc-tunables", config_base)) {
            printf("pcie: Error applying %s for %s\n", "pcie-rc-tunables", bridge);
            return -1;
        }
        if (tunables_apply_local_addr(bridge, "pcie-rc-gen3-shadow-tunables", config_base)) {
            printf("pcie: Error applying %s for %s\n", "pcie-rc-gen3-shadow-tunables", bridge);
            return -1;
        }
        if (tunables_apply_local_addr(bridge, "pcie-rc-gen4-shadow-tunables", config_base)) {
            printf("pcie: Error applying %s for %s\n", "pcie-rc-gen4-shadow-tunables", bridge);
            return -1;
        }

        u32 max_speed;
        if (ADT_GETPROP(adt, bridge_offset, "maximum-link-speed", &max_speed) >= 0) {
            /* Some devices override "maximum-link-speed" in the device child nodes.
             * The property used for the link speed seems to be ad-hoc made up.
             * The 10 GB ethernet adapter uses "target-link-speed" and the SD card
             * reader uses "expected-link-speed". Assume that PCIe link speed override
             * resides in the first (only?) child node.
             */
            if (max_speed == 1) {
                int np = adt_first_child_offset(adt, bridge_offset);
                if (np >= 0) {
                    int target_speed;
                    if (ADT_GETPROP(adt, np, "target-link-speed", &target_speed) >= 0 &&
                        target_speed > 0) {
                        max_speed = target_speed;
                    } else if (ADT_GETPROP(adt, np, "expected-link-speed", &target_speed) >= 0 &&
                               target_speed > 0) {
                        max_speed = target_speed;
                    }
                }
            }

            printf("pcie: Port %d max speed = %d\n", port, max_speed);

            if (max_speed == 0) {
                printf("pcie: Invalid max-speed\n");
                return -1;
            }
            if (max_speed > 4)
                max_speed = 4;

            mask32(config_base + PCIE_CAP_BASE + PCIE_LNKCAP, PCIE_LNKCAP_SLS,
                   FIELD_PREP(PCIE_LNKCAP_SLS, max_speed));

            mask32(config_base + PCIE_CAP_BASE + PCIE_LNKCAP2, PCIE_LNKCAP2_SLS,
                   FIELD_PREP(PCIE_LNKCAP2_SLS, (1 << max_speed) - 1));

            mask16(config_base + PCIE_CAP_BASE + PCIE_LNKCTL2, PCIE_LNKCTL2_TLS,
                   FIELD_PREP(PCIE_LNKCTL2_TLS, max_speed));

            set32(config_base + DWC_DBI_LINK_WIDTH_SPEED_CONTROL, DWC_DBI_SPEED_CHANGE);
        }

        /* Max link width */
        mask32(config_base + DWC_DBI_PORT_LINK_CONTROL, DWC_DBI_PORT_LINK_MODE,
               FIELD_PREP(DWC_DBI_PORT_LINK_MODE, lane_mode));
        mask32(config_base + DWC_DBI_LINK_WIDTH_SPEED_CONTROL, DWC_DBI_LINK_WIDTH,
               FIELD_PREP(DWC_DBI_LINK_WIDTH, link_width));
        mask32(config_base + PCIE_CAP_BASE + PCIE_LNKCAP, PCIE_LNKCAP_MLW,
               FIELD_PREP(PCIE_LNKCAP_MLW, link_width));

        /* Make Designware PCIe Core registers readonly. */
        clear32(config_base + DWC_DBI_RO_WR, DWC_DBI_RO_WR_EN);

        if (state->pcie_regs->type == APCIE_T602X || state->pcie_regs->type == APCIE_T6031) {
            write32(state->port_base[port] + 0x4020, 0x3);
            if (state->port_intr2axi_base[port])
                write32(state->port_intr2axi_base[port] + 0x80, 0x1);

            clear32(state->rc_base + 0x3c, 0x1);
            for (int i = 0; i < 32; i++)
                write32(state->port_base[port] + APCIE_T602X_PORT_MSIMAP + 4 * i, 0x80000000 | i);
        }

        u32 link_status = read32(state->port_base[port] + APCIE_PORT_LINKSTS);
        if (state->pcie_regs->type == APCIE_T8140)
            printf("pcie: T8140 port %d ready, link status 0x%x\n", port, link_status);
        state->active_ports |= BIT(port);

        /* Move to the next PCIe device on this bus. */
        config_base += (1 << 15);
    }

    printf("pcie: Initialized controller %d\n", controller);
    state->initialized = true;
    state->dirty = false;

    return 0;
}

int pcie_init(void)
{
    bool success = false;

    if (pcie_initialized)
        return 0;

    if (chip_id == T8140) {
        struct state *state = &controllers[APCIE];
        if (state->dirty) {
            printf("pcie: T8140 controller is partially initialized\n");
            return -1;
        }
        const char *path = "/arm-io/apcie";
        if (adt_path_offset(adt, path) < 0)
            path = "/arm-io/apcie0";
        if (adt_path_offset(adt, path) < 0) {
            printf("pcie: T8140 controller absent\n");
            return 1;
        }
        int ret = pcie_init_controller(APCIE, path, false);
        if (!ret) {
            pcie_initialized = true;
            return 0;
        }
        if (state->dirty) {
            printf("pcie: T8140 controller failed after power request\n");
            return -1;
        }
        printf("pcie: T8140 controller skipped before MMIO\n");
        return 1;
    }

    if (chip_id == T6050) {
        int ret = pcie_t6050_init();
        pcie_initialized = !ret;
        return ret;
    }

    success |= pcie_init_controller(APCIE, "/arm-io/apcie", false) == 0;
    success |= pcie_init_controller(APCIE, "/arm-io/apcie0", false) == 0;
    success |= pcie_init_controller(APCIE_GE0, "/arm-io/apcie-ge0", false) == 0;
    success |= pcie_init_controller(APCIE_GE1, "/arm-io/apcie-ge1", false) == 0;

    if (success)
        pcie_initialized = true;

    return success ? 0 : -1;
}

/* For a stage done with a post-M4 ANS, which runs on the APCIe PHY: the PHY and ANS powered off. */
static const char *pcie_ans_path(void)
{
    return adt_path_offset(adt, "/arm-io/apcie") >= 0 ? "/arm-io/apcie" : "/arm-io/apcie0";
}

/*
 * T6050, as macOS around a sleep (run 28). Off: the storage PHYs, children first, then the gates
 * that depend on ANS and ANS itself (the SoC's sleep takes those). On: macOS's wake order. PHY_SW
 * and the GP side stay up for the GP ports.
 */
static const char *const t6050_ans_ps[] = {"APHY_ST0_AUS5",
                                           "APHY_ST0_AUS5_A",
                                           "APHY_ST1_AUS5",
                                           "APHY_ST1_AUS5_A",
                                           "APCIE_SYS_ST1",
                                           "APCIE_SYS_ST0",
                                           "ANS"};
static const u8 t6050_ans_ps_on[] = {6, 5, 4, 1, 3, 0, 2};

static int pcie_t6050_ps(const char *name, bool on)
{
    if (!pmgr_power_steps(0, name, on))
        return 0;
    printf("pcie: %s didn't go %s\n", name, on ? "on" : "off");
    return -1;
}

int pcie_ans_power_off(void)
{
    if (chip_id == T6050) {
        for (size_t i = 0; i < ARRAY_SIZE(t6050_ans_ps); i++)
            if (pcie_t6050_ps(t6050_ans_ps[i], false))
                return -1;
        /* Longer than the 21.5 ms S2R cycles after which macOS restarted ANS (run 28). */
        udelay(100000);
        return 0;
    }
    pcie_initialized = false;
    return pcie_init_controller(APCIE, pcie_ans_path(), true);
}

int pcie_ans_power_on(void)
{
    if (chip_id != T6050)
        return pcie_init();
    for (size_t i = 0; i < ARRAY_SIZE(t6050_ans_ps_on); i++)
        if (pcie_t6050_ps(t6050_ans_ps[t6050_ans_ps_on[i]], true))
            return -1;
    /* macOS sets ANS's run bit 70 ms after the storage PHYs are back (cp9d W1). */
    udelay(70000);
    return 0;
}

int pcie_shutdown(void)
{
    if (chip_id == T8140) {
        printf("pcie: T8140 shutdown unsupported; leaving controller state intact\n");
        return -1;
    }

    if (!pcie_initialized)
        return 0;

    for (u32 controller = 0; controller < NUM_CONTROLLERS; controller++) {
        struct state *state = &controllers[controller];

        if (!state->initialized)
            continue;

        for (u32 port = 0; port < state->port_count; port++) {
            if (!(state->active_ports & BIT(port)))
                continue;
            if (state->pcie_regs->type == APCIE_T602X)
                clear32(state->port_base[port] + APCIE_T602X_PORT_RESET, APCIE_PORT_RESET_DIS);
            else
                clear32(state->port_base[port] + APCIE_PORT_RESET, APCIE_PORT_RESET_DIS);
            clear32(state->port_base[port] + APCIE_PORT_APPCLK, APCIE_PORT_APPCLK_EN);
        }

        for (int phy = 0; phy < state->num_phys; phy++) {
            if (state->pcie_regs->phy_ctrl_reset)
                clear32(state->phy_base[phy] + APCIE_PHY_CTRL, state->pcie_regs->phy_ctrl_reset);
            clear32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK1REQ);
            clear32(state->phy_base[phy] + APCIE_PHY_CTRL, APCIE_PHY_CTRL_CLK0REQ);
        }

        state->initialized = false;
        state->active_ports = 0;
    }

    pcie_initialized = false;
    printf("pcie: Shutdown.\n");

    return 0;
}
