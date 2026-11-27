# SPDX-License-Identifier: MIT
"""RTKit owns buffers until the production DART invalidation completes."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


@pytest.fixture(scope="module")
def lifetime_binary(tmp_path_factory):
    rtkit = (ROOT / "src/rtkit.c").read_text()
    dart = (ROOT / "src/dart.c").read_text()
    header = (ROOT / "src/rtkit.h").read_text()
    buffer_start = header.index("struct rtkit_buffer {")
    buffer = header[buffer_start:header.index("\n};", buffer_start) + 3]
    definitions = dart[dart.index("#define DART_T8020_CONFIG "):
                       dart.index("struct dart_params")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzl(m)) & (m))
#define SZ_16K 0x4000
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define dma_wmb() ((void)0)
#define rtkit_printf(...) ((void)0)
#define IOVA_MASK ((1ULL << 36) - 1)
typedef struct dart_dev dart_dev_t;
struct dart_params { void (*tlb_invalidate)(dart_dev_t *); };
struct dart_dev {
    bool failed, locked, legacy_mutable;
    uintptr_t regs;
    unsigned device;
    const struct dart_params *params;
};
typedef int iova_domain_t;
typedef int sart_dev_t;
''' + definitions + buffer + r'''
typedef struct {
    char *name;
    dart_dev_t *dart;
    iova_domain_t *dart_iovad;
    sart_dev_t *sart;
    u64 dva_base;
    struct rtkit_buffer syslog_bfr, crashlog_bfr, ioreport_bfr, oslog_bfr;
} rtkit_dev_t;
static bool fail_tlbi, reject_map, fail_iova, fail_sart_add, fail_sart_remove;
static unsigned allocations, physical_frees, owner_frees, iova_allocs, iova_frees;
static unsigned writes, polls, published, unmapped, sart_adds, sart_removes;
static void *backing;
static bool dart_has_failed(dart_dev_t *d) { return !d || d->failed; }
static bool dart_locked_pt_writable(dart_dev_t *d) { return false; }
static void write32(u64 address, u32 value) { writes++; }
static int poll32(u64 address, u32 mask, u32 value, unsigned timeout)
{
    polls++;
    return fail_tlbi ? -1 : 0;
}
static int dart_map_page(dart_dev_t *d, uintptr_t iova, uintptr_t pa, u32 flags)
{
    if (reject_map)
        return -1;
    published++;
    return 0;
}
static void dart_unmap_page(dart_dev_t *d, uintptr_t iova) { unmapped++; }
static void *memalign(size_t alignment, size_t size)
{
    allocations++;
    backing = aligned_alloc(alignment, size);
    assert(backing);
    memset(backing, 0xa5, size);
    return backing;
}
static void mock_free(void *p)
{
    if (p == backing) {
        physical_frees++;
        free(p);
    } else {
        owner_frees++;
    }
}
#define free mock_free
static u64 iova_alloc(iova_domain_t *d, size_t size)
{
    iova_allocs++;
    assert(size == SZ_16K);
    return fail_iova ? 0 : 0x4000;
}
static void iova_free(iova_domain_t *d, u64 address, size_t size)
{
    assert(address == 0x4000 && size == SZ_16K);
    iova_frees++;
}
static bool sart_add_allowed_region(sart_dev_t *s, void *p, size_t size)
{
    sart_adds++;
    assert(p == backing && size == SZ_16K);
    return !fail_sart_add;
}
static bool sart_remove_allowed_region(sart_dev_t *s, void *p, size_t size)
{
    sart_removes++;
    assert(p == backing && size == SZ_16K);
    return !fail_sart_remove;
}
void dart_unmap(dart_dev_t *, uintptr_t, size_t);
bool rtkit_free_buffer(rtkit_dev_t *, struct rtkit_buffer *);
'''
    for signature in ("static void dart_t8110_tlb_invalidate(", "int dart_map_flags(",
                      "int dart_map(", "bool dart_unmap_checked(", "void dart_unmap("):
        harness += function(dart, signature)
    for signature in ("bool rtkit_map(", "bool rtkit_unmap(", "bool rtkit_alloc_buffer(",
                      "bool rtkit_free_buffer(", "void rtkit_free("):
        harness += function(rtkit, signature)
    harness += r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    int mode = atoi(argv[1]);
    struct dart_params params = {.tlb_invalidate = dart_t8110_tlb_invalidate};
    dart_dev_t dart = {.regs = 0x1000, .device = 1, .params = &params};
    rtkit_dev_t rtk = {.name = "test", .dart = &dart, .dva_base = 1ULL << 40};
    struct rtkit_buffer *bfr = &rtk.syslog_bfr;
    if (mode == 3 || mode == 4 || mode == 5) {
        rtk.dart = NULL;
        rtk.sart = (sart_dev_t *)1;
    }
    fail_tlbi = mode == 1;
    reject_map = mode == 6;
    fail_iova = mode == 7;
    fail_sart_add = mode == 4;
    bool allocated = rtkit_alloc_buffer(&rtk, bfr, 1);
    if (mode == 4 || mode == 6 || mode == 7) {
        struct rtkit_buffer empty = {0};
        assert(!allocated && !memcmp(bfr, &empty, sizeof(empty)));
        assert(allocations == 1 && physical_frees == 1 && !published);
        assert(iova_frees == (mode == 6));
        assert(!dart.failed);
        return 0;
    }
    assert(allocated == (mode != 1));
    assert(bfr->bfr == backing && bfr->owned && bfr->sz == SZ_16K);
    assert(bfr->dva == (rtk.dart ? (0x4000 | rtk.dva_base) : (u64)backing));
    for (size_t i = 0; i < bfr->sz; i++)
        assert(((u8 *)backing)[i] == 0);
    if (mode == 1 || mode == 2 || mode == 5 || mode >= 8) {
        fail_tlbi = mode == 1 || mode == 2;
        fail_sart_remove = mode == 5;
        dart.locked = mode == 8;
        if (mode == 9)
            bfr->sz--;
        if (mode == 10)
            bfr->dva++;
        unsigned before_unmap = writes + polls + unmapped;
        struct rtkit_buffer saved = *bfr;
        assert(!rtkit_free_buffer(&rtk, bfr));
        assert(!memcmp(bfr, &saved, sizeof(saved)));
        assert(!physical_frees && !iova_frees);
        if (mode >= 8) {
            assert(!dart.failed && before_unmap == writes + polls + unmapped);
        }
        if (mode == 1 || mode == 2) {
            unsigned before = allocations + iova_allocs + writes + polls + unmapped;
            assert(dart_has_failed(&dart));
            assert(!rtkit_alloc_buffer(&rtk, bfr, 1));
            u64 dva = 0;
            assert(!rtkit_map(&rtk, backing, SZ_16K, &dva));
            assert(!rtkit_free_buffer(&rtk, bfr));
            assert(!memcmp(bfr, &saved, sizeof(saved)));
            assert(before == allocations + iova_allocs + writes + polls + unmapped);
        }
        rtkit_free(&rtk);
        assert(!physical_frees && !owner_frees && !iova_frees);
        assert(!memcmp(bfr, &saved, sizeof(saved)));
    } else {
        assert(rtkit_free_buffer(&rtk, bfr));
        struct rtkit_buffer empty = {0};
        assert(!memcmp(bfr, &empty, sizeof(empty)) && physical_frees == 1);
        assert(iova_frees == (mode == 0));
        assert(sart_adds == (mode == 3) && sart_removes == (mode == 3));
        rtkit_free(&rtk);
        assert(owner_frees == 2 && physical_frees == 1);
    }
    return 0;
}
'''
    binary = tmp_path_factory.mktemp("rtkit-lifetime") / "fixture"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", str(ROOT / "src"),
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    return binary


@pytest.mark.parametrize("case", range(11), ids=[
    "healthy-dart", "map-tlbi-timeout", "unmap-tlbi-timeout", "healthy-sart",
    "sart-map-rejected", "sart-unmap-rejected", "map-before-publication", "iova-exhausted",
    "locked-domain", "unaligned-size", "unaligned-iova",
])
def test_buffer_and_iova_ownership(lifetime_binary, case):
    subprocess.run([str(lifetime_binary), str(case)], check=True, timeout=5)
