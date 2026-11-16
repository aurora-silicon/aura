/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef J873_H
#define J873_H

int j873_usb_prep(void);
int j804_usb_prep(void);
void j873_usb_wake_hpms(void);
int j873_pcie_prep(void);
int j873_n1_power(void);
int j804_sdreader_power(void);

#endif
