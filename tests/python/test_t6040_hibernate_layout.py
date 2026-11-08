# SPDX-License-Identifier: MIT
"""T6040 hib=1: the fixed hibernation layout under the padded Stage 1 (EL2, CTXR exec window).

The whole hibernation block of kboot.c (layout decision, refusals, reservations, /chosen
properties, withdrawal and the device tree slot copy) and heapblock_raise_base() are the
production bodies, run against real libfdt. The machine is modeled on the J616s: boot arguments,
the iBoot memory and carveout maps (the values of a J616s ADT), the CTXR registers and where
iBoot loads the boot object. The order of the hooks in kboot_prepare_dt(), dt_set_chosen() and
kboot_boot() is checked on the source; the model runs the same hooks in that order.
"""
import re
import subprocess

import pytest

from test_locked_dart_tables import function
from test_smp_timeouts import REPO


def hibernation_block(kboot):
    start = kboot.index("#define HIB_RAM_ALIGN")
    return kboot[start:kboot.index("static int dt_set_chosen(void)\n{")]


def prepare_dt_end_check(kboot):
    body = function(kboot, "kboot_prepare_dt")
    check = re.search(r"    if \(hib_heap_end && heapblock_high_water\(\) > hib_heap_end - "
                      r"HIB_HEAP_MARGIN &&\n        kboot_withdraw_hibernate_layout\([^\n]*\)\)\n"
                      r"        return -1;\n", body)
    assert check, "kboot_prepare_dt() lost its heap check against the fixed end"
    return check.group(0)


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "libfdt.h"
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint8_t u8;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define T6040 0x6040
#define T6041 0x6041
#define SZ_16K 0x4000UL
#define SZ_1M  0x100000UL
#define SZ_32M 0x2000000UL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define sysop(op) ((void)0)
static void dc_cvac_range(void *p, size_t n) { (void)p; (void)n; }
static char log_text[16384];
#define printf(...) snprintf(log_text + strlen(log_text), sizeof(log_text) - strlen(log_text), __VA_ARGS__)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)

/* The machine */
static int chip_id;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static u64 boot_args_addr, aura_base, aura_end;
#define _base ((char *)aura_base)
#define _end ((char *)aura_end)
''' + "%(ranges)s" + r'''
static bool confined;
static u64 window_base, window_size;
static bool ctxr_exec_window(u64 *base, u64 *size)
{
    if (!confined)
        return false;
    if (base)
        *base = window_base;
    if (size)
        *size = window_size;
    return true;
}
/* ctxr_exec.c's fixed window, modelled: test_t6040_hibernate_fixed_window.py runs the real one. */
static bool fixed_asked, window_fixed;
static bool ctxr_exec_request_fixed_window(bool on) { fixed_asked = on; return true; }
static bool ctxr_exec_window_is_fixed(void) { return confined && window_fixed; }
/* [start, end) of each armed CTXR/CTRR range, as ctxr_range() reports them; end 0: not armed */
static u64 armed[6][2];
static bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end)
{
    if (!armed[range][1])
        return false;
    *start = armed[range][0];
    *end = armed[range][1];
    return true;
}
/* T6040 has no firmware RO range of its own */
static int memory_fw_ro_range(u64 *start, u64 *end) { *start = *end = 0; return 0; }
struct mcc_carveout { u64 base, size; };
static struct mcc_carveout mcc_carveouts[4];
static size_t mcc_carveout_count;

/* heapblock */
static void *heap_base;
static uintptr_t heapblock_high_water(void) { return (uintptr_t)heap_base; }
''' + "%(raise_base)s" + r'''

/* The ADT: /chosen/memory-map (node 1) and /chosen/carveout-memory-map (node 2) */
struct adt_property { char name[32]; u32 size; u8 value[16]; };
static struct adt_property adt_maps[3][64];
static int map_count[3];
static bool maps_present;
static void *adt;
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!maps_present)
        return -1;
    if (!strcmp(path, "/chosen/memory-map"))
        return 1;
    assert(!strcmp(path, "/chosen/carveout-memory-map"));
    return 2;
}
#define ADT_FOREACH_PROPERTY(tree, node, prop)                                               \
    for (int _i = 0; _i < map_count[node]; _i++)                                             \
        for (const struct adt_property *prop = &adt_maps[node][_i]; prop; prop = NULL)
