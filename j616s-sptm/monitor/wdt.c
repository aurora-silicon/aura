// SPDX-License-Identifier: GPL-2.0-only
#include "mon.h"
#define WDT_PA 0x50836c000UL                 /* ADT /arm-io/wdt; Linux apple_wdt uses it to reboot this Mac */
static u64 wdt;
int wdt_arm(u32 seconds)
{
#ifndef NO_WDT_STORE
	u64 w = hmap(WDT_PA, 1);
	if (!w)
		return 0;
	wdt = w;
	w32(w + 0x10, 0);
	w32(w + 0x14, seconds * 24000000U);
	w32(w + 0x1c, 4);
	dsb();
	(void)r32(w + 0x10);
	return 1;
#else
	(void)seconds;
	return 0;
#endif
}

/* progress code: restart the count with a new timeout; the interval between two WDT resets
 * (panicmedic-timestamps in NVRAM) then names the last stage reached, screen or not. v7b/v8 measured
 * the iBoot share twice (127.846 / 127.842 s), so millisecond offsets inside a stage are readable. */
static u32 stage_ms, sub_n;

static void wdt_ms(u32 ms)
{
	if (!wdt)
		return;
	w32(wdt + 0x10, 0);
	w32(wdt + 0x14, ms * 24000U);
	dsb();
}

void wdt_stage(u32 seconds)
{
	stage_ms = seconds * 1000;
	sub_n = 0;
	wdt_ms(stage_ms);
}

void wdt_sub(u32 what)
{
	(void)what;
	wdt_ms(stage_ms + WDT_SUB_CALL0 + (sub_n < 2000 ? sub_n : 2000) * 6 / 5);
	sub_n++;
}

/* the run finished and its result is on screen: stop the backup reboot so it stays there */
void wdt_disarm(void)
{
	if (!wdt)
		return;
	w32(wdt + 0x1c, 0);
	w32(wdt + 0x10, 0);
	dsb();
}
