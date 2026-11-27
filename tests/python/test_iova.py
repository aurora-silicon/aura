# SPDX-License-Identifier: MIT
"""Host tests for the IOVA allocator (src/iova.c).

The production iova.c is compiled unchanged against small host stand-ins for
aura's headers, then driven through fixed cases and a seeded random
alloc/free/reserve sequence checked against a page bitmap model: reserve must
succeed exactly when every page of the range is free and inside the domain.
"""
from pathlib import Path
import shutil
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

STUBS = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
''',
    "dart.h": r'''
#pragma once
#include "types.h"
typedef struct dart_dev dart_dev_t;
void dart_free_l2(dart_dev_t *dart, u64 iova);
''',
    "malloc.h": "#pragma once\n#include <stdlib.h>\n",
    "string.h": "#pragma once\n#include_next <string.h>\n",
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define SZ_16K (16 * 1024)
#define SZ_32M (32 * 1024 * 1024)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define printf(...) ((void)0)
_Noreturn void panic(const char *fmt, ...);
''',
}

HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iova.h"

#define SZ_16K (16 * 1024)
#define SZ_32M (32 * 1024 * 1024)

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

static jmp_buf panic_jmp;
static int expect_panic;

_Noreturn void panic(const char *fmt, ...)
{
    if (expect_panic)
        longjmp(panic_jmp, 1);
    fprintf(stderr, "unexpected panic: %s\n", fmt);
    abort();
}

static unsigned l2_frees, l2_max = -1u;
void dart_free_l2(dart_dev_t *dart, u64 iova)
{
    (void)dart;
    assert(iova % SZ_32M == 0);
    assert(l2_frees < l2_max); /* a walk that never reaches the limit */
    l2_frees++;
}

#define PG SZ_16K

/* The free list must be sorted, non-empty blocks, never touching. */
static void check_list(iova_domain_t *d)
{
    u64 end = 0;
    for (struct iova_block *b = d->free_list; b; b = b->next) {
        assert(b->sz && b->sz % PG == 0 && b->iova % PG == 0);
        assert(b->iova >= (d->base ? d->base : PG));
        assert(b->iova + b->sz <= d->limit);
        if (end)
            assert(b->iova > end); /* sorted and coalesced */
        end = b->iova + b->sz;
    }
}

static unsigned blocks(iova_domain_t *d)
{
    unsigned n = 0;
    for (struct iova_block *b = d->free_list; b; b = b->next)
        n++;
    return n;
}

static void test_init(void)
{
    /* base must be 32M aligned, limit 16K aligned and above the first page */
    assert(!iovad_init(SZ_16K, SZ_32M));
    assert(!iovad_init(0, SZ_16K));
    assert(!iovad_init(0, SZ_32M + 1));
    assert(!iovad_init(SZ_32M, SZ_32M));

    iova_domain_t *d = iovad_init(0, 4 * PG);
    assert(d);
    /* page 0 is never handed out */
    assert(iova_alloc(d, 1) == PG);
    assert(iova_alloc(d, PG) == 2 * PG);
    assert(iova_alloc(d, PG + 1) == 0); /* rounds to 2 pages, only 1 left */
    assert(iova_alloc(d, PG) == 3 * PG);
    assert(iova_alloc(d, PG) == 0);
    assert(!d->free_list);
    iovad_shutdown(d, NULL);

    d = iovad_init(2 * (u64)SZ_32M, 4 * (u64)SZ_32M);
    assert(d);
    assert(iova_alloc(d, 1) == 2 * (u64)SZ_32M);
    l2_frees = 0;
    iovad_shutdown(d, (dart_dev_t *)1);
    assert(l2_frees == 2);
}

static void test_free_coalescing(void)
{
    iova_domain_t *d = iovad_init(0, 64 * PG);
    u64 a = iova_alloc(d, PG), b = iova_alloc(d, PG), c = iova_alloc(d, PG);
    assert(a == PG && b == 2 * PG && c == 3 * PG);

    /* in front of the remaining block, with a gap */
    iova_free(d, a, PG);
    check_list(d);
    assert(blocks(d) == 2);
    /* directly after a free block and with a gap before the next one */
    iova_free(d, b, PG);
    check_list(d);
    assert(blocks(d) == 2);
    /* closes the gap: everything is one block again */
    iova_free(d, c, PG);
    check_list(d);
    assert(blocks(d) == 1);
    assert(iova_alloc(d, 63 * PG) == PG);
    iovad_shutdown(d, NULL);
}

