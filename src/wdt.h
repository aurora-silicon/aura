/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
/* At start: wdt_disable(), unless m1n1.wdt=keep asks to leave an armed watchdog as found */
void wdt_disable_at_start(void);
/* wdt_disable_at_start() left the watchdog armed, and nothing has disarmed or re-armed it since */
bool wdt_left_armed(void);
void wdt_reboot(void);
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
