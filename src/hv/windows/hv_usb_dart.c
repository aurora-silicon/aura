/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * D4: identity map builder and walker for the usb-drd Gen3 DARTs.  See hv_usb_dart.h for the
 * format and its sources.  Written on this branch; pure C with no includes beyond types.h, so
 * the host tests compile it unchanged.
 */

#include "hv_usb_dart.h"

#define TABLE_BYTES (USB_DART_PTES * sizeof(u64))
#define IDX_MASK    (USB_DART_PTES - 1)
#define DVA_LIMIT   (1UL << USB_DART_IAS)
#define L2_SPAN     (1UL << USB_DART_L2_SHIFT) /* one leaf table: 32 MiB */

u64 usb_dart_leaf_pte(u64 pa)
{
    return USB_DART_PTE_SP_END | ((pa >> 4) & USB_DART_PTE_ADDR) | USB_DART_PTE_VALID;
}

u64 usb_dart_table_pte(u64 table_pa)
{
    return ((table_pa >> 4) & USB_DART_PTE_ADDR) | USB_DART_PTE_VALID;
}

int usb_dart_ttbr(u64 root_pa, u32 *ttbr)
{
    if ((root_pa & (USB_DART_PAGE - 1)) || root_pa >= DVA_LIMIT)
        return USB_DART_EINVAL;
    *ttbr = (u32)(((root_pa >> 14) << 2) | 1);
    return USB_DART_OK;
}

u64 usb_dart_tlbi_lo(u32 route, u32 sid)
{
    return (1UL << 60) | ((u64)(route & 0xf) << 56) | ((u64)(sid & 0xff) << 48);
}

static bool window_ok(u64 start, u64 end)
{
    return start < end && end <= DVA_LIMIT && !((start | end) & (USB_DART_PAGE - 1));
}

int usb_dart_tables_needed(u64 start, u64 end, u32 *n_tables)
{
    if (!window_ok(start, end))
        return USB_DART_EINVAL;
    u64 l2 = ((end - 1) >> USB_DART_L1_SHIFT) - (start >> USB_DART_L1_SHIFT) + 1;
    u64 leaves = ((end - 1) >> USB_DART_L2_SHIFT) - (start >> USB_DART_L2_SHIFT) + 1;
    u64 n = 1 + l2 + leaves;
    if (n > USB_DART_MAX_TABLES)
        return USB_DART_ETOOBIG;
    *n_tables = (u32)n;
    return USB_DART_OK;
}

int usb_dart_build_identity(u64 start, u64 end, u64 *arena, u64 arena_pa, u32 n_tables,
                            u64 *root_pa)
{
    u32 need;
    int ret = usb_dart_tables_needed(start, end, &need);
    if (ret)
        return ret;
    if (!arena || n_tables < need || (arena_pa & (USB_DART_PAGE - 1)) ||
        arena_pa >= DVA_LIMIT || (u64)n_tables * TABLE_BYTES > DVA_LIMIT - arena_pa)
        return USB_DART_ESPACE;

    for (u64 i = 0; i < (u64)n_tables * USB_DART_PTES; i++)
        arena[i] = 0;

    u64 l1_first = start >> USB_DART_L1_SHIFT;
    u64 l1_count = ((end - 1) >> USB_DART_L1_SHIFT) - l1_first + 1;
    u64 blk_first = start >> USB_DART_L2_SHIFT;
    u64 blk_last = (end - 1) >> USB_DART_L2_SHIFT;
    u64 *root = arena;

    for (u64 blk = blk_first; blk <= blk_last; blk++) {
        u64 l1_idx = blk >> (USB_DART_L1_SHIFT - USB_DART_L2_SHIFT);
        u64 l2_slot = 1 + (l1_idx - l1_first);
        u64 leaf_slot = 1 + l1_count + (blk - blk_first);
        u64 *l2 = arena + l2_slot * USB_DART_PTES;
        u64 *leaf = arena + leaf_slot * USB_DART_PTES;

        root[l1_idx & IDX_MASK] = usb_dart_table_pte(arena_pa + l2_slot * TABLE_BYTES);
        l2[blk & IDX_MASK] = usb_dart_table_pte(arena_pa + leaf_slot * TABLE_BYTES);

        u64 lo = blk << USB_DART_L2_SHIFT, hi = lo + L2_SPAN;
        if (lo < start)
            lo = start;
        if (hi > end)
            hi = end;
        for (u64 pa = lo; pa < hi; pa += USB_DART_PAGE)
            leaf[(pa >> USB_DART_LEAF_SHIFT) & IDX_MASK] = usb_dart_leaf_pte(pa);
    }

    *root_pa = arena_pa;
    return USB_DART_OK;
}

