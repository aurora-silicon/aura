/* SPDX-License-Identifier: MIT */
/*
 * Host run of the sleep trace's DCP packet copier (src/s2r_dcp_pkt.c) over a fake T6050 DART.
 *
 * dart_walk_locked_t8110() is the real one, copied out of src/dart.c by run-s2r-dcp-pkt.sh. The
 * DART is built in memory at fixed addresses. Its stream 23 is shaped like the tables read from
 * the M5 guest (T6050) with the proxy: three levels, root[iova >> 36] to a mid table to a leaf
 * table, the IOMFB shared memory at the DVA 0x1000004c000 the AP sent in its init word (root
 * index 16), leaf entries with flag bits above bit 39. Streams 5 and 24 map it the same way,
 * stream 9 with the older two levels; stream 3 has no TTBR and stream 27 is a remap stream. The
 * tables are identity maps of host memory, so this checks the logic, not the hardware's tables.
 *
 * One IOMFB call and its reply go through the mailbox registers; the copier must put the packet's
 * head and its bytes in the ring from the right pages, and must say which stream it found.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint64_t u64;
typedef uint32_t u32;

extern u32 sptm_virt_s2r_dcp_pkt;
void s2r_dcp_add(const char *name, u64 mbox);
void s2r_dcp_watch(u64 addr, const u64 *val, int width);

#include "xnuboot.h"
struct boot_args cur_boot_args;
u64 mem_size_actual = 0x2000000000UL;
void *adt;
int adt_path_offset_trace(void *a, const char *p, int *o)
{
    (void)a, (void)p;
    *o = 0;
    return 0;
}
int adt_get_reg(void *a, int *p, const char *n, int i, u64 *b, u64 *s)
{
    (void)a, (void)p, (void)n, (void)i;
    *b = 0x48a340000UL;
    *s = 0x10000;
    return 0;
}
void debug_printf(const char *f, ...)
{
    (void)f;
}

#define DART    0x48a340000UL
#define MBOX    0x48ae08000UL
#define DRAM    0x10000200000UL
#define L1      (DRAM)
#define L2      (DRAM + 0x4000)
#define L1B     (DRAM + 0x8000)
#define L2B     (DRAM + 0xc000)
#define L1C     (DRAM + 0x10000)
#define L2C     (DRAM + 0x14000)
#define L3      (DRAM + 0x100000)
#define L3B     (DRAM + 0x104000)
#define L3C     (DRAM + 0x108000)
#define DATA    (DRAM + 0x20000)
#define NPG     49
#define DVA     0x1000004c000UL
#define SID     23
#define CALL_OFF 0x20100UL /* AP call packet: the 0x100 bit of the control word selects 0x20000 */

int sptm_virt_peek64(u64 p, u64 *o)
{
    if (p < DATA || p >= DATA + NPG * 0x4000UL)
        return 0;
    *o = *(u64 *)p;
    return 1;
}

struct rec {
    u64 pa, val, tag;
};
static struct rec recs[4096];
static int nrec;
void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool w)
{
    (void)w;
    if (nrec < 4096)
        recs[nrec++] = (struct rec){pa, val, tag};
}

static int failures;
#define CHECK(c, ...)                                      \
    do {                                                   \
        if (!(c)) {                                        \
            printf("FAIL line %d: ", __LINE__);            \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            failures++;                                    \
        }                                                  \
    } while (0)

static u64 pg_pa(u64 off)
{
    return DATA + (off / 0x4000) * 0x4000UL + (off & 0x3fff);
}

