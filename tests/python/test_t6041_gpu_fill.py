# SPDX-License-Identifier: MIT
"""Run kboot_gpu.c's T6041 GPU fill on real FDTs with a modelled ADT, efuse page and SGX.

The accepted cases use the operating points, CS/AFR tables, AON PTD rows, active MCC mask, regions
and live efuse page (0x508374c10..0x508374c28) of one J616C (32-core M4 Max) on 26.6.2 (two
gfx-asc segments: __TEXT and __DATA) and on 27.2 / 26B123 (three gfx-asc segments: __TEXT, __DATA
and a contiguous __DATA_SHARED_RO tail merged into gpu-fw-data), plus a 40-core (8-MCC) topology
check matching the measured 40-core T6041 leakage reference.

Failure model for 3-segment 27.2 framing and v2 calibration handoff recorded before implementation:
- segment-ranges counts other than 2 or 3 must be refused.
- A 3-segment gfx-asc must match /arm-io/sgx gfx-data-shared-ro-{base,size} on physical base,
  identity remap and non-zero 16 KiB-aligned size, sit immediately after gfx-data with no gap
  or wrap, and have zero (or absent) gfx-data-shared-rw-{base,size}.
- Extending gpu-fw-data by gfx-data-shared-ro-size must still satisfy 42-bit OAS bounds and
  pairwise non-overlap against every other UAT and firmware reservation (including gpu-fw-text).
- Efuse reads (0x508374c10, 0xc14, 0xc1c, 0xc20, 0xc24, 0xc28) happen before SGX is powered;
  any fault, unprogrammed zero, or saturated all-ones field refuses before SGX is enabled and
  deletes any stale apple,t6041-handoff-version property.
- SGX power is enabled only around the SGX register reads and restored before returning; every
  ADT/DT/efuse refusal happens before SGX is powered.
- Non-monotonic power ladders or computed power ceilings above 250,000 mW are refused after
  restoring SGX power.
- Handoff v2 emits all verified ADT, efuse, and SGX calibration properties (active-mcc-bit-vector,
  aon-table, core/sram/cs/afr-leak-coef, perf-map-a/b, perf-relative-a/b, perf-base-pstate,
  max-power-mw, 40-bit core-mask, t6041-handoff-version = 2) matching linux calibration.rs.
- Every refusal leaves the GPU node and all six reserved-memory nodes disabled.
"""

from pathlib import Path
import re
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SZ_16K 0x4000
#define MASK(x) ((1UL << (x)) - 1)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define sysop(op) ((void)0)
enum { GUARD_OFF = 0, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static int exc_guard, exc_count;

static void *adt = (void *)1;
static unsigned char blob[65536];
static void *dt = blob;
struct fw_version_info { const char *string; u32 num[3]; size_t num_length; };
static struct fw_version_info os_firmware = {"26.6.2", {26, 6, 2}, 3};
static int firmware_set_fdt(void *tree, int node, const char *prop,
                            const struct fw_version_info *version)
{
    fdt32_t cells[3];
    for (size_t i = 0; i < version->num_length; i++)
        cells[i] = cpu_to_fdt32(version->num[i]);
    return fdt_setprop(tree, node, prop, cells, version->num_length * sizeof(u32));
}

/* SGX power: the fill must power it only around the register reads and restore it after. */
struct pmgr_saved_modes { int count; };
static bool sgx_on, sgx_power_fail;
static int enables, restores, enable_attempts;
static int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved)
{
    assert(!strcmp(path, "/arm-io/sgx"));
    enable_attempts++;
    if (sgx_power_fail)
        return -1;
    enables++;
    sgx_on = true;
    saved->count = 1;
    return 0;
}
static void pmgr_restore_modes(struct pmgr_saved_modes *saved)
{
    assert(saved->count == 1);
    restores++;
    sgx_on = false;
}
'''

ADT = r'''
#define SGX_BASE 0x288000000UL
#define EFUSE    0x508374000UL
static struct perf_state states[64];
static struct {
    u64 dies, count;
    struct aux_perf_state states[8];
} cs = {.dies = 1, .count = 8}, afr = {.dies = 1, .count = 8};
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; };
static struct adt_segment_ranges segments[4];
static u32 segments_len;
static u64 region[6][2], shared_ro[2], shared_rw[2];
static u32 raw_count, raw_tables, raw_max_state, active_mcc;
static u32 base_pstate, base_length;
static u32 ptd_table[18], ptd_len;
static u32 ef_c10, ef_c14, ef_c1c, ef_c20, ef_c24, ef_c28;
static u32 sgx_masks[3], map_a[16], map_b[16];
static bool missing_sgx, missing_asc, missing_aon, missing_mcc;
static bool have_base_pstate, has_shared_ro, has_shared_rw, bad_shared_rw_len;
static bool efuse_fault, sgx_fault;
static const char *const region_names[5] = {
    "gfx-handoff", "gfx-shared-l2-region", "gfx-shared-region", "gpu-region", "gfx-data",
};

