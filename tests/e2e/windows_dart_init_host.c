/* SPDX-License-Identifier: MIT */
/* Independent allocation/MMIO model around the unmodified constructor body. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x) (1ULL << (x))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define FIELD_PREP(mask, value) (((u64)(value) << __builtin_ctzll(mask)) & (mask))
#define SZ_16K 16384
#define DART_MAX_TTBR_COUNT 4
#define DART_T8020_CONFIG 0
#define DART_T8020_CONFIG_LOCK BIT(15)
#define DART_T8020_ENABLED_STREAMS 4
#define DART_T8110_PROTECT 8
#define DART_T8110_PROTECT_TTBR_TCR 1
#define DART_T8110_ENABLE_STREAMS 12
#define DART_TCR(dart) 16
#define DART_TTBR(dart, i) (32 + 4 * (i))
enum dart_type_t { DART_T8020, DART_T8110, DART_T6000, DART_GEN3 };
struct params { unsigned sid_count; int ttbr_count; u32 ttbr_valid; u64 ttbr_addr;
    unsigned ttbr_shift; u32 tcr_enabled; void (*tlb_invalidate)(void *); };
typedef struct { uintptr_t regs; u8 device; enum dart_type_t type; bool keep, locked;
    const struct params *params; u64 *l1[4]; } dart_dev_t;
static void *allocated[5];
static unsigned allocations, fail_at, frees, writes, tlbis;
static u32 regs[16];
static bool cleaned[5], published;
static void model_sysop(const char *op) {
    assert(!strcmp(op, "dsb sy"));
    for (unsigned i = 1; i <= allocations; i++) assert(cleaned[i]);
    published = true;
}
#define sysop(op) model_sysop(op)
static void invalidate(void *p) { (void)p; ++tlbis; }
static struct params dart_t8020 = {16,4,1,0xfffffffcULL,14,1,invalidate};
static struct params dart_t6000 = {16,4,1,0xfffffffcULL,14,1,invalidate};
static struct params dart_t8110 = {256,1,1,0xfffffffcULL,14,1,invalidate};
static u32 read32(uintptr_t address) { assert(address / 4 < 16); return regs[address / 4]; }
static void write32(uintptr_t address, u32 value) {
    assert(address / 4 < 16);
    if (address == DART_T8020_ENABLED_STREAMS || address == DART_T8110_ENABLE_STREAMS)
        assert(regs[8] & 1); /* A valid root must precede enabling DMA. */
    if (address >= 32 && address < 48) assert(published);
    regs[address / 4] = value; ++writes;
}
static void set32(uintptr_t address, u32 value) { write32(address, read32(address) | value); }
static void *model_calloc(size_t count, size_t size) {
    void *p = calloc(count, size); assert(p); allocated[0] = p; return p;
}
static void *model_memalign(size_t align, size_t size) {
    ++allocations;
    if (allocations == fail_at) return NULL;
    void *p = NULL; assert(!posix_memalign(&p, align, size));
    assert(allocations < 5); allocated[allocations] = p; memset(p, 0xaa, size); return p;
}
static void model_free(void *p) {
    for (unsigned i = 0; i < 5; i++) if (p && allocated[i] == p) {
        free(p); allocated[i] = NULL; ++frees; return;
    }
    assert(!"free of embedded array or inherited root");
}
static __attribute__((unused)) void dc_cvac_range(void *p, size_t size) {
    for (unsigned i = 1; i < 5; i++) if (allocated[i] == p) {
        assert(size == SZ_16K);
        for (size_t j = 0; j < size; j++) assert(((u8 *)p)[j] == 0);
        cleaned[i] = true; return;
    }
    assert(!"clean of non-owned root");
}
#define calloc model_calloc
#define memalign model_memalign
#define free model_free
#include "dart_init_body.inc"
#undef calloc
#undef free
int main(int argc, char **argv) {
    assert(argc == 4);
    enum dart_type_t type = (enum dart_type_t)atoi(argv[1]); fail_at = atoi(argv[2]);
    unsigned mode = atoi(argv[3]); /* cold, locked sparse roots, keep sparse roots */
    unsigned count = type == DART_T8110 ? 1 : 4;
    if (mode) regs[8] = 0x101; /* inherited root must never be cleaned/freed */
    if (mode == 1) regs[(type == DART_T8110 ? 8 : 0) / 4] =
        type == DART_T8110 ? 1 : BIT(15);
    u32 original[16]; memcpy(original, regs, sizeof(regs));
    dart_dev_t *dart = dart_init(0, 0, mode == 2, type);
    unsigned expected = mode == 1 ? 0 : count - (mode == 2);
    if (fail_at && fail_at <= expected) {
        assert(!dart && !writes && !tlbis && frees == fail_at);
        assert(!memcmp(regs, original, sizeof(regs)));
    } else {
        assert(dart && allocations == expected);
        assert(tlbis == (mode == 0));
        if (mode == 1) assert(!writes && !memcmp(regs, original, sizeof(regs)));
        for (unsigned i = 1; i <= expected; i++) {
            assert(cleaned[i]); model_free(allocated[i]);
        }
        model_free(dart);
    }
    puts("PASS allocation publication ordering and inherited ownership");
}
