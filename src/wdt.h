/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
void wdt_reboot(void);
int wdt_arm_seconds(unsigned seconds);
bool wdt_simple_reconfig_reg(u64 *primary_out, u64 *reconfig_out);

/* wdt_s2r_guard_enter(): the primary watchdog as read, and whether it was borrowed */
struct wdt_guard {
    bool borrowed;
    u64 base;
    u32 ctl, count, alarm;
};
bool wdt_s2r_guard_supported(void);
int wdt_s2r_guard_enter(struct wdt_guard *guard, unsigned seconds);
void wdt_s2r_guard_exit(const struct wdt_guard *guard);
/* The longest the 24 MHz counter allows, for each stage of a trial boot */
#define WDT_TRIAL_SECONDS 178
int wdt_arm_trial(void);
/* A plain boot: the same longest period, until a host attaches or Stage 2 hands off */
#define WDT_BOOT_SECONDS 178
int wdt_arm_boot(void);
void wdt_disarm_boot(void);
void wdt_host_attached(void);
bool wdt_boot_watchdog_armed(void);
bool wdt_host_was_attached(void);

#endif
