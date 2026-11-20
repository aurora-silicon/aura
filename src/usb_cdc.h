/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USB_CDC_H
#define USB_CDC_H

#include "types.h"

int usb_cdc_schedule(u32 delay_ms, u32 reserved, u32 flags);
u32 usb_cdc_status(void);
void usb_cdc_poll(void);
int usb_cdc_arm_watchdog(void);
bool usb_cdc_ready(void);
bool usb_cdc_failed(void);
bool usb_cdc_owns_carrier(void);
int usb_cdc_cancel(void);
void usb_cdc_cleanup(void);
void usb_cdc_primary_opened(void);
void usb_cdc_link_failed(void);
/* Bring-up waits: a plain delay and poll, except when the link is taken again on a running OS
 * (sptm_virt_s2r.c), where they give the core back to it. */
void usb_cdc_wait_us(u32 us);
int usb_cdc_poll32(u64 addr, u32 mask, u32 target, u32 timeout_us);

#endif
