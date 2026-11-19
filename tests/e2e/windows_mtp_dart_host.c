/* SPDX-License-Identifier: MIT */
/*
 * Host replay of src/hv/windows/mtp_dart_t8152.c over a fake sparse PA space.
 *
 * Failure list (written before the code):
 *   D1  table and leaf entries, bit for bit, in the documented format
 *   D2  the DVA plan of the build plan: TX 2 MiB at 0x10040000000, RX at 0x10040204000, the wake
 *       ring at 0x10040408000, the RTKit pool after it (a 16 KiB guard after every mapping)
 *   D3  an independent 3-level walk of the written tables finds every page; guards stay invalid
 *   D4  a valid root entry that is not ours: refused, nothing written
 *   D5  a valid leaf: refused, nothing written
 *   D6  more tables needed than provided: refused, nothing written
 *   D7  unaligned or empty mappings, a PA past 42 bits, DVAs past the window: refused
 *   D8  the whole I1/I2 plan needs 2 tables; a mapping across a 32 MiB boundary takes a new L3
 *   D9  only root entries that were invalid are written (one), and never twice
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_dart_t8152.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* A sparse PA space of 16 KiB pages. */
#define PAGES 64
static struct {
    u64 pa;
    u64 w[2048];
} pages[PAGES];
static int npages;
static unsigned writes, root_writes;
static u64 root_pa;

static u64 *slot(u64 pa, bool create)
{
    u64 page = pa & ~0x3fffull;
    for (int i = 0; i < npages; i++)
        if (pages[i].pa == page)
            return &pages[i].w[(pa & 0x3fff) / 8];
    if (!create || npages == PAGES)
        return NULL;
    memset(&pages[npages], 0, sizeof(pages[0]));
    pages[npages].pa = page;
    return &pages[npages++].w[(pa & 0x3fff) / 8];
}

static u64 rd(void *ctx, u64 pa)
{
    (void)ctx;
    u64 *s = slot(pa, false);
    if (!s) {
        printf("FAIL: read of unbacked PA 0x%llx\n", (unsigned long long)pa);
        failures++;
        return 0;
    }
    return *s;
}

static void wr(void *ctx, u64 pa, u64 v)
{
    (void)ctx;
    u64 *s = slot(pa, false);
    if (!s) {
        printf("FAIL: write of unbacked PA 0x%llx\n", (unsigned long long)pa);
        failures++;
        return;
    }
    writes++;
    if ((pa & ~0x3fffull) == root_pa)
        root_writes++;
    *s = v;
}

static const struct mtp_dart_mem mem = {NULL, rd, wr};

static void reset(u64 root, u32 ntables, u64 *pool)
{
    npages = 0;
    writes = root_writes = 0;
    root_pa = root;
    slot(root, true);
    for (u32 i = 0; i < ntables; i++) {
        pool[i] = 0x10f00010000ull + i * 0x4000ull;
        slot(pool[i], true);
    }
}

/* Independent walk: the format as documented, not the code under test. */
static bool walk(u64 root, u64 dva, u64 *pa)
{
    u64 idx[3] = {(dva >> 36) & 0x7ff, (dva >> 25) & 0x7ff, (dva >> 14) & 0x7ff};
    u64 table = root;
    for (int level = 0; level < 3; level++) {
        u64 *s = slot(table + idx[level] * 8, false);
        if (!s || !(*s & 1))
            return false;
        u64 next = ((*s >> 10) & ((1ull << 28) - 1)) << 14;
        if (level == 2) {
            if (((*s >> 40) & 0xfff) != 0xfff)
                return false;
            *pa = next | (dva & 0x3fff);
            return true;
        }
        table = next;
    }
    return false;
}

static void case_result(const char *name, int before)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"}\n", name, failures == before ? "passed" : "failed");
}