static void map_entry(int node, const char *name, u64 base, u64 size)
{
    struct adt_property *p = &adt_maps[node][map_count[node]++];
    u64 r[2] = {base, size};
    snprintf(p->name, sizeof(p->name), "%%s", name);
    p->size = sizeof(r);
    memcpy(p->value, r, sizeof(r));
}
static void map_move(int node, const char *name, u64 base, u64 size)
{
    for (int i = 0; i < map_count[node]; i++) {
        if (!strcmp(adt_maps[node][i].name, name)) {
            u64 r[2] = {base, size};
            memcpy(adt_maps[node][i].value, r, sizeof(r));
            return;
        }
    }
    assert(0);
}

/* The device tree kboot builds */
static _Alignas(8) unsigned char blob[4 << 20];
static void *dt = blob;
''' + "%(block)s" + r'''

/*
 * The J616s (Mac16,7, 48 GiB): boot arguments and maps as its iBoot gives them. The ADT values are
 * those of a J616s ADT; where iBoot loads the boot object and what follows it are modeled.
 */
#define RAM      0x100032b8000UL
#define RAM_SIZE 0xbc98d4000UL
#define LO       0x10004000000UL /* the 32 MiB aligned start of RAM */
#define SLOT     (LO + 320 * SZ_1M)
#define END      (SLOT + 128 * SZ_1M)
/* aurora-installer's padded Stage 1: 64K of headers, aura's 16 MiB slot, 2 MiB - 16K, the window */
#define PADDED   (0x10000UL + 16 * SZ_1M + 2 * SZ_1M - SZ_16K + 128 * SZ_1M)
#define TAIL     (3 * SZ_16K)
/* The fixed window ctxr_exec.c gives this object on every placement */
#define FIXED     (LO + 0x4200000UL)
#define FIXED_END (LO + 0xa200000UL)
/* Measured on the EL2 boots, above the object's load base: CTXR_D's start, the end of what
 * iBoot placed (boot_args' top_of_kernel_data) and this stage's heap when the tree was allocated */
#define BELOW     0xf4000UL
#define TOP_OFF   0x98c0000UL
#define HEAP_OFF  0xf064000UL

static unsigned char input[1 << 16];

static void reset(void)
{
    memset(armed, 0, sizeof(armed));
    memset(map_count, 0, sizeof(map_count));
    mcc_carveout_count = 0;
    maps_present = true;
    log_text[0] = 0;
    hib_requested = hib_decided = false;
    fixed_asked = false;
    window_fixed = true;
    hib_dt_base = hib_heap_base = hib_heap_end = hib_window_base = hib_window_end = 0;
    chip_id = T6040;
    cur_boot_args.phys_base = RAM;
    cur_boot_args.mem_size = RAM_SIZE;
    map_entry(2, "region-id-1", 0x10000000000UL, 0x4000);
    map_entry(2, "region-id-25", 0x10001c18000UL, 0x16a0000);
    map_entry(2, "region-id-24", RAM, RAM_SIZE);
    map_entry(2, "region-id-45", 0x10003000000UL, 0x20000000);
    map_entry(2, "region-id-46", 0x10023000000UL, 0x1e000000);
    map_entry(2, "region-id-48", 0x10041100000UL, 0x20000000);
    map_entry(2, "region-id-167", 0x10074004000UL, 0x9c00000);
    map_entry(2, "region-id-14", 0x10bd1474000UL, 0x72c4000);
    map_entry(2, "region-id-198", 0x383040000UL, 0x20000);
    map_entry(1, "MemoryMapReserved-0", 0, 0);
    map_entry(1, "BootKC-rm", ~0UL, ~0UL);
    map_entry(1, "BootKC-entry", 0x82e8800, 0);
    mcc_carveouts[mcc_carveout_count++] = (struct mcc_carveout){0x10bd9ce0000UL, 0x25800000};
}

/*
 * A boot with iBoot's object at @load: what iBoot places after it ends @top above @load (Stage 1's
 * heap with the staged Stage 2 and its firmware block start there), and this stage's heap is at
 * @heap above @load when kboot_prepare_dt() runs. The CTXR exec window is the fixed one.
 */
