# SPDX-License-Identifier: MIT
"""T6040 hib=1 under a boot object iBoot moves: the fixed CTXR exec window and the layout around it.

ctxr_exec.c's window code (ctxr_range(), the fixed and the moving window, the checks) and the
whole hibernation block of kboot.c with heapblock_raise_base() are the production bodies, run
against real libfdt. The machine is the J616s as two EL2 boots of one padded Stage 1 measured it
on 2026-11-08: RAM, CTXR_D 0xf4000 below the object's load base and 0x920c000 above it, the
CTXR_A-C pages, boot_args and top_of_kernel_data after it, this stage's heap 0xf064000 above it
when the tree is allocated. Only the load base moves, over every 16 KiB step of the range iBoot
loads this object in, and a little past it.
"""
import subprocess

from test_locked_dart_tables import function
from test_smp_timeouts import REPO
from test_t6040_hibernate_layout import hibernation_block, prepare_dt_end_check


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint8_t u8;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define T6040 0x6040
#define T6041 0x6041
#define SZ_4K  0x1000UL
#define SZ_16K 0x4000UL
#define SZ_1M  0x100000UL
#define SZ_32M 0x2000000UL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define sysop(op) ((void)0)
static void dc_cvac_range(void *p, size_t n) { (void)p; (void)n; }
static char log_text[1 << 16];
#define printf(...) snprintf(log_text + strlen(log_text), sizeof(log_text) - strlen(log_text), __VA_ARGS__)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)

