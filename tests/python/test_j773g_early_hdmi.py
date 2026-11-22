# SPDX-License-Identifier: MIT
"""Host unit tests for J773g (M4 Mac mini, T8132) early HDMI display bring-up.

Compiles production code verbatim from src/pmgr.c, src/dart.c, src/dcp.c,
src/dcp/dptx_phy.c, src/afk.c, src/dcp/dptx_port_ep.c, and src/display.c
against deterministic host stand-ins.
"""
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]


def extract_fn(text, signature):
    m = re.search(re.escape(signature) + r"[^;{]*\{", text)
    assert m, signature
    start, end, depth = m.start(), m.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def strip_includes(text):
    return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))


def compile_and_run(tmp_path, name, source, extra_cflags=()):
    src = tmp_path / f"{name}.c"
    exe = tmp_path / name
    src.write_text(source)
    res = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
         *extra_cflags, str(src), "-o", str(exe)],
        capture_output=True, text=True,
    )
    assert res.returncode == 0, res.stdout + res.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr
    return run.stdout


def test_t8132_pmgr_protects_display_cpus(tmp_path):
    """Compile src/pmgr.h + src/pmgr.c verbatim (no local struct/bit copies)."""
    pmgr_h = strip_includes((REPO / "src/pmgr.h").read_text())
    pmgr_c = strip_includes((REPO / "src/pmgr.c").read_text())
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define FIELD_GET(f, v) (((v) & (f)) / ((f) & -(f)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PACKED __attribute__((packed))
#define T8132 0x8132
#define T8140 0x8140
#define ADT_GETPROP(a, n, p, v) (-1)
static int chip_id = T8132;
static void *adt = (void *)1;
static const void *devices_prop;
static u32 devices_len;
static const u32 ps_regs[] = {0, 0, 0};
static int adt_path_offset(const void *a, const char *path) { (void)a; (void)path; return 1; }
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    (void)a; (void)path;
    trace[0] = 1; trace[1] = 0;
    return 1;
}
static bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a; (void)node; (void)compat;
    return false;
}
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a; (void)node;
    if (!strcmp(name, "ps-regs")) { *len = sizeof(ps_regs); return ps_regs; }
    if (!strcmp(name, "devices")) { *len = devices_len; return devices_prop; }
    *len = 0;
    return NULL;
}
#define PS_BASE 0x2d0000000UL
static int adt_get_reg(const void *a, const int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)path; (void)prop; (void)idx; (void)size;
    *addr = PS_BASE;
    return 0;
}
static u32 regs[4];
static unsigned writes[4];
static int reg_idx(uintptr_t addr)
{
    assert(addr >= PS_BASE && (addr - PS_BASE) / 8 < ARRAY_SIZE(regs));
    return (int)((addr - PS_BASE) / 8);
}
static u32 read32(uintptr_t addr) { return regs[reg_idx(addr)]; }
static void write32(uintptr_t addr, u32 v) { writes[reg_idx(addr)]++; regs[reg_idx(addr)] = v; }
static u32 mask32(uintptr_t addr, u32 clr, u32 set)
{
    int i = reg_idx(addr);
    writes[i]++;
    regs[i] = (regs[i] & ~clr) | set;
    regs[i] = (regs[i] & ~0xf0u) | ((regs[i] & 0xfu) << 4);
    return regs[i];
}
static u32 set32(uintptr_t addr, u32 v) { int i = reg_idx(addr); writes[i]++; return regs[i] |= v; }
static u32 clear32(uintptr_t addr, u32 v) { int i = reg_idx(addr); writes[i]++; return regs[i] &= ~v; }
static int poll32(uintptr_t addr, u32 m, u32 v, u32 t) { (void)addr; (void)m; (void)v; (void)t; return 0; }
static void udelay(u32 us) { (void)us; }
''' + pmgr_h + "\n" + pmgr_c + r'''
static struct pmgr_device table[3];
int main(void)
{
    memset(table, 0, sizeof(table));
    table[0].id2 = 1; table[0].addr_offset = 0; strcpy((char *)table[0].name, "DISP_CPU");
    table[1].id2 = 2; table[1].addr_offset = 1; strcpy((char *)table[1].name, "DISPEXT0_CPU");
    table[2].id2 = 3; table[2].addr_offset = 2; strcpy((char *)table[2].name, "DPTX_PHY");
    devices_prop = table;
    devices_len = sizeof(table);

    /* Boot register states observed on live J773g 27.2: AUTO_ENABLE + TARGET=15 + ACTUAL=0. */
    regs[0] = 0x1000030f;
    regs[1] = 0x1000030f;
    regs[2] = 0x0;
    assert(pmgr_init() == 0);
    assert(writes[0] == 0 && writes[1] == 0 && writes[2] == 0);

    assert(pmgr_protected_device(&table[0]));
    assert(pmgr_protected_device(&table[1]));
    assert(!pmgr_protected_device(&table[2]));
    assert(pmgr_device_is_protected("DISP_CPU"));
    assert(pmgr_device_is_protected("DISPEXT0_CPU"));
    assert(!pmgr_device_is_protected("DPTX_PHY"));

    /* Auto-gated active protected domain (0x1000030f): ACTIVE succeeds read-only, power-off & reset fail. */
    assert(pmgr_power_is_requested(0, "DISPEXT0_CPU") == 1);
    assert(pmgr_set_mode(PS_BASE + 8, PMGR_PS_ACTIVE) == 0 && writes[1] == 0);
    assert(pmgr_set_mode(PS_BASE + 8, PMGR_PS_PWRGATE) == -1 && writes[1] == 0);
    assert(pmgr_reset_device(0, &table[1]) == -1 && writes[1] == 0);

    /* Fully active protected domain (ACTUAL=15): ACTIVE succeeds read-only. */
    regs[1] = 0x000000ff;
    assert(pmgr_power_is_requested(0, "DISPEXT0_CPU") == 1);
    assert(pmgr_set_mode(PS_BASE + 8, PMGR_PS_ACTIVE) == 0 && writes[1] == 0);

    /* Off (0x0) or non-auto-gated idle (0x0000000f) protected domain: refuses ACTIVE with zero writes. */
    regs[1] = 0x00000000;
    assert(pmgr_power_is_requested(0, "DISPEXT0_CPU") == 0);
    assert(pmgr_set_mode(PS_BASE + 8, PMGR_PS_ACTIVE) == -1 && writes[1] == 0);
    regs[1] = 0x0000000f;
    assert(pmgr_power_is_requested(0, "DISPEXT0_CPU") == 0);
    assert(pmgr_set_mode(PS_BASE + 8, PMGR_PS_ACTIVE) == -1 && writes[1] == 0);

    /* Non-protected DPTX_PHY powers on normally. */
    assert(pmgr_set_mode(PS_BASE + 16, PMGR_PS_ACTIVE) == 0 && writes[2] == 1);
    assert(FIELD_GET(PMGR_PS_ACTUAL, regs[2]) == PMGR_PS_ACTIVE);
    puts("PASS t8132 pmgr protection");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "pmgr-t8132", harness)
    assert "PASS t8132 pmgr protection" in out