static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/arm-io/sgx"))
        return missing_sgx ? -1 : 1;
    if (!strcmp(path, "/arm-io/gfx-asc"))
        return missing_asc ? -1 : 2;
    if (!strcmp(path, "/arm-io/aon-ptd"))
        return missing_aon ? -1 : 3;
    return -1;
}
static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    if (!strcmp(path, "/arm-io/sgx") && !missing_sgx) {
        trace[0] = 1;
        return 1;
    }
    return -1;
}
static int adt_get_reg(void *tree, int *path, const char *prop, int index, u64 *base, u64 *size)
{
    (void)tree; (void)path; (void)prop; (void)index; (void)size;
    *base = SGX_BASE;
    return 0;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    u32 dummy = 0;
    if (!len) len = &dummy;
    (void)tree;
    if (node == 2 && !strcmp(name, "segment-ranges")) { *len = segments_len; return segments; }
    if (node == 3 && !strcmp(name, "ptd_data_table")) { *len = ptd_len; return ptd_table; }
    if (node != 1) return NULL;
    if (!strcmp(name, "perf-states")) { *len = raw_count * raw_tables * sizeof(states[0]); return states; }
    if (!strcmp(name, "cs-perf-states")) { *len = sizeof(cs); return &cs; }
    if (!strcmp(name, "afr-perf-states")) { *len = sizeof(afr); return &afr; }
    if (have_base_pstate && !strcmp(name, "gpu-perf-base-pstate")) { *len = base_length; return &base_pstate; }
    if (has_shared_ro && !strcmp(name, "gfx-data-shared-ro-base")) { *len = sizeof(u64); return &shared_ro[0]; }
    if (has_shared_ro && !strcmp(name, "gfx-data-shared-ro-size")) { *len = sizeof(u64); return &shared_ro[1]; }
    if (has_shared_rw && !strcmp(name, "gfx-data-shared-rw-base")) {
        *len = bad_shared_rw_len ? sizeof(u32) : sizeof(u64);
        return &shared_rw[0];
    }
    if (has_shared_rw && !strcmp(name, "gfx-data-shared-rw-size")) {
        *len = bad_shared_rw_len ? sizeof(u32) : sizeof(u64);
        return &shared_rw[1];
    }
    return NULL;
}
static int adt_copy(const char *name, void *out, size_t size)
{
    u32 value;
    if (!strcmp(name, "perf-state-count")) value = raw_count;
    else if (!strcmp(name, "perf-state-table-count")) value = raw_tables;
    else if (!strcmp(name, "gpu-num-perf-states")) value = raw_max_state;
    else if (!missing_mcc && !strcmp(name, "active-mcc-bit-vector")) value = active_mcc;
    else if (has_shared_ro && (!strcmp(name, "gfx-data-shared-ro-base") ||
                               !strcmp(name, "gfx-data-shared-ro-size"))) {
        if (size != sizeof(u64)) return -1;
        memcpy(out, &shared_ro[strcmp(name, "gfx-data-shared-ro-base") ? 1 : 0], sizeof(u64));
        return sizeof(u64);
    } else if (has_shared_rw && (!strcmp(name, "gfx-data-shared-rw-base") ||
                                 !strcmp(name, "gfx-data-shared-rw-size"))) {
        if (bad_shared_rw_len || size != sizeof(u64)) return -1;
        memcpy(out, &shared_rw[strcmp(name, "gfx-data-shared-rw-base") ? 1 : 0], sizeof(u64));
        return sizeof(u64);
    } else {
        for (int i = 0; i < 5; i++) {
            size_t n = strlen(region_names[i]);
            if (!strncmp(name, region_names[i], n) &&
                (!strcmp(name + n, "-base") || !strcmp(name + n, "-size")) &&
                size == sizeof(u64)) {
                memcpy(out, &region[i][strcmp(name + n, "-base") ? 1 : 0], sizeof(u64));
                return sizeof(u64);
            }
        }
        return -1;
    }
    if (size != sizeof(u32)) return -1;
    memcpy(out, &value, sizeof(value));
    return sizeof(value);
}
#define ADT_GETPROP(a, n, name, ptr) adt_copy(name, (ptr), sizeof(*(ptr)))

static u32 read32(u64 addr)
{
    if (addr >= EFUSE && addr < EFUSE + 0x1000) {
        if (efuse_fault)
            exc_count++;
        switch (addr - EFUSE) {
        case 0xc10: return ef_c10;
        case 0xc14: return ef_c14;
        case 0xc1c: return ef_c1c;
        case 0xc20: return ef_c20;
        case 0xc24: return ef_c24;
        case 0xc28: return ef_c28;
        }
        exc_count++;
        return 0;
    }
    if (!sgx_on || sgx_fault) {
        exc_count++;
        return 0;
    }
    for (int i = 0; i < 3; i++)
        if (addr == SGX_BASE + 0xe01500 + 4 * i)
            return sgx_masks[i];
    for (int bank = 0; bank < 2; bank++) {
        u64 base = SGX_BASE + (bank ? 0xe01480 : 0xe01400);
        const u32 *map = bank ? map_b : map_a;
        if (addr >= base && addr < base + 32 && !(addr & 3)) {
            u32 i = (addr - base) / 2;
            return map[i] | (map[i + 1] << 16);
        }
    }
    exc_count++;
    return 0;
}
'''

MAIN = r'''
/* One J616C (M4 Max, 32 GPU cores), 26.6.2 boot firmware: MHz and mV per cluster. */
static const u32 j616c_mhz[16] = {0, 338, 618, 796, 924, 952, 1056, 1062,
                                  1182, 1182, 1312, 1242, 1380, 1326, 1470, 1578};
static const u32 j616c_mv[4][16] = {
    {125, 600, 650, 695, 720, 770, 770, 815, 815, 870, 870, 910, 910, 950, 950, 1030},
    {125, 595, 645, 690, 720, 760, 760, 805, 805, 850, 850, 890, 890, 930, 930, 1025},
    {125, 615, 660, 705, 725, 775, 775, 805, 805, 855, 855, 895, 895, 935, 935, 1030},
    {125, 600, 660, 700, 715, 770, 770, 805, 805, 860, 860, 900, 900, 940, 940, 1020},
};
static const u32 j616c_aux_mhz[8] = {24, 338, 618, 796, 992, 1188, 1380, 1470};
static const u32 j616c_aux_uv[8] = {585000, 585000, 630000, 680000,
                                    720000, 770000, 865000, 900000};
