# SPDX-License-Identifier: MIT
"""Gen3 TLB invalidation reports a stall, and dart_free_l2() stays under its own root.

The production bodies from src/dart.c run against modelled MMIO and a modelled
system counter. Only the aarch64 aperture write + TLBI broadcast is replaced, by
a stub that records the payload and advances the counter by a chosen delay.
Page tables live in a static pool so their addresses fit the PTE formats.
"""
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


def replace_body(text, signature, body):
    assert text.count(signature) == 1
    start = text.index(signature)
    end = text.index("\n}\n", start) + 3
    return text[:start] + signature + "\n{\n" + body + "}\n" + text[end:]


HEADER = r'''
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
static int chip_id = T8152;
static int writes, polls, frees, broadcasts;
static u64 ticks, delay;
static uintptr_t last_aperture;
static u64 last_lo, last_hi;
static u64 pool[10][SZ_16K / 8] __attribute__((aligned(SZ_16K)));
static void *freed[10];
static void write32(u64 addr, u32 value) { (void)addr; (void)value; writes++; }
static int poll32(u64 addr, u32 mask, u32 value, unsigned us)
{
    (void)addr; (void)mask; (void)value; (void)us;
    polls++;
    return 0;
}
/* One counter tick is one microsecond. */
static u64 get_ticks(void) { return ticks; }
static u64 ticks_to_usecs(u64 t) { return t; }
static bool is_heap(const void *p)
{
    return (const u8 *)p >= (const u8 *)pool && (const u8 *)p < (const u8 *)pool + sizeof(pool);
}
static void mock_free(void *p)
{
    assert(is_heap(p));
    for (int i = 0; i < frees; i++)
        assert(freed[i] != p);
    freed[frees++] = p;
}
#define free mock_free
static void *memalign(size_t a, size_t s);
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len);
'''

TAIL = r'''
#undef free
/* A free path must never allocate a table. */
static void *memalign(size_t a, size_t s) { (void)a; (void)s; assert(!"memalign"); return NULL; }
static void reset(void)
{
    writes = polls = frees = broadcasts = 0;
    ticks = delay = 0;
    last_aperture = 0;
    last_lo = last_hi = 0;
    memset(pool, 0, sizeof(pool));
    memset(freed, 0, sizeof(freed));
}
static u64 table(const struct dart_params *params, void *p)
{
    return FIELD_PREP(params->offset_mask, (u64)p >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
}
/* Roots in pool[0..3], L2 tables in pool[4..7], a data page in pool[8], the domain in pool[9]. */
static dart_dev_t *domain(const struct dart_params *params, enum dart_type_t type)
{
    struct dart_dev *dart = (struct dart_dev *)pool[9];
    dart->regs = 0x10000;
    dart->device = 1;
    dart->type = type;
    dart->params = params;
    for (int i = 0; i < params->ttbr_count; i++)
        dart->l1[i] = pool[i];
    return dart;
}
'''

GEN3_STUB = r'''    broadcasts++;
    last_aperture = aperture;
    last_lo = lo;
    last_hi = hi;
    ticks += delay;
'''


def build(tmp_path, name, definitions, main):
    harness = HEADER + definitions + TAIL + main
    binary = tmp_path / name
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable",
                    # The PTE address fields end at bit 37/39: a Linux PIE puts the pool near 2^47.
                    *(["-no-pie"] if sys.platform.startswith("linux") else []),
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


def dart_source():
    source = (ROOT / "src/dart.c").read_text()
    # Register layout, domain state, all three invalidation bodies and every parameter set.
    definitions = source[source.index("#define DART_T8020_CONFIG "):
                         source.index("static dart_dev_t *dart_gen3_adopt_adt(")]
    # The aperture write and TLBI broadcast are aarch64 instructions: model only that.
    definitions = replace_body(
        definitions,
        "static inline void dart_gen3_tlbi_broadcast(uintptr_t aperture, u64 lo, u64 hi)",
        GEN3_STUB)
    for signature in ("static u64 *dart_get_l2(",
                      "static int dart_map_page(", "int dart_map_flags(",
                      "static void dart_unmap_page(", "bool dart_unmap_checked(",
                      "void dart_unmap(", "void dart_free_l2(", "bool dart_shutdown_checked(",
                      "bool dart_has_failed("):
        definitions += function(source, signature)
    return definitions


