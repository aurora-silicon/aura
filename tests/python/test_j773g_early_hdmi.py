# SPDX-License-Identifier: MIT
"""Host unit tests for J773g (M4 Mac mini, T8132) 27.2 early HDMI display bring-up.

Compiles production functions verbatim from src/pmgr.c, src/dart.c,
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
    pmgr = (REPO / "src/pmgr.c").read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define FIELD_GET(f, v) (((v) & (f)) / ((f) & -(f)))
#define PACKED __attribute__((packed))
#define T8132 0x8132
#define T8140 0x8140
#define PMGR_RESET BIT(31)
#define PMGR_AUTO_ENABLE BIT(28)
#define PMGR_DEV_DISABLE BIT(10)
#define PMGR_WAS_CLKGATED BIT(9)
#define PMGR_WAS_PWRGATED BIT(8)
#define PMGR_PS_ACTUAL GENMASK(7, 4)
#define PMGR_PS_TARGET GENMASK(3, 0)
#define PMGR_PS_ACTIVE 0xf
#define PMGR_POLL_TIMEOUT 10
struct pmgr_device {
    u8 flags; u16 unk1; u8 id1;
    union { struct { u8 parent[2]; u8 unk2[2]; } u8id; struct { u16 parent[2]; } u16id; };
    u8 unk3[2]; u8 addr_offset; u8 psreg_idx; u8 unk4[4];
    struct { u32 offset : 24; u32 group : 8; } group_and_offset;
    u8 unk5[6]; u16 id2; u8 unk6[4];
    const char name[0x10];
} PACKED;
static u32 chip_id = T8132;
static int pmgr_dies = 1;
static bool pmgr_use_group_and_offset = false;
bool pmgr_keep_configured = false;
static struct pmgr_device devs[3];
static const struct pmgr_device *pmgr_devices = devs;
static u32 pmgr_devices_len = 3;
static u32 regs[3];
static unsigned writes[3];
static uintptr_t pmgr_device_get_addr(u8 die, const struct pmgr_device *dev)
{
    (void)die;
    return 0x1000 + (uintptr_t)dev->addr_offset * 8;
}
static int reg_idx(uintptr_t addr) { return (int)((addr - 0x1000) / 8); }
static u32 read32(uintptr_t addr) { return regs[reg_idx(addr)]; }
static void mask32(uintptr_t addr, u32 clr, u32 set)
{
    int i = reg_idx(addr);
    writes[i]++;
    regs[i] = (regs[i] & ~clr) | set;
    regs[i] = (regs[i] & ~0xf0u) | ((regs[i] & 0xfu) << 4);
}
static void set32(uintptr_t addr, u32 v) { writes[reg_idx(addr)]++; regs[reg_idx(addr)] |= v; }
static void clear32(uintptr_t addr, u32 v) { writes[reg_idx(addr)]++; regs[reg_idx(addr)] &= ~v; }
static int poll32(uintptr_t addr, u32 m, u32 v, u32 t) { (void)addr; (void)m; (void)v; (void)t; return 0; }
static void udelay(u32 us) { (void)us; }
static bool pmgr_protected_addr(uintptr_t addr);
''' + "\n".join([
        extract_fn(pmgr, "static bool pmgr_protected_name("),
        extract_fn(pmgr, "bool pmgr_device_is_protected("),
        extract_fn(pmgr, "static bool pmgr_protected_device("),
        extract_fn(pmgr, "static bool pmgr_protected_addr("),
        extract_fn(pmgr, "int pmgr_set_mode("),
        extract_fn(pmgr, "static int pmgr_reset_device("),
    ]) + r'''
int main(void)
{
    strcpy((char *)devs[0].name, "DISP_CPU");     devs[0].addr_offset = 0; regs[0] = 0x1000000f;
    strcpy((char *)devs[1].name, "DISPEXT0_CPU"); devs[1].addr_offset = 1; regs[1] = 0x1404024f;
    strcpy((char *)devs[2].name, "DPTX_PHY");     devs[2].addr_offset = 2; regs[2] = 0x0;

    assert(pmgr_protected_device(&devs[0]));
    assert(pmgr_protected_device(&devs[1]));
    assert(!pmgr_protected_device(&devs[2]));
    assert(pmgr_device_is_protected("DISP_CPU"));
    assert(pmgr_device_is_protected("DISPEXT0_CPU"));
    assert(!pmgr_device_is_protected("DPTX_PHY"));

    /* Protected display CPUs refuse transitions and resets with zero MMIO writes. */
    assert(pmgr_set_mode(0x1000, PMGR_PS_ACTIVE) == -1 && writes[0] == 0);
    assert(pmgr_set_mode(0x1008, PMGR_PS_ACTIVE) == -1 && writes[1] == 0);
    assert(pmgr_reset_device(0, &devs[0]) == -1 && writes[0] == 0);
    assert(pmgr_reset_device(0, &devs[1]) == -1 && writes[1] == 0);

    /* Non-protected DPTX_PHY powers on normally. */
    assert(pmgr_set_mode(0x1010, PMGR_PS_ACTIVE) == 0 && writes[2] == 1);
    assert(FIELD_GET(PMGR_PS_ACTUAL, regs[2]) == PMGR_PS_ACTIVE);
    puts("PASS t8132 pmgr protection");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "pmgr-t8132", harness)
    assert "PASS t8132 pmgr protection" in out


def test_locked_dart_releases_empty_heap_l2_keeps_active_and_iboot(tmp_path):
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
static int tlbi_fail = 0, tlbi_calls = 0;
static void write32(u64 addr, u32 v) { (void)addr; (void)v; }
static int poll32(u64 addr, u32 m, u32 v, u32 t)
{
    (void)addr; (void)m; (void)v; (void)t;
    tlbi_calls++;
    return tlbi_fail ? -1 : 0;
}
/* pool[0] = iBoot L1, pool[1] = iBoot L2 (non-heap), pool[2..5] = heap L2s + dart struct. */
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
''' + defs + "\n" + extract_fn(dart, "bool dart_shutdown_checked(") + r'''
#undef free
static u64 pte_of(const void *p)
{
    return FIELD_PREP(dart_t8110.offset_mask, ((u64)p) >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
}
int main(void)
{
    /* Case 1: locked dart-dcpext0 with iBoot L2 (idx 0), empty heap L2 (idx 1), active heap L2 (idx 2). */
    memset(iboot_pages, 0, sizeof(iboot_pages));
    memset(heap_pages, 0, sizeof(heap_pages));
    nfreed = tlbi_calls = tlbi_fail = 0;

    iboot_pages[0][0] = pte_of(iboot_pages[1]); /* iBoot L2, empty, must NOT be touched */
    iboot_pages[0][1] = pte_of(heap_pages[0]);  /* empty heap L2 -> unlink, TLBI, remove from legacy_l2, free */
    iboot_pages[0][2] = pte_of(heap_pages[1]);  /* active heap L2 (FB) -> keep! */
    heap_pages[1][0] = pte_of(iboot_pages[1]);  /* valid PTE inside active heap L2 */

    legacy_l2_pages[0] = (u64)heap_pages[0];
    legacy_l2_pages[1] = (u64)heap_pages[1];
    legacy_l2_count = 2;

    dart_dev_t *d = (dart_dev_t *)heap_pages[2];
    *d = (dart_dev_t){
        .locked = true, .legacy_mutable = true, .type = DART_T8110,
        .params = &dart_t8110, .l1 = {iboot_pages[0]},
    };
    assert(dart_shutdown_checked(d));
    assert(tlbi_calls == 1);
    assert(iboot_pages[0][0] == pte_of(iboot_pages[1])); /* iBoot L2 untouched */
    assert(iboot_pages[0][1] == 0);                      /* empty heap L2 unlinked */
    assert(iboot_pages[0][2] == pte_of(heap_pages[1]));  /* active heap L2 kept */
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)heap_pages[1]);
    assert(nfreed == 2 && freed[0] == heap_pages[0] && freed[1] == d);

    /* Case 2: TLB invalidation fails -> keep page in legacy_l2_pages and do not free. */
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
        .params = &dart_t8110, .l1 = {iboot_pages[0]},
    };
    assert(!dart_shutdown_checked(d));
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)heap_pages[0]);
    assert(nfreed == 0);

    puts("PASS locked dart L2 release");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "dart-locked-l2", harness, extra_cflags=("-no-pie",))
    assert "PASS locked dart L2 release" in out


def test_t8132_dptx_phy_sequence(tmp_path):
    phy = (REPO / "src/dcp/dptx_phy.c").read_text()
    phy_body = phy[phy.index("enum dptx_type {"):phy.index("/* Read-modify-write of a field in the core")]
    harness = r'''
#include <assert.h>
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
    /* Simulate AUX impedance calibration done + code 0x35 on 0x4200 bit 0. */
    if (addr == DPTX_BASE + 0x4200 && (val & 1)) {
        dptx_regs[0x4204 / 4] = 1;
        dptx_regs[0x4210 / 4] = 0x35U << 18;
    }
    /* Simulate PLL command ACK + lock state on 0x100c bit 3. */
    if (addr == DPTX_BASE + 0x100c && (val & 8)) {
        u32 cmd = val & 7;
        dptx_regs[0x1014 / 4] = 1;
        dptx_regs[0x1008 / 4] = (cmd == 7) ? (1U << 3) : 0;
    }
}
static void mask32(u64 addr, u32 clear, u32 set) { write32(addr, (read32(addr) & ~clear) | set); }
static void set32(u64 addr, u32 bits) { write32(addr, read32(addr) | bits); }
static void clear32(u64 addr, u32 bits) { write32(addr, read32(addr) & ~bits); }
static void udelay(u32 us) { (void)us; }
static void mdelay(u32 ms) { (void)ms; }
''' + phy_body + r'''
int main(void)
{
    dptx_phy_t phy = {
        .regs = {DPTX_BASE, CORE_BASE},
        .type = DPTX_PHY_T8132,
        .dcp_index = 1, /* dcpext0 -> source select 1 */
        .dfp_output = 4,
        /* Exact J773g 27.2 ADT calibration values */
        .cal = {4, 15, 2, 16, 3, 0},
        .cal_set = {true, true, true, true, true, true},
    };

    assert(dptx_t8132_activate(&phy) == 0);
    assert(phy.active && phy.rate == 0);
    /* Source select = 1, power enables set, isolation cleared. */
    assert((core_regs[0x10 / 4] & 0x7) == 1);
    assert((core_regs[0x48 / 4] & 0x777) == 0x333);
    /* Calibration fields programmed cleanly. */
    assert((dptx_regs[0x2230 / 4] & 0x1f) == 15);
    assert(((dptx_regs[0x22e8 / 4] >> 17) & 3) == 2);
    assert(((dptx_regs[0x2240 / 4] >> 22) & 0x1f) == 16);
    assert(((dptx_regs[0x22b0 / 4] >> 20) & 7) == 3);
    /* Impedance calibration code 0x35 written to 0x4228[22:16] with valid bit 15. */
    assert(((dptx_regs[0x4228 / 4] >> 16) & 0x7f) == 0x35);
    assert(dptx_regs[0x4228 / 4] & BIT(15));

    /* Set 4 lanes before rate -> rate stays 0, all 4 lane disable bits cleared. */
    assert(dptx_t8132_reconfigure(&phy, true, 4, false, 0) == 0);
    assert(phy.active_lanes == 4 && phy.rate == 0);
    for (int l = 0; l < 4; l++)
        assert(!(dptx_regs[(0x5000 + l * 0x1000) / 4] & 0x100));
    /* Invalid lane count rejected. */
    assert(dptx_t8132_reconfigure(&phy, true, 3, false, 0) == -1);

    /* Program RBR (1620: full rate, bypass=1), HBR (2700), HBR2 (5400), HBR3 (8100: bypass=0),
     * verifying exact integer/fractional feedback dividers computed from 24 MHz refclk. */
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 1620) == 0);
    assert(phy.rate == 1620 && (dptx_regs[0x222c / 4] & 3) == 2 && (dptx_regs[0x4004 / 4] & 0x60) == 0x20);
    assert((dptx_regs[0x20b0 / 4] & 0x3ff) == 4 * 0x87 && dptx_regs[0x20b4 / 4] == 0);
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 2700) == 0);
    assert(phy.rate == 2700 && (dptx_regs[0x4004 / 4] & 0x60) == 0x40);
    assert((dptx_regs[0x20b0 / 4] & 0x3ff) == 4 * 0x70 + 2);
    assert(dptx_regs[0x20b4 / 4] == ((0x1fffU << 14) | 0x3ffeU));
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 5400) == 0);
    assert(phy.rate == 5400 && (dptx_regs[0x222c / 4] & 3) == 2);
    assert((dptx_regs[0x20b0 / 4] & 0x3ff) == 4 * 0x70 + 2);
    assert(dptx_regs[0x20b4 / 4] == ((0x1fffU << 14) | 0x3ffeU));
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 8100) == 0);
    assert(phy.rate == 8100 && (dptx_regs[0x222c / 4] & 3) == 1 && (dptx_regs[0x1008 / 4] & BIT(3)));
    assert((dptx_regs[0x20b0 / 4] & 0x3ff) == 4 * 0xa8 + 3);
    assert(dptx_regs[0x20b4 / 4] == ((0x2ffdU << 14) | 0x3ffcU));

    /* Rate 0 powers PLL down without error. */
    assert(dptx_t8132_reconfigure(&phy, false, 0, true, 0) == 0);
    assert(phy.rate == 0 && !(dptx_regs[0x1008 / 4] & BIT(3)));

    /* Deactivate clears active state and powers off blocks. */
    assert(dptx_t8132_deactivate(&phy) == 0);
    assert(!phy.active && (core_regs[0x48 / 4] & 0x777) == 0x444);
    puts("PASS t8132 dptx phy");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "dptx-phy-t8132", harness)
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
    /* Echo target/attrs and status = 0 in reply */
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

    /* 1. dcp_dptx_connect on T8132: validate(0x100) -> connect(0) -> request_display(6). */
    assert(dcp_dptx_connect(&dptx, &phy, 0, 0) == 0);
    assert(cmd_cnt == 3);
    assert(cmd_log[0] == 12 && cmd_attrs[0] == 0x100);
    assert(cmd_log[1] == 11 && cmd_attrs[1] == 0);
    assert(cmd_log[2] == 6);

    /* 2. ACTIVATE sets initial link_rate = 0 on T8132 and zeros reply[0..3]. */
    u8 buf32[32];
    memset(buf32, 0xaa, sizeof(buf32));
    u8 *r = send_cb(&epic, 0, DPTX_APCALL_ACTIVATE, buf32, sizeof(buf32));
    u32 w0 = 99;
    memcpy(&w0, r, 4);
    assert(w0 == 0 && phy.activates == 1 && dptx.port[0].link_rate == 0);

    /* 3. GET_SUPPORTS_HPD returns 0 on T8132. */
    memset(buf32, 0xbb, sizeof(buf32));
    r = send_cb(&epic, 0, DPTX_APCALL_GET_SUPPORTS_HPD, buf32, sizeof(buf32));
    u64 val = 99;
    memcpy(&w0, r, 4);
    memcpy(&val, r + 16, 8);
    assert(w0 == 0 && val == 0);

    /* 4. SET_DRIVE_SETTINGS (group 1, cmd 3) + GET_DRIVE_SETTINGS (group 1, cmd 4) through AFKV2. */
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
    memcpy(get_req, &cnt, 4); /* count at +0x00 */
    r = send_cb(&epic, 1, DPTX_APCALL_GET_DRIVE_SETTINGS, get_req, sizeof(get_req));
    u32 gcnt = 0, gst = 99;
    dptx_drive_settings_t out_lanes[2] = {0};
    memcpy(&gcnt, r, 4);
    memcpy(&gst, r + 16, 4);
    memcpy(out_lanes, r + 32, sizeof(out_lanes));
    assert(gcnt == 2 && gst == 0); /* afk.c did NOT clobber reply[0..3]! */
    assert(out_lanes[0].voltage == 1 && out_lanes[0].pre_emphasis == 2);
    assert(out_lanes[1].voltage == 2 && out_lanes[1].pre_emphasis == 1);

    /* 5. 160-byte SET_TILED_DISPLAY_HINTS preserves hints[0] and sets status=0 at +0x90. */
    u8 tiled[160];
    memset(tiled, 0x55, sizeof(tiled));
    u32 hint0 = 0x12345678;
    memcpy(tiled, &hint0, 4);
    r = send_cb(&epic, 0, DPTX_APCALL_SET_TILED_DISPLAY_HINTS, tiled, sizeof(tiled));
    u32 rhint0 = 0, rstatus = 99;
    memcpy(&rhint0, r, 4);
    memcpy(&rstatus, r + 0x90, 4);
    assert(rhint0 == 0x12345678 && rstatus == 0 && r[0x70] == 0 && r[0x8f] == 0);

    /* 6. SET_RESOURCE_AVAILABLE (25) succeeds with status 0 on T8132, unsupported on T8152. */
    memset(buf32, 0x77, sizeof(buf32));
    r = send_cb(&epic, 0, DPTX_APCALL_SET_RESOURCE_AVAILABLE, buf32, sizeof(buf32));
    memcpy(&w0, r, 4);
    assert(w0 == 0);

    /* 7. Non-T8132 (J873g/T8152): SET_DRIVE_SETTINGS preserves request bytes 4..15,
     *    SET_RESOURCE_AVAILABLE returns kIOReturnUnsupported (0xe00002c7) written by afk.c. */
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
    r = send_cb(&epic, 0, DPTX_APCALL_SET_RESOURCE_AVAILABLE, buf32, sizeof(buf32));
    memcpy(&w0, r, 4);
    assert(w0 == 0xe00002c7);

    puts("PASS afkv2 27.2 dptx callbacks");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "afkv2-27-2-dptx", harness)
    assert "PASS afkv2 27.2 dptx callbacks" in out


def test_t8132_display_init_adopt_and_cleanup(tmp_path):
    disp = (REPO / "src/display.c").read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef unsigned long u64;
typedef enum _dcp_shutdown_mode { DCP_QUIESCED = 0, DCP_SLEEP_IF_EXTERNAL = 1, DCP_SLEEP = 2 } dcp_shutdown_mode;
static int dummy_dcp, dummy_iboot;
static void *dcp, *iboot;
static u64 fb_dva, fb_size;
static bool display_is_external, fb_dva_mapped, left_running;
static struct { struct { u64 base, width, height, depth; } video; } cur_boot_args;
static unsigned configures, unmaps, shutdowns;
static int configure_ret;
static bool dcp_left_running(void) { return left_running; }
static bool display_unmap_fb(uintptr_t iova, size_t size) { (void)iova; (void)size; unmaps++; return true; }
static int display_shutdown(dcp_shutdown_mode m)
{
    assert(m == DCP_QUIESCED);
    shutdowns++;
    iboot = NULL;
    if (!left_running)
        dcp = NULL;
    return left_running ? -1 : 0;
}
static int display_configure(const char *cfg)
{
    (void)cfg;
    configures++;
    dcp = &dummy_dcp;
    iboot = &dummy_iboot;
    fb_dva = 0x10000004000UL;
    fb_size = 0x854000;
    fb_dva_mapped = true;
    return configure_ret;
}
''' + extract_fn(disp, "static int display_init_t8132_hdmi(") + r'''
static void reset(void)
{
    dcp = iboot = NULL;
    fb_dva = fb_size = 0;
    display_is_external = fb_dva_mapped = left_running = false;
    configures = unmaps = shutdowns = 0;
    configure_ret = 1;
}
int main(void)
{
    /* 1. Real non-dummy framebuffer is adopted untouched (Stage 2 or iBoot HDMI). */
    reset();
    cur_boot_args.video = (typeof(cur_boot_args.video)){0x105e5300000UL, 1920, 1080, 32};
    assert(display_init_t8132_hdmi() == 0);
    assert(!configures && !unmaps && !shutdowns);

    /* 2. Dummy 640x1136 framebuffer configures HDMI cleanly. */
    reset();
    cur_boot_args.video = (typeof(cur_boot_args.video)){0x105e5300000UL, 640, 1136, 32};
    assert(display_init_t8132_hdmi() == 1);
    assert(display_is_external && configures == 1 && !unmaps && !shutdowns);

    /* 3. No monitor connected (ret == 0) -> unmap FB and quiesce DCP cleanly. */
    reset();
    configure_ret = 0;
    assert(display_init_t8132_hdmi() == 0);
    assert(configures == 1 && unmaps == 1 && shutdowns == 1 && !dcp && !iboot && !fb_dva_mapped);

    /* 4. Shutdown failure (dcp_left_running() == true) keeps dcp pointer non-NULL. */
    reset();
    configure_ret = -1;
    left_running = true;
    assert(display_init_t8132_hdmi() == -1);
    assert(configures == 1 && unmaps == 1 && shutdowns == 1 && dcp == &dummy_dcp && !iboot);

    puts("PASS t8132 display init");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "t8132-display-init", harness)
    assert "PASS t8132 display init" in out


def test_t8132_dcp_init_skips_protected_cpu_power_enable(tmp_path):
    """Verify dcp_init skips pmgr_adt_power_enable(cfg->dcp) when cfg->pmgr_dev is protected."""
    dcp_c = (REPO / "src/dcp.c").read_text()
    fn = extract_fn(dcp_c, "dcp_dev_t *dcp_init(")
    # Truncate right after the dptx_phy power-on block so we test the exact gating logic
    # verbatim without pulling in DART/RTKit/IOMFB symbols.
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
enum { V13_5 = 1, V27_2 = 2 };
static struct { int version; } os_firmware = { V27_2 };
static void *adt = (void *)1;
#define T8152 0x8152
static u32 chip_id = 0x8132;
static char dcp_pmgr_dev[32];
static int dcp_die;
static bool is_protected;
static int dcp_power_calls, phy_power_calls;
static int adt_path_offset(void *a, const char *p) { (void)a; (void)p; return 1; }
static bool adt_is_compatible(void *a, int n, const char *c) { (void)a; (void)n; return strcmp(c, "dptx-phy,t8132") == 0; }
static bool pmgr_device_is_protected(const char *name) { (void)name; return is_protected; }
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
    /* Protected CPU (e.g. T8132 DISPEXT0_CPU reading target=15, actual=0): must skip dcp power-on, still power PHY. */
    is_protected = true;
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) != NULL);
    assert(dcp_power_calls == 0 && phy_power_calls == 1);

    /* Unprotected CPU: must power both dcp and PHY. */
    is_protected = false;
    dcp_power_calls = phy_power_calls = 0;
    assert(dcp_init(&cfg) != NULL);
    assert(dcp_power_calls == 1 && phy_power_calls == 1);

    puts("PASS t8132 dcp_init protected cpu");
    return 0;
}
'''
    out = compile_and_run(tmp_path, "t8132-dcp-init-protected", harness)
    assert "PASS t8132 dcp_init protected cpu" in out
