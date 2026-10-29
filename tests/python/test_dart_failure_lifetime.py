# SPDX-License-Identifier: MIT
"""A timed-out DART TLB invalidation keeps every table and buffer it can reach.

The production invalidation, map, unmap, L2 release and shutdown bodies from
src/dart.c run against modelled MMIO for the T8020 and T8110 DART types.
Page tables live in a static pool so their addresses fit the PTE format.
"""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


@pytest.mark.parametrize("t8110", [False, True])
def test_failed_invalidation_retains_domain(tmp_path, t8110):
    source = (ROOT / "src/dart.c").read_text()
    # Register layout, domain state and invalidation bodies, through the T8110 parameters.
    definitions = source[source.index("#define DART_T8020_CONFIG "):
                         source.index("/* The two-level Gen3")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
typedef int64_t s64;
typedef struct dart_dev dart_dev_t;
enum dart_type_t { DART_T8020, DART_T8110, DART_T6000, DART_GEN3 };
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzll(m))
#define SZ_16K 0x4000
#define T8152 0x8152
#define dma_wmb() ((void)0)
static int chip_id;
static int failure, writes, polls, frees;
static bool forbidden;
static u64 pool[10][SZ_16K / 8] __attribute__((aligned(SZ_16K)));
static void *freed[10];
static void write32(u64 addr, u32 value) { (void)addr; (void)value; assert(!forbidden); writes++; }
static int poll32(u64 addr, u32 mask, u32 value, unsigned us)
{
    (void)addr; (void)mask; (void)value; (void)us;
    assert(!forbidden);
    polls++;
    if (failure && polls == failure) {
        forbidden = true;
        return -1;
    }
    return 0;
}
static bool is_heap(const void *p)
{
    return (const u8 *)p >= (const u8 *)pool && (const u8 *)p < (const u8 *)pool + sizeof(pool);
}
static void mock_free(void *p)
{
    assert(!forbidden && is_heap(p));
    for (int i = 0; i < frees; i++)
        assert(freed[i] != p);
    freed[frees++] = p;
}
#define free mock_free
static void *memalign(size_t a, size_t s);
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len);
'''
    harness += definitions
    for signature in ("static u64 *dart_get_l2(",
                      "static int dart_map_page(", "int dart_map_flags(",
                      "static void dart_unmap_page(", "bool dart_unmap_checked(",
                      "void dart_unmap(", "void dart_free_l2(", "bool dart_shutdown_checked(",
                      "bool dart_has_failed("):
        harness += function(source, signature)
    harness += r'''
#undef free
static void *memalign(size_t a, size_t s) { (void)a; (void)s; assert(0); return NULL; }
static void reset(void) { failure = writes = polls = frees = 0; forbidden = false; memset(pool, 0, sizeof(pool)); }
static dart_dev_t *domain(const struct dart_params *params)
{
    /* Roots in pool[0..3], their L2 tables in pool[4..7], the domain in pool[9]. */
    struct dart_dev *dart = (struct dart_dev *)pool[9];
    dart->regs = 0x10000;
    dart->device = 1;
    dart->params = params;
    for (int i = 0; i < params->ttbr_count; i++)
        dart->l1[i] = pool[i];
    /* Each root has one L2 table of its own. */
    for (int i = 0; i < params->ttbr_count; i++)
        pool[i][0] = FIELD_PREP(params->offset_mask, (u64)pool[4 + i] >> DART_PTE_OFFSET_SHIFT) |
                     DART_PTE_VALID;
    return dart;
}
static void expect_refused(dart_dev_t *dart)
{
    int before = writes + polls + frees;
    u64 root = dart->l1[0][0];
    forbidden = true;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == -1);
    assert(!dart_unmap_checked(dart, 0x4000, SZ_16K));
    dart_unmap(dart, 0x4000, SZ_16K);
    dart_free_l2(dart, 0);
    assert(!dart_shutdown_checked(dart));
    assert(dart_has_failed(dart));
    assert(before == writes + polls + frees && dart->l1[0][0] == root);
    forbidden = false;
}
#ifdef TEST_T8110
#define PARAMS (&dart_t8110)
#else
#define PARAMS (&dart_t8020)
#endif
int main(void)
{
    dart_dev_t *dart;

    /* Success: every table is freed exactly once, including those under later roots. */
    reset();
    dart = domain(PARAMS);
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == 0 && !dart_has_failed(dart));
    assert(dart_unmap_checked(dart, 0x4000, SZ_16K));
    assert(dart_shutdown_checked(dart));
    assert(frees == 2 * PARAMS->ttbr_count + 1);

    /* Map invalidation timeout: the mapping stays, later operations are refused. */
    reset();
    dart = domain(PARAMS);
    failure = 1;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == -1);
    assert(dart_has_failed(dart) && !frees && (pool[4][1] & DART_PTE_VALID));
    expect_refused(dart);

    /* Unmap invalidation timeout: the caller must not reuse the pages. */
    reset();
    dart = domain(PARAMS);
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == 0);
    failure = 2;
    assert(!dart_unmap_checked(dart, 0x4000, SZ_16K));
    assert(dart_has_failed(dart) && !frees);
    expect_refused(dart);

    /* Shutdown invalidation timeout: the stream is disabled but nothing is freed. */
    reset();
    dart = domain(PARAMS);
    failure = 1;
    assert(!dart_shutdown_checked(dart));
    assert(!frees && (pool[0][0] & DART_PTE_VALID));
    expect_refused(dart);

    /* No domain, or a misaligned range, never touches MMIO. */
    reset();
    forbidden = true;
    assert(dart_has_failed(NULL) && !dart_unmap_checked(NULL, 0x4000, SZ_16K));
    assert(!dart_shutdown_checked(NULL));
    forbidden = false;
    dart = domain(PARAMS);
    forbidden = true;
    assert(!dart_unmap_checked(dart, 0x4001, SZ_16K));
    assert(!dart_unmap_checked(dart, 0x4000, SZ_16K - 1));
    assert(!writes && !polls && !dart_has_failed(dart));
    return 0;
}
'''
    binary = tmp_path / "dart-failure"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable",
                    *(["-DTEST_T8110"] if t8110 else []), "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
