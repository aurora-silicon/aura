/* SPDX-License-Identifier: MIT */
/*
 * Host run of the sleep trace's DCP packet copier (src/s2r_dcp_pkt.c) over a fake T6050 DART.
 *
 * dart_walk_locked_t8110() is the real one, copied out of src/dart.c by run-s2r-dcp-pkt.sh. The
 * DART is built in memory at fixed addresses: its stream 23 maps the IOMFB shared memory at the
 * DVA 0x1000004c000 the AP sent in its init word (the stream's VM base is bit 40, above the 2^36
 * the walk can return); stream 3 has no TTBR and stream 27 is a remap stream. The tables are
 * identity maps of host memory, so this checks the logic, not the hardware's tables.
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
static struct rec recs[64];
static int nrec;
void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool w)
{
    (void)w;
    if (nrec < 64)
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

static void build(void)
{
    map(DART, 0x10000);
    map(DRAM, 0x400000);
    *(volatile u32 *)(DART + 0x200) = 0xff;
    *(volatile u32 *)(DART + 0x1000 + 4 * 3) = 0x1;
    *(volatile u32 *)(DART + 0x1000 + 4 * 27) = 0x1780;
    *(volatile u32 *)(DART + 0x1400 + 4 * 27) = ((L1 >> 14) << 2) | 1;
    *(volatile u32 *)(DART + 0x1000 + 4 * SID) = 0x9;
    *(volatile u32 *)(DART + 0x1400 + 4 * SID) = ((L1 >> 14) << 2) | 1;
    u64 low = DVA & ((1UL << 36) - 1);
    ((u64 *)L1)[low >> 25] = (L2 >> 4) | 1;
    for (int k = 0; k < NPG; k++)
        ((u64 *)L2)[((low >> 14) & 0x7ff) + k] = ((DATA + k * 0x4000UL) >> 4) | 1;

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

/* A stream whose tables map DVA pages first..first+n-1 to the shared memory pages of the same index. */
static void map_stream(int sid, u64 l1, u64 l2, int first, int n)
{
    u64 low = DVA & ((1UL << 36) - 1);
    memset((void *)l1, 0, 0x4000);
    memset((void *)l2, 0, 0x4000);
    ((u64 *)l1)[low >> 25] = (l2 >> 4) | 1;
    for (int k = first; k < first + n; k++)
        ((u64 *)l2)[((low >> 14) & 0x7ff) + k] = ((DATA + k * 0x4000UL) >> 4) | 1;
    *(volatile u32 *)(DART + 0x1000 + 4 * sid) = 0x9;
    *(volatile u32 *)(DART + 0x1400 + 4 * sid) = ((l1 >> 14) << 2) | 1;
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
    map_stream(24, L1, L2, 0, NPG);
    map_stream(5, L1, L2, 0, NPG);
    ap_call(DVA, 0x302);
    CHECK(find(7, 5) != NULL, "tie between streams 5, 23 and 24: want stream 5");
    unmap_stream(24);
    unmap_stream(5);

    /* A smaller stream before the chosen one: its pages must not leak into the chosen stream's. */
    map_stream(5, L1B, L2B, 0, 3);       /* maps DVA pages 0 to 2 only */
    map_stream(SID, L1C, L2C, 3, NPG - 3); /* maps pages 3 to 48: the larger stream */
    ap_call(DVA, 0x2);                   /* AP call at DVA offset 0x100: page 0 */
    r = find(7, SID);
    CHECK(r && r->val >> 32 == NPG - 3, "partial streams: want stream %d with %d pages", SID, NPG - 3);
    CHECK(count_kind(0) == 0, "page 0 is not in the chosen stream: %d records from it", count_kind(0));
    unmap_stream(5);
    map_stream(SID, L1, L2, 0, NPG);

    /* A packet head with 32-bit sizes of 0xffffffff copies a bounded number of words. */
    *(u64 *)pg_pa(CALL_OFF) = (0xffffffffUL << 32) | 0x54534554;
    *(u64 *)pg_pa(CALL_OFF + 8) = 0xffffffffUL;
    ap_call(DVA, 0x302);
    CHECK(count_kind(0) <= 2 + 16, "huge input size: %d records", count_kind(0));
    i2a(0x42, 0x37);
    CHECK(count_kind(2) <= 2 + 8, "huge output size: %d records", count_kind(2));

    printf(failures ? "FAIL: %d checks\n" : "PASS: DCP packet copier\n", failures);
    return failures != 0;
}
