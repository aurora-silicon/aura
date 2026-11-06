/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USB_CDC_ATC_H
#define USB_CDC_ATC_H

#include "types.h"

int usb_cdc_atc_power_on(uintptr_t pipehandler);
int usb_cdc_atc_switch_pipe(void);
void usb_cdc_atc_force_swapped(bool enable);
bool usb_cdc_atc_lane(void);

#endif
