// SPDX-License-Identifier: GPL-2.0-only
// gpu.c (P_GPU): GPU resources for the guest. No SPTM call here: the guest drives table 7 over APIF.
// gpu_s2_map: the ASC and SGX windows, 1:1 device. No pmap-io-range overlaps them (J616s ADT). The UAT
// regions, gfx-data and SAPT are protected I/O (GUAT/SAPT tags) and are not mapped: stage-2 maps of
// frame type 27 are refused, and SPTM owns those pages.
// gpu_adt_log: one screen line per value the driver takes from the loader on raw boot. A differing
// value disables nothing by itself; the log says so and the module must then not be loaded.
#include "mon.h"

void s2_map(u64 ipa, u64 pa, int attr);

void gpu_s2_map(void)
{
	for (u64 a = 0x288000000UL; a < 0x288000000UL + 0x3758000UL; a += PAGE)
		s2_map(a, a, 1);
}

static const char s_want[][32] = { "gpu-region-base", "gfx-shared-region-base",
	"gfx-shared-l2-region-base", "gfx-handoff-base", "gfx-data-base",
	"agx-address-space-mgmt-mode", "gptbat-ready", "table-address", "policy", "n-entries",
	"gpu-iouat" };
static const u64 s_raw[] = { 0x10bfffb8000UL, 0x10bfff78000UL, 0x10bfff74000UL, 0x10bfff70000UL,
	0x10001d44000UL, 1, 1, 0x10bd13ac000UL, 2, 0x300000, 1 };

static int s_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static u64 adt_walk(u64 off, u64 end, int depth, int *mismatch)
{
	u32 np = r32(off), nc = r32(off + 4);
	off += 8;
	for (u32 i = 0; i < np && off < end; i++) {
		const char *name = (const char *)off;
		u32 len = r32(off + 32) & 0x7fffffff;
		for (u32 w = 0; w < sizeof(s_want) / sizeof(s_want[0]); w++) {
			if (!s_eq(name, s_want[w]) || (len != 8 && len != 4))
				continue;
			u64 v = len == 8 ? *(volatile u64 *)(off + 36) : r32(off + 36);
			con_puts("gpu adt ");
			con_puts(name);
			con_puts(" ");
			con_hex(v);
			if (v != s_raw[w]) {
				con_puts(" raw ");
				con_hex(s_raw[w]);
				*mismatch = 1;
			}
			con_puts("\n");
		}
		off += 36 + ((len + 3) & ~3UL);
	}
	if (depth < 8)
		for (u32 i = 0; i < nc && off < end; i++)
			off = adt_walk(off, end, depth + 1, mismatch);
	return off;
}

void gpu_adt_log(const u64 *boot_args)
{
	u64 adt = boot_args[12];               /* boot_args.deviceTreeP, +0x60 */
	u64 len = (u32)boot_args[13];          /* deviceTreeLength, +0x68 */
	int mismatch = 0;

	if (adt >> 48 != 0xffff || !len || len > 0x1000000) {
		con_puts("gpu adt: no device tree pointer\n");
		return;
	}
	adt_walk(adt, adt + len, 0, &mismatch);
	con_puts(mismatch ? "gpu adt: DIFFERS from the raw boot, leave asahi unloaded\n"
			  : "gpu adt: as raw boot\n");
}