/* The table a descriptor points at, if it is one of the arena's; else NULL. */
static const u64 *next_table(const u64 *arena, u64 arena_pa, u32 n_tables, u64 pte)
{
    if (!(pte & USB_DART_PTE_VALID) || (pte & ~(USB_DART_PTE_ADDR | USB_DART_PTE_VALID)))
        return NULL;
    u64 pa = (pte & USB_DART_PTE_ADDR) << 4;
    if (pa < arena_pa || (pa - arena_pa) % TABLE_BYTES ||
        (pa - arena_pa) / TABLE_BYTES >= n_tables)
        return NULL;
    return arena + (pa - arena_pa) / TABLE_BYTES * USB_DART_PTES;
}

int usb_dart_walk(const u64 *arena, u64 arena_pa, u32 n_tables, u64 root_pa, u64 dva,
                  u64 *pa_out)
{
    if (!arena || !n_tables || dva >= DVA_LIMIT)
        return USB_DART_EFAULT;
    const u64 *l1 = next_table(arena, arena_pa, n_tables, usb_dart_table_pte(root_pa));
    if (!l1)
        return USB_DART_EFAULT;
    const u64 *l2 =
        next_table(arena, arena_pa, n_tables, l1[(dva >> USB_DART_L1_SHIFT) & IDX_MASK]);
    if (!l2)
        return USB_DART_EFAULT;
    const u64 *leaf =
        next_table(arena, arena_pa, n_tables, l2[(dva >> USB_DART_L2_SHIFT) & IDX_MASK]);
    if (!leaf)
        return USB_DART_EFAULT;
    u64 pte = leaf[(dva >> USB_DART_LEAF_SHIFT) & IDX_MASK];
    if (!(pte & USB_DART_PTE_VALID) || (pte & USB_DART_PTE_FLAGS) ||
        (pte & USB_DART_PTE_SP_STRT) || (pte & USB_DART_PTE_SP_END) != USB_DART_PTE_SP_END)
        return USB_DART_EFAULT;
    *pa_out = ((pte & USB_DART_PTE_ADDR) << 4) | (dva & (USB_DART_PAGE - 1));
    return USB_DART_OK;
}

static int expect_identity(const u64 *arena, u64 arena_pa, u32 n, u64 root, u64 dva)
{
    u64 pa;
    return usb_dart_walk(arena, arena_pa, n, root, dva, &pa) != USB_DART_OK || pa != dva;
}

static int expect_fault(const u64 *arena, u64 arena_pa, u32 n, u64 root, u64 dva)
{
    u64 pa;
    return usb_dart_walk(arena, arena_pa, n, root, dva, &pa) != USB_DART_EFAULT;
}

int usb_dart_self_check(const u64 *arena, u64 arena_pa, u32 n_tables, u64 root_pa, u64 start,
                        u64 end)
{
    int bad = 0;
    if (!window_ok(start, end))
        return 1;

    bad += expect_identity(arena, arena_pa, n_tables, root_pa, start);
    bad += expect_identity(arena, arena_pa, n_tables, root_pa, start + 0x123);
    bad += expect_identity(arena, arena_pa, n_tables, root_pa, end - USB_DART_PAGE);
    bad += expect_identity(arena, arena_pa, n_tables, root_pa, end - 1);
    u64 boundary = (start | (L2_SPAN - 1)) + 1; /* the first 32 MiB boundary above start */
    if (boundary < end) {
        bad += expect_identity(arena, arena_pa, n_tables, root_pa, boundary - USB_DART_PAGE);
        bad += expect_identity(arena, arena_pa, n_tables, root_pa, boundary);
    }
    if (start >= USB_DART_PAGE)
        bad += expect_fault(arena, arena_pa, n_tables, root_pa, start - USB_DART_PAGE);
    if (end < DVA_LIMIT)
        bad += expect_fault(arena, arena_pa, n_tables, root_pa, end);
    bad += expect_fault(arena, arena_pa, n_tables, root_pa, arena_pa);
    bad += expect_fault(arena, arena_pa, n_tables, root_pa,
                        arena_pa + (u64)n_tables * TABLE_BYTES - USB_DART_PAGE);
    return bad;
}