static const u32 j616c_aon_want[15] = {
    0x67305452, 0x00c20001, 0x00001401,
    0x67315452, 0x00970001, 0x00001401,
    0x67325452, 0x00920001, 0x00001401,
    0x67335452, 0x00c70001, 0x00001401,
    0x72305452, 0x012c0001, 0x00001401,
};
static const u32 good_a[16] = {0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15};
static const u32 good_b[16] = {0, 1, 2, 3, 4, 6, 7, 9, 11, 13, 15};
static const u32 want_rel_a[16] = {0, 10, 18, 25, 31, 40, 51, 62, 69, 78, 100};
static const u32 want_rel_b[16] = {0, 0, 22, 36, 47, 57, 68, 78, 84, 91, 100};
/* Public 40-core T6041 (J575C) OPP voltages (t6041-j575c.dts) and relative-A (hw/t6041.rs). */
static const u32 j575c_mv[4][16] = {
    {125, 600, 665, 705, 725, 770, 770, 810, 810, 860, 860, 900, 900, 940, 940, 1020},
    {125, 600, 665, 705, 725, 770, 770, 810, 810, 860, 860, 895, 895, 925, 925, 1020},
    {125, 600, 660, 700, 715, 770, 770, 805, 805, 860, 860, 900, 900, 940, 940, 1020},
    {125, 600, 660, 705, 730, 780, 780, 810, 810, 860, 860, 900, 900, 940, 940, 1020},
};
static const u32 j575c_aux_uv[8] = {585000, 585000, 635000, 685000,
                                    720000, 770000, 855000, 885000};
static const u32 j575c_rel_a[16] = {0, 10, 18, 25, 31, 40, 50, 62, 70, 78, 100};
/* Live J616C efuse words (0x508374c10, c14, c1c, c20, c24, c28) -> binary32 leakages. */
static const u32 j616c_core_leak_want[4] = {0x4509b800, 0x45137800, 0x4506e800, 0x45005800};
static const u32 j616c_sram_leak_want = 0x43f30000;
static const u32 j616c_cs_leak_want = 0x440d4000;
static const u32 j616c_afr_leak_want = 0x43e64000;
static const char *const region_paths[6] = {
    "/reserved-memory/uat-handoff", "/reserved-memory/uat-l2-pagetables",
    "/reserved-memory/uat-pagetables", "/reserved-memory/uat-ttbs",
    "/reserved-memory/gpu-fw-data", "/reserved-memory/gpu-fw-text",
};

static void add_opp_table(const char *name, u32 phandle, int count, int volts)
{
    char path[32];
    snprintf(path, sizeof(path), "/%s", name);
    int table = fdt_add_subnode(dt, 0, name);
    assert(table >= 0);
    assert(!fdt_setprop_u32(dt, table, "phandle", phandle));
    /* A new subnode goes before the existing ones, so add them from the last. */
    for (int i = count - 1; i >= 0; i--) {
        char node[16];
        snprintf(node, sizeof(node), "opp%02d", i);
        int opp = fdt_add_subnode(dt, fdt_path_offset(dt, path), node);
        assert(opp >= 0);
        assert(!fdt_setprop_u64(dt, opp, "opp-hz", 0));
        fdt32_t zero[4] = {0, 0, 0, 0};
        assert(!fdt_setprop(dt, opp, "opp-microvolt", zero, volts * sizeof(u32)));
    }
}