static void boot_at(u64 load, u64 top_off, u64 heap_off)
{
    reset();
    u64 object_end = load + PADDED + TAIL;
    aura_base = load + 0x10000;
    aura_end = aura_base + 0x4a0000;
    confined = true;
    window_base = FIXED;
    window_size = FIXED_END - FIXED;
    armed[CTXR_RANGE_D][0] = load - BELOW;
    armed[CTXR_RANGE_D][1] = load + PADDED + 0x3000;
    /* iBoot's pages for windows A to C and CTRR_C, after the object */
    u64 top = load + top_off;
    for (int i = 0; i < 3; i++) {
        armed[i][0] = top - 3 * SZ_16K + i * SZ_16K;
        armed[i][1] = armed[i][0] + 0x1000;
    }
    armed[CTRR_RANGE_C][0] = armed[0][0];
    armed[CTRR_RANGE_C][1] = armed[2][1];
    /* iBoot's DeviceTree and TrustCache below the object, the rest after it */
    map_entry(1, "DeviceTree", load - BELOW, 0x94000);
    map_entry(1, "TrustCache", load - BELOW + 0x94000, 0x60000);
    map_entry(1, "SEPFW", object_end + 0x4000, 0x5ac000);
    map_entry(1, "preoslog", object_end + 0x5b0000, 0x40000);
    map_entry(1, "BootArgs", top - 4 * SZ_16K, SZ_16K);
    boot_args_addr = top - 4 * SZ_16K;
    cur_boot_args.top_of_kernel_data = top;
    heap_base = (void *)(load + heap_off);
}

/* As the EL2 boots measured it */
static void boot(u64 load, u64 unused)
{
    (void)unused;
    boot_at(load, TOP_OFF, HEAP_OFF);
}

static bool meets(u64 a, u64 b, u64 c, u64 d) { return a < d && c < b; }

/* What the kernel's aura-hibernate accepts as the tree in the slot (aura_slot_tree_valid()) */
static bool kernel_accepts(const void *fdt, u64 slot)
{
    int node = fdt_path_offset(fdt, "/chosen"), len;
    const fdt64_t *v;

    if (fdt_check_header(fdt) || fdt_totalsize(fdt) > 2 * SZ_1M || node < 0)
        return false;
    v = fdt_getprop(fdt, node, "aura,hibernate-dt", &len);
    if (!v || len != 8 || fdt64_ld(v) != slot)
        return false;
    u64 dt_slot = fdt64_ld(v);
    v = fdt_getprop(fdt, node, "aura,hibernate-layout", &len);
    return v && len == 16 && dt_slot >= fdt64_ld(&v[0]) && dt_slot + 2 * SZ_1M <= fdt64_ld(&v[1]);
}

static void make_input(void)
{
    assert(!fdt_create_empty_tree(input, sizeof(input)));
    assert(fdt_add_subnode(input, 0, "chosen") >= 0);
    assert(fdt_add_subnode(input, 0, "memory") >= 0);
    assert(fdt_add_subnode(input, 0, "reserved-memory") >= 0);
    assert(!fdt_add_mem_rsv(input, 0x10bd1474000UL, 0x72c4000));
    assert(!fdt_pack(input));
}

/* kboot_prepare_dt() with the end check */
static int prepare_dt(void)
{
    assert(!fdt_open_into(input, dt, sizeof(blob)));
    heap_base = (u8 *)heap_base + 0x60000; /* the tree's own buffer */
    if (dt_reserve_hibernate_layout())
        return -1;
    assert(!fdt_add_mem_rsv(dt, (u64)heap_base - 0x60000, 0x60000));
    int node = fdt_path_offset(dt, "/chosen");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "bootargs", "rw"));
    if (dt_set_hibernate_layout(node))
        return -1;
    assert(!fdt_pack(dt));
''' + "%(end_check)s" + r'''
    return 0;
}

/* The hooks in the order kboot_prepare_dt() and kboot_boot() run them; NULL: no handoff */
static void *run(bool hib)
{
    kboot_request_hibernate_layout(hib);
    kboot_decide_hibernate_layout();
    if (prepare_dt())
        return NULL;
    heap_base = (u8 *)heap_base + 0x200000; /* USB, PCIe and the handoffs */
    return kboot_hibernate_handoff_dt();
}

