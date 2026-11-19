/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef USB_H
#define USB_H

#include "iodev.h"
#include "types.h"
#include "usb_dwc3.h"

dwc3_dev_t *usb_bringup(u32 idx);
/* Bring one port's PHY and dwc3 back up and hand it to the iodev layer (S2R wake). */
int usb_phy_bringup(u32 idx);
dwc3_dev_t *usb_iodev_bringup(u32 idx);
extern struct iodev_ops iodev_usb_ops;

void usb_init(void);
void usb_hpm_restore_irqs(bool force);
void usb_iodev_init(void);
bool usb_iodev_shutdown(void);
bool usb_iodev_shutdown_except(iodev_id_t keep);
void usb_iodev_vuart_setup(iodev_id_t iodev);
int usb_cdc_link_start(void (*set_step)(unsigned step), bool force_swapped);
bool usb_cdc_deferred_usb2(void);

dwc3_dev_t *usb_t8152_init(void (*set_step)(unsigned step));

#endif
