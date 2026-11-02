// SPDX-License-Identifier: GPL-2.0-only
// F26 went silent in tso-matrix, the first step that moves tasks between the cores it brought up. One call to every online core
// without waiting: a core that does not answer is named here instead of stopping the machine somewhere later.
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#include <asm/sysreg.h>

static atomic_t seen[NR_CPUS];
static u64 v[NR_CPUS][10];
static call_single_data_t csd[NR_CPUS];

/* in the FIQ on the target: ACTLR and which TSO bits stay on this core, the EL2 side from HVC 0x7503 */
static void hit(void *p)
{
	int me = smp_processor_id();
	u64 *o = v[me], a = read_sysreg(actlr_el1);
	register u64 x0 asm("x0") = 0x7503;
	register u64 x1 asm("x1"), x2 asm("x2"), x3 asm("x3"), x4 asm("x4");

	o[0] = a;
	write_sysreg(a ^ BIT(1), actlr_el1);
	o[1] = read_sysreg(actlr_el1);
	write_sysreg(a ^ BIT(4), actlr_el1);
	o[2] = read_sysreg(actlr_el1);
	write_sysreg(a ^ (BIT(1) | BIT(4)), actlr_el1);
	o[3] = read_sysreg(actlr_el1);
	write_sysreg(a, actlr_el1);
	isb();
	asm volatile("hvc #0" : "+r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4) : : "x5", "x6", "memory");
	o[4] = x0;	/* VM_TMR_FIQ_ENA */
	o[5] = x1;	/* HCR_EL2 */
	o[6] = x2;	/* HACR_EL2 */
	o[7] = x3;	/* CNTHCTL_EL2 */
	o[8] = x4;	/* CNTVOFF_EL2 */
	o[9] = read_cpuid_mpidr();
	atomic_set(&seen[me], 1);
}

static int __init macho_ipi_init(void)
{
	int c, i;

	for_each_online_cpu(c) {
		ktime_t t = ktime_get();

		if (c == raw_smp_processor_id())
			continue;
		/* own csd per target: smp_call_function_single(.., 0) reuses this CPU's csd_data, so the call after a core that did not answer would spin in csd_lock for ever (kernel/smp.c:729) */
		INIT_CSD(&csd[c], hit, NULL);
		smp_call_function_single_async(c, &csd[c]);
		for (i = 0; i < 100000 && !atomic_read(&seen[c]); i++)
			udelay(1);
		if (atomic_read(&seen[c]))
			pr_info("IPI: cpu%d mpidr %llx ok %lld us actlr %llx ^2 %llx ^10 %llx ^12 %llx el2 vtmr %llx hcr %llx hacr %llx cnthctl %llx cntvoff %llx\n",
				c, v[c][9], ktime_us_delta(ktime_get(), t), v[c][0], v[c][1], v[c][2], v[c][3],
				v[c][4], v[c][5], v[c][6], v[c][7], v[c][8]);
		else
			pr_info("IPI: cpu%d NO ANSWER in 100 ms\n", c);
	}
	return 0;
}
module_init(macho_ipi_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("macho call to each online core");
