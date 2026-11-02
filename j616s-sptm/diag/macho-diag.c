// SPDX-License-Identifier: GPL-2.0-only
// One-shot timer state dump for the macho guest: EL1 view plus EL2 values from monitor hvc 0x7503. Unloads itself.
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/sysreg.h>

static int __init macho_diag_init(void)
{
	register unsigned long x0 asm("x0") = 0x7503;
	register unsigned long x1 asm("x1") = 0, x2 asm("x2") = 0, x3 asm("x3") = 0;
	register unsigned long x4 asm("x4") = 0, x5 asm("x5") = 0, x6 asm("x6") = 0;
	unsigned long daif, v[7];

	asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6));
	v[0] = x0; v[1] = x1; v[2] = x2; v[3] = x3; v[4] = x4; v[5] = x5; v[6] = x6;    /* printk clobbers x0-x6 */
	asm volatile("mrs %0, daif" : "=r"(daif));
	pr_info("MDIAG EL1 frq %lx vct %lx v_ctl %lx v_cval %lx p_ctl %lx isr %lx daif %lx\n",
		(unsigned long)read_sysreg(cntfrq_el0), (unsigned long)read_sysreg(cntvct_el0),
		(unsigned long)read_sysreg(cntv_ctl_el0), (unsigned long)read_sysreg(cntv_cval_el0),
		(unsigned long)read_sysreg(cntp_ctl_el0), (unsigned long)read_sysreg(isr_el1), daif);
	pr_info("MDIAG EL2 vmtmr %lx hcr %lx hacr %lx cnthctl %lx cntvoff %lx v_ctl02 %lx p_ctl02 %lx\n",
		v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
	return -ECANCELED;
}
module_init(macho_diag_init);
MODULE_LICENSE("GPL");