static void world_with(const char *compatible, int gpu_opps, int gpu_cells, int aux_opps)
{
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    add_opp_table("gpu-opp", 10, gpu_opps, gpu_cells);
    add_opp_table("cs-opp", 11, aux_opps, 1);
    add_opp_table("afr-opp", 12, aux_opps, 1);
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(resv >= 0);
    for (int i = 0; i < 6; i++) {
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"),
                                   region_paths[i] + strlen("/reserved-memory/"));
        assert(node >= 0 && !fdt_setprop_string(dt, node, "status", "disabled"));
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    int gpu = fdt_add_subnode(dt, soc, "gpu");
    assert(gpu >= 0 && !fdt_setprop_string(dt, gpu, "compatible", compatible));
    assert(!fdt_setprop_string(dt, gpu, "status", "okay"));
    assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 10));
    assert(!fdt_setprop_u32(dt, gpu, "apple,cs-opp", 11));
    assert(!fdt_setprop_u32(dt, gpu, "apple,afr-opp", 12));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu"));

    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 16; i++)
            states[t * 16 + i] = (struct perf_state){j616c_mhz[i] * 1000000u, j616c_mv[t][i]};
    for (int i = 0; i < 8; i++) {
        cs.states[i] = (struct aux_perf_state){j616c_aux_uv[i], j616c_aux_mhz[i] * 1000000UL};
        afr.states[i] = cs.states[i];
    }
    cs.dies = afr.dies = 1;
    cs.count = afr.count = 8;
    /* gfx-handoff, gfx-shared-l2-region, gfx-shared-region, gpu-region, gfx-data, TEXT */
    const u64 j616c_regions[6][2] = {
        {0x108fff70000UL, 0x4000},  {0x108fff74000UL, 0x4000},  {0x108fff78000UL, 0x40000},
        {0x108fffb8000UL, 0x4000},  {0x10001fc4000UL, 0x138000}, {0x10000d30000UL, 0x60000},
    };
    memcpy(region, j616c_regions, sizeof(region));
    segments[0] = (struct adt_segment_ranges){.phys = region[5][0], .iova = 0xfffffc0000000000UL,
                                              .remap = region[5][0], .size = region[5][1], .unk = 1};
    segments[1] = (struct adt_segment_ranges){.phys = region[4][0],
                                              .iova = 0xfffffc0000000000UL + region[5][1],
                                              .remap = region[4][0], .size = region[4][1]};
    segments_len = 2 * sizeof(segments[0]);
    os_firmware = (struct fw_version_info){"26.6.2", {26, 6, 2}, 3};
    raw_count = 16; raw_tables = 4; raw_max_state = 15; active_mcc = 0x3f;
    base_pstate = 1; base_length = sizeof(u32);
    have_base_pstate = false;
    /* Include one unrelated PTD row alongside the five GPU/SoC rows to verify filtering. */
    ptd_table[0] = 0x73305452; ptd_table[1] = 0x00f20001; ptd_table[2] = 0x00001601;
    memcpy(&ptd_table[3], j616c_aon_want, sizeof(j616c_aon_want));
    ptd_len = 18 * sizeof(u32);
    /* Live J616C efuse capture (0x508374c10, c14, c1c, c20, c24, c28). */
    ef_c10 = 0xc3120b78; ef_c14 = 0x002ac0b3; ef_c1c = 0x58046800;
    ef_c20 = 0xa11505fc; ef_c24 = 0x602880e0; ef_c28 = 0x00001322;
    sgx_masks[0] = 0xfdbf6fdb; sgx_masks[1] = 0xf6; sgx_masks[2] = 0;
    memcpy(map_a, good_a, sizeof(map_a));
    memcpy(map_b, good_b, sizeof(map_b));
    missing_sgx = missing_asc = missing_aon = missing_mcc = false;
    has_shared_ro = has_shared_rw = bad_shared_rw_len = efuse_fault = sgx_fault = false;
    sgx_power_fail = false;
    shared_ro[0] = shared_ro[1] = shared_rw[0] = shared_rw[1] = 0;
    sgx_on = false;
    enables = restores = enable_attempts = exc_count = 0;
}

static void world(void) { world_with("apple,agx-t6041", 16, 4, 8); }

static void world_27_2(void)
{
    world();
    os_firmware = (struct fw_version_info){"27.2", {27, 2, 0}, 3};
    region[4][0] = 0x10001f54000UL;
    region[4][1] = 0xc4000;
    region[5][0] = 0x10000d70000UL;
    region[5][1] = 0x64000;
    shared_ro[0] = region[4][0] + region[4][1];
    shared_ro[1] = 0x8000;
    has_shared_ro = true;
    segments[0] = (struct adt_segment_ranges){.phys = region[5][0], .iova = 0xfffffc0000000000UL,
                                              .remap = region[5][0], .size = 0x64000, .unk = 1};
    segments[1] = (struct adt_segment_ranges){.phys = region[4][0], .iova = 0xfffffc0000064000UL,
                                              .remap = region[4][0], .size = 0xc4000, .unk = 0x10};
    segments[2] = (struct adt_segment_ranges){.phys = shared_ro[0], .iova = 0xfffffc0000128000UL,
                                              .remap = shared_ro[0], .size = 0x8000, .unk = 0};
    segments_len = 3 * sizeof(segments[0]);
}

static const char *status(const char *path)
{
    int node = fdt_path_offset(dt, path);
    assert(node >= 0);
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s ? s : "okay";
}

