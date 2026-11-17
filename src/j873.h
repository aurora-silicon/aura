/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef J873_H
#define J873_H

#include "types.h"

int j873_usb_prep(void);
int j804_usb_prep(void);
void j873_usb_wake_hpms(void);

/* D4: J804 usb-drd1/usb-drd3 register bases and host-mode state, read only (j873.c). */
struct j804_usb_port {
    u64 dwc, evt, dart0, dart1;
    const char *name;
    bool up;
};
int j804_usb_port_state(int port, struct j804_usb_port *out);
int j873_pcie_prep(void);
int j873_n1_power(void);
int j804_sdreader_power(void);

#endif
