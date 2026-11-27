/* SPDX-License-Identifier: MIT */
/*
 * Host check of dart_translate_internal()/dart_translate_range() (src/dart.c) and rtkit_crashed()
 * (src/rtkit.c), extracted directly from production sources and compiled under ASan/UBSan.
 */
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define SZ_16K                 0x4000UL
#define DART_PTE_VALID         1ULL
#define DART_PTE_OFFSET_SHIFT  12
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzll(mask))

typedef struct {
    int ttbr_count;
    u64 offset_mask;
} dart_params_t;

typedef struct {
    uintptr_t regs;
    u32 device;
    const dart_params_t *params;
    u64 *l1[4];
} dart_dev_t;

struct rtkit_buffer {
    void *bfr;
    u64 dva;
    size_t sz;
    bool owned;
};

typedef struct {
    const char *name;
    bool crashed;
    struct rtkit_buffer crashlog_bfr;
} rtkit_dev_t;

static int lines;
static char logbuf[4096];
static size_t loglen;

static void reset_log(void)
{
    lines = 0;
    loglen = 0;
    logbuf[0] = '\0';
}

static void capture_printf(const char *fmt, ...)
{
    va_list ap;
    lines++;
    va_start(ap, fmt);
    int n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n;
        if (add >= sizeof(logbuf) - loglen)
            loglen = sizeof(logbuf) - 1;
        else
            loglen += add;
    }
}

#define printf(...)       capture_printf(__VA_ARGS__)
#define rtkit_printf(...) capture_printf(__VA_ARGS__)

/* SOURCE FUNCTIONS */

#undef printf

static u64 make_pte(uintptr_t addr)
{
    return (addr & ~0xfffULL) | DART_PTE_VALID;
}

