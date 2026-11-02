// SPDX-License-Identifier: GPL-2.0-only
// iBoot leaves two boot watchdogs: WD1 and the deadline in the AON scratch 0x508330008, which holds now + 120 s (0x7d on this Mac;
// XNU's watchdog driver clears it). apple_wdt knows only WD1, so every boot reset at ~128 s. Same stores
// and read-back as m4-loader watchdog.c, which is what keeps the raw boot alive. Every 20 s the values again for the screen.
#include <linux/module.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <asm/sysreg.h>

#define WDT_PA 0x50836c000UL
#define SCRATCH_PA 0x508330000UL
static void __iomem *r, *s2;
static struct delayed_work w;

static void show(const char *when)
{
	pr_info("WDT %s: WD1 %x/%x c%x scratch %x cntpct %llx\n", when,
		readl(r + 0x10), readl(r + 0x14), readl(r + 0x1c), readl(s2 + 8), read_sysreg(cntpct_el0));
}

static void tick(struct work_struct *t)
{
	show("tick");
	schedule_delayed_work(&w, 20 * HZ);
}

static int __init macho_wdt_init(void)
{
	r = ioremap(WDT_PA, 0x4000);
	s2 = ioremap(SCRATCH_PA, 0x4000);
	if (!r || !s2)
		return -ENOMEM;
	show("boot");
	writel(0, r + 0x1c);
	dsb(sy);
	writel(0, s2 + 8);
	dsb(sy);
	show("off");
	INIT_DELAYED_WORK(&w, tick);
	schedule_delayed_work(&w, 20 * HZ);
	return 0;
}
module_init(macho_wdt_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("macho boot watchdogs off");