static void expect_refused(const char *what, int expected_enables)
{
    assert(dt_set_gpu_t6041(dt) == 0);
    if (strcmp(status("gpu"), "disabled")) {
        fprintf(stderr, "%s: GPU was not disabled\n", what);
        exit(1);
    }
    for (int i = 0; i < 6; i++)
        if (strcmp(status(region_paths[i]), "disabled")) {
            fprintf(stderr, "%s: %s left published\n", what, region_paths[i]);
            exit(1);
        }
    if (sgx_on || enables != expected_enables || restores != enables) {
        fprintf(stderr, "%s: SGX power enables %d restores %d on %d\n", what, enables, restores,
                sgx_on);
        exit(1);
    }
    int gpu = fdt_path_offset(dt, "gpu");
    assert(!fdt_getprop(dt, gpu, "apple,firmware-version", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,t6041-handoff-version", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,max-power-mw", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,core-leak-coef", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,sram-leak-coef", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,cs-leak-coef", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,afr-leak-coef", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,perf-relative-a", NULL));
    printf("PASS refused: %s\n", what);
}

static void expect_untouched(const char *what, const char *gpu_status)
{
    assert(dt_set_gpu_t6041(dt) == 0);
    assert(!strcmp(status("gpu"), gpu_status));
    assert(enables == 0 && restores == 0 && !sgx_on);
    for (int i = 0; i < 6; i++)
        assert(!strcmp(status(region_paths[i]), "disabled"));
    int gpu = fdt_path_offset(dt, "gpu");
    assert(!fdt_getprop(dt, gpu, "apple,firmware-version", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,t6041-handoff-version", NULL));
    printf("PASS untouched: %s\n", what);
}

int main(void)
{
    /* The accepted case publishes verified ADT/efuse/SGX handoff v2 and restores SGX power. */
    world();
    assert(dt_set_gpu_t6041(dt) == 0);
    assert(!strcmp(status("gpu"), "okay"));
    assert(enables == 1 && restores == 1 && !sgx_on);
    for (int i = 0; i < 6; i++) {
        assert(!strcmp(status(region_paths[i]), "okay"));
        int len;
        const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, region_paths[i]), "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == region[i][0] &&
               fdt64_ld(&reg[1]) == region[i][1]);
    }
    int table = fdt_path_offset(dt, "/gpu-opp"), opp, n = 0, len;
    fdt_for_each_subnode(opp, dt, table)
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == 16 && hz && fdt64_ld(hz) == j616c_mhz[n] * 1000000ULL);
        for (int c = 0; c < 4; c++)
            assert(fdt32_ld(&uv[c]) == j616c_mv[c][n] * 1000);
        n++;
    }
    assert(n == 16);
    n = 0;
    fdt_for_each_subnode(opp, dt, fdt_path_offset(dt, "/cs-opp"))
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == 4 && fdt32_ld(uv) == j616c_aux_uv[n] &&
               fdt64_ld(hz) == j616c_aux_mhz[n] * 1000000ULL);
        n++;
    }
    assert(n == 8);
    n = 0;
    fdt_for_each_subnode(opp, dt, fdt_path_offset(dt, "/afr-opp"))
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == 4 && fdt32_ld(uv) == j616c_aux_uv[n] &&
               fdt64_ld(hz) == j616c_aux_mhz[n] * 1000000ULL);
        n++;
    }
    assert(n == 8);
    int gpu = fdt_path_offset(dt, "gpu");
    const fdt32_t *version = fdt_getprop(dt, gpu, "apple,firmware-version", &len);
    assert(version && len == 12 && fdt32_ld(&version[0]) == 26 && fdt32_ld(&version[1]) == 6 &&
           fdt32_ld(&version[2]) == 2);
    const fdt32_t *hver = fdt_getprop(dt, gpu, "apple,t6041-handoff-version", &len);
    assert(hver && len == 4 && fdt32_ld(hver) == 2);
    const fdt32_t *mcc = fdt_getprop(dt, gpu, "apple,active-mcc-bit-vector", &len);
    assert(mcc && len == 4 && fdt32_ld(mcc) == 0x3f);
    const fdt64_t *cmask = fdt_getprop(dt, gpu, "apple,core-mask", &len);
    assert(cmask && len == 8 && fdt64_ld(cmask) == 0xf6fdbf6fdbULL);
    const fdt32_t *bp = fdt_getprop(dt, gpu, "apple,perf-base-pstate", &len);
    assert(bp && len == 4 && fdt32_ld(bp) == 1);
    const fdt32_t *aon = fdt_getprop(dt, gpu, "apple,aon-table", &len);
    assert(aon && len == 15 * 4);
    for (int i = 0; i < 15; i++)
        assert(fdt32_ld(&aon[i]) == j616c_aon_want[i]);
    const fdt32_t *ma = fdt_getprop(dt, gpu, "apple,perf-map-a", &len);
    assert(ma && len == 16 * 4);
    const fdt32_t *mb = fdt_getprop(dt, gpu, "apple,perf-map-b", &len);
    assert(mb && len == 16 * 4);
    const fdt32_t *ra = fdt_getprop(dt, gpu, "apple,perf-relative-a", &len);
    assert(ra && len == 16 * 4);
    const fdt32_t *rb = fdt_getprop(dt, gpu, "apple,perf-relative-b", &len);
    assert(rb && len == 16 * 4);
    for (int i = 0; i < 16; i++) {
        assert(fdt32_ld(&ma[i]) == good_a[i]);
        assert(fdt32_ld(&mb[i]) == good_b[i]);
        assert(fdt32_ld(&ra[i]) == want_rel_a[i]);
        assert(fdt32_ld(&rb[i]) == want_rel_b[i]);
    }
    const fdt32_t *cleak = fdt_getprop(dt, gpu, "apple,core-leak-coef", &len);
    assert(cleak && len == 4 * 4);
    const fdt32_t *sleak = fdt_getprop(dt, gpu, "apple,sram-leak-coef", &len);
    assert(sleak && len == 4 * 4);
    for (int c = 0; c < 4; c++) {
        assert(fdt32_ld(&cleak[c]) == j616c_core_leak_want[c]);
        assert(fdt32_ld(&sleak[c]) == j616c_sram_leak_want);
    }
    const fdt32_t *csleak = fdt_getprop(dt, gpu, "apple,cs-leak-coef", &len);
    assert(csleak && len == 4 && fdt32_ld(csleak) == j616c_cs_leak_want);
    const fdt32_t *afrleak = fdt_getprop(dt, gpu, "apple,afr-leak-coef", &len);
    assert(afrleak && len == 4 && fdt32_ld(afrleak) == j616c_afr_leak_want);
    const fdt32_t *mw = fdt_getprop(dt, gpu, "apple,max-power-mw", &len);
    assert(mw && len == 4 && fdt32_ld(mw) == 113169);
    puts("PASS accepted: J616C v2 handoff, leakage, power, tables and regions");

    /* 40-core (8-MCC) J575C OPPs + measured 40-core T6041 leakage reference (hw/t6041.rs). */
    world();
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 16; i++)
            states[t * 16 + i] = (struct perf_state){j616c_mhz[i] * 1000000u, j575c_mv[t][i]};
    for (int i = 0; i < 8; i++) {
        cs.states[i] = (struct aux_perf_state){j575c_aux_uv[i], j616c_aux_mhz[i] * 1000000UL};
        afr.states[i] = cs.states[i];
    }
    active_mcc = 0xff;
    sgx_masks[0] = 0xffffffff; sgx_masks[1] = 0xff; sgx_masks[2] = 0;
    have_base_pstate = true; base_pstate = 1;
    u64 ref40 = (1331ULL << 1) | (1317ULL << 15) | (1304ULL << 29) | (1286ULL << 43);
    ef_c10 = (u32)ref40; ef_c14 = (u32)(ref40 >> 32);
    ef_c1c = 527U << 9; ef_c20 = 151U << 24; ef_c24 = 0; ef_c28 = 297U << 4;
    assert(dt_set_gpu_t6041(dt) == 0 && !strcmp(status("gpu"), "okay"));
    gpu = fdt_path_offset(dt, "gpu");
    hver = fdt_getprop(dt, gpu, "apple,t6041-handoff-version", &len);
    assert(hver && len == 4 && fdt32_ld(hver) == 2);
    cmask = fdt_getprop(dt, gpu, "apple,core-mask", &len);
    assert(cmask && len == 8 && fdt64_ld(cmask) == 0xffffffffffULL);
    mcc = fdt_getprop(dt, gpu, "apple,active-mcc-bit-vector", &len);
    assert(mcc && len == 4 && fdt32_ld(mcc) == 0xff);
    cleak = fdt_getprop(dt, gpu, "apple,core-leak-coef", &len);
    assert(cleak && len == 16 && fdt32_ld(&cleak[0]) == 0x44f9c000 &&
           fdt32_ld(&cleak[1]) == 0x44f72000 && fdt32_ld(&cleak[2]) == 0x44f4b000 &&
           fdt32_ld(&cleak[3]) == 0x44f15000);
    sleak = fdt_getprop(dt, gpu, "apple,sram-leak-coef", &len);
    assert(sleak && len == 16 && fdt32_ld(&sleak[0]) == 0x43e40000 &&
           fdt32_ld(&sleak[3]) == 0x43e40000);
    csleak = fdt_getprop(dt, gpu, "apple,cs-leak-coef", &len);
    assert(csleak && len == 4 && fdt32_ld(csleak) == 0x44040000);
    afrleak = fdt_getprop(dt, gpu, "apple,afr-leak-coef", &len);
    assert(afrleak && len == 4 && fdt32_ld(afrleak) == 0x43df8000);
    ra = fdt_getprop(dt, gpu, "apple,perf-relative-a", &len);
    assert(ra && len == 16 * 4);
    rb = fdt_getprop(dt, gpu, "apple,perf-relative-b", &len);
    assert(rb && len == 16 * 4);
    for (int i = 0; i < 16; i++) {
        assert(fdt32_ld(&ra[i]) == j575c_rel_a[i]);
        assert(fdt32_ld(&rb[i]) == want_rel_b[i]);
    }
    mw = fdt_getprop(dt, gpu, "apple,max-power-mw", &len);
    assert(mw && len == 4 && fdt32_ld(mw) == 129702);
    puts("PASS accepted: 40-core 8-MCC J575C T6041 power, relative-A and leakage parity");

    world_with("apple,agx-t6040", 16, 2, 8);
    expect_untouched("T6040 GPU node", "okay");
    world(); assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "gpu"), "status", "disabled"));
    expect_untouched("GPU disabled in the device tree", "disabled");

    world_with("apple,agx-t6041", 16, 2, 8);
    expect_refused("two-cell OPP table", 0);
    world_with("apple,agx-t6041", 15, 4, 8);
    expect_refused("OPP table one entry short", 0);
    world_with("apple,agx-t6041", 16, 4, 6);
    expect_refused("CS/AFR tables shorter than the ADT's", 0);
    world(); raw_tables = 2;
    expect_refused("two cluster tables", 0);
    world(); raw_count = 15;
    expect_refused("wrong raw OPP count", 0);
    world(); raw_max_state = 10;
    expect_refused("logical count in the raw ADT count", 0);
    world(); missing_mcc = true;
    expect_refused("missing active-mcc-bit-vector", 0);
    world(); active_mcc = 0;
    expect_refused("zero active-mcc-bit-vector", 0);
    world(); active_mcc = 0x1ff;
    expect_refused("out-of-range active-mcc-bit-vector", 0);
    world(); missing_sgx = true;
    expect_refused("missing SGX", 0);
    world(); missing_asc = true;
    expect_refused("missing GFX ASC", 0);
    world(); missing_aon = true;
    expect_refused("missing /arm-io/aon-ptd", 0);
    world(); ptd_len = 14;
    expect_refused("unaligned ptd_data_table", 0);
    world(); ptd_table[15] = 0xdeadbeef;
    expect_refused("missing r0TR in ptd_data_table", 0);
    world(); ptd_table[0] = ptd_table[3];
    expect_refused("duplicate g0TR in ptd_data_table", 0);
    /* J575c: 40-core (8-MCC) topology whose ADT sets gpu-perf-base-pstate = 3. */
    world();
    active_mcc = 0xff;
    sgx_masks[0] = 0xffffffff; sgx_masks[1] = 0xff; sgx_masks[2] = 0;
    have_base_pstate = true; base_pstate = 3;
    assert(dt_set_gpu_t6041(dt) == 0 && !strcmp(status("gpu"), "okay"));
    gpu = fdt_path_offset(dt, "gpu");
    cmask = fdt_getprop(dt, gpu, "apple,core-mask", &len);
    assert(cmask && len == 8 && fdt64_ld(cmask) == 0xffffffffffULL);
    bp = fdt_getprop(dt, gpu, "apple,perf-base-pstate", &len);
    assert(bp && len == 4 && fdt32_ld(bp) == 3);
    rb = fdt_getprop(dt, gpu, "apple,perf-relative-b", &len);
    assert(rb && len == 16 * 4);
    const u32 want_rel_b_base3[16] = {0, 0, 0, 0, 16, 33, 49, 65, 74, 86, 100};
    for (int i = 0; i < 16; i++)
        assert(fdt32_ld(&rb[i]) == want_rel_b_base3[i]);
    puts("PASS accepted: 40-core T6041 with gpu-perf-base-pstate = 3");

    world(); have_base_pstate = true; base_length = 0;
    expect_refused("empty present base property", 0);
    world(); have_base_pstate = true; base_pstate = 0;
    expect_refused("zero gpu-perf-base-pstate", 0);
    world(); have_base_pstate = true; base_pstate = 15;
    expect_refused("gpu-perf-base-pstate at top raw OPP", 0);
    world(); have_base_pstate = true; base_pstate = 10;
    expect_refused("gpu-perf-base-pstate at top logical state", 1);
    world(); have_base_pstate = true; base_pstate = 11;
    expect_refused("gpu-perf-base-pstate past logical states", 1);
    world(); states[0].freq = 100000000;
    expect_refused("off operating point has a clock", 0);
    world(); states[2 * 16 + 5].freq += 1000000;
    expect_refused("a cluster runs a different clock", 0);
    world(); states[3 * 16 + 9].volt = 1400;
    expect_refused("voltage out of range", 0);
    world(); cs.dies = 2;
    expect_refused("two-die CS table", 0);
    world(); afr.states[3].freq = 0;
    expect_refused("AFR state without a clock", 0);
    world(); cs.count = 9;
    expect_refused("CS count beyond the property", 0);
    world(); segments[1].phys += 0x4000;
    expect_refused("DATA segment disagrees with gfx-data", 0);
    world(); segments[0].phys = region[0][0];
    expect_refused("TEXT overlaps handoff", 0);
    world(); region[4][1] = 0x4100;
    expect_refused("unaligned region", 0);
    world(); region[2][0] = region[1][0];
    expect_refused("overlapping regions", 0);
    world(); region[0][0] = 1UL << 42;
    expect_refused("region outside physical address width", 0);
    world(); region[3][0] = ~0UL & ~(SZ_16K - 1);
    expect_refused("region wraps", 0);
    world(); assert(!fdt_del_node(dt, fdt_path_offset(dt, "/reserved-memory/gpu-fw-data")));
    assert(dt_set_gpu_t6041(dt) == 0 && !strcmp(status("gpu"), "disabled") && !enables);
    for (int i = 0; i < 4; i++)
        assert(!strcmp(status(region_paths[i]), "disabled"));
    puts("PASS refused: missing reserved-memory node");

    /* Efuse refusal checks (must fail closed BEFORE powering SGX and strip stale version). */
    world(); efuse_fault = true;
    expect_refused("efuse read faults", 0);
    world();
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "gpu"), "apple,t6041-handoff-version", 1));
    efuse_fault = true;
    expect_refused("efuse fault deletes stale handoff version", 0);
    world(); ef_c10 &= ~(0x3fffU << 1);
    expect_refused("cluster 0 core efuse is zero", 0);
    world(); ef_c10 |= 7U << 29; ef_c14 |= 0x7ffU;
    expect_refused("cluster 2 cross-word core efuse is saturated", 0);
    world(); ef_c14 &= ~(0x3fffU << 11);
    expect_refused("cluster 3 core efuse is zero", 0);
    world(); ef_c20 &= ~(0xffU << 24); ef_c24 &= ~3U;
    expect_refused("cross-word SRAM efuse is zero", 0);
    world(); ef_c20 |= 0xffU << 24; ef_c24 |= 3U;
    expect_refused("cross-word SRAM efuse is saturated", 0);
    world(); ef_c1c &= ~(0x1fffU << 9);
    expect_refused("CS efuse is zero", 0);
    world(); ef_c1c |= 0x1fffU << 9;
    expect_refused("CS efuse is saturated", 0);
    world(); ef_c28 &= ~(0xfffU << 4);
    expect_refused("AFR efuse is zero", 0);
    world(); ef_c28 |= 0xfffU << 4;
    expect_refused("AFR efuse is saturated", 0);

    /* SGX read / mask / perf-map / power-ladder refusal checks (must power off SGX cleanly). */
    world(); sgx_power_fail = true;
    expect_refused("SGX power enable fails", 0);
    assert(enable_attempts == 1);
    world(); sgx_fault = true;
    expect_refused("SGX read faults", 1);
    world(); sgx_masks[2] = 1;
    expect_refused("leftover core mask word 2", 1);
    world(); sgx_masks[1] = 0x1ff;
    expect_refused("leftover core mask above 40 bits", 1);
    world(); sgx_masks[1] = 0; sgx_masks[0] &= ~(3U << 30);
    expect_refused("cluster 3 has zero active cores", 1);
    world(); map_a[15] = 3;
    expect_refused("nonzero unused perf map tail", 1);
    world(); map_b[10] = 14;
    expect_refused("B map does not reach top OPP", 1);
    world(); map_a[5] = 4;
    expect_refused("A map does not climb", 1);
    world(); states[6].freq = states[16 + 6].freq = states[32 + 6].freq = states[48 + 6].freq = states[4].freq;
    expect_refused("ADT frequency does not climb along A map", 1);
    world(); states[6].volt = states[16 + 6].volt = states[32 + 6].volt = states[48 + 6].volt = 500;
    expect_refused("power falls along A map", 1);
    world();
    /* Extreme legal voltage + max non-saturated leakage pushes power over the 250 W T6041 cap. */
    for (int t = 0; t < 4; t++)
        states[t * 16 + 15].volt = 1300;
    cs.states[7].volt = 1300000;
    u64 max_raw = (0x3ffeULL << 1) | (0x3ffeULL << 15) | (0x3ffeULL << 29) | (0x3ffeULL << 43);
    ef_c10 = (u32)max_raw; ef_c14 = (u32)(max_raw >> 32); ef_c1c = 0x1ffeU << 9;
    expect_refused("computed max power exceeds 250 W T6041 ceiling", 1);

    /* 27.2 (26B123) 3-segment framing: __DATA + contiguous __DATA_SHARED_RO in gpu-fw-data. */
    world_27_2();
    has_shared_rw = true;
    assert(dt_set_gpu_t6041(dt) == 0);
    assert(!strcmp(status("gpu"), "okay"));
    assert(enables == 1 && restores == 1 && !sgx_on);
    for (int i = 0; i < 6; i++) {
        assert(!strcmp(status(region_paths[i]), "okay"));
        const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, region_paths[i]), "reg", &len);
        u64 want_size = i == 4 ? region[4][1] + shared_ro[1] : region[i][1];
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == region[i][0] &&
               fdt64_ld(&reg[1]) == want_size);
    }
    version = fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,firmware-version", &len);
    assert(version && len == 12 && fdt32_ld(&version[0]) == 27 && fdt32_ld(&version[1]) == 2 &&
           fdt32_ld(&version[2]) == 0);
    hver = fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,t6041-handoff-version", &len);
    assert(hver && len == 4 && fdt32_ld(hver) == 2);
    mw = fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,max-power-mw", &len);
    assert(mw && len == 4 && fdt32_ld(mw) == 113169);
    puts("PASS accepted: J616C 27.2 3-segment regions");

    world_27_2(); segments_len = sizeof(segments[0]);
    expect_refused("1-segment gfx-asc", 0);
    world_27_2(); segments_len = 4 * sizeof(segments[0]);
    expect_refused("4-segment gfx-asc", 0);
    world_27_2(); has_shared_ro = false;
    expect_refused("missing gfx-data-shared-ro", 0);
    world_27_2(); segments[2].phys += 0x4000;
    expect_refused("SHARED_RO segment base disagrees with sgx", 0);
    world_27_2(); segments[2].remap += 0x4000;
    expect_refused("SHARED_RO segment remap is not identity", 0);
    world_27_2(); segments[2].size += 0x4000;
    expect_refused("SHARED_RO segment size disagrees with sgx", 0);
    world_27_2(); shared_ro[0] += 0x4000; segments[2].phys = segments[2].remap = shared_ro[0];
    expect_refused("SHARED_RO not contiguous with DATA", 0);
    world_27_2(); shared_ro[1] = 0x2000; segments[2].size = 0x2000;
    expect_refused("unaligned SHARED_RO size", 0);
    world_27_2(); has_shared_rw = true; shared_rw[1] = 0x4000;
    expect_refused("non-zero gfx-data-shared-rw", 0);
    world_27_2(); has_shared_rw = true; bad_shared_rw_len = true;
    expect_refused("malformed gfx-data-shared-rw", 0);
    world_27_2(); segments[0].phys = segments[0].remap = shared_ro[0];
    expect_refused("TEXT overlaps SHARED_RO tail", 0);
    world_27_2();
    region[4][0] = (1UL << 42) - region[4][1];
    shared_ro[0] = region[4][0] + region[4][1];
    segments[1].phys = segments[1].remap = region[4][0];
    segments[2].phys = segments[2].remap = shared_ro[0];
    expect_refused("extended fw-data exceeds 42-bit OAS", 0);
    return 0;
}
'''


def test_t6041_gpu_fill(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    defs = source[source.index("#define MAX_PSTATES"):source.index("int32_t rust_gpu_initdata_size(")]
    t6040_start = source.index("/*\n * T6040 (M4 Pro).")
    t6040_helpers = source[t6040_start:source.index("static int dt_set_gpu_t6040(", t6040_start)]
    t6040_helpers, replaced = re.subn(r'#define T6040_ROUND\(x\) __asm__\(""\s*:\s*"\+w"\(x\)\)',
                                      '#define T6040_ROUND(x) __asm__("" : "+m"(x))', t6040_helpers)
    assert replaced == 1
    header = (ROOT / "src/kboot.h").read_text()
    fw = header[header.index("struct gpu_fw_layout {"):]
    fw = fw[:fw.index("};\n") + 3]
    t6041_start = source.index("/*\n * T6041 (M4 Max).")
    t6041 = source[t6041_start:source.index("/*\n * T6050 G17S", t6041_start)]
    bail = '#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)\n'
    code = (HARNESS + bail + defs + ADT +
            function(source, "static int dt_set_resvmem(") +
            function(source, "static int fdt_set_aux_opp(") +
            fw + t6040_helpers + t6041 + MAIN)
    cfile = tmp_path / "t6041-gpu-fill.c"
    cfile.write_text(code)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "t6041-gpu-fill"
    subprocess.run([*host_cc(), "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(libfdt),
                    str(cfile), *sources, "-lm", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 73, result.stdout
