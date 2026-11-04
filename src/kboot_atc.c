/* SPDX-License-Identifier: MIT AND GPL-2.0-only */
#include <assert.h>
#include <string.h>

#include "kboot_atc.h"
#include "adt.h"
#include "devicetree.h"
#include "malloc.h"
#include "pmgr.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define MAX_ATC_DEVS 8

#define CIO3PLL_DCO_NCTRL            0x2a38
#define CIO3PLL_DCO_COARSEBIN_EFUSE0 GENMASK(6, 0)
#define CIO3PLL_DCO_COARSEBIN_EFUSE1 GENMASK(23, 17)

#define CIO3PLL_FRACN_CAN             0x2aa4
#define CIO3PLL_DLL_CAL_START_CAPCODE GENMASK(18, 17)

#define CIO3PLL_DTC_VREG        0x2a20
#define CIO3PLL_DTC_VREG_ADJUST GENMASK(16, 14)

#define AUS_COMMON_SHIM_BLK_VREG 0x0a04
#define AUS_VREG_TRIM            GENMASK(6, 2)

#define AUSPLL_DCO_EFUSE_SPARE         0x222c
#define AUSPLL_RODCO_ENCAP_EFUSE       GENMASK(10, 9)
#define AUSPLL_RODCO_BIAS_ADJUST_EFUSE GENMASK(14, 12)

#define AUSPLL_FRACN_CAN         0x22a4
#define AUSPLL_DLL_START_CAPCODE GENMASK(18, 17)

#define AUSPLL_CLKOUT_DTC_VREG 0x2220
#define AUSPLL_DTC_VREG_ADJUST GENMASK(16, 14)
#define AUSPLL_DTC_VREG_BYPASS BIT(7)

#define AUSPMA_RX_TOP_PMAFSM_RX_CTRL                     0x480
#define AUSPMA_RX_TOP_PMAFSM_RX_CTRL_EQ_CDRLOCK_ON_START BIT(1)

struct atc_tunable {
    u32 offset : 24;
    u32 size : 8;
    u32 mask;
    u32 value;
} PACKED;
static_assert(sizeof(struct atc_tunable) == 12, "Invalid atc_tunable size");

struct adt_tunable_info {
    const char *adt_name;
    const char *fdt_name;
    size_t reg_offset;
    size_t reg_size;
    bool required;
};

struct atc_fuse_info {
    u64 fuse_addr;
    u8 fuse_bit;
    u8 fuse_len;
    u32 reg_offset;
    u32 reg_mask;
};

struct atc_fuse_hw {
    const char *compatible;
    s32 port; /* -1 for don't care */
    const struct atc_fuse_info *fuses;
    size_t n_fuses;
};

static const struct adt_tunable_info atc_tunables[] = {
    /* global tunables applied after power on or reset */
    {"tunable_ATC0AXI2AF", "apple,tunable-axi2af", 0x0, 0x4000, true},
    {"tunable_ATC_FABRIC", "apple,tunable-common-b", 0x45000, 0x4000, true},
    {"tunable_USB_ACIOPHY_TOP", "apple,tunable-common-b", 0x0, 0x4000, true},
    {"tunable_AUS_CMN_SHM", "apple,tunable-common-b", 0xa00, 0x4000, true},
    {"tunable_AUS_CMN_TOP", "apple,tunable-common-b", 0x800, 0x4000, true},
    {"tunable_AUSPLL_CORE", "apple,tunable-common-b", 0x2200, 0x4000, true},
    {"tunable_AUSPLL_TOP", "apple,tunable-common-b", 0x2000, 0x4000, true},
    {"tunable_CIO3PLL_CORE", "apple,tunable-common-b", 0x2a00, 0x4000, true},
    {"tunable_CIO3PLL_TOP", "apple,tunable-common-b", 0x2800, 0x4000, true},
    {"tunable_CIO_CIO3PLL_TOP", "apple,tunable-common-b", 0x2800, 0x4000, false},
    /* lane-specific tunables applied after a cable is connected */
    {"tunable_DP_LN0_AUSPMA_TX_TOP", "apple,tunable-lane0-dp", 0xc000, 0x1000, true},
    {"tunable_DP_LN1_AUSPMA_TX_TOP", "apple,tunable-lane1-dp", 0x13000, 0x1000, true},
    {"tunable_USB_LN0_AUSPMA_TX_TOP", "apple,tunable-lane0-usb", 0xc000, 0x1000, true},
    {"tunable_USB_LN0_AUSPMA_RX_TOP", "apple,tunable-lane0-usb", 0x9000, 0x1000, true},
    {"tunable_USB_LN0_AUSPMA_RX_SHM", "apple,tunable-lane0-usb", 0xb000, 0x1000, true},
    {"tunable_USB_LN0_AUSPMA_RX_EQ", "apple,tunable-lane0-usb", 0xa000, 0x1000, true},
    {"tunable_USB_LN1_AUSPMA_TX_TOP", "apple,tunable-lane1-usb", 0x13000, 0x1000, true},
    {"tunable_USB_LN1_AUSPMA_RX_TOP", "apple,tunable-lane1-usb", 0x10000, 0x1000, true},
    {"tunable_USB_LN1_AUSPMA_RX_SHM", "apple,tunable-lane1-usb", 0x12000, 0x1000, true},
    {"tunable_USB_LN1_AUSPMA_RX_EQ", "apple,tunable-lane1-usb", 0x11000, 0x1000, true},
    {"tunable_CIO_LN0_AUSPMA_TX_TOP", "apple,tunable-lane0-cio", 0xc000, 0x1000, true},
    {"tunable_CIO_LN0_AUSPMA_RX_TOP", "apple,tunable-lane0-cio", 0x9000, 0x1000, true},
    {"tunable_CIO_LN0_AUSPMA_RX_SHM", "apple,tunable-lane0-cio", 0xb000, 0x1000, true},
    {"tunable_CIO_LN0_AUSPMA_RX_EQ", "apple,tunable-lane0-cio", 0xa000, 0x1000, true},
    {"tunable_CIO_LN1_AUSPMA_TX_TOP", "apple,tunable-lane1-cio", 0x13000, 0x1000, true},
    {"tunable_CIO_LN1_AUSPMA_RX_TOP", "apple,tunable-lane1-cio", 0x10000, 0x1000, true},
    {"tunable_CIO_LN1_AUSPMA_RX_SHM", "apple,tunable-lane1-cio", 0x12000, 0x1000, true},
    {"tunable_CIO_LN1_AUSPMA_RX_EQ", "apple,tunable-lane1-cio", 0x11000, 0x1000, true},
};