int main(void)
{
    static u64 l1_t0[2048] __attribute__((aligned(4096)));
    static u64 l1_t3[2048] __attribute__((aligned(4096)));
    static u64 l2_low[2048] __attribute__((aligned(4096)));
    static u64 l2_high[2048] __attribute__((aligned(4096)));

    static const dart_params_t params_4 = {
        .ttbr_count = 4,
        .offset_mask = ~0xfffULL,
    };
    static const dart_params_t params_1 = {
        .ttbr_count = 1,
        .offset_mask = ~0xfffULL,
    };

    /* Map ttbr=0, l1=0, l2=0..2 -> contiguous 0x80000000, 0x80004000, non-contiguous 0x90000000 */
    l1_t0[0] = make_pte((uintptr_t)l2_low);
    l2_low[0] = make_pte(0x80000000ULL);
    l2_low[1] = make_pte(0x80004000ULL);
    l2_low[2] = make_pte(0x90000000ULL);
    /* Page 4 maps to the top page of the 64-bit address space (one-past-end wraps to 0) */
    l2_low[4] = make_pte((uintptr_t)(UINTPTR_MAX - (SZ_16K - 1)));

    /* Map last page of 4-TTBR space (0x3fffffc000) to 0x7fffc000 so page 0 (0x80000000) would
     * look contiguous if 0x4000000000 aliased back to ttbr=0, l1=0, l2=0. */
    l1_t3[2047] = make_pte((uintptr_t)l2_high);
    l2_high[2047] = make_pte(0x7fffc000ULL);
    /* Also map last page of 1-TTBR space (0xffffffc000) for silent TTBR-crossing check. */
    l1_t0[2047] = make_pte((uintptr_t)l2_high);

    dart_dev_t d4 = {.regs = 0x200000000ULL, .device = 0, .params = &params_4};
    d4.l1[0] = l1_t0;
    d4.l1[3] = l1_t3;

    dart_dev_t d1 = {.regs = 0x200000000ULL, .device = 1, .params = &params_1};
    d1.l1[0] = l1_t0;

    reset_log();
    assert(dart_translate_range(&d4, 0x0, 0x8000) == (void *)0x80000000);
    assert(dart_translate_range(&d4, 0x1000, 0x4000) == (void *)0x80001000);
    assert(!dart_translate_range(&d4, 0x5000, 0x4000)); /* page 2 non-contiguous */
    assert(!dart_translate_range(&d4, 0x9000, 0x8000)); /* page 3 unmapped */
    assert(!dart_translate_range(&d4, 0x0, 0));
    assert(!dart_translate_range(NULL, 0x0, 0x4000));
    assert(!dart_translate_range(&d4, UINTPTR_MAX - 0x10, 0x100));
    /* One-past-end pointer wrap: start + size == 0 must be rejected. */
    assert(!dart_translate_range(&d4, 4 * SZ_16K, SZ_16K));
    /* 4-TTBR 256 GiB boundary crossing must not alias into ttbr=0. */
    assert(dart_translate_range(&d4, 0x3fffffc000ULL, SZ_16K) == (void *)0x7fffc000);
    assert(!dart_translate_range(&d4, 0x3fffffc000ULL, 2 * SZ_16K));
    /* Bit-40-tagged IOVAs (e.g. Gen3 vm_base / display segment remaps) must still translate,
     * while crossing a 256 GiB boundary inside a tagged window must still be rejected. */
    assert(dart_translate_range(&d4, (1ULL << 40), 2 * SZ_16K) == (void *)0x80000000);
    assert(!dart_translate_range(&d4, (1ULL << 40) | 0x3fffffc000ULL, 2 * SZ_16K));
    /* 1-TTBR 64 GiB boundary crossing must fail silently without logging. */
    assert(dart_translate_range(&d1, 0xfffffc000ULL, SZ_16K) == (void *)0x7fffc000);
    assert(!dart_translate_range(&d1, 0xfffffc000ULL, 2 * SZ_16K));
    assert(lines == 0);

    static u8 buf[256] __attribute__((aligned(8)));
    rtkit_dev_t rtk = {.name = "t", .crashlog_bfr = {.bfr = buf, .sz = sizeof(buf)}};
    struct crashlog_hdr h = {.type = 'CLHE'};
    struct crashlog_entry e = {.type = 'Cstr', .len = 1000};

    /* Oversized entry */
    reset_log();
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &e, sizeof(e));
    rtkit_crashed(&rtk);
    assert(lines > 0 && strstr(logbuf, "bad crashlog entry") != NULL);

    /* Zero-length entry (must not loop) */
    reset_log();
    memset(buf, 'A', sizeof(buf));
    e.len = 0;
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &e, sizeof(e));
    rtkit_crashed(&rtk);
    assert(lines > 0 && strstr(logbuf, "bad crashlog entry") != NULL);

    /* Unterminated string running to end of buffer */
    reset_log();
    memset(buf, 'A', sizeof(buf));
    e.len = sizeof(buf) - sizeof(h);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &e, sizeof(e));
    rtkit_crashed(&rtk);
    assert(lines > 0 && strstr(logbuf, "AAAA") != NULL);
    assert(strstr(logbuf, "bad crashlog entry") == NULL);

    /* Unaligned odd-length entry followed by end sentinel */
    reset_log();
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &h, sizeof(h));
    e.type = 'Cstr';
    e.len = sizeof(e) + 7; /* 23 bytes: odd length -> next entry is unaligned */
    memcpy(buf + sizeof(h), &e, sizeof(e));
    memcpy(buf + sizeof(h) + sizeof(e) + 4, "abc", 3);
    struct crashlog_entry e2 = {.type = 'CLHE', .len = sizeof(e2)};
    memcpy(buf + sizeof(h) + e.len, &e2, sizeof(e2));
    rtkit_crashed(&rtk);
    assert(lines > 0 && strstr(logbuf, "abc") != NULL);
    assert(strstr(logbuf, "bad crashlog entry") == NULL);

    /* Buffer smaller than header */
    reset_log();
    rtk.crashlog_bfr.sz = 8;
    rtkit_crashed(&rtk);
    assert(lines == 2 && strstr(logbuf, "IOP crashed") != NULL &&
           strstr(logbuf, "too small") != NULL);

    puts("rtkit placed buffer: all cases pass");
    return 0;
}
