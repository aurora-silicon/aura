/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef DEBUGPORT_H
#define DEBUGPORT_H

/* Mac mini only: warn on the boot console when a USB host (the debug cable)
 * is plugged into a USB-C port other than the debug (DFU) port.
 * debugport_powerup() puts the ports in S0; call it early, then
 * debugport_check() once the ports had time to see their partners. */
void debugport_powerup(void);
void debugport_check(void);

#endif