/* The same steps with no hibernation code at all: the boot j616s/el2-full gives */
static unsigned char reference[sizeof(blob)];
static u64 reference_heap;
static void run_reference(u64 load, u64 after)
{
    boot(load, after);
    assert(!fdt_open_into(input, dt, sizeof(blob)));
    heap_base = (u8 *)heap_base + 0x60000;
    assert(!fdt_add_mem_rsv(dt, (u64)heap_base - 0x60000, 0x60000));
    int node = fdt_path_offset(dt, "/chosen");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "bootargs", "rw"));
    assert(!fdt_pack(dt));
    heap_base = (u8 *)heap_base + 0x200000;
    memcpy(reference, dt, fdt_totalsize(dt));
    reference_heap = (u64)heap_base;
}
static bool unchanged(void *handoff)
{
    return handoff == dt && fdt_totalsize(dt) == fdt_totalsize(reference) &&
           !memcmp(dt, reference, fdt_totalsize(dt)) && (u64)heap_base == reference_heap &&
           !hib_dt_base && !hib_heap_end;
}

static int rsv_count(void) { return fdt_num_mem_rsv(dt); }
static bool rsv_covers(u64 start, u64 end)
{
    for (int i = 0; i < rsv_count(); i++) {
        uint64_t a, s;
        assert(!fdt_get_mem_rsv(dt, i, &a, &s));
        if (a <= start && a + s >= end)
            return true;
    }
    return false;
}
static bool rsv_meets(u64 start, u64 end)
{
    for (int i = 0; i < rsv_count(); i++) {
        uint64_t a, s;
        assert(!fdt_get_mem_rsv(dt, i, &a, &s));
        if (meets(a, a + s, start, end))
            return true;
    }
    return false;
}
static bool has_chosen(const char *name)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), name, NULL) != NULL;
}

