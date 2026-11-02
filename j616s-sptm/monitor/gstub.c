// SPDX-License-Identifier: GPL-2.0-only
// Guest stub: the first HW-visible console. Renders the monitor's log on the boot framebuffer through the guest's
// stage-2 Normal-NC mapping (the host has no fb mapping: the PAPT stops at memSize, the fb is above it), tests ACTLR
// at guest EL1, holds a moment for a photo, returns Linux's entry.
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#include "font8x16.h"
#include "gstub.h"

#define rd(r) ({ u64 __v; __asm__ volatile("mrs %0, " #r : "=r"(__v)); __v; })
#define RED 0x3FF00000u
#define GREEN 0x000FFC00u
#define YELLOW 0x3FFFFC00u
#define CYAN 0x000FFFFFu
#define BLUE 0x000003FFu
#define SC 2
#define TOP 96

static struct gstub_params *P;
static u64 cx, cy;

static void put(u64 x, u64 y, u32 c)
{
	*(volatile u32 *)(P->fb + y * P->stride + x * 4) = c;
}

static void band(u32 c)
{
	for (u64 y = 0; y < TOP; y++)
		for (u64 x = 0; x < P->width; x++)
			put(x, y, c);
	__asm__ volatile("dsb sy");
}

static void ch(char ch)
{
	u64 cols = P->width / (8 * SC), rows = (P->height - TOP) / (16 * SC);
	if (ch == '\r')
		return;
	if (ch == '\n' || cx >= cols) {
		cx = 0;
		cy = cy + 1 < rows ? cy + 1 : 0;
		if (ch == '\n')
			return;
	}
	const u8 *g = font8x16 + (u8)ch * 16;
	for (u64 r = 0; r < 16 * SC; r++)
		for (u64 c = 0; c < 8 * SC; c++)
			put(cx * 8 * SC + c, TOP + cy * 16 * SC + r, (g[r / SC] & (0x80 >> (c / SC))) ? 0x3FFFFFFFu : 0);
	cx++;
}

static void puts(const char *s)
{
	while (*s)
		ch(*s++);
}

static void hex(u64 v)
{
	puts("0x");
	for (int i = 60; i >= 0; i -= 4)
		ch("0123456789abcdef"[(v >> i) & 0xf]);
}

static u64 actlr_try(u64 v)
{
	u64 o = rd(actlr_el1), r;
	__asm__ volatile("dsb sy; msr actlr_el1, %0; isb" :: "r"(v));
	r = rd(actlr_el1);
	__asm__ volatile("dsb sy; msr actlr_el1, %0; isb" :: "r"(o));
	return r;
}

#ifdef MHV_SELFTEST
/* the monitor's VM path against the real SPTM, no Linux: qemu only. A VM with one page, a run that exits by HVC, a stage-2
 * fault, a map and a resume, unmap, destroy, a second VM on the freed VMID. Every result through hvc 0x7520. */