def test_gen3_invalidation_reports_a_stall(tmp_path):
    build(tmp_path, "dart-gen3-tlbi", dart_source(), r'''
static dart_dev_t *gen3(void)
{
    dart_dev_t *dart = domain(&dart_gen3, DART_GEN3);
    dart->device = 23;
    dart->tlbi_route = 3;
    dart->tlbi_aperture = 0x2f0000040;
    pool[0][0] = table(&dart_gen3, pool[4]);
    return dart;
}
int main(void)
{
    dart_dev_t *dart;

    /* A prompt broadcast: one aperture write with the SID and route, the domain stays usable. */
    reset();
    dart = gen3();
    delay = 3;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == 0 && !dart_has_failed(dart));
    assert(broadcasts == 1 && last_aperture == 0x2f0000040 && last_hi == 0);
    assert(last_lo == ((1ULL << 60) | (3ULL << 56) | (23ULL << 48)));
    assert(pool[4][1] & DART_PTE_VALID);
    assert(dart_unmap_checked(dart, 0x4000, SZ_16K) && broadcasts == 2 && !pool[4][1]);

    /* Exactly at the bound still counts as complete. */
    reset();
    dart = gen3();
    delay = DART_GEN3_TLBI_TIMEOUT_US;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == 0 && !dart_has_failed(dart));

    /* A stalled map invalidation fails the domain and keeps the mapping, as on T8020/T8110. */
    reset();
    dart = gen3();
    delay = DART_GEN3_TLBI_TIMEOUT_US + 1;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == -1);
    assert(dart_has_failed(dart) && (pool[4][1] & DART_PTE_VALID) && broadcasts == 1);
    /* Every later operation is refused without touching the DART or freeing anything. */
    u64 root = pool[0][0];
    assert(dart_map_flags(dart, 0x8000, pool[8], SZ_16K, 0) == -1);
    assert(!dart_unmap_checked(dart, 0x4000, SZ_16K));
    dart_unmap(dart, 0x4000, SZ_16K);
    dart_free_l2(dart, 0);
    assert(!dart_shutdown_checked(dart));
    assert(broadcasts == 1 && !frees && !writes && pool[0][0] == root && (pool[4][1] & DART_PTE_VALID));

    /* A stalled unmap invalidation: the caller must not reuse the page. */
    reset();
    dart = gen3();
    delay = 1;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == 0);
    delay = 5000;
    assert(!dart_unmap_checked(dart, 0x4000, SZ_16K) && dart_has_failed(dart) && !frees);

    /* A stalled shutdown invalidation: the stream is disabled but no table is freed. */
    reset();
    dart = gen3();
    dart->l1[0] = pool[0];
    delay = 5000;
    assert(!dart_shutdown_checked(dart));
    assert(dart_has_failed(dart) && !frees && (pool[0][0] & DART_PTE_VALID));

    /* No usable aperture: nothing is broadcast and the domain is failed. */
    reset();
    dart = gen3();
    dart->tlbi_aperture = 0;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == -1);
    assert(dart_has_failed(dart) && !broadcasts);
    reset();
    dart = gen3();
    dart->tlbi_aperture = 0x2f0000048;
    assert(dart_map_flags(dart, 0x4000, pool[8], SZ_16K, 0) == -1);
    assert(dart_has_failed(dart) && !broadcasts);
    return 0;
}
''')


@pytest.mark.parametrize("params", ["dart_t8020", "dart_t6000", "dart_t8110"])
def test_free_l2_uses_the_iova_root(tmp_path, params):
    build(tmp_path, "dart-free-l2-" + params, dart_source(), r'''
#define PARAMS (&%s)
int main(void)
{
    dart_dev_t *dart;
    int roots = PARAMS->ttbr_count;

    /* Root 0: an empty table is freed, a table with a live page is kept. */
    reset();
    dart = domain(PARAMS, DART_T8020);
    pool[0][0] = table(PARAMS, pool[4]);
    pool[4][1] = table(PARAMS, pool[8]);
    dart_free_l2(dart, 0);
    assert(!frees && (pool[0][0] & DART_PTE_VALID));
    pool[4][1] = 0;
    dart_free_l2(dart, 0);
    assert(frees == 1 && freed[0] == pool[4] && !pool[0][0]);

    /* A misaligned address and an empty L1 entry free nothing. */
    reset();
    dart = domain(PARAMS, DART_T8020);
    dart_free_l2(dart, 0x4000);
    dart_free_l2(dart, 1UL << 25);
    assert(!frees);

    /* An address under a root this DART does not have is ignored. */
    reset();
    dart = domain(PARAMS, DART_T8020);
    for (int r = roots; r < 4; r++)
        dart_free_l2(dart, (u64)r << 36);
    assert(!frees);

    if (roots < 4)
        return 0;

    /* Root 1's table is released even when root 0's table at the same L1 index has a live
     * page, and root 0 is left alone. */
    reset();
    dart = domain(PARAMS, DART_T8020);
    pool[0][0] = table(PARAMS, pool[4]);
    pool[4][1] = table(PARAMS, pool[8]);
    pool[1][0] = table(PARAMS, pool[5]);
    dart_free_l2(dart, 1UL << 36);
    assert(frees == 1 && freed[0] == pool[5] && !pool[1][0]);
    assert(pool[0][0] == table(PARAMS, pool[4]) && pool[4][1]);

    /* With no table under root 0 nothing is allocated there (memalign asserts). */
    reset();
    dart = domain(PARAMS, DART_T8020);
    pool[2][5] = table(PARAMS, pool[6]);
    dart_free_l2(dart, (2UL << 36) | (5UL << 25));
    assert(frees == 1 && freed[0] == pool[6] && !pool[2][5] && !pool[0][5]);

    /* A live page under root 3 keeps root 3's table; root 0's empty table is untouched. */
    reset();
    dart = domain(PARAMS, DART_T8020);
    pool[0][7] = table(PARAMS, pool[4]);
    pool[3][7] = table(PARAMS, pool[7]);
    pool[7][9] = table(PARAMS, pool[8]);
    dart_free_l2(dart, (3UL << 36) | (7UL << 25));
    assert(!frees && pool[3][7] && pool[0][7]);
    return 0;
}
''' % params)