int main(void)
{
    const u64 root = 0x105ffae0000ull, area = 0x10f00000000ull, base = 0x10040000000ull;
    const u64 end = base + (64ull << 30);
    u64 pool[16];
    struct mtp_dart d;

    int before = failures;
    CHECK(mtp_dart_table_pte(0x105ffae0000ull) == ((0x105ffae0000ull >> 14) << 10 | 1));
    CHECK(mtp_dart_leaf_pte(0x10f00100000ull) ==
          ((0x10f00100000ull >> 14) << 10 | 1 | 0xfffull << 40));
    CHECK(mtp_dart_table_pte(0x3fffffffc000ull) == (0x3ffffffc00ull | 1));
    case_result("pte_format", before);

    before = failures;
    reset(root, 16, pool);
    mtp_dart_init(&d, &mem, root, base, end, pool, 16);
    u64 tx, rx, wake, rtk, fw;
    CHECK(mtp_dart_map(&d, area + 0x100000, 0x200000, &tx) == MTP_DART_OK);
    CHECK(mtp_dart_map(&d, area + 0x300000, 0x200000, &rx) == MTP_DART_OK);
    CHECK(mtp_dart_map(&d, area + 0x500000, 0x4000, &wake) == MTP_DART_OK);
    CHECK(mtp_dart_map(&d, area + 0x504000, 0x100000, &rtk) == MTP_DART_OK);
    CHECK(mtp_dart_map(&d, area + 0x700000, 79928, &fw) == MTP_DART_OK);
    CHECK(tx == 0x10040000000ull && rx == 0x10040204000ull && wake == 0x10040408000ull);
    CHECK(rtk == 0x10040410000ull && fw == 0x10040514000ull);
    CHECK(d.used == 2 && root_writes == 1);
    printf("{\"case\":\"dva_plan\",\"result\":\"%s\",\"tx\":\"0x%llx\",\"rx\":\"0x%llx\","
           "\"wake\":\"0x%llx\",\"rtkit_pool\":\"0x%llx\",\"fw\":\"0x%llx\",\"tables\":%u}\n",
           failures == before ? "passed" : "failed", (unsigned long long)tx,
           (unsigned long long)rx, (unsigned long long)wake, (unsigned long long)rtk,
           (unsigned long long)fw, d.used);

    before = failures;
    u64 pa;
    const struct {
        u64 dva, pa, size;
    } maps[] = {{tx, area + 0x100000, 0x200000},
                {rx, area + 0x300000, 0x200000},
                {wake, area + 0x500000, 0x4000},
                {rtk, area + 0x504000, 0x100000},
                {fw, area + 0x700000, 0x14000}};
    for (unsigned m = 0; m < 5; m++) {
        for (u64 off = 0; off < maps[m].size; off += 0x4000) {
            CHECK(walk(root, maps[m].dva + off + 0x10, &pa) && pa == maps[m].pa + off + 0x10);
            CHECK(mtp_dart_lookup(&d, maps[m].dva + off, &pa) && pa == maps[m].pa + off);
        }
        CHECK(!walk(root, maps[m].dva + maps[m].size, &pa)); /* the guard */
        CHECK(!mtp_dart_lookup(&d, maps[m].dva + maps[m].size, &pa));
    }
    case_result("walk", before);

    /* D4: a foreign root entry where ours would go. */
    before = failures;
    reset(root, 16, pool);
    *slot(root + 0x10 * 8, true) = mtp_dart_table_pte(0x200000000ull);
    mtp_dart_init(&d, &mem, root, base, end, pool, 16);
    CHECK(mtp_dart_map(&d, area + 0x100000, 0x200000, &tx) == MTP_DART_E_FOREIGN && !writes);
    /* Foreign entries elsewhere in the root are left alone and harmless. */
    reset(root, 16, pool);
    *slot(root + 0x11 * 8, true) = mtp_dart_table_pte(0x200000000ull);
    mtp_dart_init(&d, &mem, root, base, end, pool, 16);
    CHECK(mtp_dart_map(&d, area + 0x100000, 0x4000, &tx) == MTP_DART_OK &&
          *slot(root + 0x11 * 8, false) == mtp_dart_table_pte(0x200000000ull));
    case_result("foreign", before);

    /* D5: a valid leaf. */
    before = failures;
    reset(root, 16, pool);
    mtp_dart_init(&d, &mem, root, base, end, pool, 16);
    CHECK(mtp_dart_map(&d, area + 0x100000, 0x8000, &tx) == MTP_DART_OK);
    unsigned w = writes;
    d.next_dva = base + 0x4000; /* overlapping the second page */
    CHECK(mtp_dart_map(&d, area + 0x300000, 0x4000, &rx) == MTP_DART_E_MAPPED && writes == w);
    case_result("mapped", before);

    /* D6: one table is not enough for a new L2 and L3. */
    before = failures;
    reset(root, 1, pool);
    mtp_dart_init(&d, &mem, root, base, end, pool, 1);
    CHECK(mtp_dart_map(&d, area + 0x100000, 0x4000, &tx) == MTP_DART_E_TABLES && !writes);
    case_result("tables", before);

    /* D7 */
    before = failures;
    reset(root, 16, pool);
    mtp_dart_init(&d, &mem, root, base, base + 0x10000, pool, 16);
    CHECK(mtp_dart_map(&d, area + 0x100001, 0x4000, &tx) == MTP_DART_E_ARGS);
    CHECK(mtp_dart_map(&d, area + 0x100000, 0, &tx) == MTP_DART_E_ARGS);
    CHECK(mtp_dart_map(&d, 1ull << 42, 0x4000, &tx) == MTP_DART_E_ARGS);
    CHECK(mtp_dart_map(&d, area, 0x10000, &tx) == MTP_DART_E_ARGS); /* page + guard > window */
    CHECK(mtp_dart_map(&d, area, 0xc000, &tx) == MTP_DART_OK && writes > 0);
    CHECK(mtp_dart_map(&d, area, 0x4000, &tx) == MTP_DART_E_ARGS);
    case_result("args", before);

    /* D8: across a 32 MiB (one L3 table) boundary. */
    before = failures;
    reset(root, 16, pool);
    mtp_dart_init(&d, &mem, root, base + 0x2000000 - 0x8000, end, pool, 16);
    CHECK(mtp_dart_map(&d, area, 0x10000, &tx) == MTP_DART_OK && d.used == 3);
    for (u64 off = 0; off < 0x10000; off += 0x4000)
        CHECK(walk(root, tx + off, &pa) && pa == area + off);
    case_result("l3_boundary", before);
    return failures ? 1 : 0;
}
