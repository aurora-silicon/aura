/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ATC USB2 PHY, PIPE handler and event-counter registers used by the USB
 * device paths. Names and fields follow Linux drivers/phy/apple/atc.c.
 */
#ifndef USB_ATC_REGS_H
#define USB_ATC_REGS_H

#include "types.h"

/* USB2 PHY regs */
#define USB2PHY_USBCTL           0x00
#define USB2PHY_USBCTL_USB_MODE  0x7
#define USB2PHY_USBCTL_RUN       2
#define USB2PHY_USBCTL_ISOLATION 4

#define USB2PHY_CTL             0x04
#define USB2PHY_CTL_RESET       BIT(0)
#define USB2PHY_CTL_PORT_RESET  BIT(1)
#define USB2PHY_CTL_APB_RESET_N BIT(2)
#define USB2PHY_CTL_SIDDQ       BIT(3)
#define USB2PHY_CTL_MASK                                                                           \
    (USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET | USB2PHY_CTL_APB_RESET_N | USB2PHY_CTL_SIDDQ)

#define USB2PHY_SIG                      0x08
#define USB2PHY_SIG_VBUSDET_FORCE_VAL    BIT(0)
#define USB2PHY_SIG_VBUSDET_FORCE_EN     BIT(1)
#define USB2PHY_SIG_VBUSVLDEXT_FORCE_VAL BIT(2)
#define USB2PHY_SIG_VBUSVLDEXT_FORCE_EN  BIT(3)
#define USB2PHY_SIG_VBUS_FORCE                                                                     \
    (USB2PHY_SIG_VBUSDET_FORCE_VAL | USB2PHY_SIG_VBUSDET_FORCE_EN |                                \
     USB2PHY_SIG_VBUSVLDEXT_FORCE_VAL | USB2PHY_SIG_VBUSVLDEXT_FORCE_EN)
#define USB2PHY_SIG_HOST (7 << 12)

#define USB2PHY_MISCTUNE                 0x1c
#define USB2PHY_MISCTUNE_APBCLK_GATE_OFF BIT(29)
#define USB2PHY_MISCTUNE_REFCLK_GATE_OFF BIT(30)

/* USB2 event counter block */
#define ATCPHY_EVT_USB2_CTL          0x00
#define ATCPHY_EVT_USB2_CTL_EVT_EN   BIT(0)
#define ATCPHY_EVT_USB2_CTL_LOAD_CNT BIT(3)

#define PIPEHANDLER_OVERRIDE          0x00
#define PIPEHANDLER_OVERRIDE_RXVALID  BIT(0)
#define PIPEHANDLER_OVERRIDE_RXDETECT BIT(2)

/*
 * MUX_CTRL selects the PIPE clock and data source. The fields moved between
 * generations: T8103 layout CLK [5:3] DATA [2:0], T8142 layout CLK [6:4]
 * DATA [1:0].
 */
#define PIPEHANDLER_MUX_CTRL             0x0c
#define PIPEHANDLER_MUX_CTRL_USB3        0x08
#define PIPEHANDLER_MUX_CTRL_USB4_TUNNEL 0x11
#define PIPEHANDLER_MUX_CTRL_DUMMY       0x22
#define PIPEHANDLER_MUX_CTRL_CLK_T8103   GENMASK(5, 3)
#define PIPEHANDLER_MUX_CTRL_DATA_T8103  GENMASK(2, 0)
#define PIPEHANDLER_MUX_CTRL_CLK_T8142   GENMASK(6, 4)
#define PIPEHANDLER_MUX_CTRL_DATA_T8142  GENMASK(1, 0)
#define PIPEHANDLER_MUX_CTRL_CLK_OFF     0
#define PIPEHANDLER_MUX_CTRL_CLK_USB3    1
#define PIPEHANDLER_MUX_CTRL_CLK_USB4    2
#define PIPEHANDLER_MUX_CTRL_CLK_DUMMY   4
#define PIPEHANDLER_MUX_CTRL_DATA_USB3   0
#define PIPEHANDLER_MUX_CTRL_DATA_USB4   1
#define PIPEHANDLER_MUX_CTRL_DATA_DUMMY  2
#define PIPEHANDLER_MUX_CTRL_T8142_MASK                                                            \
    (PIPEHANDLER_MUX_CTRL_CLK_T8142 | PIPEHANDLER_MUX_CTRL_DATA_T8142)
#define PIPEHANDLER_MUX_T8103(clk, data)                                                           \
    (FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK_T8103, PIPEHANDLER_MUX_CTRL_CLK_##clk) |                  \
     FIELD_PREP(PIPEHANDLER_MUX_CTRL_DATA_T8103, PIPEHANDLER_MUX_CTRL_DATA_##data))
#define PIPEHANDLER_MUX_T8142(clk, data)                                                           \
    (FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK_T8142, PIPEHANDLER_MUX_CTRL_CLK_##clk) |                  \
     FIELD_PREP(PIPEHANDLER_MUX_CTRL_DATA_T8142, PIPEHANDLER_MUX_CTRL_DATA_##data))

#define PIPEHANDLER_LOCK_REQ 0x10
#define PIPEHANDLER_LOCK_ACK 0x14
#define PIPEHANDLER_LOCK_EN  BIT(0)

#define PIPEHANDLER_AON_GEN                     0x1C
#define PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN BIT(4)
#define PIPEHANDLER_AON_GEN_DWC3_RESET_N        BIT(0)

#define PIPEHANDLER_NONSELECTED_OVERRIDE 0x20
#define PIPEHANDLER_NATIVE_RESET         BIT(12)
#define PIPEHANDLER_DUMMY_PHY_EN         BIT(15)
#define PIPEHANDLER_NATIVE_POWER_DOWN    GENMASK(3, 0)

#endif
