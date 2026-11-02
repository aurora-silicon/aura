// SPDX-License-Identifier: GPL-2.0-only
// omarchy-under-SPTM monitor, entered from the arm_init hook. One draw per boot: a ladder prefix with fall-through to
// XNU (ladder.c), or native: stage-2, guest stub (screen text from the log, ACTLR test), Linux. The log carries what
// was never read on this machine: slide, boot_args, PAPT, frames, stage-2 status.
#include "mon.h"

extern char __bss_start[], __bss_end[];
u64 g_vbase, g_vstride, g_vw, g_vh;
u64 *g_boot_args;

static void kv(const char *k, u64 v)
{
	con_puts(k);
	con_hex(v);
	con_puts("\n");
}

/* The loader's abort guard prints ESR, ELR, FAR on the panel; here the same line replaces a silent stop. */
/* vec: 0, 4 synchronous; 1, 5 IRQ and 2, 6 FIQ, ESR and FAR stale; 3, 7 SError, ESR valid (EC 0x2f), FAR stale. 0-3 on SP_EL0, 4-7 on SP_EL2 */
void mon_host_fault(u64 esr, u64 elr, u64 far, u64 spsr, u64 vec)
{
	static volatile u32 once;
	if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
		con_fault_unlock();
		con_puts("mon: EL2 FAULT esr=");
		con_hex(esr);
		con_puts(" elr=");
		con_hex(elr);
		con_puts(" far=");
		con_hex(far);
		con_puts(" spsr=");
		con_hex(spsr);
		con_puts(" vec=");
		con_hex(vec);
		con_puts(" mpidr=");
		con_hex(sysreg_read(mpidr_el1));
		con_puts("\n");
	}
	for (;;)
		__asm__ volatile("wfi");
}

/* start.S mon_sptm_panic. SPTM's violation text instead of XNU's panic() on a kernel that never initialised. The per-CPU buffer is
 * what XNU's panic() would read in this same state; if it does not read, mon_host_fault prints the FAR. No SPTM calls from here. */
u8 g_panic_stack[MAX_SLOTS][2048] __attribute__((aligned(16)));
void mon_sptm_panic_c(const volatile char *s)
{
	static volatile u32 once;
	if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
		con_fault_unlock();
		con_puts("mon: SPTM PANIC mpidr=");
		con_hex(sysreg_read(mpidr_el1));
		con_puts(" ");
		for (int i = 0; i < 600 && s[i]; i++)
			con_putc(s[i] >= 32 && s[i] < 127 ? s[i] : '.');
		con_puts("\n");
		fb_band(15, RED);
	}
	for (;;)
		__asm__ volatile("wfi");
}

void mon_main(u64 *boot_args, u64 *bootstrap)
{
	int draw = ladder_draw(boot_args, bootstrap);
	if (!draw)
		return;
	for (char *p = __bss_start; p < __bss_end; p++)
		*(volatile char *)p = 0;
	{ extern u64 host_active; host_active = 0x4e4956454e455447UL; __asm__ volatile("dsb sy; isb"); }
	{
		u64 b = boot_args[5], s = boot_args[7], w = boot_args[8], h = boot_args[9];
		g_vbase = ((b >> 40) == 1) ? b : 0x10bd1474000UL;
		g_vstride = (s && s < 0x10000) ? s : 3456UL * 4;
		g_vw = (w && w < 0x2000) ? w : 3456UL;
		g_vh = (h && h < 0x2000) ? h : 2234UL;
	}
	g_boot_args = boot_args;
	mm_init(bootstrap);
	g_ba_top = boot_args[2] + boot_args[3];
	if (draw == 1)
		fb_init(boot_args);
	fb_band(0, BLUE);
	con_init(boot_args);
	con_replay();
	con_puts(draw == 1 ? "mon: native-f2 draw 7, host fb\n" : "mon: native-f2 draw 9, guest stub screen\n");
	/* XNU arm_init 0xb91231c: ep46 closes SPTM's serial and sets stage bit 23, without which sptm_panic on SPTM's SP1 stack ends in a
	 * silent wfe instead of the reflect to _start. Before LOCKDOWN: ep46 panics after bit 17. No endpoint needs the bit. */
	sptm_call(46, 0, 0, 0, 0, 0, 0);
	kv("mon: host fb ok/fail ", fb_ok | (fb_fail << 4));
	u64 pc;
	__asm__ volatile("adr %0, ." : "=r"(pc));
	kv("mon: pc ", pc);
	kv("mon: CurrentEL ", sysreg_read(CurrentEL));
	kv("mon: MPIDR ", sysreg_read(mpidr_el1));
	kv("mon: ACTLR ", sysreg_read(actlr_el1));
	kv("mon: TTBR1 ", sysreg_read(ttbr1_el1));
	kv("mon: args+0x1d8 ", bootstrap[0x1d8 / 8]);
	kv("mon: first_avail ", bootstrap[3]);
	kv("mon: boot_args physBase ", boot_args[2]);
	kv("mon: boot_args memSize ", boot_args[3]);
	kv("mon: video base ", boot_args[5]);
	kv("mon: video rowBytes ", boot_args[7]);
	kv("mon: video width ", boot_args[8]);
	kv("mon: video height ", boot_args[9]);
	kv("mon: video depth ", boot_args[10]);
	u64 n = *(const u64 *)bootstrap[(0x1a0 + 8) / 8];
	const u64 *e = (const u64 *)bootstrap[(0x1a0 + 0x10) / 8];
	kv("mon: PAPT ranges ", n);
	for (u64 i = 0; i < n && i < 24; i++, e += 3) {
		con_hex(e[0]);
		con_puts(" ");
		con_hex(e[1]);
		con_puts(" ");
		con_hex(e[2]);
		con_puts("\n");
	}
	s2_init();
	fb_band(2, BLUE);
#ifdef P_IOMMU
	if (adt_init(boot_args))
		con_puts("mon: ADT not readable, DART devices stay off\n");
	else
		iommu_setup();
#endif
	u64 entry, dtb;
#ifdef P_GPU
	{ extern void gpu_adt_log(const u64 *); gpu_adt_log(boot_args); }
#endif
#ifdef UAT_SELFTEST
	{ extern void uat_selftest(void); uat_selftest(); }
#endif
	if (linux_load(&entry, &dtb) == 0) {
		fb_band(3, BLUE);
		fb_band(4, BLUE);
		guest_run(entry, dtb, entry + PAGE);
		con_puts("mon: back in host, halt\n");
		fb_band(15, RED);
	} else {
		con_puts("mon: Linux payload not loaded\n");
	}
	for (;;)                                 /* the iBoot WDT resets us */
		__asm__ volatile("wfi");
}
