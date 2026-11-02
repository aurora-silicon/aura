// SPDX-License-Identifier: GPL-2.0-only
// Host framebuffer console (native draw 7): 16 bands along the top edge, text below. Draw 9 has no host mapping: the
// bands go to the log and the guest stub renders the log (gstub.c).
#include "mon.h"
#include "font8x16.h"

#define FB_PA_DEF  0x10bd1474000UL
#define FB_W_DEF   3456UL
#define FB_H_DEF   2234UL
#define FB_PAGES   1984UL                 /* 31 MiB >= 3456 x 2234 x 4 */

static u64 stride, height;
u64 fb;
int fb_ok, fb_fail;

/* the way XNU maps it (hmap_fb, public arm_init_sptm.c:1704-1711): the fb is above memSize, so not in the PAPT */
void fb_init(u64 *boot_args)
{
	u64 pa = boot_args[5], rb = boot_args[7], h = boot_args[9];   /* Video base, rowBytes, height */
	if ((pa >> 40) != 1)
		pa = FB_PA_DEF;
	stride = (rb && rb < 0x10000) ? rb : FB_W_DEF * 4;
	height = (h && h < 0x2000) ? h : FB_H_DEF;
	if (stride * height > FB_PAGES * PAGE)
		height = FB_PAGES * PAGE / stride;
	u64 base = pa & ~(PAGE - 1);
	u64 pages = (pa - base + stride * height + PAGE - 1) / PAGE;
	u64 va0 = hmap_fb(base, pages);
	if (!va0) {
		fb_fail = 1;
		return;
	}
	fb = va0 + (pa - base);
	fb_ok = 1;
	for (u64 p = fb & ~63UL; p < fb + stride * height; p += 64)   /* dirty lines from before the NC map */
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	dsb();
}

void fb_off(void)
{
	fb = 0;
}

static void clean(u64 va, u64 len)
{
	for (u64 p = va & ~63UL; p < va + len; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
}

/* band k of 16 along the top edge strip (the text console uses the rest) */
void fb_band(u64 k, u32 colour)
{
	if (!fb) {
		con_puts("mon: band ");
		con_hex(k);
		con_puts(" colour ");
		con_hex(colour);
		con_puts("\n");
		return;
	}
	u64 bw = stride / 16, y1 = height / 16;
	for (u64 y = 0; y < y1; y++) {
		u64 row = fb + y * stride + k * bw;
		for (u64 x = 0; x < bw; x += 4)
			w32(row + x, colour);
		clean(row, bw);
	}
	dsb();
}

/* text console below band 0: 8x16 font at 2x, wraps to the top when full */
#define SC 2
static u64 cx, cy;

static void glyph(u64 x0, u64 y0, u8 ch, u32 fg)
{
	const u8 *g = font8x16 + ch * 16;
	for (u64 r = 0; r < 16 * SC; r++) {
		u8 bits = g[r / SC];
		u64 row = fb + (y0 + r) * stride + x0 * 4;
		for (u64 c = 0; c < 8 * SC; c++)
			w32(row + c * 4, (bits & (0x80 >> (c / SC))) ? fg : 0);
		clean(row, 8 * SC * 4);
	}
	dsb();
}

void fb_putc(char ch)
{
	if (!fb)
		return;
	u64 cols = stride / 4 / (8 * SC), top = height / 16, rows = (height - top) / (16 * SC);
	if (ch == '\r')
		return;
	if (ch == '\n' || cx >= cols) {
		cx = 0;
		if (++cy >= rows)
			cy = 0;
		for (u64 c = 0; c < cols; c++)               /* clear the new line */
			glyph(c * 8 * SC, top + cy * 16 * SC, ' ', 0);
		if (ch == '\n')
			return;
	}
	glyph(cx * 8 * SC, top + cy * 16 * SC, (u8)ch, WHITE);
	cx++;
}