/* What ctxr_range() reads: the registers, at EL2, none of them faulting */
enum exc_guard_t { GUARD_OFF = 0, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static volatile int exc_guard, exc_count;
static bool in_el2(void) { return true; }
#define mrs(reg) (reg)
static u64 SYS_IMP_APL_CTXR_A_LWR_EL1, SYS_IMP_APL_CTXR_A_UPR_EL1, SYS_IMP_APL_CTXR_B_LWR_EL1,
    SYS_IMP_APL_CTXR_B_UPR_EL1, SYS_IMP_APL_CTXR_C_LWR_EL1, SYS_IMP_APL_CTXR_C_UPR_EL1,
    SYS_IMP_APL_CTXR_D_LWR_EL1, SYS_IMP_APL_CTXR_D_UPR_EL1, SYS_IMP_APL_CTRR_C_LWR_EL1,
    SYS_IMP_APL_CTRR_C_UPR_EL1, SYS_IMP_APL_CTRR_D_LWR_EL1, SYS_IMP_APL_CTRR_D_UPR_EL1;
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;

/* The machine */
static int chip_id;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static u64 boot_args_addr, aura_base, aura_end;
#define _base ((char *)aura_base)
#define _end ((char *)aura_end)
/* T6040 has no firmware RO range of its own */
static int memory_fw_ro_range(u64 *start, u64 *end) { *start = *end = 0; return 0; }
static bool confined;
static int probe_stuck_cpu = -1;
static bool ctxr_exec_confined(void) { return confined; }
static void fb_set_active(bool active) { (void)active; }
''' + "%(ranges)s\n%(ctxr_range)s\n%(window)s" + r'''
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
static void *adt;
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    return strcmp(path, "/chosen/memory-map") ? 2 : 1;
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

static _Alignas(8) unsigned char blob[4 << 20];
static void *dt = blob;
''' + "%(block)s" + r'''

#define RAM       0x100032b8000UL
#define RAM_SIZE  0xbc98d4000UL
#define LO        0x10004000000UL    /* the 32 MiB aligned start of RAM */
#define SLOT      (LO + 320 * SZ_1M)
#define END       (SLOT + 128 * SZ_1M)
/* Measured above the object's load base on both EL2 boots */
#define BELOW     0xf4000UL          /* CTXR_D (the ADT) starts this far below it */
#define ARMED     0x920c000UL        /* CTXR_D ends this far above it */
#define BOOT_ARGS 0x98b0000UL
#define LOW_WINS  0x98b4000UL        /* CTXR_A-C and CTRR_C, 16K each */
#define TOP       0x98c0000UL        /* top_of_kernel_data */
#define HEAP      0xf064000UL        /* this stage's heap when the tree is allocated */
/* Where iBoot loads this object: its first iBoot page at least 0x1004000 above LO, the object at
 * most 0x3000000 above it (ctxr_exec.c). */
#define LOAD_MIN  (LO + 0x1004000UL + BELOW)
#define LOAD_MAX  (LO + 0x3000000UL)
#define FIXED     (LO + 0x4200000UL)
#define FIXED_END (LO + 0xa200000UL)
#define WINDOW    (128 * SZ_1M)

static unsigned char input[1 << 16];

static void reset(void)
{
    window_checked = false;
    window_problem = NULL;
    window_base = window_size = 0;
    fixed_requested = window_fixed = false;
    hib_requested = hib_decided = false;
    hib_dt_base = hib_heap_base = hib_heap_end = hib_window_base = hib_window_end = 0;
    log_text[0] = 0;
    memset(map_count, 0, sizeof(map_count));
    mcc_carveout_count = 0;
    confined = true;
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
    mcc_carveouts[mcc_carveout_count++] = (struct mcc_carveout){0x10bd9ce0000UL, 0x25800000};
}

/* A boot with iBoot's object at @load, arming CTXR_D @armed above it. */
static void boot_armed(u64 load, u64 armed)
{
    reset();
    aura_base = load + 0x10000;
    aura_end = aura_base + 0x4a0000;
    SYS_IMP_APL_CTXR_D_LWR_EL1 = load - BELOW;
    SYS_IMP_APL_CTXR_D_UPR_EL1 = load + armed + 0x3000 - SZ_4K;
    u64 w = load + (armed > ARMED ? armed - ARMED : 0) + LOW_WINS;
    SYS_IMP_APL_CTXR_A_LWR_EL1 = SYS_IMP_APL_CTRR_C_LWR_EL1 = w;
    SYS_IMP_APL_CTXR_A_UPR_EL1 = w + 0x3000;
    SYS_IMP_APL_CTXR_B_LWR_EL1 = w + SZ_16K;
    SYS_IMP_APL_CTXR_B_UPR_EL1 = w + SZ_16K + 0x3000;
    SYS_IMP_APL_CTXR_C_LWR_EL1 = w + 2 * SZ_16K;
    SYS_IMP_APL_CTXR_C_UPR_EL1 = SYS_IMP_APL_CTRR_C_UPR_EL1 = w + 2 * SZ_16K + 0x3000;
    SYS_IMP_APL_CTRR_D_LWR_EL1 = SYS_IMP_APL_CTRR_D_UPR_EL1 = 0;
    u64 shift = armed > ARMED ? armed - ARMED : 0;
    boot_args_addr = load + shift + BOOT_ARGS;
    cur_boot_args.top_of_kernel_data = load + shift + TOP;
    heap_base = (void *)(load + shift + HEAP);
    map_entry(1, "DeviceTree", load - BELOW, 0x94000);
    map_entry(1, "TrustCache", load - BELOW + 0x94000, 0x60000);
    map_entry(1, "SEPFW", load + shift + ARMED + 0x84000, 0x5ac000);
    map_entry(1, "BootArgs", boot_args_addr, SZ_16K);
}
static void boot(u64 load) { boot_armed(load, ARMED); }

static void make_input(void)
{
    assert(!fdt_create_empty_tree(input, sizeof(input)));
    assert(fdt_add_subnode(input, 0, "chosen") >= 0);
    assert(!fdt_add_mem_rsv(input, 0x10bd1474000UL, 0x72c4000));
    assert(!fdt_pack(input));
}

/* kboot_prepare_dt() from the window to the end check, as test_t6040_hibernate_layout.py has it */
static int prepare_dt(void)
{
    if (!ctxr_exec_os_may_start())
        return -1;
    kboot_decide_hibernate_layout();
    assert(!fdt_open_into(input, dt, sizeof(blob)));
    heap_base = (u8 *)heap_base + 0x60000;
    if (dt_reserve_hibernate_layout())
        return -1;
    assert(!fdt_add_mem_rsv(dt, (u64)heap_base - 0x60000, 0x60000));
    int node = fdt_path_offset(dt, "/chosen");
    assert(node >= 0);
    u64 base, size;
    if (ctxr_exec_window(&base, &size)) {
        fdt64_t win[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
        assert(!fdt_setprop(dt, node, "aurora,ctxr-exec-window", win, sizeof(win)));
    }
    if (dt_set_hibernate_layout(node))
        return -1;
    assert(!fdt_pack(dt));
''' + "%(end_check)s" + r'''
    return 0;
}

static bool window(u64 *base, u64 *size) { *base = *size = 0; return ctxr_exec_window(base, size); }

/* The window above aura's slot, as without hib=1 */
static void moving(u64 load, u64 *start, u64 *end)
{
    *start = ALIGN_UP(load + 0x10000 + 16 * SZ_1M, 2 * SZ_1M);
    *end = *start + WINDOW;
}

static void chosen_u64s(const char *name, u64 *out, int n)
{
    int len;
    const fdt64_t *v = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), name, &len);
    assert(v && len == n * 8);
    for (int i = 0; i < n; i++)
        out[i] = fdt64_ld(&v[i]);
}