def test_dart_dva_base_scope_and_locked_l2_release(tmp_path):
    """Verify dva_base stays 0 unless explicitly opted in on locked T8110, and
    dart_shutdown_checked only releases empty heap L2 tables when dva_base != 0."""
    dart = (REPO / "src/dart.c").read_text()
    defs = dart[dart.index("#define DART_T8020_CONFIG "):dart.index("/* The two-level Gen3")]
    sig = "static void dart_gen3_tlb_invalidate(dart_dev_t *dart)"
    s = defs.index(sig)
    e = defs.index("\n}\n", s) + 3
    defs = defs[:s] + sig + "\n{ (void)dart; }\n" + defs[e:]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef unsigned long u64; typedef long s64;
typedef struct dart_dev dart_dev_t;
enum dart_type_t { DART_T8020, DART_T8110, DART_T6000, DART_GEN3 };
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzll(m))
#define SZ_16K 0x4000
#define T8152 0x8152
#define dma_wmb() ((void)0)
static int chip_id = 0x8132;
static void *adt = (void *)1;
static int tlbi_fail = 0, tlbi_calls = 0;
static void write32(u64 addr, u32 v) { (void)addr; (void)v; }
static int poll32(u64 addr, u32 m, u32 v, u32 t)
{
    (void)addr; (void)m; (void)v; (void)t;
    tlbi_calls++;
    return tlbi_fail ? -1 : 0;
}
static u64 iboot_pages[2][SZ_16K / 8] __attribute__((aligned(SZ_16K)));
static u64 heap_pages[4][SZ_16K / 8] __attribute__((aligned(SZ_16K)));
static void *freed[8];
static int nfreed;
static bool is_heap(const void *p)
{
    return (const u8 *)p >= (const u8 *)heap_pages &&
           (const u8 *)p < (const u8 *)heap_pages + sizeof(heap_pages);
}
static void mock_free(void *p) { freed[nfreed++] = p; }
#define free mock_free
''' + defs + r'''
static const char *mock_compat = "dart,t8110";
static u64 mock_vm_base = 0x10000000000UL;
static bool mock_locked = true;
static dart_dev_t mock_dev_storage;
static int adt_path_offset_trace(void *a, const char *p, int *trace)
{
    (void)a; (void)p; trace[0] = 1; trace[1] = 0; return 1;
}
static int adt_get_reg(void *a, int *trace, const char *p, int idx, u64 *base, u64 *sz)
{
    (void)a; (void)trace; (void)p; (void)idx; (void)sz;
    *base = 0x300000000UL;
    return 0;
}
static bool adt_is_compatible(void *a, int node, const char *c)
{
    (void)a; (void)node;
    return strcmp(mock_compat, c) == 0;
}
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a; (void)node;
    if (!strcmp(name, "vm-base")) {
        if (len) *len = sizeof(mock_vm_base);
        return &mock_vm_base;
    }
    return NULL;
}
static dart_dev_t *dart_gen3_adopt_adt(int node, uintptr_t base, int dev, bool keep_pts)
{
    (void)node; (void)base; (void)dev; (void)keep_pts;
    memset(&mock_dev_storage, 0, sizeof(mock_dev_storage));
    mock_dev_storage.type = DART_GEN3;
    mock_dev_storage.locked = true;
    return &mock_dev_storage;
}
static dart_dev_t *dart_init(uintptr_t base, u8 dev, bool keep_pts, enum dart_type_t type)
{
    (void)base; (void)dev; (void)keep_pts;
    memset(&mock_dev_storage, 0, sizeof(mock_dev_storage));
    mock_dev_storage.type = type;
    mock_dev_storage.locked = mock_locked;
    mock_dev_storage.params = (type == DART_T8110) ? &dart_t8110 : &dart_t6000;
    return &mock_dev_storage;
}
''' + "\n".join([
        extract_fn(dart, "dart_dev_t *dart_init_adt("),
        extract_fn(dart, "bool dart_shutdown_checked("),
        extract_fn(dart, "u64 dart_vm_base("),
        extract_fn(dart, "u64 dart_dva_base("),
        extract_fn(dart, "void dart_preserve_dva_base("),
    ]) + r'''