#include "mhv.h"
static u64 mhv_c(u64 f, u64 a, u64 b, u64 c, u64 d, u64 e)
{
	register u64 x0 __asm__("x0") = f, x1 __asm__("x1") = a, x2 __asm__("x2") = b;
	register u64 x3 __asm__("x3") = c, x4 __asm__("x4") = d, x5 __asm__("x5") = e;
	__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5) :: "memory");
	return x0;
}
static void st_log(u64 tag, u64 v) { mhv_c(0x7520, tag, v, 0, 0, 0); }
#define S2RWX (0x400UL | (3UL << 8) | (6UL << 2) | (1UL << 6) | (1UL << 7))
static void mhv_selftest(struct gstub_params *p)
{
	u64 page = 0x4000, base = (u64)p;
	u64 run_pa = base - 2 * page, code_pa = base - 3 * page, data_pa = base - 4 * page;
	volatile u64 *gs = (volatile u64 *)run_pa;
	struct mhv_run *r = (struct mhv_run *)(run_pa + MHV_RUN_OFF);
	volatile unsigned int *code = (volatile unsigned int *)code_pa;
	for (u64 i = 0; i < page / 8; i++)
		gs[i] = 0;
	*(volatile u64 *)data_pa = 0x1234567890UL;
	code[0] = 0xd2800ee2;                   /* mov x2, #0x77 */
	code[1] = 0xd4000002;                   /* hvc #0 */
	code[2] = 0xd2a80004;                   /* mov x4, #0x40000000 */
	code[3] = 0xf9400083;                   /* ldr x3, [x4] */
	code[4] = 0xd4000002;                   /* hvc #0 */
	code[5] = 0x14000000;                   /* b . */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb");
	u64 h = mhv_c(MHV_VM_CREATE, 0, 0, 0, 0, 0);
	st_log(1, h);
	st_log(2, mhv_c(MHV_MAP, h, 0x80000000UL, code_pa, page, S2RWX));
	r->handle = h;
	r->el1[MHV_SCTLR] = 0x30d00800;
	r->el1[MHV_ACTLR] = 0xc12;              /* the vCPU's TSO bits */
	r->vmpidr = 0x80000000;
	r->vpidr = 0x611f0551;
	gs[MHV_GS_HCR / 8] = 0x80000000UL | (1UL << 19);     /* RW | TSC; the rest comes from SPTM */
	gs[MHV_GS_PC / 8] = 0x80000000UL;
	gs[MHV_GS_PSTATE / 8] = 0x3c5;
	u64 c = mhv_c(MHV_RUN, run_pa, 0, 0, 0, 0);
	st_log(3, c);
	st_log(4, r->esr);
	st_log(5, gs[(MHV_GS_X0 + 2 * 8) / 8]);
	st_log(6, r->el1[MHV_ACTLR]);
	c = mhv_c(MHV_RUN, run_pa, 0, 0, 0, 0);
	st_log(7, r->esr);
	st_log(8, r->hpfar);
	st_log(9, mhv_c(MHV_MAP, h, 0x40000000UL, data_pa, page, S2RWX));
	c = mhv_c(MHV_RUN, run_pa, 0, 0, 0, 0);
	st_log(10, r->esr);
	st_log(11, gs[(MHV_GS_X0 + 3 * 8) / 8]);
	st_log(12, mhv_c(MHV_RELAX, h, 0x40000000UL, S2RWX & ~(1UL << 7), 0, 0));
	st_log(13, mhv_c(MHV_UNMAP, h, 0x40000000UL, page, 0, 0));
	st_log(14, mhv_c(MHV_VM_DESTROY, h, 0, 0, 0, 0));
	h = mhv_c(MHV_VM_CREATE, 0, 0, 0, 0, 0);
	st_log(15, h);
	st_log(16, mhv_c(MHV_VM_DESTROY, h, 0, 0, 0, 0));
	st_log(17, 0xd09e);
	for (u64 i = 0; i < page / 8; i++)
		gs[i] = 0;
}
#endif

u64 stub_main(struct gstub_params *p)
{
	P = p;
#ifdef MHV_SELFTEST
	mhv_selftest(p);
#ifndef MHV_SELFTEST_GO
	mhv_c(0xDEAD, 0, 0, 0, 0, 0);            /* keep the result on the panel */
#endif
#endif
	if (!p->host_fb) {
		/* dirty lines from before the NC map would hide or overwrite our writes (Gravity memory.c, aurora m1n1 #60) */
		for (u64 a = p->fb & ~63UL; a < p->fb + p->stride * p->height; a += 64)
			__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
		__asm__ volatile("dsb sy");
		band(BLUE);
		for (const char *s = (const char *)p + sizeof(*p), *e = s + p->log_len; s < e; s++)
			ch(*s);
	}
	/* ACTLR from guest EL1, as Linux has_apple_tso() and XNU 0xb74a53c write it. SPTM does not load it on dispatch: its
	 * only slot, +0x7f8, belongs to the per-CPU host block. Readback is the bit, not ordering. */
	u64 a = rd(actlr_el1), b1 = actlr_try(a | 2), b4 = actlr_try(a | 0x10), b12 = actlr_try(a | 0x12);
	register u64 x0 __asm__("x0") = 0x7501, x1 __asm__("x1") = a, x2 __asm__("x2") = b1;
	register u64 x3 __asm__("x3") = b4, x4 __asm__("x4") = b12;
	__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4));
	{
		register u64 y0 __asm__("x0") = 0x7502;
		__asm__ volatile("hvc #0" : "+r"(y0));
	}
	if (p->host_fb)
		return p->entry;
	puts("guest EL1: ACTLR_EL1 ");
	hex(a);
	puts(" |2 ");
	hex(b1);
	puts(" |10 ");
	hex(b4);
	puts(" |12 ");
	hex(b12);
	puts("\n");
	band((b12 & 0x12) == 0x12 ? GREEN : (b1 & 2) ? YELLOW : RED);
	puts("guest: Linux in 20 s\n");
	u64 t0 = rd(cntpct_el0), hz = rd(cntfrq_el0);
	while (rd(cntpct_el0) - t0 < 20 * (hz ? hz : 1000000000UL))
		__asm__ volatile("isb");
	return p->entry;
}
