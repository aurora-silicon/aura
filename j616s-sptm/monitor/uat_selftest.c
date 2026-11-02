// SPDX-License-Identifier: GPL-2.0-only
// uat_selftest.c: drive SPTM table 7 from the monitor, in the order mmu/sptm.rs uses it, under the
// real sptm in qemu. Each step prints before the call: the last line names a terminal violation.
#include "mon.h"
#define T7(ep) ((7UL << 32) | (ep))
#define IOMMU_UAT 3UL
static u64 step(const char *what, u64 sel, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4)
{
	con_puts("uat: ");
	con_puts(what);
	con_puts(" sel ");
	con_hex(sel);
	con_puts(" a0 ");
	con_hex(a0);
	con_puts("\n");
	u64 r = sptm_call(sel, a0, a1, a2, a3, a4, 0);
	con_puts("uat:   = ");
	con_hex(r);
	con_puts("\n");
	return r;
}
static u64 uat_frame(u64 subtype)
{
	u64 pa = frame_alloc();
	step("retype", SPTM_RETYPE, pa, XNU_DEFAULT, 24, IOMMU_UAT | (subtype << 32), 0);
	return pa;
}
void uat_selftest(void)
{
	static const char names[][16] = { "mode", "shared-global", "global", "va-top", "va-mask",
					     "seg-limit", "i6", "i7", "state-size", "l2-va" };
	for (u64 i = 0; i < 10; i++) {
		if (i == 1)
			continue;              /* panics in private mode (0x400001b) */
		step(names[i], T7(12), i, 0, 0, 0, 0);
	}
	u64 global = sptm_call_raw(T7(12), 2, 0, 0, 0, 0, 0);

	/* A user Vm: dual root state, as State::new */
	u64 st = uat_frame(0), r0 = uat_frame(1), r1 = uat_frame(1);
	step("init_state", T7(0), st, r0, r1, 0, 0);
	u64 va = 0x20000000UL;
	u64 l2 = uat_frame(4), l3 = uat_frame(5);
	step("map_table l1", T7(2), st, va, 1, l2, 0);
	step("map_table l2", T7(2), st, va, 2, l3, 0);

	/* The segment page: XNU_DEFAULT, as a guest page is. Stays outside SPTM's type changes. */
	u64 seg = frame_alloc(), segva = hmap(seg, 0);
	u64 leaf = frame_alloc();
	w64(segva + 0, leaf);
	w64(segva + 8, 1);
	dsb();
	u64 s = step("map_begin hw rw uncached", T7(4), st, va, seg, 1, 0x308);
	while (s == 1)
		s = step("map_continue", T7(5), st, 0, 0, 0, 0);

	/* The kernel window of the global state: firmware-visible, opens the SAPT entry */
	u64 kva = (~0UL << 42) + (2UL << 36);
	u64 kl3 = uat_frame(5);
	step("global map_table l2", T7(2), global, kva, 2, kl3, 0);
	u64 leaf2 = frame_alloc();
	w64(segva + 0, leaf2);
	dsb();
	s = step("global map_begin fw+hw rw", T7(4), global, kva, seg, 1, 0x30b);
	while (s == 1)
		s = step("map_continue", T7(5), global, 0, 0, 0, 0);

	step("set_ctx 5", T7(10), st, 5, 0, 0, 0);
	step("prepare_fw_unmap (inactive here)", T7(6), st, va, 1, 0, 0);

	w64(segva + 0, va);
	w64(segva + 8, 1);
	dsb();
	s = step("unmap_begin", T7(8), st, seg, 1, 0, 0);
	while (s == 1)
		s = step("unmap_continue", T7(9), st, 0, 0, 0, 0);
	step("remove_ctx", T7(11), st, 0, 0, 0, 0);
	step("unmap_table l2", T7(3), st, va, 2, 0, 0);
	step("unmap_table l1", T7(3), st, va, 1, 0, 0);
	step("destroy", T7(1), st, 0, 0, 0, 0);
	step("retype out l3", SPTM_RETYPE, l3, 24, XNU_DEFAULT, 0, 0);
	step("retype out root", SPTM_RETYPE, r0, 24, XNU_DEFAULT, 0, 0);
	con_puts("uat: selftest done\n");
}