#undef free
static u64 pte_of(const void *p)
{
    return FIELD_PREP(dart_t8110.offset_mask, ((u64)p) >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
}
int main(void)
{
    /* Part A: dva_base scoping (zero blast radius on non-opted-in DARTs and non-T8110 DARTs). */
    mock_compat = "dart,t8110"; mock_locked = true; mock_vm_base = 0x10000000000UL;
    dart_dev_t *d = dart_init_adt("/arm-io/dart-dcpext0", 0, 23, true);
    assert(d && dart_vm_base(d) == 0 && dart_dva_base(d) == 0); /* default 0 (e.g. USB/M4 Pro 26.x) */
    dart_preserve_dva_base(d);
    assert(dart_dva_base(d) == 0x10000000000UL && dart_vm_base(d) == 0);

    /* Non-T8110 locked DART (M1/M1 Pro) never enables dva_base even if requested. */
    mock_compat = "dart,t6000"; mock_locked = true; mock_vm_base = 0x10000000000UL;
    d = dart_init_adt("/arm-io/dart-dcp", 0, 0, true);
    dart_preserve_dva_base(d);
    assert(d && dart_dva_base(d) == 0 && dart_vm_base(d) == 0);

    /* Low vm-base (M2) has dva_base == 0 even on locked T8110. */
    mock_compat = "dart,t8110"; mock_locked = true; mock_vm_base = 0x10000000UL;
    d = dart_init_adt("/arm-io/dart-dcp", 0, 0, true);
    dart_preserve_dva_base(d);
    assert(d && dart_dva_base(d) == 0 && dart_vm_base(d) == 0x10000000UL);

    /* Unlocked T8110 never enables dva_base. */
    mock_compat = "dart,t8110"; mock_locked = false; mock_vm_base = 0x10000000000UL;
    d = dart_init_adt("/arm-io/dart-usb0", 0, 0, false);
    dart_preserve_dva_base(d);
    assert(d && dart_dva_base(d) == 0);

    /* Gen3 DART never enables dva_base. */
    mock_compat = "dart,gen3";
    d = dart_init_adt("/arm-io/dart-dcp", 0, 0, true);
    dart_preserve_dva_base(d);
    assert(d && dart_dva_base(d) == 0);

    /* Part B Case 1: locked high-dva-base T8110 releases empty heap L2, keeps iBoot & active heap L2. */
    memset(iboot_pages, 0, sizeof(iboot_pages));
    memset(heap_pages, 0, sizeof(heap_pages));
    nfreed = tlbi_calls = tlbi_fail = 0;

    iboot_pages[0][0] = pte_of(iboot_pages[1]); /* iBoot L2, empty, must NOT be touched */
    iboot_pages[0][1] = pte_of(heap_pages[0]);  /* empty heap L2 -> unlink, TLBI, remove, free */
    iboot_pages[0][2] = pte_of(heap_pages[1]);  /* active heap L2 (FB) -> keep */
    heap_pages[1][0] = pte_of(iboot_pages[1]);

    legacy_l2_pages[0] = (u64)heap_pages[0];
    legacy_l2_pages[1] = (u64)heap_pages[1];
    legacy_l2_count = 2;

    d = (dart_dev_t *)heap_pages[2];
    *d = (dart_dev_t){
        .locked = true, .legacy_mutable = true, .type = DART_T8110,
        .dva_base = 0x10000000000UL, .params = &dart_t8110, .l1 = {iboot_pages[0]},
    };
    assert(dart_shutdown_checked(d));
    assert(tlbi_calls == 1);
    assert(iboot_pages[0][0] == pte_of(iboot_pages[1]));
    assert(iboot_pages[0][1] == 0);
    assert(iboot_pages[0][2] == pte_of(heap_pages[1]));
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)heap_pages[1]);
    assert(nfreed == 2 && freed[0] == heap_pages[0] && freed[1] == d);

    /* Part B Case 2: TLBI failure keeps page in legacy_l2_pages and frees nothing. */
    memset(iboot_pages, 0, sizeof(iboot_pages));
    memset(heap_pages, 0, sizeof(heap_pages));
    nfreed = tlbi_calls = 0;
    tlbi_fail = 1;
    iboot_pages[0][1] = pte_of(heap_pages[0]);
    legacy_l2_pages[0] = (u64)heap_pages[0];
    legacy_l2_count = 1;
    d = (dart_dev_t *)heap_pages[2];
    *d = (dart_dev_t){
        .locked = true, .legacy_mutable = true, .type = DART_T8110,
        .dva_base = 0x10000000000UL, .params = &dart_t8110, .l1 = {iboot_pages[0]},
    };
    assert(!dart_shutdown_checked(d));
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)heap_pages[0]);
    assert(nfreed == 0);

    /* Part B Case 3: M1/M2 locked legacy DART (dva_base == 0) is untouched on shutdown. */
    memset(iboot_pages, 0, sizeof(iboot_pages));
    memset(heap_pages, 0, sizeof(heap_pages));
    nfreed = tlbi_calls = tlbi_fail = 0;
    iboot_pages[0][1] = pte_of(heap_pages[0]);
    legacy_l2_pages[0] = (u64)heap_pages[0];
    legacy_l2_count = 1;
    d = (dart_dev_t *)heap_pages[2];
    *d = (dart_dev_t){
        .locked = true, .legacy_mutable = true, .type = DART_T8110,
        .dva_base = 0, .params = &dart_t8110, .l1 = {iboot_pages[0]},
    };
    assert(dart_shutdown_checked(d));
    assert(tlbi_calls == 0);
    assert(iboot_pages[0][1] == pte_of(heap_pages[0]));
    assert(legacy_l2_count == 1 && nfreed == 1 && freed[0] == d);

    puts("PASS dart dva_base and L2 release");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "dart-locked-l2", harness, extra_cflags=("-no-pie",))
    assert "PASS dart dva_base and L2 release" in out


def test_t8132_dptx_phy_sequence_and_math(tmp_path):
    """Independently verify 24 MHz PLL frequency math, symbol clock divider,
    absent calibration handling, and error paths from written MMIO registers."""
    phy = (REPO / "src/dcp/dptx_phy.c").read_text()
    phy_body = phy[phy.index("enum dptx_type {"):phy.index("/* Read-modify-write of a field in the core")]
    harness = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef unsigned long u64;
#define BIT(n) (1ULL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DPTX_MAX_LANES 4
#define DPTX_LANE0_OFFSET 0x5000
#define DPTX_LANE_STRIDE 0x1000
#define dprintf(...) ((void)0)
typedef struct atc_phy atc_phy_t;
static u32 dptx_regs[0x10000 / 4];
static u32 core_regs[0x4000 / 4];
static bool sim_impcal_fail, sim_pll_no_ack, sim_pll_no_lock;
#define DPTX_BASE 0x100000UL
#define CORE_BASE 0x200000UL
static u32 *reg_ptr(u64 addr)
{
    if (addr >= DPTX_BASE && addr < DPTX_BASE + sizeof(dptx_regs))
        return &dptx_regs[(addr - DPTX_BASE) / 4];
    if (addr >= CORE_BASE && addr < CORE_BASE + sizeof(core_regs))
        return &core_regs[(addr - CORE_BASE) / 4];
    assert(!"MMIO out of bounds");
    return NULL;
}
static u32 read32(u64 addr) { return *reg_ptr(addr); }
static void write32(u64 addr, u32 val)
{
    *reg_ptr(addr) = val;
    if (addr == DPTX_BASE + 0x4200 && (val & 1)) {
        dptx_regs[0x4204 / 4] = sim_impcal_fail ? 0 : 1;
        dptx_regs[0x4210 / 4] = 0x35U << 18;
    }
    if (addr == DPTX_BASE + 0x100c && (val & 8)) {
        u32 cmd = val & 7;
        dptx_regs[0x1014 / 4] = sim_pll_no_ack ? 0 : 1;
        dptx_regs[0x1008 / 4] = (cmd == 7 && !sim_pll_no_lock) ? (1U << 3) : 0;
    }
}
static void mask32(u64 addr, u32 clear, u32 set) { write32(addr, (read32(addr) & ~clear) | set); }
static void set32(u64 addr, u32 bits) { write32(addr, read32(addr) | bits); }
static void clear32(u64 addr, u32 bits) { write32(addr, read32(addr) & ~bits); }
static void udelay(u32 us) { (void)us; }
static void mdelay(u32 ms) { (void)ms; }
''' + phy_body + r'''
static void check_rate_math(dptx_phy_t *phy, u32 rate_mhz, double expected_vco_mhz, bool expect_half)
{
    assert(dptx_t8132_reconfigure(phy, false, 0, true, rate_mhz) == 0);
    assert(phy->rate == rate_mhz);

    /* Independently reconstruct VCO and symbol clock from written hardware registers. */
    u32 fb_quad = dptx_regs[0x20b0 / 4] & 0x3ff;
    u32 frac_num = (dptx_regs[0x20b4 / 4] >> 14) & 0x3fff;
    u32 frac_den = dptx_regs[0x20b4 / 4] & 0x3fff;
    u32 fb_int = fb_quad / 4;
    double frac = frac_den ? ((double)frac_num / (double)frac_den) : 0.0;
    double vco_mhz = 24.0 * ((double)fb_int + frac);
    assert(fabs(vco_mhz - expected_vco_mhz) < 0.01);
    assert((fb_quad % 4) == (u32)(4.0 * frac + 1e-9));

    /* Symbol clock = VCO / (pclk_div + 1) must equal link_rate / 10 (8b/10b). */
    u32 pclk_div = (dptx_regs[0x20b8 / 4] >> 9) & 0x1f;
    assert(fabs((vco_mhz / (double)(pclk_div + 1)) - ((double)rate_mhz / 10.0)) < 0.01);

    /* Half-rate mode bits in PHY + PLL. */
    assert((dptx_regs[0x4004 / 4] & 0x60) == (expect_half ? 0x40U : 0x20U));
    assert(dptx_regs[0x1008 / 4] & BIT(3));
}