/* A refusal: said why, nothing set, the heap where it was, the tree as without hib=1 */
static void refused(const char *why)
{
    u64 heap = (u64)heap_base;
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    if (!strstr(log_text, why) || !strstr(log_text, "Linux will not hibernate")) {
        fprintf(stderr, "expected '%%s' in:\n%%s\n", why, log_text);
        assert(0);
    }
    assert(!hib_dt_base && !hib_heap_base && !hib_heap_end && (u64)heap_base == heap);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    make_input();
    assert(SLOT == 0x10018000000UL && END == 0x10020000000UL);

    /*
     * The two EL2 boots of 2026-11-08 (load bases 0x10005904000 and 0x10005cbc000) and the two
     * ends of the range iBoot loads this object in (ctxr_exec.c): the same layout.
     */
    static const u64 loads[] = {0x10005904000UL, 0x10005cbc000UL, LO + 0x1004000UL + BELOW,
                                LO + 0x3000000UL};
    static const u64 afters[] = {0, 0, 0, 0};
    unsigned char props[4][64];
    for (int i = 0; i < 4; i++) {
        boot(loads[i], afters[i]);
        u64 win = window_base, win_end = window_base + window_size;
        assert(win == 0x10008200000UL && win_end == 0x1000e200000UL);
        kboot_request_hibernate_layout(true);
        assert(fixed_asked);
        kboot_decide_hibernate_layout();
        assert(hib_dt_base == SLOT && hib_heap_base == SLOT + 2 * SZ_1M && hib_heap_end == END);
        assert(heapblock_high_water() == SLOT + 2 * SZ_1M);
        assert(hib_window_base == win && hib_window_end == win_end);
        /* The window, the object, iBoot's ranges and Stage 1 lie below the slot. */
        assert(win > RAM && win_end <= SLOT && armed[CTXR_RANGE_D][1] <= SLOT);
        assert(cur_boot_args.top_of_kernel_data <= SLOT);
        assert(!meets(SLOT, END, win, win_end));
        assert(!prepare_dt());
        /* Linux keeps the window and nothing else of [RAM, END). */
        assert(rsv_covers(RAM, win) && rsv_covers(win_end, END) && !rsv_meets(win, win_end));
        /*
         * Those two before anything kboot adds (the J616s tree brings none of its own): U-Boot's
         * LMB drops a later range that overlaps one it has recorded.
         */
        uint64_t a, sz;
        int first = fdt_num_mem_rsv(input);
        assert(!fdt_get_mem_rsv(dt, first, &a, &sz) && a == RAM && a + sz == win);
        assert(!fdt_get_mem_rsv(dt, first + 1, &a, &sz) && a == win_end && a + sz == END);
        assert(has_chosen("aura,hibernate-layout") && has_chosen("aura,hibernate-dt"));
        int node = fdt_path_offset(dt, "/chosen"), len;
        const fdt64_t *layout = fdt_getprop(dt, node, "aura,hibernate-layout", &len);
        assert(len == 16 && fdt64_ld(&layout[0]) == RAM && fdt64_ld(&layout[1]) == END);
        const fdt64_t *slot = fdt_getprop(dt, node, "aura,hibernate-dt", &len);
        assert(len == 8 && fdt64_ld(slot) == SLOT);
        memcpy(props[i], layout, 16);
        memcpy(props[i] + 16, slot, 8);
        assert(kernel_accepts(dt, SLOT));
        /* Once a boot: a second preparation decides nothing again. */
        u64 heap = (u64)heap_base;
        size_t logged = strlen(log_text);
        kboot_decide_hibernate_layout();
        assert((u64)heap_base == heap && hib_dt_base == SLOT && strlen(log_text) == logged);
    }
    assert(!memcmp(props[0], props[1], 24) && !memcmp(props[0], props[2], 24) &&
           !memcmp(props[0], props[3], 24));
    /* Another chip doesn't ask ctxr_exec.c for the fixed window. */
    boot(loads[0], afters[0]);
    chip_id = T6041;
    kboot_request_hibernate_layout(true);
    assert(!fixed_asked);

    /* Without a window (a raw Stage 1, or no secondary to probe): one range, RAM to the end. */
    boot(loads[0], afters[0]);
    confined = false;
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    assert(hib_heap_end == END && !hib_window_base && !hib_window_end);
    assert(!prepare_dt() && rsv_covers(RAM, END));

    /* Without hib=1, and with hib=0 after hib=1: the tree, the heap and the handoff as before. */
    for (int i = 0; i < 4; i++) {
        run_reference(loads[i], afters[i]);
        boot(loads[i], afters[i]);
        assert(unchanged(run(false)) && !strstr(log_text, "hib:"));
        boot(loads[i], afters[i]);
        kboot_request_hibernate_layout(true);
        assert(unchanged(run(false)));
    }

    /* Refused: nothing changes, and the tree is the one without hib=1. */
    struct { const char *why; int what; } cases[] = {
        {"only known for T6040", 0},
        {"invalid RAM extent", 1},
        {"invalid RAM extent", 2},
        {"too little RAM", 3},
        {"the CTXR exec window is not below the fixed slot", 4},
        {"the CTXR exec window is not below the fixed slot", 5},
        {"aura is not below the fixed slot", 6},
        {"boot_args is not below the fixed slot", 7},
        {"what iBoot and Stage 1 placed reaches the fixed slot", 8},
        {"the heap reaches the fixed slot", 9},
        {"meets CTXR/CTRR range 3", 10},
        {"meets CTXR/CTRR range 0", 11},
        {"meets CTXR/CTRR range 5", 12},
        {"meets MCC carveout", 13},
        {"meets ADT /chosen/memory-map/SEPFW", 14},
        {"meets ADT /chosen/carveout-memory-map/region-id-46", 15},
        {"ADT /chosen/memory-map not found", 16},
        {"the CTXR exec window is not at a fixed address", 17},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        u64 load = loads[0], after = afters[0];
        run_reference(load, after);
        boot(load, after);
        switch (cases[i].what) {
        case 0: chip_id = T6041; break;
        case 1: cur_boot_args.mem_size = 0; break;
        case 2: cur_boot_args.phys_base = ~0UL - SZ_16K; break;
        case 3: cur_boot_args.mem_size = 300 * SZ_1M; break;
        case 4: window_base = SLOT - 64 * SZ_1M; break;          /* the window past the slot */
        case 5: window_base = RAM; break;
        case 6: confined = false; aura_end = SLOT + SZ_1M; break;
        case 7: boot_args_addr = SLOT - SZ_16K + 1; break;
        case 8: cur_boot_args.top_of_kernel_data = SLOT + SZ_16K; break;
        case 9: heap_base = (void *)(SLOT + SZ_1M); break;
        case 10: armed[CTXR_RANGE_D][1] = SLOT + SZ_16K; break; /* armed for more */
        case 11: armed[CTXR_RANGE_A][0] = END - SZ_16K; armed[CTXR_RANGE_A][1] = END - 0x3000; break;
        case 12: armed[CTRR_RANGE_D][0] = SLOT + 4 * SZ_1M; armed[CTRR_RANGE_D][1] = SLOT + 5 * SZ_1M; break;
        case 13: mcc_carveouts[mcc_carveout_count++] = (struct mcc_carveout){END - SZ_1M, SZ_1M}; break;
        case 14: map_move(1, "SEPFW", SLOT + 2 * SZ_1M, 0x5ac000); break;
        case 15: map_move(2, "region-id-46", END - SZ_16K, 0x1e000000); break;
        case 16: maps_present = false; break;
        case 17: window_fixed = false; break;
        }
        refused(cases[i].why);
        u64 heap = (u64)heap_base;
        (void)heap;
        if (cases[i].what >= 1 && cases[i].what <= 3)
            continue; /* the model's later steps need valid RAM */
        boot(load, after);
        switch (cases[i].what) { /* again, through the whole flow */
        case 0: chip_id = T6041; break;
        case 4: window_base = SLOT - 64 * SZ_1M; break;
        case 5: window_base = RAM; break;
        case 6: confined = false; aura_end = SLOT + SZ_1M; break;
        case 7: boot_args_addr = SLOT - SZ_16K + 1; break;
        case 8: cur_boot_args.top_of_kernel_data = SLOT + SZ_16K; break;
        case 10: armed[CTXR_RANGE_D][1] = SLOT + SZ_16K; break;
        case 11: armed[CTXR_RANGE_A][0] = END - SZ_16K; armed[CTXR_RANGE_A][1] = END - 0x3000; break;
        case 12: armed[CTRR_RANGE_D][0] = SLOT + 4 * SZ_1M; armed[CTRR_RANGE_D][1] = SLOT + 5 * SZ_1M; break;
        case 13: mcc_carveouts[mcc_carveout_count++] = (struct mcc_carveout){END - SZ_1M, SZ_1M}; break;
        case 14: map_move(1, "SEPFW", SLOT + 2 * SZ_1M, 0x5ac000); break;
        case 15: map_move(2, "region-id-46", END - SZ_16K, 0x1e000000); break;
        case 16: maps_present = false; break;
        case 17: window_fixed = false; break;
        default: continue; /* the heap or RAM itself is what is modeled away */
        }
        assert(unchanged(run(true)));
    }

    /* Region 24 (RAM) and 45 (iBoot's region for the object) cover the fixed range: accepted. */
    boot(loads[0], afters[0]);
    map_entry(2, "region-id-45", SLOT, 128 * SZ_1M);
    map_entry(2, "region-id-24", SLOT, 128 * SZ_1M);
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    assert(hib_heap_end == END);

    /* Late: the heap within 8 MiB of the end when the tree is done. The layout is taken out. */
    boot(loads[0], afters[0]);
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    heap_base = (void *)(END - 8 * SZ_1M - 0x60000 + SZ_16K);
    assert(!prepare_dt() && strstr(log_text, "fixed layout withdrawn"));
    assert(!has_chosen("aura,hibernate-layout") && !has_chosen("aura,hibernate-dt"));
    assert(kboot_hibernate_handoff_dt() == dt && !hib_dt_base);

    /* Late: the heap within 8 MiB of the end at the handoff. */
    boot(loads[0], afters[0]);
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    assert(!prepare_dt() && has_chosen("aura,hibernate-dt"));
    heap_base = (void *)(END - SZ_1M);
    assert(kboot_hibernate_handoff_dt() == dt && strstr(log_text, "fixed layout withdrawn"));
    assert(!has_chosen("aura,hibernate-layout") && !has_chosen("aura,hibernate-dt"));

    /* Late: a tree larger than the slot. */
    boot(loads[0], afters[0]);
    kboot_request_hibernate_layout(true);
    kboot_decide_hibernate_layout();
    assert(!prepare_dt());
    assert(!fdt_open_into(dt, dt, sizeof(blob)));
    static char big[2 << 20];
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/chosen"), "filler", big, sizeof(big)));
    assert(kboot_hibernate_handoff_dt() == dt && strstr(log_text, "does not fit its slot"));
    assert(!has_chosen("aura,hibernate-layout") && !has_chosen("aura,hibernate-dt"));

    puts("T6040 hibernation layout PASS");

    /* The copy into the slot, at the slot's own address: what the kernel then finds there. */
    void *slot = mmap((void *)SLOT, 2 * SZ_1M, PROT_READ | PROT_WRITE,
#ifdef MAP_FIXED_NOREPLACE
                      MAP_FIXED_NOREPLACE |
#endif
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slot != (void *)SLOT) {
        puts("the slot address cannot be mapped on this host");
        return 77;
    }
    boot(loads[1], afters[1]);
    void *handoff = run(true);
    assert(handoff == slot && !memcmp(slot, dt, fdt_totalsize(dt)));
    assert(kernel_accepts(slot, SLOT) && strstr(log_text, "device tree copied to its slot"));
    puts("T6040 hibernation slot copy PASS");
    return 0;
}
'''


def build(tmp_path):
    kboot = (REPO / "src/kboot.c").read_text()
    heap = (REPO / "src/heapblock.c").read_text()
    header = (REPO / "src/ctxr_exec.h").read_text()
    start = header.index("enum ctxr_range {")
    source = HARNESS % {
        "ranges": header[start:header.index("};", start) + 2],
        "raise_base": function(heap, "heapblock_raise_base"),
        "block": hibernation_block(kboot),
        "end_check": prepare_dt_end_check(kboot),
    }
    source_file = tmp_path / "t6040-hibernate-layout.c"
    source_file.write_text(source)
    binary = tmp_path / "t6040-hibernate-layout"
    libfdt = REPO / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_empty_tree.c", "fdt_ro.c",
        "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    # No ASan: its shadow covers the slot's address on aarch64 hosts.
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-function", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
         "-I", str(libfdt), str(source_file), *map(str, sources), "-o", str(binary)],
        check=True,
    )
    return binary


def test_t6040_hibernate_layout(tmp_path):
    result = subprocess.run([str(build(tmp_path))], capture_output=True, text=True)
    assert "T6040 hibernation layout PASS" in result.stdout, result.stdout + result.stderr
    if result.returncode == 77:
        pytest.skip(result.stdout.splitlines()[-1])
    assert result.returncode == 0, result.stdout + result.stderr
    assert "T6040 hibernation slot copy PASS" in result.stdout


def test_hooks_run_where_the_layout_needs_them():
    kboot = (REPO / "src/kboot.c").read_text()
    prepare = function(kboot, "kboot_prepare_dt")
    # Decided once the window is known, before the tree or anything kboot reserves is allocated.
    decide = prepare.index("kboot_decide_hibernate_layout();")
    assert prepare.index("if (!ctxr_exec_os_may_start())") < decide
    assert decide < prepare.index("isp_init();") < prepare.index("dt = memalign(DT_ALIGN")
    assert prepare.count("kboot_decide_hibernate_layout();") == 1
    # Reserved first in the map, as soon as the tree is open; the check last before it is done.
    reserve = prepare.index("dt_reserve_hibernate_layout()")
    assert prepare.index("fdt_open_into(fdt, dt, dt_bufsize)") < reserve
    assert reserve < prepare.index("fdt_add_mem_rsv(") and prepare.count("dt_reserve_hibernate_layout()") == 1
    assert prepare.index("dt_set_memory()") < prepare.index("HIB_HEAP_MARGIN") < \
        prepare.index('printf("FDT prepared at %p\\n", dt);')
    chosen = function(kboot, "dt_set_chosen")
    assert chosen.count("dt_set_hibernate_layout(node)") == 1
    # The copy is the last thing before the kernel's arguments, after every write to the tree.
    boot = function(kboot, "kboot_boot")
    copy = boot.index("void *handoff_dt = kboot_hibernate_handoff_dt();")
    assert boot.index("kboot_move_to_exec_window(&kernel)") < copy
    assert boot.rindex("smp_has_pending_calls()") < copy < boot.index("next_stage.entry = kernel;")
    assert "next_stage.args[0] = (u64)handoff_dt;" in boot and "next_stage.args[0] = (u64)dt;" not in boot
    # Nothing else reads or writes the state the block keeps.
    outside = kboot.replace(hibernation_block(kboot), "")
    for name in ("hib_requested", "hib_decided", "hib_dt_base", "hib_heap_base", "hib_window_"):
        assert name not in outside, name
    assert outside.count("hib_heap_end") == prepare_dt_end_check(kboot).count("hib_heap_end")


def test_hib_line_requests_the_layout():
    payload = (REPO / "src/payload.c").read_text()
    names = payload[payload.index("static const char *const config_names[]"):]
    assert '"hib="' in names[:names.index("};")]
    var = payload[payload.index('    } else if (IS_VAR("hib=")) {'):]
    assert var.split("\n")[1].strip() == "kboot_request_hibernate_layout(val[0] == '1');"
