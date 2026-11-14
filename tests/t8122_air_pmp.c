/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt/libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define T8122 0x8122
static void *dt;
static void *adt;
static u32 chip_id = T8122;
static u32 raw_irqs[] = {622, 621, 624, 623};
static u64 raw_dart = 0x2d0300000, raw_dart_size = 0x4000;
static u32 board_value;
static unsigned fills;
static int adt_path_offset(void *tree, const char *path) { (void)tree; (void)path; return 1; }
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node; assert(!strcmp(name, "interrupts"));
    *len = sizeof(raw_irqs); return raw_irqs;
}
static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree; assert(!strcmp(path, "/arm-io/dart-pmp")); trace[0] = 1; return 1;
}
static int adt_get_reg(void *tree, int *trace, const char *name, int index, u64 *base, u64 *size)
{
    (void)tree; (void)trace; (void)name; assert(!index);
    *base = raw_dart; *size = raw_dart_size; return 0;
}
/* Populate the board identifier only after resource admission. */
static int dt_fill_pmp(int node)
{
    fills++; return fdt_setprop_u32(dt, node, "apple,board-id", board_value);
}
/* SOURCE FUNCTIONS */
static void setup(char *tree, const char *board, bool armed)
{
    dt = tree; fills = 0; raw_dart = 0x2d0300000; raw_dart_size = 0x4000;
    assert(!fdt_create_empty_tree(dt, 4096));
    char root[80]; size_t n = strlen(board) + 1;
    memcpy(root, board, n); memcpy(root + n, "apple,t8122", sizeof("apple,t8122"));
    assert(!fdt_setprop(dt, 0, "compatible", root, n + sizeof("apple,t8122")));
    int chosen = fdt_add_subnode(dt, 0, "chosen"); assert(chosen >= 0);
    if (armed) assert(!fdt_setprop_string(dt, chosen, "asahi,t8122-dcp", "1"));
}
int main(void)
{
    char tree[4096]; const char *boards[] = {"apple,j613", "apple,j615", "apple,j504"};
    for (unsigned b = 0; b < 3; b++) {
        setup(tree, boards[b], true); board_value = 0x40 + b;
        assert(facts_wanted(dt)); assert(!dt_set_pmp_t8122()); assert(fills == 1);
        int node = fdt_path_offset(dt, "/chosen/asahi,t8122-pmp"); assert(node >= 0);
        const fdt32_t *value = fdt_getprop(dt, node, "apple,board-id", NULL);
        assert(value && fdt32_ld(value) == board_value);
        setup(tree, boards[b], false); assert(!facts_wanted(dt));
        assert(!dt_set_pmp_t8122() && !fills);
        setup(tree, boards[b], true); raw_dart += 0x4000;
        assert(!dt_set_pmp_t8122() && !fills);
        node = fdt_path_offset(dt, "/chosen/asahi,t8122-pmp"); assert(node >= 0);
        assert(!fdt_getprop(dt, node, "apple,board-id", NULL));
    }
    /* Other base-M3 boards (the J433/J434 iMacs) stay refused. */
    setup(tree, "apple,j433", true); assert(!facts_wanted(dt));
    assert(!dt_set_pmp_t8122() && !fills);
    setup(tree, "apple,j504", true);
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j504"));
    assert(!facts_wanted(dt)); assert(!dt_set_pmp_t8122() && !fills);
    setup(tree, "apple,j615", true);
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j615"));
    assert(!facts_wanted(dt)); assert(!dt_set_pmp_t8122() && !fills);
    puts("actual Air PMP admission/callers: J613/J615/J504, explicit intent, own data and resource refusal PASS");
}