int main(void)
{
    dptx_phy_t phy = {
        .regs = {DPTX_BASE, CORE_BASE},
        .type = DPTX_PHY_T8132,
        .dcp_index = 1,
        .dfp_output = 4,
        .cal = {4, 15, 2, 16, 3, 0},
        .cal_set = {true, true, true, true, true, true},
    };

    /* Calibration omission leaves pre-existing register fields untouched. */
    dptx_regs[0x2230 / 4] = 0x1a;
    dptx_phy_t uncal = {.regs = {DPTX_BASE, CORE_BASE}, .type = DPTX_PHY_T8132};
    dptx_t8132_pll_core_cfg(&uncal);
    assert((dptx_regs[0x2230 / 4] & 0x1f) == 0x1a);

    /* Impedance calibration timeout is reported as failure. */
    sim_impcal_fail = true;
    dptx_phy_t fail_phy = phy;
    assert(dptx_t8132_activate(&fail_phy) == -1);
    sim_impcal_fail = false;

    /* Normal activation and calibration. */
    memset(dptx_regs, 0, sizeof(dptx_regs));
    memset(core_regs, 0, sizeof(core_regs));
    assert(dptx_t8132_activate(&phy) == 0);
    assert(phy.active && phy.rate == 0);
    assert((core_regs[0x10 / 4] & 0x7) == 1);
    assert((core_regs[0x48 / 4] & 0x777) == 0x333);
    assert((dptx_regs[0x2230 / 4] & 0x1f) == 15);
    assert(((dptx_regs[0x4228 / 4] >> 16) & 0x7f) == 0x35 && (dptx_regs[0x4228 / 4] & BIT(15)));

    /* Lane programming + invalid 3-lane rejection. */
    assert(dptx_t8132_reconfigure(&phy, true, 4, false, 0) == 0);
    assert(phy.active_lanes == 4 && phy.rate == 0);
    assert(dptx_t8132_reconfigure(&phy, true, 3, false, 0) == -1);

    /* Independent 24 MHz VCO & symbol-clock verification for RBR..HBR3. */
    check_rate_math(&phy, 1620, 3240.0, false);
    check_rate_math(&phy, 2700, 2700.0, true);
    check_rate_math(&phy, 5400, 2700.0, true);
    check_rate_math(&phy, 8100, 4050.0, true);

    /* PLL error paths: no ACK and missing lock both fail cleanly. */
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 0) == 0);
    sim_pll_no_ack = true;
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 2700) == -1);
    sim_pll_no_ack = false;
    sim_pll_no_lock = true;
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 2700) == -1);
    sim_pll_no_lock = false;

    /* Deactivate powers blocks down cleanly. */
    assert(dptx_t8132_deactivate(&phy) == 0);
    assert(!phy.active && (core_regs[0x48 / 4] & 0x777) == 0x444);
    puts("PASS t8132 dptx phy");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "dptx-phy-t8132", harness, extra_cflags=("-lm",))
    assert "PASS t8132 dptx phy" in out