static void test_free_after_last_block(void)
{
    iova_domain_t *d = iovad_init(0, 4 * PG);
    u64 a = iova_alloc(d, PG);
    u64 rest = iova_alloc(d, 2 * PG); /* takes the tail: the list is empty */
    assert(a == PG && rest == 2 * PG && !d->free_list);
    iova_free(d, a, PG);
    /* the freed range lies after the only free block */
    iova_free(d, rest, 2 * PG);
    check_list(d);
    assert(blocks(d) == 1);
    assert(iova_alloc(d, 3 * PG) == PG);
    iovad_shutdown(d, NULL);
}

static void test_reserve(void)
{
    iova_domain_t *d = iovad_init(0, 16 * PG);

    /* page 0 is not part of the domain: reserving it is a no-op */
    assert(iova_reserve(d, 0, PG));
    /* middle split */
    assert(iova_reserve(d, 4 * PG, 2 * PG));
    check_list(d);
    assert(blocks(d) == 2);
    /* front cut */
    assert(iova_reserve(d, PG, PG));
    /* up to the exact end of a block (2..3 of [2,4)) */
    assert(iova_reserve(d, 3 * PG, PG));
    check_list(d);
    /* a whole block that is first in the list; the rest of the list stays */
    assert(iova_reserve(d, 2 * PG, PG));
    check_list(d);
    assert(d->free_list && d->free_list->iova == 6 * PG);
    /* tail of the domain */
    assert(iova_reserve(d, 14 * PG, 2 * PG));
    check_list(d);
    assert(d->free_list->iova == 6 * PG && d->free_list->sz == 8 * PG);
    /* already used, or crossing a block boundary */
    assert(!iova_reserve(d, 4 * PG, PG));
    assert(!iova_reserve(d, 13 * PG, 2 * PG));
    /* unaligned requests are widened to whole pages */
    assert(iova_reserve(d, 6 * PG + 1, 1));
    assert(d->free_list->iova == 7 * PG);
    assert(iova_alloc(d, 7 * PG) == 7 * PG);
    assert(!d->free_list);
    assert(!iova_reserve(d, 8 * PG, PG));
    iovad_shutdown(d, NULL);
}

/* Seeded random sequence against a page bitmap model. */
#define NPAGES 256
static unsigned char used[NPAGES];
static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

struct live {
    u64 iova;
    unsigned pages;
};

static void test_random(void)
{
    iova_domain_t *d = iovad_init(0, NPAGES * (u64)PG);
    struct live live[NPAGES];
    unsigned nlive = 0, allocs = 0, failed = 0, reserved = 0, refused = 0;
    memset(used, 0, sizeof(used));
    used[0] = 1;

    for (int step = 0; step < 200000; step++) {
        if (nlive && (rnd(2) || nlive > 64)) {
            unsigned i = rnd(nlive);
            for (unsigned p = 0; p < live[i].pages; p++)
                used[live[i].iova / PG + p] = 0;
            iova_free(d, live[i].iova, live[i].pages * (u64)PG - rnd(PG));
            live[i] = live[--nlive];
        } else if (!rnd(6)) {
            /* reserve n pages from p (may run past the end), unaligned start */
            unsigned p = 1 + rnd(NPAGES - 1), n = 1 + rnd(8);
            u64 off = rnd(PG);
            bool want = p + n <= NPAGES;
            for (unsigned q = p; want && q < p + n; q++)
                want = !used[q];
            bool ok = iova_reserve(d, p * (u64)PG + off, n * (u64)PG - off);
            assert(ok == want);
            if (!ok) {
                refused++;
            } else {
                for (unsigned q = p; q < p + n; q++)
                    used[q] = 1;
                live[nlive++] = (struct live){p * (u64)PG, n};
                reserved++;
            }
        } else {
            unsigned pages = 1 + rnd(rnd(4) ? 4 : 32);
            u64 iova = iova_alloc(d, pages * (u64)PG - rnd(PG));
            /* the allocator is first fit: it fails only if no run is free */
            unsigned run = 0, first = 0;
            for (unsigned p = 1; p < NPAGES && !first; p++) {
                run = used[p] ? 0 : run + 1;
                if (run == pages)
                    first = p + 1 - pages;
            }
            if (!iova) {
                assert(!first);
                failed++;
                continue;
            }
            assert(iova == first * (u64)PG);
            for (unsigned p = 0; p < pages; p++) {
                assert(!used[first + p]);
                used[first + p] = 1;
            }
            live[nlive++] = (struct live){iova, pages};
            allocs++;
        }
        check_list(d);
    }
    while (nlive) {
        iova_free(d, live[nlive - 1].iova, live[nlive - 1].pages * (u64)PG);
        nlive--;
    }
    check_list(d);
    assert(blocks(d) == 1);
    assert(iova_alloc(d, (NPAGES - 1) * (u64)PG) == PG);
    printf("random: %u allocations, %u refused; %u reserves, %u refused\n", allocs, failed,
           reserved, refused);
    assert(allocs > 10000 && failed > 100 && reserved > 1000 && refused > 1000);
    iovad_shutdown(d, NULL);
}