static const struct adt_tunable_info atc_tunables_t8122[] = {
    {"tunable_ATC0AXI2AF", "apple,tunable-axi2af", 0x0, 0x8000, true},
    {"tunable_ATC_FABRIC", "apple,tunable-common-b", 0x44000, 0x4000, true},

    {"tunable_CIO3PLL_CORE", "apple,tunable-common-b", 0x2a00, 0x200, true},
    {"tunable_CIO3PLL_TOP", "apple,tunable-common-b", 0x2800, 0x200, true},
    {"tunable_ACIOPHY_LANE_USBC0", "apple,tunable-common-b", 0x5000, 0x1000, true},
    {"tunable_ACIOPHY_PLL_TOP", "apple,tunable-common-b", 0x1000, 0x4000, true},
    {"tunable_ACIOPHY_TOP", "apple,tunable-common-b", 0x0, 0x4000, true},

    {"tunable_AUSCMN_DIG", "apple,tunable-common-b", 0x800, 0x200, true},
    {"tunable_AUSPLL_CORE", "apple,tunable-common-b", 0x2200, 0x4000, true},
    //{"tunable_AUX_SHM", "apple,tunable-common-b", 0x0, 0x0, true}, // TODO: offset?
    {"tunable_AUX_TOP", "apple,tunable-common-b", 0x16000, 0x4000, true},
    {"tunable_AUSCMN_SHM", "apple,tunable-common-b", 0xa00, 0x200, true},
    {"tunable_CLKMON_CFG", "apple,tunable-common-b", 0x2600, 0x100, false},
    {"tunable_CIO_SHIM", "apple,tunable-common-b", 0x4000, 0x4000, true},

    {"tunable_LN0_RX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0x9000, 0x1000, true},
    {"tunable_LN0_RX_TOP_USB_EQA", "apple,tunable-lane0-usb", 0x9000, 0x1000, false},
    {"tunable_LN0_RX_EQ_USB_EQA", "apple,tunable-lane0-usb", 0xa000, 0x1000, true},
    {"tunable_LN0_RX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xb000, 0x1000, true},
    {"tunable_LN0_TX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0xc000, 0x1000, true},
    {"tunable_LN0_TX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xd000, 0x1000, true},
    {"tunable_LN0_RX_TOP_CIO_DFLT", "apple,tunable-lane0-cio", 0x9000, 0x1000, true},
    {"tunable_LN0_RX_EQ_CIO_EQA", "apple,tunable-lane0-cio", 0xa000, 0x1000, true},
    {"tunable_LN0_RX_SHM_CIO_DFLT", "apple,tunable-lane0-cio", 0xb000, 0x1000, true},
    {"tunable_LN0_TX_TOP_CIO_DFLT", "apple,tunable-lane0-cio", 0xc000, 0x1000, true},
    {"tunable_LN0_TX_SHM_CIO_DFLT", "apple,tunable-lane0-cio", 0xd000, 0x1000, true},

    {"tunable_LN1_RX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x10000, 0x1000, true},
    {"tunable_LN1_RX_TOP_USB_EQA", "apple,tunable-lane1-usb", 0x10000, 0x1000, false},
    {"tunable_LN1_RX_EQ_USB_EQA", "apple,tunable-lane1-usb", 0x11000, 0x1000, true},
    {"tunable_LN1_RX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x12000, 0x1000, true},
    {"tunable_LN1_TX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x13000, 0x1000, true},
    {"tunable_LN1_TX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x14000, 0x1000, true},
    {"tunable_LN1_RX_TOP_CIO_DFLT", "apple,tunable-lane1-cio", 0x10000, 0x1000, true},
    {"tunable_LN1_RX_EQ_CIO_EQA", "apple,tunable-lane1-cio", 0x11000, 0x1000, true},
    {"tunable_LN1_RX_SHM_CIO_DFLT", "apple,tunable-lane1-cio", 0x12000, 0x1000, true},
    {"tunable_LN1_TX_TOP_CIO_DFLT", "apple,tunable-lane1-cio", 0x13000, 0x1000, true},
    {"tunable_LN1_TX_SHM_CIO_DFLT", "apple,tunable-lane1-cio", 0x14000, 0x1000, true},
};

/*
 * T8132 has separate AXI, LIOA, fabric and common-configuration banks. Keep
 * their lists apart: the Linux PHY must not map the intervening display
 * crossbar, nor apply the fabric list through its smaller core resource.
 */
static const struct adt_tunable_info atc_tunables_t8132_usb3[] = {
    {"tunable_ATCAXI2AF", "apple,tunable-usb3-axi2af", 0, 0x8000, true},
    {"tunable_ATCAXI2AF_WR_DIS", "apple,tunable-usb3-axi2af", 0, 0x8000, true},
    {"tunable_ATCAXI2AF_LOCK_LOCK", "apple,tunable-usb3-axi2af", 0, 0x8000, true},
    {"tunable_ATCAXI2AF_LIOA_IOA", "apple,tunable-usb3-lioa", 0, 0x4000, true},
    {"tunable_ATC_FABRIC", "apple,tunable-usb3-fabric", 0, 0x4000, true},
    {"tunable_ATC_COMMON_CFG", "apple,tunable-usb3-config", 0, 0x4000, true},
    {"tunable_AUSCMN_DIG", "apple,tunable-usb3-common", 0x800, 0x200, true},
    {"tunable_AUSPLL_CORE", "apple,tunable-usb3-common", 0x2200, 0x4000, true},
    {"tunable_CIO3PLL_CORE", "apple,tunable-usb3-common", 0x2a00, 0x200, true},
    {"tunable_CIO3PLL_TOP", "apple,tunable-usb3-common", 0x2800, 0x200, true},
    {"tunable_ACIOPHY_LANE_USBC0", "apple,tunable-usb3-common", 0x5000, 0x1000, true},
    {"tunable_ACIOPHY_PLL_TOP", "apple,tunable-usb3-common", 0x1000, 0x4000, true},
    {"tunable_ACIOPHY_TOP", "apple,tunable-usb3-common", 0, 0x4000, true},
    {"tunable_AUSCMN_SHM", "apple,tunable-usb3-efuse", 0xa00, 0x200, true},
    {"tunable_CLKMON_CFG", "apple,tunable-usb3-efuse", 0x2600, 0x100, true},
    {"tunable_LN0_RX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0x9000, 0x1000, true},
    {"tunable_LN0_RX_TOP_USB_EQA", "apple,tunable-lane0-usb", 0x9000, 0x1000, false},
    {"tunable_LN0_RX_EQ_USB_EQA", "apple,tunable-lane0-usb", 0xa000, 0x1000, true},
    {"tunable_LN0_RX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xb000, 0x1000, true},
    {"tunable_LN0_TX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0xc000, 0x1000, true},
    {"tunable_LN0_TX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xd000, 0x1000, true},
    {"tunable_LN1_RX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x10000, 0x1000, true},
    {"tunable_LN1_RX_TOP_USB_EQA", "apple,tunable-lane1-usb", 0x10000, 0x1000, false},
    {"tunable_LN1_RX_EQ_USB_EQA", "apple,tunable-lane1-usb", 0x11000, 0x1000, true},
    {"tunable_LN1_RX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x12000, 0x1000, true},
    {"tunable_LN1_TX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x13000, 0x1000, true},
    {"tunable_LN1_TX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x14000, 0x1000, true},
};

/* Optional for USB3-only: the USB half of multifunction mode also needs AUX. */
static const struct adt_tunable_info atc_tunable_t8132_usb3_dp = {
    "tunable_AUX_TOP", "apple,tunable-usb3-dp", 0x16000, 0x4000, true,
};

static const struct adt_tunable_info atc_tunables_t8130[] = {
    {"tunable_ATC0AXI2AF", "apple,tunable-axi2af", 0, 0x8000, true},
    {"tunable_USB2PHY_REG_DFLT", "apple,tunable-usb2phy-reg-dflt", 0, 0x4000, true},
    {"tunable_ATC_FABRIC", "apple,tunable-common-b", 0x44000, 0x4000, true},
    {"tunable_CIO3PLL_CORE", "apple,tunable-common-b", 0x2a00, 0x200, true},
    {"tunable_CIO3PLL_TOP", "apple,tunable-common-b", 0x2800, 0x200, true},
    {"tunable_ACIOPHY_LANE_USBC0", "apple,tunable-common-b", 0x5000, 0x1000, true},
    {"tunable_ACIOPHY_PLL_TOP", "apple,tunable-common-b", 0x1000, 0x4000, true},
    {"tunable_ACIOPHY_TOP", "apple,tunable-common-b", 0, 0x4000, true},
    {"tunable_AUSCMN_DIG", "apple,tunable-common-b", 0x800, 0x200, true},
    {"tunable_AUSPLL_CORE", "apple,tunable-common-b", 0x2200, 0x4000, true},
    {"tunable_AUX_TOP", "apple,tunable-common-b", 0x16000, 0x4000, true},
    {"tunable_AUSCMN_SHM", "apple,tunable-common-b", 0xa00, 0x200, true},
    {"tunable_CLKMON_CFG", "apple,tunable-common-b", 0x2600, 0x100, false},
    {"tunable_CIO_SHIM", "apple,tunable-common-b", 0x4000, 0x4000, false},
    {"tunable_LN0_RX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0x9000, 0x1000, true},
    {"tunable_LN0_RX_EQ_USB_EQA", "apple,tunable-lane0-usb", 0xa000, 0x1000, true},
    {"tunable_LN0_RX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xb000, 0x1000, true},
    {"tunable_LN0_TX_TOP_USB_DFLT", "apple,tunable-lane0-usb", 0xc000, 0x1000, true},
    {"tunable_LN0_TX_SHM_USB_DFLT", "apple,tunable-lane0-usb", 0xd000, 0x1000, true},
    {"tunable_LN1_RX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x10000, 0x1000, true},
    {"tunable_LN1_RX_EQ_USB_EQA", "apple,tunable-lane1-usb", 0x11000, 0x1000, true},
    {"tunable_LN1_RX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x12000, 0x1000, true},
    {"tunable_LN1_TX_TOP_USB_DFLT", "apple,tunable-lane1-usb", 0x13000, 0x1000, true},
    {"tunable_LN1_TX_SHM_USB_DFLT", "apple,tunable-lane1-usb", 0x14000, 0x1000, true},
};

/*
 * A T6030 ATC PHY wired as a fixed DisplayPort transmitter (the J514S HDMI
 * port's): the calibration its power-up applies, in this order, in one list.
 */
static const struct adt_tunable_info atc_tunables_t6030_dp[] = {
    {"tunable_AUSCMN_DIG", "apple,tunable-dp-calibration", 0x800, 0x200, true},
    {"tunable_AUSPLL_CORE", "apple,tunable-dp-calibration", 0x2200, 0x4000, true},
    {"tunable_AUX_TOP", "apple,tunable-dp-calibration", 0x16000, 0x4000, true},
    {"tunable_AUSCMN_SHM", "apple,tunable-dp-calibration", 0xa00, 0x200, true},
    {"tunable_CLKMON_CFG", "apple,tunable-dp-calibration", 0x2600, 0x100, true},
};

static const struct atc_fuse_info atc_fuses_t8103_port0[] = {
    {0x23d2bc434, 9, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x23d2bc434, 15, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x23d2bc434, 21, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x23d2bc434, 23, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x23d2bc434, 4, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x23d2bc430, 29, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x23d2bc430, 26, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x23d2bc434, 2, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x23d2bc430, 31, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x23d2bc434, 4, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t8103_port1[] = {
    {0x23d2bc438, 19, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x23d2bc438, 25, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x23d2bc438, 31, 1, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    /* next three rows are some kind of workaround for port 1 */
    {0x23d2bc438, 14, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x23d2bc43c, 0, 1, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x23d2bc43c, 1, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x23d2bc438, 7, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x23d2bc438, 4, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x23d2bc438, 12, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x23d2bc438, 9, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x23d2bc438, 14, 4, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port0[] = {
    {0x2922bca14, 5, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x2922bca14, 11, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x2922bca14, 17, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x2922bca14, 19, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x2922bca14, 0, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x2922bca10, 25, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x2922bca10, 22, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x2922bca10, 30, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x2922bca10, 27, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x2922bca14, 0, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port1[] = {
    {0x2922bca18, 15, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x2922bca18, 21, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x2922bca18, 27, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x2922bca18, 29, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x2922bca18, 10, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x2922bca18, 3, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x2922bca18, 0, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x2922bca18, 8, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x2922bca18, 5, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x2922bca18, 10, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port2[] = {
    {0x2922bca1c, 25, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x2922bca1c, 31, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x2922bca20, 5, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x2922bca20, 7, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x2922bca1c, 20, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x2922bca1c, 13, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x2922bca1c, 10, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x2922bca1c, 18, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x2922bca1c, 15, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x2922bca1c, 20, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port3[] = {
    {0x2922bca24, 3, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x2922bca24, 9, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x2922bca24, 15, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x2922bca24, 17, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x2922bca20, 30, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x2922bca20, 23, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x2922bca20, 20, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x2922bca20, 28, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x2922bca20, 25, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x2922bca20, 30, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port4[] = {
    {0x22922bca14, 5, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x22922bca14, 11, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x22922bca14, 17, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x22922bca14, 19, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x22922bca14, 0, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x22922bca10, 25, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x22922bca10, 22, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x22922bca10, 30, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x22922bca10, 27, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x22922bca14, 0, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t6000_port5[] = {
    {0x22922bca18, 15, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x22922bca18, 21, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x22922bca18, 27, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x22922bca18, 29, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x22922bca18, 10, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x22922bca18, 3, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x22922bca18, 0, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x22922bca18, 8, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x22922bca18, 5, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x22922bca18, 10, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t8112_port0[] = {
    {0x23d2c8484, 3, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x23d2c8484, 9, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x23d2c8484, 15, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x23d2c8484, 17, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x23d2c8480, 30, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x23d2c8480, 23, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x23d2c8480, 20, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x23d2c8480, 28, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x23d2c8480, 25, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x23d2c8480, 30, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

static const struct atc_fuse_info atc_fuses_t8112_port1[] = {
    {0x23d2c8488, 13, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE0},
    {0x23d2c8488, 19, 6, CIO3PLL_DCO_NCTRL, CIO3PLL_DCO_COARSEBIN_EFUSE1},
    {0x23d2c8488, 25, 2, CIO3PLL_FRACN_CAN, CIO3PLL_DLL_CAL_START_CAPCODE},
    {0x23d2c8488, 27, 3, CIO3PLL_DTC_VREG, CIO3PLL_DTC_VREG_ADJUST},
    {0x23d2c8488, 8, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
    {0x23d2c8488, 1, 2, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_ENCAP_EFUSE},
    {0x23d2c8484, 30, 3, AUSPLL_DCO_EFUSE_SPARE, AUSPLL_RODCO_BIAS_ADJUST_EFUSE},
    {0x23d2c8488, 6, 2, AUSPLL_FRACN_CAN, AUSPLL_DLL_START_CAPCODE},
    {0x23d2c8488, 3, 3, AUSPLL_CLKOUT_DTC_VREG, AUSPLL_DTC_VREG_ADJUST},
    {0x23d2c8488, 8, 5, AUS_COMMON_SHIM_BLK_VREG, AUS_VREG_TRIM},
};

// Order "atc-phy" compatibles in reverse chronologically order to deal with mutliple compatible
// strings in ADT atc-phy nodes.
static const struct atc_fuse_hw atc_fuses[] = {
    {"atc-phy,t8142", -1, NULL, 0},
    {"atc-phy,t8132", -1, NULL, 0},
    {"atc-phy,t8130", -1, NULL, 0},
    {"atc-phy,t8122", -1, NULL, 0},
    {"atc-phy,t6020", -1, NULL, 0},
    {"atc-phy,t8112", 0, atc_fuses_t8112_port0, ARRAY_SIZE(atc_fuses_t8112_port0)},
    {"atc-phy,t8112", 1, atc_fuses_t8112_port1, ARRAY_SIZE(atc_fuses_t8112_port1)},
    /* t6002 uses the same fuses and the same atc-phy,t6000 compatible */
    {"atc-phy,t6000", 0, atc_fuses_t6000_port0, ARRAY_SIZE(atc_fuses_t6000_port0)},
    {"atc-phy,t6000", 1, atc_fuses_t6000_port1, ARRAY_SIZE(atc_fuses_t6000_port1)},
    {"atc-phy,t6000", 2, atc_fuses_t6000_port2, ARRAY_SIZE(atc_fuses_t6000_port2)},
    {"atc-phy,t6000", 3, atc_fuses_t6000_port3, ARRAY_SIZE(atc_fuses_t6000_port3)},
    {"atc-phy,t6000", 4, atc_fuses_t6000_port4, ARRAY_SIZE(atc_fuses_t6000_port4)},
    {"atc-phy,t6000", 5, atc_fuses_t6000_port5, ARRAY_SIZE(atc_fuses_t6000_port5)},
    {"atc-phy,t8103", 0, atc_fuses_t8103_port0, ARRAY_SIZE(atc_fuses_t8103_port0)},
    {"atc-phy,t8103", 1, atc_fuses_t8103_port1, ARRAY_SIZE(atc_fuses_t8103_port1)},
};

static u32 read_fuse(const struct atc_fuse_info *fuse)
{
    union {
        u64 dword;
        u32 words[2];
    } fuse_data;

    if (fuse->fuse_bit + fuse->fuse_len > 64) {
        printf("kboot: ATC fuse 0x%lx:%d:%d out of range\n", fuse->fuse_addr, fuse->fuse_bit,
               fuse->fuse_len);
        return 0;
    }

    /* Any other read triggers SErrors */
    fuse_data.words[0] = read32(fuse->fuse_addr);
    fuse_data.words[1] = read32(fuse->fuse_addr + 4);

    /*
     * Assuming we read 01 23 45 67 89 ab cd ef above and have bit_offset 12
     * and len 4 we want to end up with 0x2. When treating the data as u64
     * this is 0xefcdab8967452301 such that we can simply shift it by 12 bits
     * to get 0x000efcdab8967452 and then AND it with 0xf to
     * finally get 0x2 which is the value we want.
     */
    fuse_data.dword >>= fuse->fuse_bit;
    fuse_data.dword &= (1ULL << fuse->fuse_len) - 1;
    return FIELD_PREP(fuse->reg_mask, fuse_data.dword);
}

static int dt_append_atc_fuses_helper(void *dt, int fdt_node, const struct atc_fuse_info *fuses,
                                      size_t n_fuses)
{
    for (size_t i = 0; i < n_fuses; ++i) {
        if (fdt_appendprop_u32(dt, fdt_node, "apple,tunable-common-a", fuses[i].reg_offset) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, "apple,tunable-common-a", fuses[i].reg_mask) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, "apple,tunable-common-a", read_fuse(&fuses[i])) < 0)
            return -1;
    }

    return 0;
}

static int dt_append_fuses(void *dt, int adt_node, int fdt_node, int port)
{
    for (size_t i = 0; i < ARRAY_SIZE(atc_fuses); ++i) {
        if (!adt_is_compatible_at(adt, adt_node, atc_fuses[i].compatible, 0))
            continue;
        if (atc_fuses[i].port >= 0 && port != atc_fuses[i].port)
            continue;

        /*
         * Starting with t6020 fuses are no longer required. Create an empty
         * property to indicate to the driver that no fuses are intentional.
         */
        if (!atc_fuses[i].fuses)
            return fdt_setprop(dt, fdt_node, "apple,tunable-common-a", NULL, 0);

        return dt_append_atc_fuses_helper(dt, fdt_node, atc_fuses[i].fuses, atc_fuses[i].n_fuses);
    }

    /*
     * don't fail here until we have added all devices to retain backwards
     * compatibility with the previous atcphy version
     */
    printf("kboot: no fuses found for atcphy port %d\n", port);
    return 0;
}

static int dt_append_atc_tunable(void *dt, int adt_node, int fdt_node,
                                 const struct adt_tunable_info *tunable_info)
{
    u32 tunables_len;
    const struct atc_tunable *tunable_adt =
        adt_getprop(adt, adt_node, tunable_info->adt_name, &tunables_len);

    // Alias tunable_ATC0AXI2AF to tunable_ATCAXI2AF, seen on M4 Mac mini (J773g)
    if (!tunable_adt && !strcmp(tunable_info->adt_name, "tunable_ATC0AXI2AF")) {
        tunable_adt = adt_getprop(adt, adt_node, "tunable_ATCAXI2AF", &tunables_len);
    }

    if (!tunable_adt) {
        printf("ADT: tunable %s not found\n", tunable_info->adt_name);

        if (tunable_info->required)
            return -1;
        else
            return 0;
    }

    if (tunables_len % sizeof(*tunable_adt)) {
        printf("ADT: tunable %s with invalid length %d\n", tunable_info->adt_name, tunables_len);
        return -1;
    }

    u32 n_tunables = tunables_len / sizeof(*tunable_adt);
    for (size_t j = 0; j < n_tunables; j++) {
        const struct atc_tunable *tunable = &tunable_adt[j];

        if (tunable->size != 32) {
            printf("kboot: ATC tunable has invalid size %d\n", tunable->size);
            return -1;
        }

        if (tunable->offset % (tunable->size / 8)) {
            printf("kboot: ATC tunable has unaligned offset %x\n", tunable->offset);
            return -1;
        }

        if (tunable->offset + (tunable->size / 8) > tunable_info->reg_size) {
            printf("kboot: ATC tunable has invalid offset %x\n", tunable->offset);
            return -1;
        }

        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name,
                               tunable->offset + tunable_info->reg_offset) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name, tunable->mask) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name, tunable->value) < 0)
            return -1;
    }

    return 0;
}

/*
 * T6030 atc-phy3 drives only the HDMI port, as a DisplayPort PHY. Its USB and CIO lane tunables
 * are optional: if the ADT does not have them, the common tunables (which DisplayPort needs) are
 * still passed, and the kernel keeps the PHY to DisplayPort. Every other PHY needs every required
 * entry, as before.
 */
static bool atc_lanes_optional(int port)
{
    return chip_id == T6030 && port == 3;
}

static bool atc_lane_tunable(const struct adt_tunable_info *t)
{
    static const char prefix[] = "apple,tunable-lane";
    return !strncmp(t->fdt_name, prefix, sizeof(prefix) - 1);
}

/* The absent optional entries, for one log line: a ';'-separated list, cut short with "..." */
struct atc_absent {
    char list[192];
    int count;
    bool cut;
};

static void atc_note_absent(struct atc_absent *a, const char *name)
{
    size_t used = strlen(a->list);
    size_t sep = used ? 1 : 0;

    a->count++;
    if (a->cut)
        return;
    /* Keep room for ";..." */
    if (used + sep + strlen(name) + 4 >= sizeof(a->list)) {
        strcpy(a->list + used, sep ? ";..." : "...");
        a->cut = true;
        return;
    }
    if (sep)
        a->list[used++] = ';';
    strcpy(a->list + used, name);
}

/* J700's DP lists use the same native ATC records as the USB lists above, but
 * their grouping and destination windows are distinct. Keep this map in the
 * order in which the Linux ATC PHY consumes the lists. */
struct j700_atc_group {
    const char *adt_name;
    u8 reg_index;
    u8 output;
    u64 bank_base;
    u64 bank_size;
    u32 rebase;
};

enum { J700_AXI2AF, J700_DP_PRE, J700_DP_LPDPTX, J700_DP_POST, J700_LISTS };

static const char *const j700_list_names[J700_LISTS] = {
    "apple,tunable-axi2af",
    "apple,tunable-dp-common-pre",
    "apple,tunable-dp-lpdptx",
    "apple,tunable-dp-common-post",
};

static const struct j700_atc_group j700_atc_groups[] = {
    {"tunable_ATC0AXI2AF", 31, J700_AXI2AF, 0x408000000, 0x1000000, 0},
    {"tunable_ATC_FABRIC", 3, J700_DP_PRE, 0x40b044000, 0x4000, 0x44000},
    {"tunable_ATC_COMMON_CFG", 32, J700_DP_LPDPTX, 0x40b050000, 0x4000, 0},
    {"tunable_AUSCMN_DIG", 5, J700_DP_POST, 0x40b000800, 0x4000, 0x800},
    {"tunable_AUSPLL_CORE", 9, J700_DP_POST, 0x40b002200, 0x4000, 0x2200},
    {"tunable_AUX_TOP", 26, J700_DP_POST, 0x40b016000, 0x4000, 0x16000},
};

static const char *const j700_training_names[] = {
    "dp-training-table",      "dp-training-table-rbr",  "dp-training-table-hbr",
    "dp-training-table-hbr2", "dp-training-table-hbr3",
};

struct j700_atc_output {
    u8 *data;
    int len;
    bool present;
};

static u32 j700_le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 j700_le64(const u8 *p)
{
    return (u64)j700_le32(p) | (u64)j700_le32(p + 4) << 32;
}

static void j700_be32(u8 *p, u32 value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static int j700_atc_resource(void *dt, int node, const char *name, u64 base, u64 size)
{
    int index = fdt_stringlist_search(dt, node, "reg-names", name);
    int len;
    const u8 *reg = fdt_getprop(dt, node, "reg", &len);

    if (index < 0 || !reg || len < 0 || len % 16 || (size_t)index >= (size_t)len / 16)
        return -1;
    const u8 *entry = reg + 16 * index;
    if (fdt64_ld((const fdt64_t *)entry) != base || fdt64_ld((const fdt64_t *)(entry + 8)) != size)
        return -1;
    return 0;
}

static void j700_atc_clear_dp(void *dt, int node)
{
    for (size_t i = J700_DP_PRE; i < J700_LISTS; i++)
        fdt_delprop(dt, node, j700_list_names[i]);
    for (size_t i = 0; i < ARRAY_SIZE(j700_training_names); i++) {
        char name[48];
        snprintf(name, sizeof(name), "apple,%s", j700_training_names[i]);
        fdt_delprop(dt, node, name);
    }
}

static int j700_atc_check_bank(int *adt_path, int adt_node, const struct j700_atc_group *group)
{
    u32 reg_len, ranges_len;
    const u8 *reg = adt_getprop(adt, adt_node, "reg", &reg_len);
    int arm_io = adt_path_offset(adt, "/arm-io");
    if (arm_io < 0 || !reg || reg_len % 16 || (size_t)group->reg_index >= reg_len / 16)
        return -1;
    const u8 *ranges = adt_getprop(adt, arm_io, "ranges", &ranges_len);
    if (!ranges || ranges_len != 7 * 24)
        return -1;

    const u8 *entry = reg + group->reg_index * 16;
    u64 child_addr = j700_le64(entry);
    u64 size = j700_le64(entry + 8);
    if (size != group->bank_size)
        return -1;

    int matches = 0;
    for (u32 pos = 0; pos < ranges_len; pos += 24) {
        u64 start = j700_le64(ranges + pos);
        u64 parent = j700_le64(ranges + pos + 8);
        u64 length = j700_le64(ranges + pos + 16);
        if (child_addr < start || size > length || child_addr - start > length - size)
            continue;
        if (parent > UINT64_MAX - (child_addr - start) ||
            parent + (child_addr - start) != group->bank_base)
            return -1;
        matches++;
    }
    if (matches != 1)
        return -1;

    u64 translated, translated_size;
    if (adt_get_reg(adt, adt_path, "reg", group->reg_index, &translated, &translated_size) < 0 ||
        translated != group->bank_base || translated_size != group->bank_size)
        return -1;
    return 0;
}

static int j700_atc_prepare_group(int *adt_path, int adt_node, const struct j700_atc_group *group,
                                  struct j700_atc_output *out, u32 resource_size)
{
    u32 len;
    const u8 *raw = adt_getprop(adt, adt_node, group->adt_name, &len);
    if (!raw || len % 12 || j700_atc_check_bank(adt_path, adt_node, group) ||
        len > (u32)(INT32_MAX - out->len))
        return -1;

    for (u32 pos = 0; pos < len; pos += 12) {
        u32 packed = j700_le32(raw + pos);
        u32 local = packed & 0xffffff;
        u64 dest = (u64)group->rebase + local;
        if (packed >> 24 != 32 || local % 4 || (u64)local + 4 > group->bank_size ||
            dest + 4 > resource_size)
            return -1;
    }

    int old_len = out->len;
    int new_len = old_len + len;
    u8 *data = malloc(new_len ? new_len : 1);
    if (!data)
        return -1;
    if (old_len)
        memcpy(data, out->data, old_len);
    free(out->data);
    out->data = data;
    out->len = new_len;
    out->present = true;

    for (u32 pos = 0; pos < len; pos += 12) {
        u32 packed = j700_le32(raw + pos);
        j700_be32(data + old_len + pos, group->rebase + (packed & 0xffffff));
        j700_be32(data + old_len + pos + 4, j700_le32(raw + pos + 4));
        j700_be32(data + old_len + pos + 8, j700_le32(raw + pos + 8));
    }
    return 0;
}

static void dt_export_j700_atc(void *dt, int adt_node, int fdt_node)
{
    if (chip_id != T8140 || fdt_node_check_compatible(dt, 0, "apple,j700") ||
        fdt_node_check_compatible(dt, fdt_node, "apple,t8140-atcphy"))
        return;

    fdt_delprop(dt, fdt_node, j700_list_names[J700_AXI2AF]);
    j700_atc_clear_dp(dt, fdt_node);
    if (adt_node < 0 || !adt_is_compatible_at(adt, adt_node, "atc-phy,t8130", 0))
        return;

    int adt_path[8];
    if (adt_path_offset_trace(adt, "/arm-io/atc-phy0", adt_path) != adt_node)
        return;

    struct j700_atc_output output[J700_LISTS + ARRAY_SIZE(j700_training_names)] = {0};
    bool dp_valid = true;
    if (j700_atc_resource(dt, fdt_node, "axi2af", 0x408000000, 0x8000) ||
        j700_atc_prepare_group(adt_path, adt_node, &j700_atc_groups[0], &output[J700_AXI2AF],
                               0x8000)) {
        fdt_delprop(dt, fdt_node, j700_list_names[J700_AXI2AF]);
    } else if (fdt_setprop(dt, fdt_node, j700_list_names[J700_AXI2AF], output[J700_AXI2AF].data,
                           output[J700_AXI2AF].len)) {
        fdt_delprop(dt, fdt_node, j700_list_names[J700_AXI2AF]);
    }

    if (j700_atc_resource(dt, fdt_node, "core", 0x40b000000, 0x4c000) ||
        j700_atc_resource(dt, fdt_node, "lpdptx", 0x40b050000, 0x4000))
        dp_valid = false;
    for (size_t i = 1; dp_valid && i < ARRAY_SIZE(j700_atc_groups); i++) {
        const struct j700_atc_group *group = &j700_atc_groups[i];
        u32 size = group->output == J700_DP_LPDPTX ? 0x4000 : 0x4c000;
        if (j700_atc_prepare_group(adt_path, adt_node, group, &output[group->output], size))
            dp_valid = false;
    }
    for (size_t i = 0; dp_valid && i < ARRAY_SIZE(j700_training_names); i++) {
        u32 len;
        const u8 *raw = adt_getprop(adt, adt_node, j700_training_names[i], &len);
        if (!raw) {
            if (i == 0)
                dp_valid = false;
            continue;
        }
        if (len != 64) {
            dp_valid = false;
            break;
        }
        output[J700_LISTS + i].data = malloc(64);
        if (!output[J700_LISTS + i].data) {
            dp_valid = false;
            break;
        }
        output[J700_LISTS + i].len = 64;
        output[J700_LISTS + i].present = true;
        for (size_t cell = 0; cell < 16; cell++)
            j700_be32(output[J700_LISTS + i].data + cell * 4, j700_le32(raw + cell * 4));
    }

    /* Remove template values even when the current ADT bundle is absent. */
    j700_atc_clear_dp(dt, fdt_node);
    if (dp_valid) {
        for (size_t i = J700_DP_PRE; i < J700_LISTS; i++) {
            if (fdt_setprop(dt, fdt_node, j700_list_names[i], output[i].data, output[i].len))
                dp_valid = false;
        }
        for (size_t i = 0; dp_valid && i < ARRAY_SIZE(j700_training_names); i++) {
            if (!output[J700_LISTS + i].present)
                continue;
            char name[48];
            snprintf(name, sizeof(name), "apple,%s", j700_training_names[i]);
            if (fdt_setprop(dt, fdt_node, name, output[J700_LISTS + i].data,
                            output[J700_LISTS + i].len))
                dp_valid = false;
        }
        if (!dp_valid)
            j700_atc_clear_dp(dt, fdt_node);
    }
    if (!dp_valid)
        printf("kboot: J700 ATC DisplayPort tables unavailable\n");
    for (size_t i = 0; i < ARRAY_SIZE(output); i++)
        free(output[i].data);
}

/* Only the DP calibration: this PHY has no USB, USB4 or Type-C side. */
static void dt_copy_atc_t6030_dp_tunables(void *dt, int adt_node, int fdt_node,
                                          const char *adt_path)
{
    const char *name = atc_tunables_t6030_dp[0].fdt_name;

    fdt_delprop(dt, fdt_node, name);
    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t6030_dp); ++i) {
        if (dt_append_atc_tunable(dt, adt_node, fdt_node, &atc_tunables_t6030_dp[i])) {
            fdt_delprop(dt, fdt_node, name);
            printf("FDT: Unable to copy the DP calibration for %s\n", adt_path);
            return;
        }
    }
    printf("FDT: Copied the DP calibration for %s\n", adt_path);
}

/* Publish the complete USB3 bundle or none of it, preserving USB2 fallback. */
static void dt_copy_t8132_usb3_tunables(void *dt, int adt_node, int fdt_node)
{
    fdt_delprop(dt, fdt_node, atc_tunable_t8132_usb3_dp.fdt_name);
    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t8132_usb3); i++)
        fdt_delprop(dt, fdt_node, atc_tunables_t8132_usb3[i].fdt_name);

    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t8132_usb3); i++) {
        const struct adt_tunable_info *group = &atc_tunables_t8132_usb3[i];
        u32 len;
        const void *raw = adt_getprop(adt, adt_node, group->adt_name, &len);

        if (group->required && (!raw || !len))
            goto cleanup;
        if (dt_append_atc_tunable(dt, adt_node, fdt_node, group))
            goto cleanup;
    }
    printf("FDT: Copied T8132 USB3 calibration\n");

    /* Missing or malformed AUX data must not discard a complete USB bundle. */
    u32 len;
    const struct adt_tunable_info *dp = &atc_tunable_t8132_usb3_dp;
    const void *raw = adt_getprop(adt, adt_node, dp->adt_name, &len);

    if (!raw || !len || dt_append_atc_tunable(dt, adt_node, fdt_node, dp)) {
        fdt_delprop(dt, fdt_node, dp->fdt_name);
        printf("FDT: T8132 multifunction calibration unavailable\n");
    }
    return;

cleanup:
    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t8132_usb3); i++)
        fdt_delprop(dt, fdt_node, atc_tunables_t8132_usb3[i].fdt_name);
    printf("FDT: T8132 USB3 calibration incomplete; preserving USB2 fallback\n");
}

static void dt_copy_atc_tunables(void *dt, const char *adt_path, const char *dt_alias, int port)
{
    int ret;
    const struct adt_tunable_info *tunables;
    size_t tunable_count;
    bool lanes_optional = atc_lanes_optional(port);
    struct atc_absent absent = {0};

    const char *fdt_path = fdt_get_alias(dt, dt_alias);
    if (fdt_path == NULL) {
        printf("FDT: Unable to find alias %s\n", dt_alias);
        return;
    }

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0) {
        printf("FDT: Unable to find path %s for alias %s\n", fdt_path, dt_alias);
        return;
    }

    int adt_node = adt_path_offset(adt, adt_path);
    dt_export_j700_atc(dt, adt_node, fdt_node);
    if (adt_node < 0)
        return;

    if (chip_id == T8132 && !fdt_node_check_compatible(dt, 0, "apple,j713") &&
        !fdt_node_check_compatible(dt, fdt_node, "apple,t8132-atcphy") &&
        adt_is_compatible_at(adt, adt_node, "atc-phy,t8132", 0)) {
        dt_copy_t8132_usb3_tunables(dt, adt_node, fdt_node);
        return;
    }

    if (!fdt_node_check_compatible(dt, fdt_node, "apple,t6030-atcphy-dp")) {
        if (adt_is_compatible_at(adt, adt_node, "atc-phy,t8122", 0))
            dt_copy_atc_t6030_dp_tunables(dt, adt_node, fdt_node, adt_path);
        return;
    }

    ret = dt_append_fuses(dt, adt_node, fdt_node, port);
    if (ret) {
        printf("kboot: Unable to copy ATC fuses for %s - USB3/Thunderbolt will not work\n",
               adt_path);
        goto cleanup;
    }

    if (adt_is_compatible_at(adt, adt_node, "atc-phy,t8130", 0)) {
        tunables = atc_tunables_t8130;
        tunable_count = ARRAY_SIZE(atc_tunables_t8130);
    } else if (adt_is_compatible_at(adt, adt_node, "atc-phy,t8142", 0) ||
               adt_is_compatible_at(adt, adt_node, "atc-phy,t8122", 0) ||
               adt_is_compatible_at(adt, adt_node, "atc-phy,t8132", 0)) {
        tunables = &atc_tunables_t8122[0];
        tunable_count = sizeof(atc_tunables_t8122) / sizeof(*atc_tunables_t8122);
    } else {
        tunables = &atc_tunables[0];
        tunable_count = sizeof(atc_tunables) / sizeof(*atc_tunables);
    }

    for (size_t i = 0; i < tunable_count; ++i) {
        if (lanes_optional && atc_lane_tunable(&tunables[i]) &&
            !adt_getprop(adt, adt_node, tunables[i].adt_name, NULL)) {
            atc_note_absent(&absent, tunables[i].adt_name);
            continue;
        }
        if (chip_id == T8140 && !fdt_node_check_compatible(dt, 0, "apple,j700") &&
            !strcmp(tunables[i].fdt_name, "apple,tunable-axi2af"))
            continue;
        ret = dt_append_atc_tunable(dt, adt_node, fdt_node, &tunables[i]);
        if (ret)
            goto cleanup;
    }
    if (absent.count)
        printf("kboot: %s: %d optional lane tunables absent (%s); common tunables passed\n",
               adt_path, absent.count, absent.list);

    if (adt_is_compatible_at(adt, adt_node, "atc-phy,t8142", 0) ||
        adt_is_compatible_at(adt, adt_node, "atc-phy,t8122", 0) ||
        adt_is_compatible_at(adt, adt_node, "atc-phy,t8132", 0)) {
        static const char *fdt_names[] = {"apple,tunable-lane0-cio", "apple,tunable-lane1-cio"};
        static const u32 lane_offsets[] = {0x9000, 0x10000};
        for (size_t i = 0; i < 2; i++) {
            /* A lane without its CIO tunables gets none */
            if (lanes_optional && !fdt_getprop(dt, fdt_node, fdt_names[i], NULL))
                continue;
            if (fdt_appendprop_u32(dt, fdt_node, fdt_names[i],
                                   lane_offsets[i] + AUSPMA_RX_TOP_PMAFSM_RX_CTRL) < 0)
                goto cleanup;
            if (fdt_appendprop_u32(dt, fdt_node, fdt_names[i],
                                   AUSPMA_RX_TOP_PMAFSM_RX_CTRL_EQ_CDRLOCK_ON_START) < 0)
                goto cleanup;
            if (fdt_appendprop_u32(dt, fdt_node, fdt_names[i], 0) < 0)
                goto cleanup;
        }
    }
    /*
     * For backwards compatibility with downstream drivers copy apple,tunable-common-b to
     * apple,tunable-common.
     * Don't remove this before 2027-01-01.
     */
    int prop_len;
    const void *tunable_common_b_fdt =
        fdt_getprop(dt, fdt_node, "apple,tunable-common-b", &prop_len);
    if (!tunable_common_b_fdt) {
        printf("kboot: Unable to find apple,tunable-common-b for %s\n", adt_path);
        goto cleanup;
    }

    void *tunable_common_b = malloc(prop_len);
    if (!tunable_common_b) {
        printf("kboot: Unable to copy apple,tunable-common-b to apple,tunable-common for %s\n",
               adt_path);
        goto cleanup;
    }
    memcpy(tunable_common_b, tunable_common_b_fdt, prop_len);

    ret = fdt_setprop(dt, fdt_node, "apple,tunable-common", tunable_common_b, prop_len);
    free(tunable_common_b);
    if (ret) {
        printf("kboot: Unable to copy apple,tunable-common-b to apple,tunable-common for %s\n",
               adt_path);
        goto cleanup;
    }

    return;

cleanup:
    /*
     * USB3 and Thunderbolt won't work if something went wrong. Clean up to make
     * sure we don't leave half-filled properties around so that we can at least
     * try to boot with USB2 support only.
     */
    for (size_t i = 0; i < sizeof(atc_tunables) / sizeof(*atc_tunables); ++i)
        fdt_delprop(dt, fdt_node, atc_tunables[i].fdt_name);
    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t8122); i++)
        fdt_delprop(dt, fdt_node, atc_tunables_t8122[i].fdt_name);
    for (size_t i = 0; i < ARRAY_SIZE(atc_tunables_t8130); i++)
        if (chip_id != T8140 || fdt_node_check_compatible(dt, 0, "apple,j700") ||
            strcmp(atc_tunables_t8130[i].fdt_name, "apple,tunable-axi2af"))
            fdt_delprop(dt, fdt_node, atc_tunables_t8130[i].fdt_name);
    fdt_delprop(dt, fdt_node, "apple,tunable-common-a");
    fdt_delprop(dt, fdt_node, "apple,tunable-common");

    printf("FDT: Unable to setup ATC tunables for %s - USB3/Thunderbolt will not work\n", adt_path);
}

/* T6040 uses separate register resources for these tables. */
struct j614s_atc_tunable {
    const char *adt_name, *fdt_name, *resource;
    u32 bias, window, limit;
    bool controller_source, controller_target;
};

static const struct j614s_atc_tunable j614s_atc_tunables[] = {
    {"tunable_ATC0AXI2AF", "apple,tunable-axi2af", "axi2af", 0, 0, 0, false, false},
    {"tunable_ATC_FABRIC", "apple,tunable-fabric", "fabric", 0, 0, 0, false, false},
    {"tunable_ATC_COMMON_CFG", "apple,tunable-common-cfg", "common-cfg", 0, 0, 0, false, false},
    {"tunable_ATC0AXI2AF_LIOA", "apple,tunable-axi2af-lioa", "axi2af-lioa", 0, 0, 0, false, false},
    {"tunable_LN0_RX_CFG_TX_OF_RXCLK", "apple,tunable-lane0", "core", 0xd000, 0, 0, false, false},
    {"tunable_LN1_RX_CFG_TX_OF_RXCLK", "apple,tunable-lane1", "core", 0x1d000, 0, 0, false, false},
    {"tunable_AUS40CMN_SHM", "apple,tunable-aus40cmn-shm", "core", 0xa00, 0, 0, false, false},
    {"tunable_AUSPLL_CORE", "apple,tunable-auspll", "core", 0x3200, 0, 0, false, false},
    {"tunable_CIO4PLL_CORE", "apple,tunable-cio4pll", "core", 0x3a00, 0, 0, false, false},
    {"tunable_USB2PHY_HOST", "apple,tunable-usb2-host", "bank0", 0, 0, 0, false, false},
    {"tunable_PIPE_HANDLER_DEFAULT", "apple,tunable-pipehandler", "pipehandler", 0, 0, 0, true,
     false},
    {"tunable_DRD_USB31_GBL_DEFAULT", "apple,tunable-gbl-default", "dwc3-core", 0, 0xc100, 0x600,
     true, true},
    {"tunable_DRD_USB31_GBL_HOST", "apple,tunable-gbl-host", "dwc3-core", 0, 0xc100, 0x600, true,
     true},
    {"tunable_DRD_USB31_DBG_DEFAULT", "apple,tunable-dbg", "dwc3-apple", 0, 0xb00, 0x800, true,
     true},
    {"tunable_DRD_USB31_CFG_HOST", "apple,tunable-cfg-host", "drd-cfg", 0, 0, 0, true, true},
    {"tunable_LINK_REGS_DEFAULT", "apple,tunable-link", "dwc3-apple", 0, 0x300, 0x800, true, true},
};

static bool j614s_hdmi_ready;
static const char *const j614s_phys[] = {"/phy@382a90000", "/phy@38aa90000",
                                        "/phy@392a90000", "/phy@39aa90000"};

bool kboot_j614s_hdmi_ready(void)
{
    return j614s_hdmi_ready;
}
static const char *const j614s_controllers[] = {"/usb@382280000", "/usb@38a280000",
                                                "/usb@392280000"};

static int j614s_resource_size(void *dt, int node, const char *name, u64 *size)
{
    int parent = fdt_parent_offset(dt, node), len;
    int index = fdt_stringlist_search(dt, node, "reg-names", name);
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2 ||
        index < 0 || !reg || len < 0 || len % 16 || (u32)index >= (u32)len / 16)
        return -1;
    *size = fdt64_ld(reg + 2 * index + 1);
    return *size ? 0 : -1;
}

static int j614s_export_atc(void *dt, int port)
{
    char adt_path[32];
    const char *phy_path = j614s_phys[port];
    const char *usb_path = port < 3 ? j614s_controllers[port] : NULL;
    int phy, usb = -1, source, usb_source = -1;

    phy = fdt_path_offset(dt, phy_path);
    snprintf(adt_path, sizeof(adt_path), "/arm-io/atc-phy%d", port);
    source = adt_path_offset(adt, adt_path);
    if (phy < 0 || source < 0 || fdt_node_check_compatible(dt, phy, "apple,t6040-atcphy"))
        return -1;
    if (port == 3) {
        int len;
        const char *status = fdt_getprop(dt, phy, "status", &len);
        if (!status || len != sizeof("disabled") || memcmp(status, "disabled", len))
            return -1;
    } else {
        usb = fdt_path_offset(dt, usb_path);
        snprintf(adt_path, sizeof(adt_path), "/arm-io/usb-drd%d", port);
        usb_source = adt_path_offset(adt, adt_path);
        if (usb < 0 || usb_source < 0 || fdt_node_check_compatible(dt, usb, "apple,t6040-dwc3"))
            return -1;
    }
    size_t count = port == 3 ? 9 : ARRAY_SIZE(j614s_atc_tunables);
    for (size_t i = 0; i < count; i++) {
        const struct j614s_atc_tunable *item = &j614s_atc_tunables[i];
        const char *target_path = item->controller_target ? usb_path : phy_path;
        int target = fdt_path_offset(dt, target_path);
        int src = item->controller_source ? usb_source : source;
        u64 size;
        u32 len;
        if (target < 0 || j614s_resource_size(dt, target, item->resource, &size) ||
            item->window > size || (item->limit && item->limit > size - item->window) ||
            item->bias >= (item->limit ? item->limit : size - item->window) ||
            !adt_getprop(adt, src, item->adt_name, &len) || !len)
            return -1;
        struct adt_tunable_info info = {
            item->adt_name, item->fdt_name, item->bias,
            (item->limit ? item->limit : size - item->window) - item->bias, true};
        int ret = fdt_delprop(dt, target, item->fdt_name);
        if ((ret && ret != -FDT_ERR_NOTFOUND) || dt_append_atc_tunable(dt, src, target, &info))
            return -1;
    }
    if (port == 3)
        return 0; /* The PMP-aware display gate owns PHY power. */
    phy = fdt_path_offset(dt, phy_path);
    if (phy < 0 || fdt_setprop_string(dt, phy, "status", "okay"))
        return -1;
    usb = fdt_path_offset(dt, usb_path);
    return usb < 0 ? -1 : fdt_setprop_string(dt, usb, "status", "okay");
}

int kboot_setup_atc(void *dt)
{
    char adt_path[32];
    char fdt_alias[32];

    if (chip_id == T6040 && board_id == 4 && adt_is_compatible(adt, 0, "J614sAP") &&
        !fdt_node_check_compatible(dt, 0, "apple,j614s")) {
        int capacity = fdt_totalsize(dt);
        void *scratch = malloc(capacity);
        j614s_hdmi_ready = false;
        for (size_t i = 0; i < ARRAY_SIZE(j614s_phys); i++) {
            int ret = scratch ? fdt_open_into(dt, scratch, capacity) : -1;
            if (!ret)
                ret = j614s_export_atc(scratch, i);
            if (!ret) {
                memcpy(dt, scratch, capacity);
                if (i == 3)
                    j614s_hdmi_ready = true;
            } else {
                printf("FDT: J614s ATC%zu tuning unavailable\n", i);
                int node = fdt_path_offset(dt, j614s_phys[i]);
                if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled")) {
                    free(scratch);
                    return -1;
                }
                node = i < 3 ? fdt_path_offset(dt, j614s_controllers[i]) : -1;
                if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled")) {
                    free(scratch);
                    return -1;
                }
            }
        }
        free(scratch);
        return 0;
    }
    for (int i = 0; i < MAX_ATC_DEVS; ++i) {
        memset(adt_path, 0, sizeof(adt_path));
        snprintf(adt_path, sizeof(adt_path), "/arm-io/atc-phy%d", i);

        memset(fdt_alias, 0, sizeof(adt_path));
        snprintf(fdt_alias, sizeof(fdt_alias), "atcphy%d", i);

        dt_copy_atc_tunables(dt, adt_path, fdt_alias, i);
    }

    return 0;
}
