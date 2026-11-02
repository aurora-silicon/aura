// SPDX-License-Identifier: GPL-2.0-only
// One draw per boot from CNTPCT. Non-native draws run a prefix of the monitor's SPTM path on frames reserved by the
// first_avail bump, then fall through to XNU; the reset period names the outcome (classes 10 s apart: XNU lives
// ~13-18 s after fall-through; k0 calibrates this image).
//   k0 control                                             spin 10
//   k3 PAPT r/w + RETYPE root(33) + L2(34) + MAP_TABLE L1  spin 20
//   k6 k3 + L3 + MAP_PAGE WB + fb NC                       spin 30
//   kL host ACTLR |= 0x12 as XNU (0xb74a53c): stuck spin 40, not stuck spin 50
//   failure up to k3 spin 60, failure in k4-k6 spin 70
//   native: return 1 with the host fb console (hmap_fb), 2 with the guest stub as the only screen
// Since F3 every boot is draw 7, the host fb console that shows Linux from its first line (HW, F2); draw 9 and the k
// draws stay reachable through FORCE_K.
#include "mon.h"

#define S2_WB ((3UL << 8) | (0xfUL << 2))
#define S2_NC ((2UL << 8) | (0x5UL << 2))
#define L3SPAN (2048 * PAGE)
#define B_PAPT_N ((0x1a0 + 0x08) / 8)
#define B_PAPT ((0x1a0 + 0x10) / 8)

static void spin_s(u64 s)
{
	u64 t0 = sysreg_read(cntpct_el0), hz = sysreg_read(cntfrq_el0);
	while (sysreg_read(cntpct_el0) - t0 < s * (hz ? hz : 1000000000UL))
		isb();
}

static u64 s2pte(u64 pa, u64 attr)
{
	return pa | 3 | (1UL << 10) | (3UL << 6) | attr;
}

#define st(r) do { if ((r) > 1) bad = 1; } while (0)

static void stage2_prefix(u64 *boot_args, u64 *args, int k)
{
	/* frames: 0 scratch, 1 root, 2 L2, 3 L3 guest, 4.. L3 fb */
	u64 f = (args[3] + PAGE - 1) & ~(PAGE - 1);
	args[3] = f + 16 * PAGE;
	dsb();
	u64 code = f, root = f + PAGE, l2 = f + 2 * PAGE, l3 = f + 3 * PAGE, nf = f + 4 * PAGE, bad = 0;
	const u64 *e = (const u64 *)args[B_PAPT];
	u64 n = *(const u64 *)args[B_PAPT_N], cv = 0;
	for (u64 i = 0; i < n; i++, e += 3)
		if (code >= e[0] && code < e[0] + e[2] * PAGE)
			cv = e[1] + (code - e[0]);
	if (!cv) {
		spin_s(60);
		return;
	}
	volatile u64 *p = (volatile u64 *)cv;
	p[0] = 0x5a5a5a5a12345678UL;
	if (p[0] != 0x5a5a5a5a12345678UL) {
		spin_s(60);
		return;
	}
	sptm_call_raw(SPTM_RETYPE, root, XNU_DEFAULT, XNU_STAGE2_ROOT_TABLE, 0x100000003UL, 0, 0);
	sptm_call_raw(SPTM_RETYPE, l2, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 2, 0, 0);
	st(sptm_call_raw(SPTM_MAP_TABLE, root, code & ~((1UL << 36) - 1), 1, l2 | 3, 0, 0));
	if (bad) {
		spin_s(60);
		return;
	}
	if (k == 3) {
		spin_s(20);
		return;
	}
	sptm_call_raw(SPTM_RETYPE, l3, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 3, 0, 0);
	st(sptm_call_raw(SPTM_MAP_TABLE, root, code & ~(L3SPAN - 1), 2, l3 | 3, 0, 0));
	st(sptm_call_raw(SPTM_MAP_PAGE, root, code, s2pte(code, S2_WB), 0, 0, 0));
	u64 fb = boot_args[5], size = boot_args[7] * boot_args[9], win = 0;
	if ((fb >> 40) != 1 || !size || size > 0x4000000) {
		fb = 0x10bd1474000UL;
		size = 0x1d73c00;
	}
	for (u64 a = fb & ~(PAGE - 1); !bad && a < fb + size; a += PAGE) {
		if ((a & ~(L3SPAN - 1)) != win) {
			win = a & ~(L3SPAN - 1);
			sptm_call_raw(SPTM_RETYPE, nf, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 3, 0, 0);
			st(sptm_call_raw(SPTM_MAP_TABLE, root, win, 2, nf | 3, 0, 0));
			nf += PAGE;
		}
		st(sptm_call_raw(SPTM_MAP_PAGE, root, a, s2pte(a, S2_NC), 0, 0, 0));
	}
	spin_s(bad ? 70 : 30);
}

/* returns 0 for fall-through, 1 or 2 when this boot goes native */
int ladder_draw(u64 *boot_args, u64 *args)
{
#ifdef FORCE_K
	u64 k = FORCE_K;
#else
	u64 k = 7;
#endif
	if (k == 7)
		return 1;
	if (k == 9)
		return 2;
	if (k == 0) {
		spin_s(10);
	} else if (k == 8) {
		/* exactly XNU's enable (its ACTLR sequence), without the per-thread IMPDEF companions */
		u64 o = sysreg_read(actlr_el1), r;
		dsb(); sysreg_write(actlr_el1, o | 0x12); isb();
		r = sysreg_read(actlr_el1);
		dsb(); sysreg_write(actlr_el1, o); isb();
		spin_s((r & 0x12) == 0x12 ? 40 : 50);
	} else {
		stage2_prefix(boot_args, args, k);
	}
	return 0;
}
