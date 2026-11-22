/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "iova.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

struct iova_block {
    u64 iova;
    size_t sz;
    struct iova_block *next;
};

struct iova_domain {
    u64 base;
    u64 limit;
    struct iova_block *free_list;
};

iova_domain_t *iovad_init(u64 base, u64 limit)
{
    if (base != ALIGN_UP(base, SZ_32M)) {
        printf("iovad_init: base it not is not aligned to SZ_32M\n");
        return NULL;
    }

    /* limit is exclusive, including when the ADT supplies a high IOVA base. */
    u64 first = base ? base : SZ_16K;
    if (limit <= first || limit != ALIGN_DOWN(limit, SZ_16K))
        return NULL;

    iova_domain_t *iovad = calloc(1, sizeof(*iovad));
    if (!iovad)
        return NULL;

    struct iova_block *blk = calloc(1, sizeof(*blk));
    if (!blk) {
        free(iovad);
        return NULL;
    }

    /* don't hand out NULL pointers */
    blk->iova = first;
    blk->sz = limit - first;
    blk->next = NULL;
    iovad->base = base;
    iovad->limit = limit;
    iovad->free_list = blk;

    return iovad;
}

void iovad_shutdown(iova_domain_t *iovad, dart_dev_t *dart)
{
    struct iova_block *blk = iovad->free_list;

    while (blk != NULL) {
        struct iova_block *blk_free = blk;
        blk = blk->next;

        free(blk_free);
    }

    if (dart)
        for (u64 addr = iovad->base; addr < iovad->limit; addr += SZ_32M)
            dart_free_l2(dart, addr);

    free(iovad);
}

bool iova_reserve(iova_domain_t *iovad, u64 iova, size_t sz)
{
    /* Widen to whole pages from the range's end, so an unaligned range that
     * straddles a page boundary keeps both pages. */
    const u64 top = ALIGN_DOWN(UINT64_MAX, SZ_16K);
    if (sz > top || iova > top - sz) {
        printf("iova_reserve: [%lx; +%lx] wraps\n", iova, sz);
        return false;
    }
    u64 end = ALIGN_UP(iova + sz, SZ_16K);
    iova = ALIGN_DOWN(iova, SZ_16K);

    /* page 0 is never handed out */
    if (iova == 0)
        iova = SZ_16K;
    /* nothing left: a zero-size range, or one inside page 0 */
    if (end <= iova)
        return true;
    sz = end - iova;

    if (!iovad->free_list) {
        printf("iova_reserve: trying to reserve iova range but empty free list\n");
        return false;
    }

    struct iova_block *blk = iovad->free_list;
    struct iova_block *blk_prev = NULL;
    while (blk != NULL) {
        if (iova >= blk->iova && iova < (blk->iova + blk->sz)) {
            if (sz > blk->iova + blk->sz - iova) {
                printf("iova_reserve: tried to reserve [%lx; +%lx] but block in free list has "
                       "range [%lx; +%lx]\n",
                       iova, sz, blk->iova, blk->sz);
                return false;
            }

            if (iova == blk->iova && sz == blk->sz) {
                /* if the to-be-reserved range is present as a single block in the free list we just
                 * need to remove it */
                if (blk_prev)
                    blk_prev->next = blk->next;
                else
                    iovad->free_list = blk->next;

                free(blk);
                return true;
            } else if (iova == blk->iova) {
                /* cut off the reserved range from the beginning */
                blk->iova += sz;
                blk->sz -= sz;
                return true;
            } else if (iova + sz == blk->iova + blk->sz) {
                /* cut off the reserved range from the end */
                blk->sz -= sz;
                return true;
            } else {
                /* the to-be-reserved range is in the middle and we'll have to split this block */
                struct iova_block *blk_new = calloc(1, sizeof(*blk_new));
                if (!blk_new) {
                    printf("iova_reserve: out of memory.\n");
                    return false;
                }

                blk_new->iova = iova + sz;
                blk_new->sz = blk->iova + blk->sz - blk_new->iova;
                blk_new->next = blk->next;
                blk->next = blk_new;
                blk->sz = iova - blk->iova;
                return true;
            }
        }

        blk_prev = blk;
        blk = blk->next;
    }

    printf("iova_reserve: tried to reserve [%lx; +%lx] but range is already used.\n", iova, sz);
    return false;
}

u64 iova_alloc(iova_domain_t *iovad, size_t sz)
{
    sz = ALIGN_UP(sz, SZ_16K);

    struct iova_block *blk_prev = NULL;
    struct iova_block *blk = iovad->free_list;
    while (blk != NULL) {
        if (blk->sz == sz) {
            u64 iova = blk->iova;

            if (blk_prev)
                blk_prev->next = blk->next;
            else
                iovad->free_list = blk->next;

            free(blk);
            return iova;
        } else if (blk->sz > sz) {
            u64 iova = blk->iova;

            blk->iova += sz;
            blk->sz -= sz;

            return iova;
        }

        blk_prev = blk;
        blk = blk->next;
    }

    return 0;
}

void iova_free(iova_domain_t *iovad, u64 iova, size_t sz)
{
    if (sz == 0)
        return;

    /* Only ranges iova_alloc() can have handed out may come back: anything
     * else would put addresses outside the domain on the free list. */
    u64 first = iovad->base ? iovad->base : SZ_16K;
    if (iova != ALIGN_DOWN(iova, SZ_16K) || iova < first || iova >= iovad->limit ||
        sz > iovad->limit - iova)
        panic("iova_free: [%lx; +%lx] is outside the domain [%lx; %lx)\n", iova, sz, first,
              iovad->limit);
    /* limit is page aligned, so the rounded size still ends inside it */
    sz = ALIGN_UP(sz, SZ_16K);

    /* The free list is sorted and coalesced: find the blocks around the range. */
    struct iova_block *blk_prev = NULL;
    struct iova_block *blk = iovad->free_list;
    while (blk && blk->iova < iova + sz) {
        blk_prev = blk;
        blk = blk->next;
    }

    /* The range must not overlap a free block (double free or corruption). */
    if (blk_prev && blk_prev->iova + blk_prev->sz > iova)
        panic("iova_free: [%lx; +%lx] is already free\n", iova, sz);

    bool join_prev = blk_prev && blk_prev->iova + blk_prev->sz == iova;
    bool join_next = blk && iova + sz == blk->iova;

    if (join_prev && join_next) {
        blk_prev->sz += sz + blk->sz;
        blk_prev->next = blk->next;
        free(blk);
    } else if (join_prev) {
        blk_prev->sz += sz;
    } else if (join_next) {
        blk->iova = iova;
        blk->sz += sz;
    } else {
        struct iova_block *blk_new = calloc(1, sizeof(*blk_new));
        if (!blk_new)
            panic("iova_free: out of memory\n");

        blk_new->iova = iova;
        blk_new->sz = sz;
        blk_new->next = blk;

        if (blk_prev)
            blk_prev->next = blk_new;
        else
            iovad->free_list = blk_new;
    }
}