static void map(u64 addr, u64 len)
{
    if (mmap((void *)addr, len, PROT_READ | PROT_WRITE,
             MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
}

static void map_stream(int sid, u64 root, u64 mid, u64 leaf, int first, int n, int levels);

static void build(void)
{
    map(DART, 0x10000);
    map(DRAM, 0x400000);
    *(volatile u32 *)(DART + 0x200) = 0xff;
    *(volatile u32 *)(DART + 0x1000 + 4 * 3) = 0x1;
    *(volatile u32 *)(DART + 0x1000 + 4 * 27) = 0x1780;
    *(volatile u32 *)(DART + 0x1400 + 4 * 27) = ((L1 >> 14) << 2) | 1;
    map_stream(SID, L1, L2, L3, 0, NPG, 3);

    /* the call's packet: head (tag, input size 0x20), head 2 (output size 0x10), 4 input words */
    *(u64 *)pg_pa(CALL_OFF) = (0x20UL << 32) | 0x54534554;
    *(u64 *)pg_pa(CALL_OFF + 8) = 0x10;
    for (int i = 0; i < 6; i++)
        *(u64 *)pg_pa(CALL_OFF + 16 + 8 * i) = 0xa0 + i;
}

static void a2i(u64 m0, u64 m1)
{
    u64 v[2] = {m0, m1};
    s2r_dcp_watch(MBOX + 0x800, v, 4);
}
static void i2a(u64 m0, u64 m1)
{
    u64 v[2] = {m0, m1};
    s2r_dcp_watch(MBOX + 0x830, v, 4);
}

/* A stream whose tables map DVA pages first..first+n-1 to the shared memory pages of the same index.
 * levels 3: root[dva >> 36] -> mid[dva >> 25 & 0x7ff] -> leaf; levels 2: root[dva >> 25 & 0x7ff] -> mid
 * (the leaf argument is unused). A leaf entry carries the flag bits the hardware sets above bit 39,
 * and the page before the window is mapped to a decoy. */
static void map_stream(int sid, u64 root, u64 mid, u64 leaf, int first, int n, int levels)
{
    u64 low = DVA & ((1UL << 36) - 1), flags = 0xfffUL << 40;
    u64 *pg = (u64 *)(levels == 3 ? leaf : mid);
    memset((void *)root, 0, 0x4000);
    memset((void *)mid, 0, 0x4000);
    memset((void *)leaf, 0, 0x4000);
    if (levels == 3) {
        ((u64 *)root)[DVA >> 36] = (mid >> 4) | 1;
        ((u64 *)mid)[(low >> 25) & 0x7ff] = (leaf >> 4) | 1;
    } else {
        ((u64 *)root)[low >> 25] = (mid >> 4) | 1;
    }
    pg[((low >> 14) & 0x7ff) - 2] = (DATA >> 4) | 1 | flags;
    for (int k = first; k < first + n; k++)
        pg[((low >> 14) & 0x7ff) + k] = ((DATA + k * 0x4000UL) >> 4) | 1 | flags;
    *(volatile u32 *)(DART + 0x1000 + 4 * sid) = 0x9;
    *(volatile u32 *)(DART + 0x1400 + 4 * sid) = ((root >> 14) << 2) | 1;
}

static void unmap_stream(int sid)
{
    *(volatile u32 *)(DART + 0x1000 + 4 * sid) = 0;
}

static void ap_call(u64 dva, u64 ctl)
{
    a2i((dva << 16) | 0x40, 0x37);
    nrec = 0;
    a2i(((CALL_OFF & 0xffff) << 16) | ctl, 0x37);
}

static int count_kind(int kind)
{
    int n = 0;
    for (int i = 0; i < nrec; i++)
        n += (int)((recs[i].tag >> 36) & 0xf) == kind;
    return n;
}

static const struct rec *find(int kind, int word)
{
    for (int i = 0; i < nrec; i++)
        if ((int)((recs[i].tag >> 36) & 0xf) == kind && (int)(recs[i].tag >> 40) == word &&
            (recs[i].tag & 0xffffffff) == 0x544b5043)
            return &recs[i];
    return NULL;
}

int main(void)
{
    cur_boot_args.phys_base = 0x10000000000UL;
    build();
    sptm_virt_s2r_dcp_pkt = 1;
    s2r_dcp_add("dcp", MBOX);

    a2i((DVA << 16) | 0x40, 0x37); /* the AP's init word: the shared memory's DVA */
    CHECK(nrec == 0, "the init word alone made %d records", nrec);

    a2i((CALL_OFF & 0xffff) << 16 | 0x302, 0x37); /* an AP call */
    const struct rec *r = find(7, SID);
    CHECK(count_kind(7) == 1 && r, "want one resolve record for stream %d, got %d", SID, count_kind(7));
    if (r) {
        CHECK(r->val >> 32 == NPG, "found %lu pages, want %d", (unsigned long)(r->val >> 32), NPG);
        CHECK((u32)r->val == 63, "refused %u streams, want 63", (u32)r->val);
        CHECK(r->pa == DVA, "resolve record DVA 0x%lx", (unsigned long)r->pa);
    }
    CHECK(count_kind(0) == 6, "AP call: %d records, want 2 head words + 4 input words", count_kind(0));
    for (int w = 0; w < 6; w++) {
        r = find(0, w);
        u64 off = CALL_OFF + 8 * w, want = w == 0 ? (0x20UL << 32) | 0x54534554 : w == 1 ? 0x10 : 0xa0 + w - 2;
        CHECK(r && r->pa == pg_pa(off) && r->val == want, "AP call word %d: pa 0x%lx val 0x%lx", w,
              r ? (unsigned long)r->pa : 0, r ? (unsigned long)r->val : 0);
    }

    i2a(0x42, 0x37); /* the reply closes the call: its output is copied */
    CHECK(count_kind(2) == 5, "reply: %d records, want 2 head words + 3 output words", count_kind(2));
    r = find(2, 2);
    CHECK(r && r->pa == pg_pa(CALL_OFF + 0x28) && r->val == 0xa0 + 3, "output word at 0x%lx",
          r ? (unsigned long)r->pa : 0);
    CHECK(count_kind(7) == 1, "the shared memory was found again: %d resolve records", count_kind(7));

    /* A DVA no stream maps: one attempt at the first call, one more at call 257, nothing else. */
    nrec = 0;
    a2i((0x10080000000UL << 16) | 0x40, 0x37);
    for (int i = 1; i <= 1000; i++) {
        a2i(((CALL_OFF & 0xffff) << 16) | 0x302, 0x37);
        if (i == 256)
            CHECK(count_kind(7) == 1, "after 256 failing calls: %d attempts, want 1", count_kind(7));
        if (i == 257)
            CHECK(count_kind(7) == 2, "at failing call 257: %d attempts, want 2", count_kind(7));
    }
    CHECK(count_kind(7) == 4, "1000 failing calls: %d attempts, want 4 (calls 1, 257, 513, 769)", count_kind(7));
    CHECK(nrec == 4, "failing resolves wrote %d records, want only the 4 attempts", nrec);
    r = find(7, 0);
    CHECK(r && r->val >> 32 == 0, "a failed attempt must report 0 pages found");

    /* The next init word re-arms at once: the good DVA resolves on the next call. */
    ap_call(DVA, 0x302);
    CHECK(count_kind(7) == 1 && find(7, SID) && count_kind(0) == 6,
          "re-arm: %d resolve records, %d AP-call records", count_kind(7), count_kind(0));

    /* Two streams map every page: the lower stream wins the tie. */
    map_stream(24, L1, L2, L3, 0, NPG, 3);
    map_stream(5, L1, L2, L3, 0, NPG, 3);
    ap_call(DVA, 0x302);
    CHECK(find(7, 5) != NULL, "tie between streams 5, 23 and 24: want stream 5");
    unmap_stream(24);
    unmap_stream(5);

    /* A smaller stream before the chosen one: its pages must not leak into the chosen stream's. */
    map_stream(5, L1B, L2B, L3B, 0, 3, 3);         /* maps DVA pages 0 to 2 only */
    map_stream(SID, L1C, L2C, L3C, 3, NPG - 3, 3); /* maps pages 3 to 48: the larger stream */
    ap_call(DVA, 0x2);                   /* AP call at DVA offset 0x100: page 0 */
    r = find(7, SID);
    CHECK(r && r->val >> 32 == NPG - 3, "partial streams: want stream %d with %d pages", SID, NPG - 3);
    CHECK(count_kind(0) == 0, "page 0 is not in the chosen stream: %d records from it", count_kind(0));
    unmap_stream(5);
    map_stream(SID, L1, L2, L3, 0, NPG, 3);

    /* An older two-level stream still resolves, and wins the tie against stream 23 as the lower one. */
    map_stream(9, L1B, L2B, L3B, 0, NPG, 2);
    ap_call(DVA, 0x302);
    r = find(7, 9);
    CHECK(r && r->val >> 32 == NPG && count_kind(0) == 6, "two-level stream 9: want %d pages", NPG);
    unmap_stream(9);

    /* A three-level stream that maps the same low bits under root[0], not the DVA's root[16], is a
     * different address space: it must not match, though it is the lower stream. */
    map_stream(11, L1B, L2B, L3B, 0, NPG, 3);
    ((u64 *)L1B)[0] = ((u64 *)L1B)[DVA >> 36];
    ((u64 *)L1B)[DVA >> 36] = 0;
    ap_call(DVA, 0x302);
    CHECK(!find(7, 11) && find(7, SID), "stream 11 maps the low bits only: it must not match");
    unmap_stream(11);

    /* A packet head with 32-bit sizes of 0xffffffff copies a bounded number of words. */
    *(u64 *)pg_pa(CALL_OFF) = (0xffffffffUL << 32) | 0x54534554;
    *(u64 *)pg_pa(CALL_OFF + 8) = 0xffffffffUL;
    ap_call(DVA, 0x302);
    CHECK(count_kind(0) <= 2 + 16, "huge input size: %d records", count_kind(0));
    i2a(0x42, 0x37);
    CHECK(count_kind(2) <= 2 + 512, "huge output size: %d records", count_kind(2));

    /* A call with a large input (swap_submit, 0x1144 bytes): its changed words are traced. */
    *(u64 *)pg_pa(CALL_OFF) = (0x1144UL << 32) | 0x38353441; /* "A458" */
    for (int w = 2; w < 560; w++)
        *(u64 *)pg_pa(CALL_OFF + 8 * w) = 0x5000 + w;
    ap_call(DVA, 0x302);
    CHECK(count_kind(4) == 1, "big call: %d summary records", count_kind(4));
    CHECK(find(4, 1) && find(4, 1)->val == ((0x1144UL << 32) | 0x38353441),
          "big call summary: slot 1 (slot 0 is the huge-input tag) with size and tag");
    CHECK(count_kind(5) == 536, "big call: %d changed words, want 536 (words 18 to 553)", count_kind(5));
    for (int i = 0; i < nrec; i++)
        if (((recs[i].tag >> 36) & 0xf) == 5 && (recs[i].tag >> 40) == (1 << 12 | 100))
            CHECK(recs[i].pa == pg_pa(CALL_OFF + 800) && recs[i].val == 0x5000 + 100, "word 100: pa 0x%lx val 0x%lx",
                  (unsigned long)recs[i].pa, (unsigned long)recs[i].val);
    CHECK(count_kind(0) == 18, "big call: %d raw records, want the head and 16 input words", count_kind(0));

    nrec = 0;
    a2i(((CALL_OFF & 0xffff) << 16) | 0x302, 0x37);
    CHECK(count_kind(4) == 1 && count_kind(5) == 0, "same call again: %d changed words", count_kind(5));

    /* One word moves (the brightness): exactly that word is recorded, then it stops at the churn limit. */
    int sent = 0, volatile_at = 0;
    for (int i = 0; i < 250; i++) {
        *(u64 *)pg_pa(CALL_OFF + 800) = 0x9000 + i;
        nrec = 0;
        a2i(((CALL_OFF & 0xffff) << 16) | 0x302, 0x37);
        if (i == 0)
            CHECK(count_kind(5) == 1 && find(5, 1 << 12 | 100) && find(5, 1 << 12 | 100)->val == 0x9000,
                  "one word moved: %d records", count_kind(5));
        sent += count_kind(5);
        if (count_kind(6) && !volatile_at)
            volatile_at = i + 1;
    }
    CHECK(sent == 199, "word 100 recorded %d more times, want 199 (200 in all)", sent);
    CHECK(volatile_at == 199, "word 100 marked volatile at change %d, want 199", volatile_at);

    /* A second large call (a color table) has its own baseline: neither call records the other's words. */
    *(u64 *)pg_pa(CALL_OFF) = (0x600UL << 32) | 0x3054554c; /* "LUT0" */
    for (int w = 2; w < 200; w++)
        *(u64 *)pg_pa(CALL_OFF + 8 * w) = 0x7000 + w;
    ap_call(DVA, 0x302);
    CHECK(count_kind(4) == 1 && find(4, 2) && find(4, 2)->val == ((0x600UL << 32) | 0x3054554c),
          "second tag: summary in slot 2");
    CHECK(count_kind(5) == 176, "second tag: %d words, want 176 (words 18 to 193)", count_kind(5));
    *(u64 *)pg_pa(CALL_OFF) = (0x1144UL << 32) | 0x38353441;
    for (int w = 2; w < 200; w++)
        *(u64 *)pg_pa(CALL_OFF + 8 * w) = 0x5000 + w;
    *(u64 *)pg_pa(CALL_OFF + 800) = 0x9000 + 249;
    nrec = 0;
    a2i(((CALL_OFF & 0xffff) << 16) | 0x302, 0x37);
    CHECK(find(4, 1) && count_kind(5) == 0, "first tag again after the second: %d changed words", count_kind(5));

    /* A DCP that runs out of slots records the call's summary only. */
    for (int n = 0; n < 4; n++) {
        *(u64 *)pg_pa(CALL_OFF) = (0x300UL << 32) | (0x10000 + n);
        ap_call(DVA, 0x302);
    }
    CHECK(find(4, 15) && count_kind(5) == 0, "no slot left: %d changed words", count_kind(5));

    /* A call under the large-call size is copied raw, up to S2R_IN_MAX bytes. */
    *(u64 *)pg_pa(CALL_OFF) = (400UL << 32) | 0x31313131;
    ap_call(DVA, 0x302);
    CHECK(count_kind(0) == 2 + 50 && count_kind(4) == 0, "400 byte input: %d raw records, want 52", count_kind(0));

    /* A 3072 byte answer (a data property) is copied whole. */
    *(u64 *)pg_pa(CALL_OFF) = (0x10UL << 32) | 0x31313131;
    *(u64 *)pg_pa(CALL_OFF + 8) = 0xc00UL;
    ap_call(DVA, 0x302);
    i2a(0x42, 0x37);
    CHECK(count_kind(2) == 2 + 385, "3072 byte output: %d records, want 387", count_kind(2));

    printf(failures ? "FAIL: %d checks\n" : "PASS: DCP packet copier\n", failures);
    return failures != 0;
}
