/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * I1: static dart-sbr stream 4 translations for the T8152 MTP.  See mtp_dart_t8152.h.  Written
 * on this branch from the table format our Linux driver measured; no third-party source.
 */

#include "mtp_dart_t8152.h"

static u32 idx(u64 dva, int level)
{
    static const u8 shift[3] = {36, 25, 14};
    return (u32)((dva >> shift[level]) & (MTP_DART_ENTRIES - 1));
}

static u64 entry_pa(u64 pte)
{
    return ((pte & MTP_DART_OFFSET) >> 10) << 14;
}

u64 mtp_dart_table_pte(u64 pa)
{
    return (((pa >> 14) << 10) & MTP_DART_OFFSET) | MTP_DART_VALID;
}

u64 mtp_dart_leaf_pte(u64 pa)
{
    return mtp_dart_table_pte(pa) | MTP_DART_SP_END;
}

void mtp_dart_init(struct mtp_dart *d, const struct mtp_dart_mem *mem, u64 root, u64 dva_base,
                   u64 dva_end, const u64 *pool, u32 count)
{
    d->mem = *mem;
    d->root = root;
    d->next_dva = dva_base;
    d->dva_end = dva_end;
    d->pool_count = count > MTP_DART_MAX_TABLES ? MTP_DART_MAX_TABLES : count;
    for (u32 i = 0; i < d->pool_count; i++)
        d->pool[i] = pool[i];
    d->used = 0;
    d->writes = 0;
}

static bool owned(const struct mtp_dart *d, u64 pa, u32 used)
{
    for (u32 i = 0; i < used; i++)
        if (d->pool[i] == pa)
            return true;
    return false;
}

/* The table behind a valid entry: ours, or MTP_DART_E_FOREIGN. 0 when the entry is invalid. */
static int follow(const struct mtp_dart *d, u64 table, u32 i, u64 *out)
{
    u64 pte = d->mem.read64(d->mem.ctx, table + 8ull * i);
    *out = 0;
    if (!(pte & MTP_DART_VALID))
        return MTP_DART_OK;
    if (!owned(d, entry_pa(pte), d->used))
        return MTP_DART_E_FOREIGN;
    *out = entry_pa(pte);
    return MTP_DART_OK;
}

/*
 * Check [dva, dva + size) without writing: no foreign table on the way, no valid leaf, and
 * count the tables that would be created.  DVAs rise page by page, so a table key that changes
 * is a new table.
 */
static int plan(const struct mtp_dart *d, u64 dva, u64 size, u32 *need)
{
    u64 last_l2 = ~0ull, last_l3 = ~0ull;
    *need = 0;
    for (u64 off = 0; off < size; off += MTP_DART_PAGE) {
        u64 v = dva + off, l2, l3 = 0;
        int rc = follow(d, d->root, idx(v, 0), &l2);
        if (rc)
            return rc;
        if (!l2 && v >> 36 != last_l2) {
            (*need)++;
            last_l2 = v >> 36;
        }
        if (l2) {
            rc = follow(d, l2, idx(v, 1), &l3);
            if (rc)
                return rc;
        }
        if (!l3) {
            if (v >> 25 != last_l3) {
                (*need)++;
                last_l3 = v >> 25;
            }
            continue;
        }
        if (d->mem.read64(d->mem.ctx, l3 + 8ull * idx(v, 2)) & MTP_DART_VALID)
            return MTP_DART_E_MAPPED;
    }
    return MTP_DART_OK;
}

/* The table behind entry i of `table`, created from the pool when the entry is invalid. */
static u64 table_at(struct mtp_dart *d, u64 table, u32 i)
{
    u64 pa;
    if (follow(d, table, i, &pa) || pa)
        return pa; /* plan() ruled out foreign entries */
    pa = d->pool[d->used++];
    d->mem.write64(d->mem.ctx, table + 8ull * i, mtp_dart_table_pte(pa));
    d->writes++;
    return pa;
}

int mtp_dart_map(struct mtp_dart *d, u64 pa, u64 size, u64 *dva)
{
    if (!size || (pa & (MTP_DART_PAGE - 1)) || size > (1ull << 32))
        return MTP_DART_E_ARGS;
    size = (size + MTP_DART_PAGE - 1) & ~(MTP_DART_PAGE - 1);
    u64 v = d->next_dva;
    if (pa >= MTP_DART_PA_MAX || size > MTP_DART_PA_MAX - pa || v > d->dva_end ||
        size + MTP_DART_PAGE > d->dva_end - v)
        return MTP_DART_E_ARGS;
    u32 need;
    int rc = plan(d, v, size, &need);
    if (rc)
        return rc;
    if (need > d->pool_count - d->used)
        return MTP_DART_E_TABLES;
    for (u64 off = 0; off < size; off += MTP_DART_PAGE) {
        u64 l2 = table_at(d, d->root, idx(v + off, 0));
        u64 l3 = table_at(d, l2, idx(v + off, 1));
        d->mem.write64(d->mem.ctx, l3 + 8ull * idx(v + off, 2), mtp_dart_leaf_pte(pa + off));
        d->writes++;
    }
    d->next_dva = v + size + MTP_DART_PAGE; /* one unmapped guard page */
    *dva = v;
    return MTP_DART_OK;
}

bool mtp_dart_lookup(const struct mtp_dart *d, u64 dva, u64 *pa)
{
    u64 table = d->root;
    for (int level = 0; level < 3; level++) {
        u64 pte = d->mem.read64(d->mem.ctx, table + 8ull * idx(dva, level));
        if (!(pte & MTP_DART_VALID))
            return false;
        u64 next = entry_pa(pte);
        if (level == 2) {
            if ((pte & MTP_DART_SP_END) != MTP_DART_SP_END)
                return false;
            *pa = next | (dva & (MTP_DART_PAGE - 1));
            return true;
        }
        if (!owned(d, next, d->used))
            return false;
        table = next;
    }
    return false;
}