def test_afkv2_27_2_dptx_port_callbacks(tmp_path):
    port_c = (REPO / "src/dcp/dptx_port_ep.c").read_text()
    port_h = (REPO / "src/dcp/dptx_port_ep.h").read_text()
    afk_c = (REPO / "src/afk.c").read_text()
    enums = port_h[port_h.index("enum dptx_apcall {"):port_h.index("dcp_dptx_if_t *dcp_dptx_init")]
    structs = port_c[port_c.index("struct dcpdptx_connection_cmd {"):port_c.index("static int afk_service_call(")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
typedef unsigned long u64; typedef long s64;
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define dprintf(...) ((void)0)
#define PACKED __attribute__((packed))
#define T8132 0x8132
#define T8152 0x8152
#define SUBTYPE_STD_SERVICE 0x20
#define AFKV2_COMMAND 0x10
#define AFKV2_RESPONSE 0x20
static u32 chip_id = T8132;
typedef struct dptx_phy { bool is_t8132; u32 lanes; u32 rate; int activates; } dptx_phy_t;
typedef struct dptx_drive_settings { u32 format, voltage, pre_emphasis; } dptx_drive_settings_t;
typedef struct dcp_dev dcp_dev_t;
typedef struct afk_epic_ep afk_epic_ep_t;
typedef struct afk_epic_service afk_epic_service_t;
typedef struct afk_epic_service_ops {
    u32 (*call_v2)(afk_epic_service_t *, u16, u32, const void *, size_t, void *, size_t);
} afk_epic_service_ops_t;
struct afk_epic_service { afk_epic_ep_t *epic; u32 channel; void *cookie; const afk_epic_service_ops_t *ops; };
struct afk_epic_ep { u32 seq; afk_epic_service_t *svc; };
struct afkv2_hdr { u16 seq; u16 interface; u32 length; } PACKED;
struct afkv2_msg { u64 token; u32 type; u32 category; } PACKED;
struct afkv2_cmd { u32 flags; u32 id; u64 value; } PACKED;
struct afkv2_service_call { u16 unk; u16 group; u32 command; u32 length; u32 magic; u8 pad[48]; } PACKED;

static u8 last_tx[512];
static size_t last_tx_len;
static int afk_epic_tx(afk_epic_ep_t *epic, u32 a, u32 b, const void *buf, size_t len)
{
    (void)epic; (void)a; (void)b;
    assert(len <= sizeof(last_tx));
    memcpy(last_tx, buf, len);
    last_tx_len = len;
    return 0;
}
static afk_epic_service_t *afk_epic_find_service(afk_epic_ep_t *epic, u32 iface)
{
    (void)iface;
    return epic->svc;
}
static int afkv2_reject_command(afk_epic_ep_t *e, const struct afkv2_hdr *h, const struct afkv2_msg *m,
                                const void *p, size_t s)
{
    (void)e; (void)h; (void)m; (void)p; (void)s;
    assert(!"unexpected reject");
    return -1;
}
static bool afk_epic_is_v2(afk_epic_ep_t *e) { (void)e; return true; }
static int cmd_log[8], cmd_attrs[8], cmd_cnt;
static int afk_epic_command(afk_epic_ep_t *e, u32 ch, u32 sub, void *tx, size_t txlen, void *rx, size_t *rxlen)
{
    (void)e; (void)ch; (void)sub;
    struct { u8 pad0[2]; u16 group; u32 command; u32 data_len; u32 magic; u8 pad1[48]; } *call = tx;
    cmd_log[cmd_cnt] = (int)call->command;
    u32 attrs = 0;
    if (call->data_len >= 4)
        memcpy(&attrs, (u8 *)tx + 64, 4);
    cmd_attrs[cmd_cnt++] = (int)attrs;
    if (call->command == 11 || call->command == 12)
        memset((u8 *)tx + 64 + 16, 0, 4);
    else if (call->command == 6)
        memset((u8 *)tx + 64, 0, 4);
    *rxlen = txlen;
    (void)rx;
    return 0;
}
static int dptx_phy_activate(dptx_phy_t *p) { p->activates++; return 0; }
static int dptx_phy_deactivate(dptx_phy_t *p) { (void)p; return 0; }
static int dptx_phy_set_route(dptx_phy_t *p, bool en) { (void)p; (void)en; return 0; }
static u32 dptx_phy_max_link_rate(dptx_phy_t *p) { (void)p; return 8100; }
static u32 dptx_phy_get_active_lane_count(dptx_phy_t *p) { return p->lanes; }
static int dptx_phy_set_active_lane_count(dptx_phy_t *p, u32 n) { p->lanes = n; return 0; }
static int dptx_phy_set_link_rate(dptx_phy_t *p, u32 r) { p->rate = r; return 0; }
static int dptx_phy_set_drive_settings(dptx_phy_t *p, const dptx_drive_settings_t *s, u32 c) { (void)p; (void)s; (void)c; return 0; }
static bool dptx_phy_supports_hpd(dptx_phy_t *p) { return p && !p->is_t8132; }
static bool dptx_phy_needs_validate(dptx_phy_t *p) { return p && p->is_t8132; }
static u32 dptx_phy_dcp_output(dptx_phy_t *p) { (void)p; return 4; }
''' + enums + "\n" + structs + "\n" + "\n".join([
        extract_fn(port_c, "static int afk_service_call("),
        extract_fn(port_c, "int dptxport_validate_connection("),
        extract_fn(port_c, "int dptxport_connect("),
        extract_fn(port_c, "int dptxport_request_display("),
        extract_fn(port_c, "static void dptxport_reply_ok("),
        extract_fn(port_c, "static u32 dptxport_call_v2("),
        extract_fn(port_c, "int dcp_dptx_connect("),
        extract_fn(afk_c, "static int afkv2_handle_command("),
    ]) + r'''
static u8 *send_cb(afk_epic_ep_t *epic, u16 group, u32 command, const void *body, size_t body_len)
{
    size_t sz = sizeof(struct afkv2_cmd) + sizeof(struct afkv2_service_call) + body_len;
    u8 *pkt = calloc(1, sz);
    struct afkv2_cmd *cmd = (void *)pkt;
    struct afkv2_service_call *sc = (void *)(cmd + 1);
    cmd->value = sizeof(*sc) + body_len;
    sc->group = group;
    sc->command = command;
    sc->length = body_len;
    sc->magic = 0x69706378;
    memcpy(sc + 1, body, body_len);
    struct afkv2_hdr hdr = {.interface = 1, .length = sz};
    struct afkv2_msg msg = {.type = SUBTYPE_STD_SERVICE, .category = AFKV2_COMMAND};
    assert(afkv2_handle_command(epic, &hdr, &msg, pkt, sz) == 0);
    free(pkt);
    return last_tx + sizeof(struct afkv2_hdr) + sizeof(struct afkv2_msg) +
           sizeof(struct afkv2_cmd) + sizeof(struct afkv2_service_call);
}

int main(void)
{
    dptx_phy_t phy = {.is_t8132 = true};
    dcp_dptx_if_t dptx = {0};
    afk_epic_service_ops_t ops = {.call_v2 = dptxport_call_v2};
    afk_epic_ep_t epic = {0};
    afk_epic_service_t svc = {.epic = &epic, .channel = 1, .cookie = &dptx.port[0], .ops = &ops};
    epic.svc = &svc;
    dptx.port[0].service = &svc;
    dptx.port[0].enabled = true;

    /* 1. dcp_dptx_connect on dptx-phy,t8132: validate(0x100) -> connect(0) -> request_display(6). */
    assert(dcp_dptx_connect(&dptx, &phy, 0, 0) == 0);
    assert(cmd_cnt == 3);
    assert(cmd_log[0] == 12 && cmd_attrs[0] == 0x100);
    assert(cmd_log[1] == 11 && cmd_attrs[1] == 0);
    assert(cmd_log[2] == 6);

    /* 2. ACTIVATE sets initial link_rate = 0 and zeros reply[0..3]. */
    u8 buf32[32];
    memset(buf32, 0xaa, sizeof(buf32));
    u8 *r = send_cb(&epic, 0, DPTX_APCALL_ACTIVATE, buf32, sizeof(buf32));
    u32 w0 = 99;
    memcpy(&w0, r, 4);
    assert(w0 == 0 && phy.activates == 1 && dptx.port[0].link_rate == 0);

    /* 3. GET_SUPPORTS_HPD returns 0 on dptx-phy,t8132. */
    memset(buf32, 0xbb, sizeof(buf32));
    r = send_cb(&epic, 0, DPTX_APCALL_GET_SUPPORTS_HPD, buf32, sizeof(buf32));
    u64 val = 99;
    memcpy(&w0, r, 4);
    memcpy(&val, r + 16, 8);
    assert(w0 == 0 && val == 0);

    /* 4. SET_DRIVE_SETTINGS + GET_DRIVE_SETTINGS through AFKV2: afk.c preserves reply[0..3] on success. */
    u8 drive_req[32 + 4 * sizeof(dptx_drive_settings_t)] = {0};
    u32 cnt = 2;
    dptx_drive_settings_t lanes[2] = {{0, 1, 2}, {0, 2, 1}};
    memcpy(drive_req + 16, &cnt, 4);
    memcpy(drive_req + 32, lanes, sizeof(lanes));
    r = send_cb(&epic, 1, DPTX_APCALL_SET_DRIVE_SETTINGS, drive_req, sizeof(drive_req));
    u32 st = 99, rcnt = 0;
    memcpy(&st, r, 4);
    memcpy(&rcnt, r + 16, 4);
    assert(st == 0 && rcnt == 2);

    u8 get_req[32 + 4 * sizeof(dptx_drive_settings_t)] = {0};
    memcpy(get_req, &cnt, 4);
    r = send_cb(&epic, 1, DPTX_APCALL_GET_DRIVE_SETTINGS, get_req, sizeof(get_req));
    u32 gcnt = 0, gst = 99;
    dptx_drive_settings_t out_lanes[2] = {0};
    memcpy(&gcnt, r, 4);
    memcpy(&gst, r + 16, 4);
    memcpy(out_lanes, r + 32, sizeof(out_lanes));
    assert(gcnt == 2 && gst == 0);
    assert(out_lanes[0].voltage == 1 && out_lanes[0].pre_emphasis == 2);
    assert(out_lanes[1].voltage == 2 && out_lanes[1].pre_emphasis == 1);

    /* 5. 160-byte SET_TILED_DISPLAY_HINTS selects layout purely by size (even with chip_id != T8132/T8152). */
    chip_id = 0x6040;
    u8 tiled[160];
    memset(tiled, 0x55, sizeof(tiled));
    u32 hint0 = 0x12345678;
    memcpy(tiled, &hint0, 4);
    r = send_cb(&epic, 0, DPTX_APCALL_SET_TILED_DISPLAY_HINTS, tiled, sizeof(tiled));
    u32 rhint0 = 0, rstatus = 99;
    memcpy(&rhint0, r, 4);
    memcpy(&rstatus, r + 0x90, 4);
    assert(rhint0 == 0x12345678 && rstatus == 0 && r[0x70] == 0 && r[0x8f] == 0);

    /* 6. Short (<160) SET_TILED_DISPLAY_HINTS on non-T8152 fails with kIOReturnUnsupported written by afk.c. */
    r = send_cb(&epic, 0, DPTX_APCALL_SET_TILED_DISPLAY_HINTS, buf32, sizeof(buf32));
    memcpy(&w0, r, 4);
    assert(w0 == 0xe00002c7);

    /* 7. J873g (T8152): preserves SET_DRIVE_SETTINGS bytes 4..15, zeros <160 tiled-hints reply[0..3],
     *    and returns kIOReturnUnsupported on SET_RESOURCE_AVAILABLE. */
    phy.is_t8132 = false;
    chip_id = T8152;
    memset(drive_req, 0, sizeof(drive_req));
    u32 marker = 0xdeadbeef;
    memcpy(drive_req + 8, &marker, 4);
    memcpy(drive_req + 16, &cnt, 4);
    memcpy(drive_req + 32, lanes, sizeof(lanes));
    r = send_cb(&epic, 1, DPTX_APCALL_SET_DRIVE_SETTINGS, drive_req, sizeof(drive_req));
    u32 rmarker = 0;
    memcpy(&st, r, 4);
    memcpy(&rmarker, r + 8, 4);
    assert(st == 0 && rmarker == 0xdeadbeef);

    memset(buf32, 0x77, sizeof(buf32));
    r = send_cb(&epic, 0, DPTX_APCALL_SET_TILED_DISPLAY_HINTS, buf32, sizeof(buf32));
    memcpy(&w0, r, 4);
    assert(w0 == 0 && r[4] == 0x77);

    r = send_cb(&epic, 0, DPTX_APCALL_SET_RESOURCE_AVAILABLE, buf32, sizeof(buf32));
    memcpy(&w0, r, 4);
    assert(w0 == 0xe00002c7);

    /* 8. Bad-argument error path clobbers reply[0..3] with 0xe00002c2. */
    u8 short_buf[8] = {0};
    r = send_cb(&epic, 0, DPTX_APCALL_SET_LINK_RATE, short_buf, sizeof(short_buf));
    memcpy(&w0, r, 4);
    assert(w0 == 0xe00002c2);

    puts("PASS afkv2 27.2 dptx callbacks");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "afkv2-27-2-dptx", harness)
    assert "PASS afkv2 27.2 dptx callbacks" in out


def test_t8132_display_selection_bpp_and_cleanup(tmp_path):
    """Test hardware-driven display_is_t8132_hdmi(), cbest.bpp -> FMT_BGRA/FMT_w30r,
    and display_init_t8132_hdmi()'s own dcp_left_running() cleanup guard."""
    disp = (REPO / "src/display.c").read_text()
    ibh = (REPO / "src/dcp_iboot.h").read_text()
    dcph = (REPO / "src/dcp.h").read_text()
    ib_types = ibh[ibh.index("enum DCPEOTF {"):ibh.index("dcp_iboot_if_t *dcp_ib_init")]
    disp_cfg_type = dcph[dcph.index("typedef struct {"):dcph.index("} display_config_t;")] + "} display_config_t;\n"
    cfg_decl = disp[disp.index("static const display_config_t display_config_t8132_hdmi_ext ="):disp.index("#define abs(x)")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef unsigned long u64; typedef long s64;
#define PACKED __attribute__((packed))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SZ_16K (1 << 14)
#define SZ_32M (1 << 25)
#define T6030 0x6030
#define T8132 0x8132
#define T8152 0x8152
#define DART_PTR_ERR (1ULL << 63)
#define DART_IS_ERR(v) (((v) >> 63) & 1)
#define FB_DEPTH_MASK 0xff
#define FB_DEPTH_FLAG_RETINA 0x10000
#define DISPLAY_STATUS_DELAY 100
#define DISPLAY_STATUS_RETRIES(dptx) ((dptx) ? 100 : 20)
#define COMPARE(a, b) if ((a) > (b)) { *best = modes[i]; continue; } else if ((a) < (b)) { continue; }
#define abs(x) ((x) >= 0 ? (x) : -(x))
typedef enum _dcp_shutdown_mode { DCP_QUIESCED = 0, DCP_SLEEP_IF_EXTERNAL = 1, DCP_SLEEP = 2 } dcp_shutdown_mode;
''' + ib_types + disp_cfg_type + r'''
typedef struct { void *dart_disp, *dart_dcp, *iovad_dcp, *dptx_ep; } dcp_dev_t;
typedef struct { int id; } dcp_iboot_if_t;
static u32 chip_id = 0x6040;
static void *adt = (void *)1;
static dcp_dev_t dummy_dcp, *dcp;
static dcp_iboot_if_t dummy_iboot, *iboot;
static u64 fb_dva, fb_size;
static bool display_is_external, display_is_dptx, display_needs_power_cycle;
static bool display_mode_max, fb_dva_allocated, fb_dva_mapped, left_running;
static struct { struct { u64 base, stride, width, height, depth; } video; } cur_boot_args;
static u8 boot_args_buf[sizeof(cur_boot_args)];
static u64 boot_args_addr = (u64)boot_args_buf;

/* Configurable ADT mock for display_is_t8132_hdmi() */
static bool adt_has_disp0, adt_disp0_ext, adt_has_dcpext0, adt_has_gpio, adt_has_phy, adt_phy_gpio_parent;
static const char *adt_phy_compat;
static int adt_path_offset(void *a, const char *p)
{
    (void)a;
    if (!strcmp(p, "/arm-io/disp0")) return adt_has_disp0 ? 1 : -1;
    if (!strcmp(p, "/arm-io/dcpext0")) return adt_has_dcpext0 ? 2 : -1;
    if (!strcmp(p, "/arm-io/dp2hdmi-gpio0")) return adt_has_gpio ? 3 : -1;
    if (!strcmp(p, "/arm-io/dptx-phy")) return adt_has_phy ? 4 : -1;
    return -1;
}
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a; (void)len;
    if (node == 1 && !strcmp(name, "external")) return adt_disp0_ext ? (void *)1 : NULL;
    if (node == 4 && !strcmp(name, "dp2hdmi-gpio-parent")) return adt_phy_gpio_parent ? (void *)1 : NULL;
    return NULL;
}
static bool adt_is_compatible(void *a, int node, const char *compat)
{
    (void)a;
    return node == 4 && adt_phy_compat && !strcmp(adt_phy_compat, compat);
}
static int adt_setprop(void *a, int n, const char *p, void *v, size_t s) { (void)a; (void)n; (void)p; (void)v; (void)s; return 0; }
''' + cfg_decl + r'''
static unsigned configures, unmaps, shutdowns;
static int configure_ret;
static bool use_real_configure;
static u32 last_surface_fmt;
static u8 fb_mem[640 * 480 * 4 + 24 * SZ_16K] __attribute__((aligned(SZ_16K)));
static dcp_timing_mode_t test_tmode = {.valid = 1, .width = 640, .height = 480, .fps = 60 << 16};
static dcp_color_mode_t test_cmode = {.valid = 1, .bpp = 32};

static bool dcp_left_running(void) { return left_running; }
static bool display_unmap_fb(uintptr_t iova, size_t size) { (void)iova; (void)size; unmaps++; return true; }
static uintptr_t display_map_fb(uintptr_t iova, u64 pa, u64 size) { (void)pa; (void)size; return iova; }
/* Intentionally does NOT clear dcp so display_init_t8132_hdmi()'s own guard is tested! */
static int display_shutdown(dcp_shutdown_mode m)
{
    assert(m == DCP_QUIESCED);
    shutdowns++;
    iboot = NULL;
    return left_running ? -1 : 0;
}
static int display_start_dcp(void)
{
    dcp = &dummy_dcp;
    iboot = &dummy_iboot;
    fb_dva = 0x10004000UL;
    fb_size = sizeof(fb_mem);
    cur_boot_args.video.base = (u64)fb_mem;
    return 0;
}
static int dcp_connect_dptx(dcp_dev_t *d) { (void)d; return 0; }
static int dcp_dptx_hpd(void *ep, u32 port, bool hpd) { (void)ep; (void)port; (void)hpd; return 0; }
static int dcp_work(dcp_dev_t *d) { (void)d; return 0; }
static int dcp_ib_set_power(dcp_iboot_if_t *ib, bool on) { (void)ib; (void)on; return 0; }
static int dcp_ib_get_hpd(dcp_iboot_if_t *ib, int *tc, int *cc) { (void)ib; *tc = 1; *cc = 1; return 1; }
static int dcp_ib_get_timing_modes(dcp_iboot_if_t *ib, dcp_timing_mode_t **m) { (void)ib; *m = &test_tmode; return 1; }
static int dcp_ib_get_color_modes(dcp_iboot_if_t *ib, dcp_color_mode_t **m) { (void)ib; *m = &test_cmode; return 1; }
static int dcp_ib_set_mode(dcp_iboot_if_t *ib, dcp_timing_mode_t *t, dcp_color_mode_t *c) { (void)ib; (void)t; (void)c; return 0; }
static int dcp_ib_set_surface(dcp_iboot_if_t *ib, dcp_layer_t *layer) { (void)ib; last_surface_fmt = layer->surface_fmt; return 0; }
static void mdelay(unsigned ms) { (void)ms; }
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_msecs(u64 t) { return t; }
static u64 top_of_memory_alloc(size_t s) { (void)s; assert(0); return 0; }
static u64 iova_alloc(void *d, size_t s) { (void)d; (void)s; assert(0); return 0; }
static void iova_free(void *d, u64 a, size_t s) { (void)d; (void)a; (void)s; }
static void mmu_map_framebuffer(u64 a, u64 s) { (void)a; (void)s; }
static void fb_reinit(void) {}
static u64 dart_dva_base(void *d) { (void)d; return 0; }
static bool dart_unmap_checked(void *d, u64 a, size_t s) { (void)d; (void)a; (void)s; return true; }
''' + "\n".join([
        extract_fn(disp, "u64 display_mode_fb_size("),
        extract_fn(disp, "static void display_choose_timing_mode("),
        extract_fn(disp, "static void display_choose_color_mode("),
        extract_fn(disp, "static bool display_t6030_hands_off("),
        "struct display_options { bool retina; };",
        extract_fn(disp, "int display_parse_mode("),
        extract_fn(disp, "static int display_swap("),
        extract_fn(disp, "int display_configure(").replace("int display_configure(", "static int real_display_configure("),
    ]) + r'''
static int display_configure(const char *cfg)
{
    if (use_real_configure)
        return real_display_configure(cfg);
    configures++;
    dcp = &dummy_dcp;
    iboot = &dummy_iboot;
    fb_dva = 0x10000004000UL;
    fb_size = 0x854000;
    fb_dva_mapped = true;
    return configure_ret;
}
''' + extract_fn(disp, "static int display_init_t8132_hdmi(") + r'''
static void reset_init(void)
{
    dcp = NULL; iboot = NULL;
    fb_dva = fb_size = 0;
    display_is_external = display_is_dptx = fb_dva_mapped = left_running = use_real_configure = false;
    configures = unmaps = shutdowns = 0;
    configure_ret = 1;
}
int main(void)
{
    /* 1. Hardware-driven display_is_t8132_hdmi() matrix (no chip_id or fw-version gate). */
    adt_has_disp0 = adt_disp0_ext = adt_has_dcpext0 = adt_has_gpio = adt_has_phy = adt_phy_gpio_parent = true;
    adt_phy_compat = "dptx-phy,t8132";
    assert(display_is_t8132_hdmi());       /* even with chip_id=0x6040 (J773s) */
    adt_has_disp0 = false;
    assert(display_is_t8132_hdmi());       /* disp0 absent = headless */
    adt_has_disp0 = true; adt_disp0_ext = false;
    assert(!display_is_t8132_hdmi());      /* internal panel (J713/J604) */
    adt_disp0_ext = true; adt_phy_gpio_parent = false;
    assert(!display_is_t8132_hdmi());      /* missing dp2hdmi-gpio-parent */
    adt_phy_gpio_parent = true; adt_phy_compat = "dptx-phy,t8122";
    assert(!display_is_t8132_hdmi());      /* 26.x ADT compatible */
    adt_phy_compat = "dptx-phy,t8152";
    assert(!display_is_t8132_hdmi());      /* J873g */
    adt_phy_compat = "dptx-phy,t8132"; adt_has_dcpext0 = false;
    assert(!display_is_t8132_hdmi());
    adt_has_dcpext0 = true; adt_has_gpio = false;
    assert(!display_is_t8132_hdmi());

    /* 2. Real non-dummy framebuffer adopted untouched. */
    reset_init();
    cur_boot_args.video = (typeof(cur_boot_args.video)){0x105e5300000UL, 0, 1920, 1080, 32};
    assert(display_init_t8132_hdmi() == 0 && !configures && !unmaps && !shutdowns);

    /* 3. Dummy 640x1136 framebuffer configures HDMI cleanly. */
    reset_init();
    cur_boot_args.video = (typeof(cur_boot_args.video)){0x105e5300000UL, 0, 640, 1136, 32};
    assert(display_init_t8132_hdmi() == 1 && display_is_external && configures == 1 && !shutdowns);

    /* 4. No monitor connected: display_shutdown stub leaves dcp set, so display_init_t8132_hdmi()
     *    itself clears dcp when !dcp_left_running() and keeps dcp when dcp_left_running(). */
    reset_init();
    configure_ret = 0;
    left_running = false;
    assert(display_init_t8132_hdmi() == 0);
    assert(unmaps == 1 && shutdowns == 1 && dcp == NULL && iboot == NULL && !fb_dva_mapped);

    reset_init();
    configure_ret = -1;
    left_running = true;
    assert(display_init_t8132_hdmi() == -1);
    assert(unmaps == 1 && shutdowns == 1 && dcp == &dummy_dcp && iboot == NULL);

    /* 5. cbest.bpp selects FMT_BGRA/32 vs FMT_w30r/30 without SoC or DVA-base gates. */
    reset_init();
    use_real_configure = true;
    test_cmode.bpp = 32;
    assert(display_configure(NULL) == 1);
    assert(last_surface_fmt == FMT_BGRA && (cur_boot_args.video.depth & FB_DEPTH_MASK) == 32);

    reset_init();
    use_real_configure = true;
    test_cmode.bpp = 30;
    assert(display_configure(NULL) == 1);
    assert(last_surface_fmt == FMT_w30r && (cur_boot_args.video.depth & FB_DEPTH_MASK) == 30);

    puts("PASS t8132 display selection bpp cleanup");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "t8132-display-init", harness)
    assert "PASS t8132 display selection bpp cleanup" in out


def test_t8132_dcp_init_protected_cpu_and_phy_compat(tmp_path):
    """Verify dcp_init keys DPTX support on dptx-phy compatible (no fw-version gate),
    skips pmgr_adt_power_enable(cfg->dcp) on protected CPUs, and bails out cleanly
    if iBoot left the protected CPU unpowered."""
    dcp_c = (REPO / "src/dcp.c").read_text()
    fn = extract_fn(dcp_c, "dcp_dev_t *dcp_init(")
    cut = fn.index("int dart_node = adt_path_offset(")
    fn_head = fn[:cut] + "return (dcp_dev_t *)0x1;\n}\n"
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef struct dcp_dev dcp_dev_t;
typedef struct {
    const char *dcp;
    const char *dcp_dart;
    const char *disp_dart;
    const char *pmgr_dev;
    const char *dptx_phy;
    int die;
} display_config_t;
enum { V13_5 = 1, V_UNKNOWN = 99 };
static struct { int version; } os_firmware = { V_UNKNOWN };
static void *adt = (void *)1;
#define T8152 0x8152
static u32 chip_id = 0x8132;
static char dcp_pmgr_dev[32];
static int dcp_die;
static bool dcp_init_quiesced;
static bool is_protected;
static int protected_requested = 1;
static const char *phy_compat = "dptx-phy,t8132";
static int dcp_power_calls, phy_power_calls;
static int adt_path_offset(void *a, const char *p) { (void)a; (void)p; return 1; }
static bool adt_is_compatible(void *a, int n, const char *c)
{
    (void)a; (void)n;
    return strcmp(c, phy_compat) == 0;
}
static bool pmgr_device_is_protected(const char *name) { (void)name; return is_protected; }
static int pmgr_power_is_requested(int die, const char *name) { (void)die; (void)name; return protected_requested; }
static int pmgr_adt_power_enable(const char *path)
{
    if (strcmp(path, "/arm-io/dcpext0") == 0) {
        dcp_power_calls++;
        return is_protected ? -1 : 0;
    }
    if (strcmp(path, "/arm-io/dptx-phy") == 0) {
        phy_power_calls++;
        return 0;
    }
    return -1;
}
static int pmgr_power_on(int die, const char *name) { (void)die; (void)name; return 0; }
static int t8152_psd_power_on(u32 gate) { (void)gate; return 0; }
#define ADT_GETPROP(a, n, p, v) (-1)
static void mdelay(u32 ms) { (void)ms; }
''' + fn_head + r'''
int main(void)
{
    display_config_t cfg = {
        .dcp = "/arm-io/dcpext0",
        .pmgr_dev = "DISPEXT0_CPU",
        .dptx_phy = "/arm-io/dptx-phy",
        .die = 0,
    };

    /* 1. dptx-phy,t8132 works with any firmware version (V_UNKNOWN) when protected CPU is requested on. */
    is_protected = true; protected_requested = 1; phy_compat = "dptx-phy,t8132";
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) != NULL);
    assert(dcp_power_calls == 0 && phy_power_calls == 1);

    /* 2. Protected CPU left off by iBoot: clean bail-out with zero PMGR writes/calls. */
    protected_requested = 0;
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) == NULL);
    assert(dcp_power_calls == 0 && phy_power_calls == 0);

    /* 3. Unprotected CPU: powers both dcp and PHY. */
    is_protected = false;
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) != NULL);
    assert(dcp_power_calls == 1 && phy_power_calls == 1);

    /* 4. Legacy PHY compatible (t8112) still requires V13_5. */
    phy_compat = "dptx-phy,t8112";
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) == NULL);
    assert(dcp_power_calls == 0 && phy_power_calls == 0);

    puts("PASS t8132 dcp_init protected cpu and phy compat");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "t8132-dcp-init-protected", harness)
    assert "PASS t8132 dcp_init protected cpu and phy compat" in out