#define EXPECT_PANIC(call)                                                                         \
    do {                                                                                           \
        expect_panic = 1;                                                                          \
        if (!setjmp(panic_jmp)) {                                                                  \
            call;                                                                                  \
            assert(!"no panic: " #call);                                                           \
        }                                                                                          \
        expect_panic = 0;                                                                          \
    } while (0)

static void test_double_free_panics(void)
{
    iova_domain_t *d = iovad_init(0, 16 * PG);
    u64 a = iova_alloc(d, PG);
    (void)iova_alloc(d, PG);
    iova_free(d, a, PG);
    EXPECT_PANIC(iova_free(d, a, PG));
    iovad_shutdown(d, NULL);

    /* A free that covers a used page and the start of the next free block. */
    d = iovad_init(0, 16 * PG);
    a = iova_alloc(d, PG);
    u64 b = iova_alloc(d, PG);
    assert(a == PG && b == 2 * PG && d->free_list->iova == 3 * PG);
    EXPECT_PANIC(iova_free(d, b, 2 * PG));
    iovad_shutdown(d, NULL);
}

static void test_free_outside_domain_panics(void)
{
    iova_domain_t *d = iovad_init(0, 4 * PG);
    assert(iova_alloc(d, PG) == PG); /* the free list is not empty */
    EXPECT_PANIC(iova_free(d, 100 * PG, PG));
    EXPECT_PANIC(iova_free(d, 0, PG));                  /* page 0 is not in the domain */
    EXPECT_PANIC(iova_free(d, PG, (size_t)-PG));        /* wraps */
    EXPECT_PANIC(iova_free(d, (1ull << 40) | PG, PG));  /* a stray high bit */
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == 2 * PG);
    iova_free(d, PG, PG);
    assert(blocks(d) == 1 && d->free_list->iova == PG);
    iovad_shutdown(d, NULL);

    /* With every page in use no free block can catch these as overlaps. */
    d = iovad_init(0, 4 * PG);
    assert(iova_alloc(d, 3 * PG) == PG && !d->free_list);
    EXPECT_PANIC(iova_free(d, 3 * PG, 2 * PG));         /* ends past the limit */
    EXPECT_PANIC(iova_free(d, 3 * PG, PG + 1));         /* rounds up past the limit */
    EXPECT_PANIC(iova_free(d, PG + 1, PG));             /* not page aligned */
    assert(!d->free_list);
    iova_free(d, 3 * PG, PG - 1);                       /* the last page */
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == 3 * PG && d->free_list->sz == PG);
    iovad_shutdown(d, NULL);

    d = iovad_init(2 * (u64)SZ_32M, 3 * (u64)SZ_32M);
    assert(iova_alloc(d, PG) == 2 * (u64)SZ_32M);
    EXPECT_PANIC(iova_free(d, PG, PG));                 /* below the base */
    EXPECT_PANIC(iova_free(d, 3 * (u64)SZ_32M, PG));    /* at the limit */
    iova_free(d, 2 * (u64)SZ_32M, PG);
    assert(blocks(d) == 1);
    iovad_shutdown(d, NULL);
}

static void test_zero_size(void)
{
    iova_domain_t *d = iovad_init(0, 16 * PG);
    for (int i = 1; i <= 3; i++)
        assert(iova_alloc(d, PG) == i * (u64)PG);
    /* page 2 touches no free block: a zero-size free must not add one */
    iova_free(d, 2 * PG, 0);
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == 4 * PG);

    assert(iova_reserve(d, 0, 0));
    assert(iova_reserve(d, 8 * PG, 0));
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == 4 * PG && d->free_list->sz == 12 * PG);
    iovad_shutdown(d, NULL);
}

