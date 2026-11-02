// SPDX-License-Identifier: GPL-2.0-only
// Read-only walk of the live Apple Device Tree iBoot passes in boot_args (deviceTreeP +0x60, deviceTreeLength +0x68,
// xnu pexpert/arm64/boot.h). DART unit ids, SID lists, apertures, pt-regions and DCP segments all come from here at boot:
// nothing below is a captured value. Format as in m1n1 adt.h: node = u32 nprops, u32 nchild, props, children; prop = char
// name[32], u32 len (top bit is a flag), data padded to 4.
#include "mon.h"

static const u8 *dt;
static u64 dtlen;

static int same(const char *a, const char *b, u64 n)
{
	for (u64 i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

static u32 rd32(const u8 *p) { return *(const volatile u32 *)p; }

/* first byte after this node, or 0 when it runs past the blob */
static const u8 *node_end(const u8 *n, int depth)
{
	if (depth > 12 || n + 8 > dt + dtlen)
		return 0;
	u32 np = rd32(n), nc = rd32(n + 4);
	const u8 *p = n + 8;
	for (u32 i = 0; i < np; i++) {
		if (p + 36 > dt + dtlen)
			return 0;
		p += 36 + ((rd32(p + 32) & 0x7fffffff) + 3 & ~3U);
	}
	for (u32 i = 0; i < nc; i++) {
		p = node_end(p, depth + 1);
		if (!p)
			return 0;
	}
	return p;
}

const void *adt_prop(const void *node, const char *name, u32 *len)
{
	const u8 *n = node, *p;
	if (!n)
		return 0;
	u32 np = rd32(n);
	p = n + 8;
	for (u32 i = 0; i < np; i++) {
		u32 l = rd32(p + 32) & 0x7fffffff;
		if (same((const char *)p, name, 32)) {
			if (len)
				*len = l;
			return p + 36;
		}
		p += 36 + ((l + 3) & ~3U);
	}
	return 0;
}

static const u8 *child(const u8 *n, const char *name)
{
	u32 np = rd32(n), nc = rd32(n + 4);
	const u8 *p = n + 8;
	for (u32 i = 0; i < np; i++)
		p += 36 + ((rd32(p + 32) & 0x7fffffff) + 3 & ~3U);
	for (u32 i = 0; i < nc; i++) {
		u32 l;
		const char *nm = adt_prop(p, "name", &l);
		if (nm && same(nm, name, l))
			return p;
		p = node_end(p, 1);
		if (!p)
			return 0;
	}
	return 0;
}

/* "arm-io/dart-mtp": components split on '/', from the root */
const void *adt_path(const char *path)
{
	const u8 *n = dt;
	char c[32];
	while (n && *path) {
		u64 i = 0;
		while (*path && *path != '/' && i < sizeof(c) - 1)
			c[i++] = *path++;
		c[i] = 0;
		if (*path == '/')
			path++;
		n = child(n, c);
	}
	return n;
}

u32 adt_u32(const void *node, const char *name, u32 dflt)
{
	u32 l = 0;
	const u8 *v = adt_prop(node, name, &l);
	return v && l >= 4 ? rd32(v) : dflt;
}

u64 adt_u64(const void *node, const char *name, u64 dflt)
{
	u32 l = 0;
	const u8 *v = adt_prop(node, name, &l);
	return v && l >= 8 ? *(const volatile u64 *)v : dflt;
}

/* The next span of /chosen/carveout-memory-map at or above from, cut at lim: what iBoot used inside the RAM it hands over
 * (m4-loader metadata.c gives none of it to Linux). region-id-24 is the container of that RAM, not a carveout. Regions closer
 * than 4 MiB merge. 0 and *base, *end when found. */
int adt_carveout_span(u64 from, u64 lim, u64 *base, u64 *end)
{
	const u8 *n = adt_path("chosen/carveout-memory-map");
	u64 b = ~0UL, e = 0;
	if (!n)
		return -1;
	for (int grown = 1; grown;) {
		const u8 *p = n + 8;
		u32 np = rd32(n);
		grown = 0;
		for (u32 i = 0; i < np; i++) {
			u32 l = rd32(p + 32) & 0x7fffffff;
			if (l == 16 && same((const char *)p, "region-id-", 10) && !same((const char *)p, "region-id-24", 32)) {
				u64 rb = *(const volatile u64 *)(p + 36), re = rb + *(const volatile u64 *)(p + 44);
				if (re > from && rb < lim && re > rb) {
					if (!e && rb < b)
						b = rb;
					else if (e && rb <= e + (4UL << 20) && re > e)
						e = re, grown = 1;
				}
			}
			p += 36 + ((l + 3) & ~3U);
		}
		if (!e && b != ~0UL) {           /* first pass found the lowest start: grow from its own end */
			e = b + 1;
			grown = 1;
		}
	}
	if (!e)
		return -1;
	*base = b < from ? from : b;
	*end = e > lim ? lim : e;
	return 0;
}

/* deviceTreeP is a kernel VA in the boot mapping; go through the PAPT as the rest of the monitor does, and take it only
 * when the whole blob sits in one range and the root node has a sane shape. */
int adt_init(u64 *boot_args)
{
	u64 va = boot_args[12], len = (u32)boot_args[13], virt = boot_args[1], phys = boot_args[2];
	u64 pa = va - virt + phys;
	u64 m = hmap(pa, 0), e = len ? hmap(pa + len - 1, 0) : 0;
	if (!m || e != m + len - 1)               /* the VA itself, when the boot_args bases do not translate it */
		m = papt_has_va(va, len) ? va : 0, e = m + len - 1;
	con_puts("mon: ADT va ");
	con_hex(va);
	con_puts(" pa ");
	con_hex(pa);
	con_puts(" len ");
	con_hex(len);
	con_puts(" papt ");
	con_hex(m);
	con_puts("\n");
	if (!m || e != m + len - 1 || len < 64 || len > 0x400000)
		return -1;
	if (rd32((const u8 *)m) > 512 || rd32((const u8 *)m + 4) > 512)
		return -1;
	dt = (const u8 *)m;
	dtlen = len;
	return 0;
}
