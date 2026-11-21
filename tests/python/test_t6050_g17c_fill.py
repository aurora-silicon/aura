# SPDX-License-Identifier: MIT
"""Run kboot_gpu.c's T6050 G17C (M5 Max) GPU fill on real FDTs with a modelled ADT.

The ADT values are synthetic, laid out as a T6050 ADT lays them out: per-cluster core and SRAM
operating points with an off state, CS and AFR tables with an off state and their own SRAM floor,
UAT and firmware regions and AON records (with other records between them). The number of states
and clusters is the test's own and varies between cases. The trees come in both shapes the fill
takes: a "gpu" alias, and no alias with the node found by its compatible, at a path and under power
domain and mailbox node names of the test's own. The test checks what the fill publishes, that it
enables exactly the power domains and mailboxes the GPU node names (with their arguments and
parents), what it refuses, and that a refusal leaves the GPU node disabled and nothing else
written, also in trees with no room left.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3] + "\n"


HARNESS = r'''
#include <assert.h>
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
#define min(a, b) ((a) < (b) ? (a) : (b))
#define T6050_GPU_INIT
#define T6050_GPU_TEXT_FN
#define G15_PS_MIN_MV 400
#define G15_PS_MAX_MV 1500
#define G15_PS_MAX_HZ 3000000000u
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
struct perf_state { u32 freq; u32 volt; };
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; } __attribute__((packed));

static void *adt = (void *)1;
static unsigned char blob[65536];
static void *dt = blob;
struct fw_version_info { const char *string; u32 num[3]; size_t num_length; };
static struct fw_version_info os_firmware = {"27.0", {27, 0, 0}, 3};
static int firmware_set_fdt(void *tree, int node, const char *prop,
                            const struct fw_version_info *version)
{
    fdt32_t cells[3];
    for (size_t i = 0; i < version->num_length; i++)
        cells[i] = cpu_to_fdt32(version->num[i]);
    return fdt_setprop(tree, node, prop, cells, version->num_length * sizeof(u32));
}
/* Whether the heap has room for the copy of the tree: the test can say no */
static bool heap_full;
static bool heapblock_malloc_fits(size_t size) { (void)size; return !heap_full; }

/* The leakage fuses: three words in the PMGR window on the GPU's die */
#define LEAK_DIE  0x2300000000UL
#define LEAK_BASE (LEAK_DIE + 0x88300000UL)
static bool leak_fault, leak_window_missing;
static int g15_guard_begin(void) { return 0; }
static int g15_guard_end(int before) { (void)before; return leak_fault ? -1 : 0; }
static u32 read32(u64 addr)
{
    return 0x5ea10000u + (u32)(addr - LEAK_BASE - 0x74198);
}
'''

ADT = r'''
/*
 * Synthetic operating points: MHz per state and, per cluster, core and SRAM mV. State 0 is the off
 * state (0 MHz, 125 mV core). Every cluster has its own voltages.
 */
#define MAX_STATES 16
#define MAX_TABLES 9
static u32 state_mhz(int i) { return i ? 300 + 100 * i : 0; }
static u32 core_mv(int c, int i) { return i ? 600 + 30 * i + 5 * c * (i % 3) : 125; }
static u32 sram_mv(int c, int i)
{
    u32 v = core_mv(c, i) + 10 * (c % 2);
    return !i || v < 800 ? 800 : v;
}
/*
 * CS (t 0) and AFR (t 1): an off state at 125 mV and 24 MHz, then active states, then their SRAM
 * floor. The two tables have different active states.
 */
static u64 aux_uv(int t, int i) { return i ? 560000 + 32000 * i + 8000 * t : 125000; }
static u64 aux_hz(int t, int i) { return (i ? 300 + 100 * i + 50 * t : 24) * 1000000UL; }
#define AUX_SRAM_UV 800000
/* gfx-handoff, gfx-shared-l2-region, gfx-shared-region, gpu-region, then the firmware */
static const char *const region_names[8] = {
    "gfx-handoff", "gfx-shared-l2-region", "gfx-shared-region", "gpu-region",
    "gfx-data", "gfx-data-shared-ro", "gfx1-data", "gfx1-data-shared-ro",
};
static const u64 test_regions[8][2] = {
    {0x1ff00000000UL, 0x4000}, {0x1ff00004000UL, 0x4000}, {0x1ff00008000UL, 0x40000},
    {0x1ff00048000UL, 0x4000}, {0x10003000000UL, 0x80000}, {0x10003080000UL, 0x4000},
    {0x10003084000UL, 0x80000}, {0x10003104000UL, 0x4000},
};
static const u64 test_text[2][2] = {{0x10002000000UL, 0x40000}, {0x10002040000UL, 0x44000}};

static struct perf_state states[MAX_STATES * MAX_TABLES], sram[MAX_STATES * MAX_TABLES];
/* {dies, count}, count {uV, Hz} states, then the SRAM floor: up to 2 + 2 * 16 + 1 words */
struct aux { u64 words[2 + 2 * MAX_STATES + 1]; };
#define DIES(a) ((a).words[0])
#define STATE_UV(a, i) ((a).words[2 + 2 * (i)])
#define STATE_HZ(a, i) ((a).words[3 + 2 * (i)])
#define FLOOR_UV(a, n) ((a).words[2 + 2 * (n)])
static struct aux cs, afr;
static u32 aux_len[2];
static struct adt_segment_ranges segments[2][3];
static u64 region[8][2];
static u32 raw_count, raw_tables, die_count;
static bool has_die_count, missing_sram;
static u32 aon[6][3];
static u32 aon_count;

static int adt_path_offset(void *tree, const char *path)
{
    static const char *const paths[] = {"/arm-io/sgx", "/arm-io/gfx-asc", "/arm-io/gfx1-asc",
                                        "/arm-io/aon-ptd", "/arm-io", "/arm-io/pmgr"};
    (void)tree;
    for (size_t i = 0; i < ARRAY_SIZE(paths); i++)
        if (!strcmp(path, paths[i]))
            return i + 1;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if ((node == 2 || node == 3) && !strcmp(name, "segment-ranges")) {
        *len = sizeof(segments[0]);
        return segments[node - 2];
    }
    if (node == 4 && !strcmp(name, "ptd_data_table")) {
        *len = aon_count * 12;
        return aon;
    }
    if (node != 1) return NULL;
    if (!strcmp(name, "perf-states")) { *len = raw_count * raw_tables * 8; return states; }
    if (!strcmp(name, "perf-states-sram") && !missing_sram) {
        *len = raw_count * raw_tables * 8;
        return sram;
    }
    if (!strcmp(name, "cs-perf-states")) { *len = aux_len[0]; return &cs; }
    if (!strcmp(name, "afr-perf-states")) { *len = aux_len[1]; return &afr; }
    return NULL;
}
static int adt_copy(int node, const char *name, void *out, size_t size)
{
    u32 value;
    if (node == 5 && !strcmp(name, "die-count") && has_die_count) value = die_count;
    else if (node != 1) return -1;
    else if (!strcmp(name, "perf-state-count")) value = raw_count;
    else if (!strcmp(name, "perf-state-table-count")) value = raw_tables;
    else {
        for (int i = 0; i < 8; i++) {
            size_t n = strlen(region_names[i]);
            if (!strncmp(name, region_names[i], n) && size == sizeof(u64) &&
                (!strcmp(name + n, "-base") || !strcmp(name + n, "-size"))) {
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
#define ADT_GETPROP(a, n, name, ptr) adt_copy(n, name, (ptr), sizeof(*(ptr)))
static int adt_path_offset_trace(void *tree, const char *path, int *offsets)
{
    return offsets[0] = adt_path_offset(tree, path);
}
static int adt_get_reg(void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    /* sgx on die 0; PMGR windows on dies 0 and 1, the GPU's leakage window second */
    static const u64 pmgr[3][2] = {{LEAK_DIE + 0x80000000UL, 0x100000},
                                   {LEAK_BASE, 0x80000}, {0x6388300000UL, 0x80000}};
    (void)tree, (void)prop;
    if (path[0] == 1 && !idx) { *addr = LEAK_DIE; return 0; }
    if (path[0] != 6 || idx >= 3 || (leak_window_missing && idx == 1)) return -1;
    *addr = pmgr[idx][0];
    if (size) *size = pmgr[idx][1];
    return 0;
}
'''

MAIN = r'''
static const char *const resv_paths[6] = {
    "/reserved-memory/uat-handoff", "/reserved-memory/uat-l2-pagetables",
    "/reserved-memory/uat-pagetables", "/reserved-memory/uat-ttbs",
    "/reserved-memory/gpu-fw-text", "/reserved-memory/gpu-fw-data",
};
#define GPU_PATH "/soc/graphics@2600000"
/* Power domains and mailboxes, in the order the fill must enable them, then one nothing names */
static const char *const dep_paths[6] = {
    "/pm@0/afr", "/pm@0/gpx", "/pm@0/gfx", "/soc/mbox-a", "/soc/mbox-b", "/pm@0/other",
};
enum { PH_GPU_OPP = 10, PH_CS_OPP, PH_AFR_OPP, PH_AFR = 20, PH_GPX, PH_GFX, PH_MBOX_A,
       PH_MBOX_B, PH_OTHER };

/* Exact names, so that a soc@... sibling ahead of soc does not hide the test's nodes */
static int node(const char *path)
{
    int n = kboot_fdt_path_exact(dt, path);
    assert(n >= 0);
    return n;
}

static int add(const char *parent, const char *name)
{
    int n = fdt_add_subnode(dt, node(parent), name);
    assert(n >= 0);
    return n;
}

static void add_opp_table(const char *name, u32 phandle, int count, int volts, bool sram_prop)
{
    char path[32];
    snprintf(path, sizeof(path), "/%s", name);
    assert(!fdt_setprop_u32(dt, add("/", name), "phandle", phandle));
    /* A new subnode goes before the existing ones, so add them from the last. */
    for (int i = count - 1; i >= 0; i--) {
        char opp_name[16];
        snprintf(opp_name, sizeof(opp_name), "opp%02d", i);
        int opp = add(path, opp_name);
        fdt32_t zero[MAX_TABLES] = {0};
        assert(!fdt_setprop_u64(dt, opp, "opp-hz", 0));
        assert(!fdt_setprop(dt, opp, "opp-microvolt", zero, volts * sizeof(u32)));
        if (sram_prop)
            assert(!fdt_setprop(dt, opp, "opp-microvolt-sram", zero, volts * sizeof(u32)));
    }
}

static void set_cells(const char *path, const char *prop, const u32 *v, int count)
{
    fdt32_t cells[16];
    assert(count <= (int)ARRAY_SIZE(cells));
    for (int i = 0; i < count; i++)
        cells[i] = cpu_to_fdt32(v[i]);
    assert(!fdt_setprop(dt, node(path), prop, cells, count * sizeof(u32)));
}

static void add_provider(const char *parent, const char *name, u32 phandle, const char *cells,
                         u32 args)
{
    int n = add(parent, name);
    assert(!fdt_setprop_u32(dt, n, "phandle", phandle));
    assert(!fdt_setprop_u32(dt, n, cells, args));
    assert(!fdt_setprop_string(dt, n, "status", "disabled"));
}

enum shape { ALIAS, COMPATIBLE };

/* The shape of one case: the ADT's states, clusters and CS/AFR states, and the tree's */
static struct {
    enum shape shape;
    int states, tables, aux;          /* ADT */
    int dt_states, dt_cells, dt_aux;  /* DT */
    bool dt_sram, sibling;
} cfg;

static void config(enum shape shape)
{
    cfg.shape = shape;
    cfg.states = cfg.dt_states = 14;
    cfg.tables = cfg.dt_cells = 4;
    cfg.aux = cfg.dt_aux = 14;
    cfg.dt_sram = true;
    cfg.sibling = false;
}

static void build(void)
{
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    add_opp_table("opp-afr", PH_AFR_OPP, cfg.dt_aux, 1, false);
    add_opp_table("opp-cs", PH_CS_OPP, cfg.dt_aux, 1, false);
    add_opp_table("opp-gpu", PH_GPU_OPP, cfg.dt_states, cfg.dt_cells, cfg.dt_sram);
    add("/", "reserved-memory");
    for (int i = 5; i >= 0; i--) {
        int n = add("/reserved-memory", resv_paths[i] + strlen("/reserved-memory/"));
        fdt64_t zero[2] = {0, 0};
        assert(!fdt_setprop(dt, n, "reg", zero, sizeof(zero)));
        assert(!fdt_setprop_string(dt, n, "status", "disabled"));
    }
    add("/", "pm@0");
    add_provider("/pm@0", "other", PH_OTHER, "#power-domain-cells", 0);
    add_provider("/pm@0", "gfx", PH_GFX, "#power-domain-cells", 0);
    add_provider("/pm@0", "gpx", PH_GPX, "#power-domain-cells", 0);
    add_provider("/pm@0", "afr", PH_AFR, "#power-domain-cells", 0);
    set_cells("/pm@0/gfx", "power-domains", (const u32[]){PH_AFR, PH_GPX}, 2);
    add("/", "soc");
    /* Added after soc, so it comes first: a path lookup that ignores unit addresses takes it */
    if (cfg.sibling)
        add("/", "soc@1");
    add_provider("/soc", "mbox-b", PH_MBOX_B, "#mbox-cells", 0);
    add_provider("/soc", "mbox-a", PH_MBOX_A, "#mbox-cells", 0);
    set_cells("/soc/mbox-a", "power-domains", (const u32[]){PH_GFX}, 1);
    set_cells("/soc/mbox-b", "power-domains", (const u32[]){PH_GFX}, 1);
    int gpu = add("/soc", "graphics@2600000");
    static const char compat[] = "apple,agx-g17c\0apple,agx-g17s";
    assert(!fdt_setprop(dt, gpu, "compatible", compat, sizeof(compat)));
    assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", PH_GPU_OPP));
    assert(!fdt_setprop_u32(dt, gpu, "apple,afr-opp", PH_AFR_OPP));
    assert(!fdt_setprop_u32(dt, gpu, "apple,cs-opp", PH_CS_OPP));
    assert(!fdt_setprop_u32(dt, gpu, "apple,csafr-min-sram-microvolt", 0));
    set_cells(GPU_PATH, "apple,aon-ptd", (const u32[]){0, 0, 0, 0, 0, 0, 0, 0, 0}, 9);
    set_cells(GPU_PATH, "power-domains", (const u32[]){PH_GFX}, 1);
    set_cells(GPU_PATH, "mboxes", (const u32[]){PH_MBOX_A, PH_MBOX_B}, 2);
    assert(!fdt_setprop_string(dt, node(GPU_PATH), "status", "disabled"));
    if (cfg.shape == ALIAS)
        assert(!fdt_setprop_string(dt, add("/", "aliases"), "gpu", GPU_PATH));

    for (int t = 0; t < cfg.tables; t++)
        for (int i = 0; i < cfg.states; i++) {
            states[t * cfg.states + i] = (struct perf_state){state_mhz(i) * 1000000u,
                                                             core_mv(t, i)};
            sram[t * cfg.states + i] = (struct perf_state){state_mhz(i) * 1000000u,
                                                           sram_mv(t, i)};
        }
    for (int t = 0; t < 2; t++) {
        struct aux *a = t ? &afr : &cs;
        memset(a, 0, sizeof(*a));
        DIES(*a) = 1;
        a->words[1] = cfg.aux;
        for (int i = 0; i < cfg.aux; i++) {
            STATE_UV(*a, i) = aux_uv(t, i);
            STATE_HZ(*a, i) = aux_hz(t, i);
        }
        FLOOR_UV(*a, cfg.aux) = AUX_SRAM_UV;
    }
    aux_len[0] = aux_len[1] = 16 + 16 * cfg.aux + 8;
    memcpy(region, test_regions, sizeof(region));
    for (int r = 0; r < 2; r++) {
        segments[r][0] = (struct adt_segment_ranges){test_text[r][0], 0, test_text[r][0],
                                                     test_text[r][1], 1};
        for (int s = 1; s < 3; s++)
            segments[r][s] = (struct adt_segment_ranges){region[4 + 2 * r + s - 1][0], 0,
                                                         region[4 + 2 * r + s - 1][0],
                                                         region[4 + 2 * r + s - 1][1], 0};
    }
    raw_count = cfg.states; raw_tables = cfg.tables; die_count = 1;
    has_die_count = true; missing_sram = leak_fault = leak_window_missing = heap_full = false;
    /* The three records the fill takes, between others (FourCC little-endian, then two words) */
    static const u32 records[6][3] = {
        {0x7a7a3031, 0x110001, 0x2001}, {0x67305452, 0x120001, 0x2002},
        {0x67315452, 0x130001, 0x2003}, {0x67325452, 0x140001, 0x2004},
        {0x72305452, 0x150001, 0x2005}, {0x72315452, 0x160001, 0x2006},
    };
    memcpy(aon, records, sizeof(aon));
    aon_count = 6;
}

static void world(enum shape shape) { config(shape); build(); }

static void world_with(enum shape shape, int gpu_cells, bool gpu_sram)
{
    config(shape);
    cfg.dt_cells = gpu_cells;
    cfg.dt_sram = gpu_sram;
    build();
}

static const char *status(const char *path)
{
    const char *s = fdt_getprop(dt, node(path), "status", NULL);
    return s ? s : "okay";
}

static void expect_accepted(const char *what)
{
    assert(dt_set_gpu_g17c(dt) == 0);
    for (int i = 0; i < 5; i++)
        if (strcmp(status(dep_paths[i]), "okay")) {
            fprintf(stderr, "%s: %s not enabled\n", what, dep_paths[i]);
            exit(1);
        }
    assert(!strcmp(status(dep_paths[5]), "disabled"));
    if (strcmp(status(GPU_PATH), "okay")) {
        fprintf(stderr, "%s: GPU not enabled\n", what);
        exit(1);
    }
    const u64 want[6][2] = {
        {test_regions[0][0], test_regions[0][1]}, {test_regions[1][0], test_regions[1][1]},
        {test_regions[2][0], test_regions[2][1]}, {test_regions[3][0], test_regions[3][1]},
        {test_text[0][0], test_text[1][0] + test_text[1][1] - test_text[0][0]},
        {test_regions[4][0], test_regions[7][0] + test_regions[7][1] - test_regions[4][0]},
    };
    for (int i = 0; i < 6; i++) {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node(resv_paths[i]), "reg", &len);
        assert(!strcmp(status(resv_paths[i]), "okay"));
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == want[i][0] &&
               fdt64_ld(&reg[1]) == want[i][1]);
    }
    int opp, n = 0, len;
    fdt_for_each_subnode(opp, dt, node("/opp-gpu"))
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt32_t *sv = fdt_getprop(dt, opp, "opp-microvolt-sram", NULL);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == cfg.tables * 4 && fdt64_ld(hz) == state_mhz(n) * 1000000ULL);
        for (int c = 0; c < cfg.tables; c++)
            assert(fdt32_ld(&uv[c]) == core_mv(c, n) * 1000 &&
                   fdt32_ld(&sv[c]) == sram_mv(c, n) * 1000);
        n++;
    }
    assert(n == cfg.states);
    for (int t = 0; t < 2; t++) {
        n = 0;
        fdt_for_each_subnode(opp, dt, node(t ? "/opp-afr" : "/opp-cs"))
        {
            const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
            const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
            assert(uv && len == 4 && fdt32_ld(uv) == aux_uv(t, n) &&
                   fdt64_ld(hz) == aux_hz(t, n));
            n++;
        }
        assert(n == cfg.aux);
    }
    int gpu = node(GPU_PATH);
    const fdt32_t *cells = fdt_getprop(dt, gpu, "apple,aon-ptd", &len);
    static const u32 aon_want[9] = {0x67305452, 0x120001, 0x2002, 0x67315452, 0x130001,
                                    0x2003,     0x72305452, 0x150001, 0x2005};
    assert(cells && len == 36);
    for (int i = 0; i < 9; i++)
        assert(fdt32_ld(&cells[i]) == aon_want[i]);
    cells = fdt_getprop(dt, gpu, "apple,csafr-min-sram-microvolt", &len);
    assert(cells && len == 4 && fdt32_ld(cells) == AUX_SRAM_UV);
    cells = fdt_getprop(dt, gpu, "apple,leakage-calibration-words", &len);
    if (leak_fault || leak_window_missing) {
        assert(!cells);
    } else {
        assert(cells && len == 12 && fdt32_ld(&cells[0]) == 0x5ea10000 &&
               fdt32_ld(&cells[2]) == 0x5ea10008);
    }
    cells = fdt_getprop(dt, gpu, "apple,firmware-version", &len);
    assert(cells && len == 12 && fdt32_ld(&cells[0]) == 27);
    printf("PASS accepted: %s\n", what);
}

/* The deps and reservations stay disabled and nothing was published; the GPU is @gpu_status */
static void expect_untouched(const char *what)
{
    for (int i = 0; i < 6; i++)
        if (strcmp(status(dep_paths[i]), "disabled")) {
            fprintf(stderr, "%s: %s enabled\n", what, dep_paths[i]);
            exit(1);
        }
    for (int i = 0; i < 6; i++) {
        int n = fdt_path_offset(dt, resv_paths[i]);
        if (n >= 0 && strcmp(status(resv_paths[i]), "disabled")) {
            fprintf(stderr, "%s: %s left published\n", what, resv_paths[i]);
            exit(1);
        }
    }
}

static void expect_refused(const char *what)
{
    assert(dt_set_gpu_g17c(dt) == 0);
    if (strcmp(status(GPU_PATH), "disabled")) {
        fprintf(stderr, "%s: GPU was not disabled\n", what);
        exit(1);
    }
    expect_untouched(what);
    int gpu = node(GPU_PATH);
    assert(!fdt_getprop(dt, gpu, "apple,firmware-version", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,leakage-calibration-words", NULL));
    const fdt32_t *floor = fdt_getprop(dt, gpu, "apple,csafr-min-sram-microvolt", NULL);
    assert(!floor || !fdt32_ld(floor));
    printf("PASS refused: %s\n", what);
}

/* The GPU node starts enabled: "okay", or no status at all */
static void start_enabled(bool absent)
{
    if (absent)
        assert(!fdt_delprop(dt, node(GPU_PATH), "status"));
    else
        assert(!fdt_setprop_string(dt, node(GPU_PATH), "status", "okay"));
}

/* The tree packed, then given @room bytes */
static void room(int bytes)
{
    assert(!fdt_pack(dt));
    if (bytes)
        assert(!fdt_open_into(dt, dt, fdt_totalsize(dt) + bytes));
}

int main(void)
{
    /* Unbuffered, so a failing check still shows the cases that passed before it */
    setvbuf(stdout, NULL, _IONBF, 0);
    static unsigned char before[sizeof(blob)];

    /* Both tree shapes, found by the alias and by the compatible */
    world(ALIAS);
    assert(t6050_g17c_node(dt) == node(GPU_PATH));
    expect_accepted("gpu alias");
    world(COMPATIBLE);
    assert(t6050_g17c_node(dt) == node(GPU_PATH));
    expect_accepted("no alias, found by compatible");
    /* An alias to /soc/... with a soc@1 sibling first: the plain path lookup misses the node */
    config(ALIAS); cfg.sibling = true; build();
    assert(fdt_path_offset(dt, GPU_PATH) != node(GPU_PATH));
    assert(t6050_g17c_node(dt) == node(GPU_PATH));
    expect_accepted("gpu alias with a soc@1 sibling ahead of soc");

    /* Which node, if any, the G17C path takes */
    world(COMPATIBLE);
    static const char g17s[] = "apple,agx-g17s";
    assert(!fdt_setprop_string(dt, add("/", "aliases"), "gpu", "/pm@0/other"));
    assert(!fdt_setprop(dt, node("/pm@0/other"), "compatible", g17s, sizeof(g17s)));
    assert(t6050_g17c_node(dt) < 0);
    world(COMPATIBLE);
    assert(!fdt_setprop_string(dt, node("/pm@0/other"), "compatible", "apple,agx-g17c"));
    assert(t6050_g17c_node(dt) < 0);
    world(ALIAS);
    assert(!fdt_setprop_string(dt, node("/pm@0/other"), "compatible", "apple,agx-g17c"));
    assert(t6050_g17c_node(dt) == node(GPU_PATH));
    world(COMPATIBLE);
    assert(!fdt_setprop_string(dt, node(GPU_PATH), "compatible", "apple,agx-g17s"));
    assert(t6050_g17c_node(dt) < 0);
    puts("PASS node: alias first, else the one apple,agx-g17c node, never a G17S");

    /* The ADT sizes the tables: other counts pass when the tree has the same shape */
    config(COMPATIBLE); cfg.states = cfg.dt_states = 13; build();
    expect_accepted("13 states, 13-entry OPP table");
    config(ALIAS); cfg.tables = cfg.dt_cells = 3; build();
    expect_accepted("three clusters, three voltages per OPP");
    config(COMPATIBLE); cfg.aux = cfg.dt_aux = 12; build();
    expect_accepted("12 CS and AFR states, 12-entry tables");
    config(COMPATIBLE); cfg.states = cfg.dt_states = 16; cfg.tables = cfg.dt_cells = 8;
    cfg.aux = cfg.dt_aux = 16; build();
    expect_accepted("16 states of eight clusters, 16 CS and AFR states");

    /* Provider argument cells are skipped, and a domain named twice is enabled once */
    world(COMPATIBLE);
    assert(!fdt_setprop_u32(dt, node("/soc/mbox-a"), "#mbox-cells", 1));
    set_cells(GPU_PATH, "mboxes", (const u32[]){PH_MBOX_A, 7, PH_MBOX_B}, 3);
    set_cells(GPU_PATH, "power-domains", (const u32[]){PH_GFX, PH_GPX}, 2);
    expect_accepted("mailbox argument cell, power domain named twice");
    world(COMPATIBLE); has_die_count = false;
    expect_accepted("ADT without die-count");

    world(ALIAS); die_count = 2;
    expect_refused("two dies");
    config(COMPATIBLE); cfg.tables = cfg.dt_cells = 9; build();
    expect_refused("nine cluster tables");
    config(COMPATIBLE); cfg.states = 13; build();
    expect_refused("13-state ADT, 14-entry OPP table");
    config(COMPATIBLE); cfg.tables = 3; build();
    expect_refused("three cluster tables, four voltages per OPP");
    config(COMPATIBLE); cfg.aux = 13; build();
    expect_refused("13 CS and AFR states, 14-entry tables");
    world(COMPATIBLE); raw_count = 1;
    expect_refused("one perf state");
    world(COMPATIBLE); states[2 * 14 + 5].freq += 1000000;
    expect_refused("a cluster runs a different clock");
    world(COMPATIBLE); states[3 * 14 + 9].volt = 1600;
    expect_refused("active core voltage above the range");
    world(COMPATIBLE); states[1 * 14 + 1].volt = 300;
    expect_refused("active core voltage below the floor");
    world(COMPATIBLE); states[2 * 14].volt = 1600;
    expect_refused("off-state core voltage above the range");
    world(COMPATIBLE); missing_sram = true;
    expect_refused("no perf-states-sram");
    world(COMPATIBLE); sram[3].freq = 1;
    expect_refused("SRAM table at other clocks");
    world(COMPATIBLE); sram[2 * 14 + 12].volt = 1600;
    expect_refused("active SRAM voltage above the range");
    world(COMPATIBLE); sram[3 * 14 + 4].volt = 300;
    expect_refused("active SRAM voltage below the floor");
    world(COMPATIBLE); DIES(cs) = 2;
    expect_refused("two-die CS table");
    world(COMPATIBLE); STATE_UV(afr, 4) = 300000;
    expect_refused("AFR voltage below the floor");
    world(COMPATIBLE); STATE_UV(cs, 6) = 1600000;
    expect_refused("CS voltage above the range");
    world(COMPATIBLE); STATE_UV(afr, 0) = 1600000;
    expect_refused("AFR off state above the range");
    world(COMPATIBLE); FLOOR_UV(afr, 14) = 780000;
    expect_refused("CS and AFR SRAM floors disagree");
    world(COMPATIBLE); aux_len[1] -= 8;
    expect_refused("only CS has an SRAM floor");
    world(COMPATIBLE); aux_len[0] -= 8; aux_len[1] -= 8;
    expect_refused("neither CS nor AFR has an SRAM floor");
    world(COMPATIBLE); aon[2][0] = 0;
    expect_refused("AON record g1TR missing");
    world(COMPATIBLE); aon[5][0] = 0x67305452;
    expect_refused("AON record g0TR twice");
    world(COMPATIBLE);
    set_cells(GPU_PATH, "apple,aon-ptd", (const u32[]){0, 0, 0, 0, 0, 0}, 6);
    expect_refused("AON property for two records");
    world_with(ALIAS, 1, false);
    expect_refused("one-voltage OPP table");
    world_with(COMPATIBLE, 4, false);
    expect_refused("OPP table without SRAM voltages");
    world(COMPATIBLE);
    assert(!fdt_delprop(dt, node(GPU_PATH), "apple,cs-opp"));
    expect_refused("no CS table in the device tree");
    /* A table named twice, of the right shape for both names: refused, nothing written */
    world(COMPATIBLE);
    assert(!fdt_setprop_u32(dt, node(GPU_PATH), "apple,afr-opp", PH_CS_OPP));
    memcpy(before, blob, sizeof(blob));
    expect_refused("AFR names the CS table");
    assert(!memcmp(before, blob, sizeof(blob)));
    config(COMPATIBLE); cfg.tables = cfg.dt_cells = 1; build();
    assert(!fdt_setprop_u32(dt, node(GPU_PATH), "apple,cs-opp", PH_GPU_OPP));
    memcpy(before, blob, sizeof(blob));
    expect_refused("one cluster, CS names the GPU OPP table");
    assert(!memcmp(before, blob, sizeof(blob)));
    world(COMPATIBLE); assert(!fdt_del_node(dt, node("/reserved-memory/gpu-fw-data")));
    expect_refused("gpu-fw-data reservation missing");
    world(COMPATIBLE); region[3][1] = 0x4100;
    expect_refused("unaligned UAT region");
    world(COMPATIBLE); region[2][0] = region[1][0];
    expect_refused("overlapping UAT regions");
    world(COMPATIBLE); segments[1][0].phys += 0x4000;
    expect_refused("GFX1 text apart from GFX's");
    world(COMPATIBLE); segments[0][1].phys += 0x4000;
    expect_refused("GFX data disagrees with gfx-data");
    world(COMPATIBLE); assert(!fdt_delprop(dt, node(GPU_PATH), "mboxes"));
    expect_refused("no mailboxes");
    world(COMPATIBLE); assert(!fdt_delprop(dt, node(GPU_PATH), "power-domains"));
    expect_refused("no power domains");
    world(COMPATIBLE); set_cells(GPU_PATH, "mboxes", (const u32[]){PH_MBOX_A, 99}, 2);
    expect_refused("mailbox phandle without a node");
    world(COMPATIBLE);
    assert(!fdt_delprop(dt, node("/pm@0/gpx"), "#power-domain-cells"));
    expect_refused("power domain without #power-domain-cells");
    world(COMPATIBLE);
    assert(!fdt_setprop_u32(dt, node("/soc/mbox-b"), "#mbox-cells", 1));
    expect_refused("mailbox arguments past the end of mboxes");
    world(COMPATIBLE);
    set_cells("/pm@0/afr", "power-domains", (const u32[]){PH_GFX}, 1);
    expect_refused("power domain cycle");
    world(ALIAS); raw_tables = 1;
    start_enabled(false);
    expect_refused("GPU enabled in the device tree, then refused");
    world(COMPATIBLE); raw_tables = 1;
    start_enabled(true);
    expect_refused("GPU without a status in the device tree, then refused");

    /* No heap room for the copy of the tree: nothing is written. */
    world(COMPATIBLE); heap_full = true;
    memcpy(before, blob, sizeof(blob));
    expect_refused("no heap room for the copy of the tree");
    assert(!memcmp(before, blob, sizeof(blob)));

    /* A disabled GPU in a tree with no room for the properties the fill adds: the fill fails on
     * its copy, the boot goes on with the GPU disabled and the tree is exactly as it was. */
    for (int shape = ALIAS; shape <= COMPATIBLE; shape++) {
        world(shape);
        room(0);
        memcpy(before, blob, sizeof(blob));
        expect_refused(shape == ALIAS ? "no room, aliased" : "no room, by compatible");
        assert(!memcmp(before, blob, sizeof(blob)));
    }
    /* An enabled GPU ("okay", or no status) in a tree with no room: not even the disable write
     * fits, so the node is removed and the boot goes on; nothing else changes. With room for
     * the disable write only, the GPU is left disabled and nothing else is written. */
    for (int shape = ALIAS; shape <= COMPATIBLE; shape++)
        for (int absent = 0; absent < 2; absent++) {
            world(shape);
            start_enabled(absent);
            room(0);
            assert(dt_set_gpu_g17c(dt) == 0);
            assert(kboot_fdt_path_exact(dt, GPU_PATH) == -FDT_ERR_NOTFOUND);
            assert(t6050_g17c_node(dt) < 0);
            expect_untouched("enabled, no room");
            printf("PASS removed: enabled (%s), no room, %s\n", absent ? "no status" : "okay",
                   shape == ALIAS ? "aliased" : "by compatible");
            world(shape);
            start_enabled(absent);
            room(32);
            expect_refused(absent ? "no status, room for the disable write only"
                                  : "okay, room for the disable write only");
        }

    /*
     * The writes are one transaction: give the tree exactly the room the fill needs, and one byte
     * less. The leakage words are the fill's last write that needs room (enabling only shrinks
     * status values), so one byte less fails that write, which unwinds the whole fill.
     */
    world(COMPATIBLE);
    room(0);
    int packed = fdt_totalsize(dt);
    assert(!fdt_open_into(dt, dt, sizeof(blob)));
    expect_accepted("room to spare");
    assert(!fdt_pack(dt));
    /* Five dependencies and the GPU went from "disabled" to "okay": 4 bytes each */
    int need = fdt_totalsize(dt) + 6 * 4 - packed;
    world(COMPATIBLE);
    room(need);
    expect_accepted("exactly the room the fill needs");
    world(COMPATIBLE);
    room(need - 1);
    memcpy(before, blob, sizeof(blob));
    expect_refused("one byte short: the leakage words do not fit");
    assert(!memcmp(before, blob, sizeof(blob)));

    /* Leakage words that cannot be read, or a part without the window, do not stop the hand-off */
    world(COMPATIBLE); leak_fault = true;
    expect_accepted("leakage words unreadable");
    world(ALIAS); leak_window_missing = true;
    expect_accepted("no leakage window in the ADT");
    /* Words already in the tree were not read from this part: without the part's, none remain */
    world(COMPATIBLE); leak_window_missing = true;
    set_cells(GPU_PATH, "apple,leakage-calibration-words",
              (const u32[]){0x11110000, 0x22220000, 0x33330000}, 3);
    expect_accepted("no leakage window, stale words in the tree");
    world(ALIAS); leak_fault = true;
    set_cells(GPU_PATH, "apple,leakage-calibration-words",
              (const u32[]){0x11110000, 0x22220000, 0x33330000}, 3);
    expect_accepted("leakage words unreadable, stale words in the tree");
    return 0;
}
'''


def test_t6050_g17c_fill(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    kboot = (ROOT / "src/kboot.c").read_text()
    leak = source[source.index("#define T6050_GPU_LEAK_WINDOW"):]
    leak = leak[:leak.index("\n\n")] + "\n\n"
    # The CS/AFR parser and SRAM floor the G15C path shares with this one
    aux = source[source.index("#define G15_AUX_MAX "):]
    aux = aux[:aux.index("static int g15_set_aux_opps(")]
    max_states = source[source.index("#define G17_MAX_PERF_STATES "):]
    max_states = max_states[:max_states.index("\n") + 1]
    g17c = source[source.index("#define T6050_G17C_CLUSTERS_MAX "):]
    # The G17C fill ends where the generic G13/G14 path's helpers begin.
    g17c = g17c[:min(g17c.index(marker) for marker in (
        "\n/* The firmware version whose initdata layout", "\nint dt_set_gpu(void *dt)\n{")
        if marker in g17c)]
    code = (HARNESS + function(kboot, "int kboot_fdt_path_exact(") + ADT + leak + aux +
            max_states + function(source, "static int dt_set_resvmem(") +
            function(source, "static T6050_GPU_TEXT_FN int t6050_gpu_leakage(") + g17c + MAIN)
    cfile = tmp_path / "t6050-g17c-fill.c"
    cfile.write_text(code)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "t6050-g17c-fill"
    built = subprocess.run([*host_cc(), "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra",
                            "-Werror", "-Wno-unused-function", "-Wno-unused-parameter", "-I",
                            str(libfdt), str(cfile), *sources, "-o", str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 70, result.stdout
