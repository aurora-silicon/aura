/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
/* Disarm even with m1n1.wdt=keep: a host asked for it */
void wdt_disable_forced(void);
void wdt_reboot(void);
/* Reset the count of a watchdog left armed by m1n1.wdt=keep; nothing otherwise */
void wdt_kick(void);
/* wdt_disable() found m1n1.wdt=keep and left the watchdog armed */
bool wdt_left_armed(void);
int wdt_arm_seconds(unsigned seconds);
/* The longest the 24 MHz counter allows, for each stage of a trial boot */
#define WDT_TRIAL_SECONDS 178
int wdt_arm_trial(void);
/* Stage 1 on a plain boot: the same longest period, until a host attaches or Stage 2 disarms it */
#define WDT_BOOT_SECONDS 178
int wdt_arm_boot(void);
void wdt_host_attached(void);
bool wdt_boot_watchdog_armed(void);

#endif