static void test_reserve_wrap_and_straddle(void)
{
    iova_domain_t *d = iovad_init(0, 16 * PG);
    /* ranges whose end wraps are refused and leave the list alone */
    assert(!iova_reserve(d, PG, (size_t)-PG));
    assert(!iova_reserve(d, 0, (size_t)-1));
    assert(!iova_reserve(d, (u64)-PG, PG));
    assert(!iova_reserve(d, (u64)-1, 1));
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == PG && d->free_list->sz == 15 * PG);
    assert(iova_alloc(d, PG) == PG);

    /* an unaligned range across a page boundary takes both pages */
    assert(iova_reserve(d, 4 * PG - 1, 2)); /* pages 3 and 4 */
    check_list(d);
    assert(d->free_list->iova == 2 * PG && d->free_list->sz == PG);
    assert(d->free_list->next->iova == 5 * PG);
    /* a range ending inside page 0 reserves nothing */
    assert(iova_reserve(d, 0, PG - 1));
    assert(iova_reserve(d, 0, PG));
    check_list(d);
    assert(blocks(d) == 2);
    iovad_shutdown(d, NULL);
}

static void test_alloc_zero_and_wrapping_size(void)
{
    iova_domain_t *d = iovad_init(0, 4 * PG);
    /* nothing to hand out, and the domain is left alone */
    assert(iova_alloc(d, 0) == 0);
    /* sizes that round up past SIZE_MAX must not become 0 */
    assert(iova_alloc(d, (size_t)-1) == 0);
    assert(iova_alloc(d, (size_t)-PG + 1) == 0);
    assert(iova_alloc(d, (size_t)-PG) == 0); /* page aligned, but far too large */
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == PG && d->free_list->sz == 3 * PG);
    /* the first real allocation still gets the first page, once */
    assert(iova_alloc(d, PG) == PG);
    assert(iova_alloc(d, PG) == 2 * PG);
    iovad_shutdown(d, NULL);
}

static void test_reserve_zero_size_unaligned(void)
{
    iova_domain_t *d = iovad_init(0, 16 * PG);
    /* a zero-size range holds no page, whatever its address */
    assert(iova_reserve(d, 8 * PG + 1, 0));
    assert(iova_reserve(d, 1, 0));
    assert(iova_reserve(d, (u64)-1, 0));
    check_list(d);
    assert(blocks(d) == 1 && d->free_list->iova == PG && d->free_list->sz == 15 * PG);
    iovad_shutdown(d, NULL);
}

static void test_shutdown_at_top_of_address_space(void)
{
    /* The last two L2 tables below 2^64: stepping an address past the limit wraps to 0. */
    u64 base = 0 - 2 * (u64)SZ_32M;
    iova_domain_t *d = iovad_init(base, 0 - (u64)PG);
    assert(d);
    assert(iova_alloc(d, PG) == base);
    l2_frees = 0;
    l2_max = 2;
    iovad_shutdown(d, (dart_dev_t *)1);
    assert(l2_frees == 2);
    l2_max = -1u;

    /* a limit inside an L2 table still frees that table */
    d = iovad_init(2 * (u64)SZ_32M, 3 * (u64)SZ_32M + PG);
    l2_frees = 0;
    iovad_shutdown(d, (dart_dev_t *)1);
    assert(l2_frees == 2);
}

int main(void)
{
    test_init();
    test_free_coalescing();
    test_free_after_last_block();
    test_reserve();
    test_random();
    test_double_free_panics();
    test_free_outside_domain_panics();
    test_zero_size();
    test_reserve_wrap_and_straddle();
    test_alloc_zero_and_wrapping_size();
    test_reserve_zero_size_unaligned();
    test_shutdown_at_top_of_address_space();
    puts("iova: ok");
    return 0;
}
'''


def build(tmp_path, source=None):
    # The stand-ins live in their own include directory so the string.h
    # wrapper can #include_next the host header.
    inc = tmp_path / "inc"
    inc.mkdir(exist_ok=True)
    for name, text in STUBS.items():
        (inc / name).write_text(text)
    shutil.copy(ROOT / "src/iova.h", tmp_path / "iova.h")
    (tmp_path / "iova.c").write_text(source or (ROOT / "src/iova.c").read_text())
    (tmp_path / "harness.c").write_text(HARNESS)
    binary = tmp_path / "iova-host"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-I", str(inc),
         str(tmp_path / "iova.c"), str(tmp_path / "harness.c"), "-o", str(binary)],
        check=True,
    )
    return binary


def test_iova_allocator(tmp_path):
    result = subprocess.run([str(build(tmp_path))], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "iova: ok" in result.stdout