int main(void)
{
    u64 base, size, s, e;
    make_input();
    _Static_assert(FIXED_END - FIXED == 96 * SZ_1M, "this object's fixed window is 96 MiB");
    /* Why the slot moved from 288 MiB: the highest object's heap reaches past it. */
    _Static_assert(LOAD_MAX + HEAP > LO + 288 * SZ_1M && LOAD_MAX + HEAP + 31 * SZ_1M < SLOT,
                   "the slot clears the highest object's heap by 31 MiB");

    /*
     * Without hib=1 the two EL2 boots come out as aura logged them, to the byte, and nothing about
     * a fixed window is said.
     */
    boot(0x10005904000UL);
    assert(window(&base, &size) && base == 0x10006a00000UL && size == WINDOW);
    assert(!strcmp(log_text, "CTXR exec: armed [0x10005810000, 0x1000eb10000)\n"
                             "CTXR exec: OS window [0x10006a00000, 0x1000ea00000); 0x110000 armed "
                             "bytes above it stay reserved\n"));
    assert(!ctxr_exec_window_is_fixed());
    boot(0x10005cbc000UL);
    assert(window(&base, &size) && base == 0x10006e00000UL && size == WINDOW);
    assert(!strcmp(log_text, "CTXR exec: armed [0x10005bc8000, 0x1000eec8000)\n"
                             "CTXR exec: OS window [0x10006e00000, 0x1000ee00000); 0xc8000 armed "
                             "bytes above it stay reserved\n"));

    /* Every placement without hib=1, and with hib=0 after hib=1: the window above aura's slot. */
    for (u64 load = LOAD_MIN - 16 * SZ_1M; load <= LOAD_MAX + 16 * SZ_1M; load += SZ_16K) {
        for (int again = 0; again < 2; again++) {
            boot(load);
            if (again) {
                kboot_request_hibernate_layout(true);
                kboot_request_hibernate_layout(false);
            }
            moving(load, &s, &e);
            assert(window(&base, &size) && base == s && base + size == e);
            assert(!ctxr_exec_window_is_fixed() && !strstr(log_text, "fixed") &&
                   !strstr(log_text, "iBoot places it"));
        }
    }

    /*
     * hib=1 on every placement iBoot can choose: one window, the same on every boot, inside what
     * every one of them arms and above aura's slot; and the same hibernation layout around it.
     */
    unsigned placements = 0;
    static unsigned char first[4 << 20];
    u32 first_size = 0;
    for (u64 load = LOAD_MIN; load <= LOAD_MAX; load += SZ_16K, placements++) {
        boot(load);
        kboot_request_hibernate_layout(true);
        if (prepare_dt()) {
            fprintf(stderr, "load 0x%%lx:\n%%s\n", load, log_text);
            assert(0);
        }
        assert(window(&base, &size) && base == FIXED && base + size == FIXED_END);
        assert(ctxr_exec_window_is_fixed() && strstr(log_text, "fixed OS window"));
        assert(strstr(log_text, "iBoot places it at +0x1014000 to +0x3010000\n"));
        assert(load - BELOW < FIXED && FIXED_END <= load + ARMED);
        assert(load + 0x10000 + 16 * SZ_1M <= FIXED);
        assert(hib_dt_base == SLOT && hib_heap_end == END && hib_window_base == FIXED &&
               hib_window_end == FIXED_END && strstr(log_text, "hib: fixed layout: RAM"));
        u64 layout[2], slot, win[2];
        chosen_u64s("aura,hibernate-layout", layout, 2);
        chosen_u64s("aura,hibernate-dt", &slot, 1);
        chosen_u64s("aurora,ctxr-exec-window", win, 2);
        assert(layout[0] == RAM && layout[1] == END && slot == SLOT && win[0] == FIXED &&
               win[1] == FIXED_END - FIXED);
        uint64_t a, sz;
        int n = fdt_num_mem_rsv(input);
        assert(!fdt_get_mem_rsv(dt, n, &a, &sz) && a == RAM && a + sz == FIXED);
        assert(!fdt_get_mem_rsv(dt, n + 1, &a, &sz) && a == FIXED_END && a + sz == END);
        /* What this model puts in the tree is all fixed: the tree is the same on every boot. */
        if (!first_size) {
            first_size = fdt_totalsize(dt);
            memcpy(first, dt, first_size);
        }
        assert(fdt_totalsize(dt) == first_size && !memcmp(first, dt, first_size));
    }
    assert(placements == (LOAD_MAX - LOAD_MIN) / SZ_16K + 1);

    /* The two EL2 boots with hib=1: the fixed window. */
    boot(0x10005904000UL);
    kboot_request_hibernate_layout(true);
    assert(window(&base, &size) && base == 0x10008200000UL && size == 96 * SZ_1M);
    assert(strstr(log_text, "CTXR exec: aura at 0x10005914000 = 0x10004000000 + 0x1914000, iBoot "
                            "places it at +0x1014000 to +0x3010000\n"
                            "CTXR exec: fixed OS window [0x10008200000, 0x1000e200000); 0x910000 "
                            "armed bytes above it stay reserved\n"));
    boot(0x10005cbc000UL);
    kboot_request_hibernate_layout(true);
    assert(window(&base, &size) && base == 0x10008200000UL && size == 96 * SZ_1M);

    /*
     * Past iBoot's range, as far as the window still fits it: the same window, said to be outside.
     * Further out it doesn't fit, the OS gets the window above aura's slot, and kboot gives no
     * layout, so Linux won't hibernate on that boot.
     */
    static const struct { u64 load; bool fits; const char *why; } outside[] = {
        {LO + 0x31f0000UL, true, NULL},
        {LO + 0x31f4000UL, false, "aura is not inside CTXR_D and its slot"},
        {LO + 0xff4000UL, true, NULL},
        {LO + 0xff0000UL, false, "this Stage 1 did not arm a large enough window"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(outside); i++) {
        u64 load = outside[i].load;
        boot(load);
        kboot_request_hibernate_layout(true);
        assert(!prepare_dt());
        assert(strstr(log_text, "(this boot is outside that)"));
        if (outside[i].fits) {
            assert(window(&base, &size) && base == FIXED && hib_heap_end == END);
            continue;
        }
        moving(load, &s, &e);
        assert(window(&base, &size) && base == s && base + size == e);
        assert(strstr(log_text, outside[i].why) && strstr(log_text, "no fixed window"));
        assert(strstr(log_text, "the CTXR exec window is not at a fixed address") &&
               strstr(log_text, "Linux will not hibernate") && !hib_heap_end && !hib_dt_base);
        assert(!fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "aura,hibernate-layout", NULL));
    }

    /* Padded for 256 MiB: still 128 MiB, at the same fixed base. */
    for (u64 load = LOAD_MIN; load <= LOAD_MAX; load += 61 * SZ_16K) {
        boot_armed(load, ARMED + WINDOW);
        kboot_request_hibernate_layout(true);
        assert(window(&base, &size) && base == FIXED && size == WINDOW && ctxr_exec_window_is_fixed());
        assert(base + size <= load + ARMED + WINDOW);
    }

    /* An unpadded object: no fixed window, and the boot stops as it does without hib=1. */
    boot_armed(LOAD_MIN, 0x508000);
    kboot_request_hibernate_layout(true);
    assert(!window(&base, &size) && !ctxr_exec_os_may_start());
    assert(strstr(log_text, "too small for a window every placement arms") &&
           strstr(log_text, "did not arm a large enough window"));

    /* Another chip doesn't get the fixed window; its hib=1 is refused as before. */
    boot(0x10005904000UL);
    chip_id = T6041;
    kboot_request_hibernate_layout(true);
    moving(0x10005904000UL, &s, &e);
    assert(window(&base, &size) && base == s && !ctxr_exec_window_is_fixed());

    /* Asked for once the window was looked up: too late, and no layout on that boot. */
    boot(0x10005904000UL);
    assert(window(&base, &size));
    assert(!ctxr_exec_request_fixed_window(true));
    kboot_request_hibernate_layout(true);
    assert(!prepare_dt() && !ctxr_exec_window_is_fixed() &&
           strstr(log_text, "the CTXR exec window is not at a fixed address") && !hib_heap_end);

    /* Unknown RAM: no fixed window. */
    boot(0x10005904000UL);
    cur_boot_args.phys_base = 0;
    kboot_request_hibernate_layout(true);
    assert(window(&base, &size) && !ctxr_exec_window_is_fixed() &&
           strstr(log_text, "the start of RAM is not known"));

    printf("%%u placements\n", placements);
    fputs(log_text, stdout);
    puts("T6040 fixed window PASS");
    return 0;
}
'''


def build(tmp_path):
    kboot = (REPO / "src/kboot.c").read_text()
    heap = (REPO / "src/heapblock.c").read_text()
    ctxr = (REPO / "src/ctxr_exec.c").read_text()
    header = (REPO / "src/ctxr_exec.h").read_text()
    start = header.index("enum ctxr_range {")
    source = HARNESS % {
        "ranges": header[start:header.index("};", start) + 2],
        "ctxr_range": function(ctxr, "ctxr_range"),
        "window": ctxr[ctxr.index("#define CTXR_EXEC_AURA_SLOT"):],
        "raise_base": function(heap, "heapblock_raise_base"),
        "block": hibernation_block(kboot),
        "end_check": prepare_dt_end_check(kboot),
    }
    source_file = tmp_path / "t6040-fixed-window.c"
    source_file.write_text(source)
    binary = tmp_path / "t6040-fixed-window"
    libfdt = REPO / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_empty_tree.c", "fdt_ro.c",
        "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-I", str(libfdt), str(source_file), *map(str, sources), "-o", str(binary)],
        check=True,
    )
    return binary


def test_t6040_fixed_window(tmp_path):
    result = subprocess.run([str(build(tmp_path))], capture_output=True, text=True)
    assert result.returncode == 0 and "T6040 fixed window PASS" in result.stdout, \
        result.stdout[-4000:] + result.stderr[-4000:]


def test_fixed_window_is_asked_for_before_the_window_is_found():
    # The hib= line is read with the payloads, before kboot_prepare_dt() looks the window up.
    kboot = (REPO / "src/kboot.c").read_text()

    def body(signature):
        start = kboot.index(signature)
        return kboot[start:kboot.index("\n}\n", start)]

    request = body("void kboot_request_hibernate_layout(bool on)\n{")
    assert "ctxr_exec_request_fixed_window(on && chip_id == T6040);" in request
    refused = body("static const char *hib_layout_refused(")
    assert refused.index("ctxr_exec_window(&win, &win_size)") < \
        refused.index("!ctxr_exec_window_is_fixed()")
    ctxr = (REPO / "src/ctxr_exec.c").read_text()
    # Only the window search reads the request; nothing else changes the window it gives.
    assert ctxr.count("fixed_requested") == 3
    assert ctxr.count("window_fixed = true;") == 1
