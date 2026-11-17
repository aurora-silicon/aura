# SPDX-License-Identifier: MIT
"""Host test of the T8152 (J804) Windows carrier safety policy, src/hv/windows/hv_t8152.c.

The production file is compiled unchanged against small stand-in headers (a fake ADT,
fake system registers, a fake stage-2 leaf walk) and exercised for every rule the
carrier relies on: the preflight refusals, the guest RAM window checks, the map-time
filter (denylist, allowlist, Device-nGnRnE), the HCR filter and the pre-entry walk.
All addresses are synthetic; nothing here is read from a real machine.
"""

from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]

STUBS = {
    "types.h": r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned long u64; /* as in aura (LP64): printf %lx */
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef long s64;
""",
    "utils.h": r"""
#pragma once
#include <stdio.h>
#include "types.h"
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzl(m))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzl(m)) & (m))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HCR_VM BIT(0)
#define HCR_TWI BIT(13)
#define HCR_TWE BIT(14)
#define HCR_TSC BIT(19)
#define HCR_FWB BIT(46)
extern u64 fake_mpidr, fake_hcr, fake_cntfrq, fake_actlr12;
#define mrs(r) fake_mrs_##r
#define msr(r, v) fake_msr_##r(v)
#define fake_mrs_MPIDR_EL1 fake_mpidr
#define fake_mrs_HCR_EL2 fake_hcr
#define fake_mrs_CNTFRQ_EL0 fake_cntfrq
/* Only the architectural ACTLR_EL12 exists here: an access to any other ACTLR encoding
 * (the implementation-defined alias that UNDEFs on T8152) fails to compile. */
#define fake_mrs_SYS_ACTLR_EL12 fake_actlr12
void fake_msr_SYS_ACTLR_EL12(u64 value);
extern u32 board_id, chip_id;
extern char *fake_base, *fake_rodata_end;
#define _base fake_base
#define _rodata_end fake_rodata_end
""",
    "soc.h": "#pragma once\n#define T8142 0x8142\n#define T8152 0x8152\n",
    "string.h": "#pragma once\n#include <string.h>\n",
    "memory.h": "#pragma once\n",
    "smp.h": "#pragma once\n#include \"types.h\"\n#define MAX_CPUS 32\n"
             "extern int boot_cpu_idx;\nbool smp_is_alive(int cpu);\n",
    "heapblock.h": "#pragma once\n#include \"types.h\"\nuintptr_t heapblock_high_water(void);\n"
                   "uintptr_t heapblock_limit(void);\n",
    "ctxr_exec.h": r"""
#pragma once
#include "types.h"
enum ctxr_range { CTXR_RANGE_A, CTXR_RANGE_B, CTXR_RANGE_C, CTXR_RANGE_D, CTRR_RANGE_C,
                  CTRR_RANGE_D };
bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end);
""",
    "aic.h": r"""
#pragma once
#include "types.h"
struct aic { u64 base; u32 version; u32 nr_irq; };
extern struct aic *aic;
""",
    "xnuboot.h": r"""
#pragma once
#include "types.h"
struct boot_video { u64 base, display, stride, width, height, depth; };
struct boot_args { u64 phys_base, virt_base, mem_size; void *devtree; u32 devtree_size;
                   struct boot_video video; };
extern struct boot_args cur_boot_args;
""",
    "hv.h": r"""
#pragma once
#include "types.h"
#define ENABLE_NATIVE_AIC_PASSTHROUGH
typedef bool(hv_pt_leaf_fn)(void *ctx, u64 ipa, u64 size, u64 pte, int level);
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx);
""",
    "adt.h": r"""
#pragma once
#include "types.h"
struct adt_property { char name[32]; u32 size; u8 value[]; };
extern void *adt;
#define ADT_PROP(adt, offset) ((const struct adt_property *)(((u8 *)(adt)) + (offset)))
int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
const char *adt_get_name(const void *adt, int nodeoffset);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
int adt_get_child_count(const void *adt, int offset);
int adt_first_child_offset(const void *adt, int offset);
int adt_next_sibling_offset(const void *adt, int offset);
int adt_get_property_count(const void *adt, int offset);
int adt_first_property_offset(const void *adt, int offset);
int adt_next_property_offset(const void *adt, int offset);
#define ADT_FOREACH_CHILD(adt, node)                                                          \
    for (int _child_count = adt_get_child_count(adt, node); _child_count; _child_count = 0) \
        for (node = adt_first_child_offset(adt, node); _child_count--;                      \
             node = adt_next_sibling_offset(adt, node))
#define ADT_FOREACH_PROPERTY(adt, node, prop)                                                \
    for (int _prop_count = adt_get_property_count(adt, node),                               \
             _poff = adt_first_property_offset(adt, node);                                  \
         _prop_count; _prop_count = 0)                                                       \
        for (const struct adt_property *prop = ADT_PROP(adt, _poff); _prop_count--;          \
             prop = ADT_PROP(adt, _poff = adt_next_property_offset(adt, _poff)))
""",
}

HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include "adt.h"
#include "aic.h"
#include "ctxr_exec.h"
#include "hv.h"
#include "hv_t8152.h"
#include "xnuboot.h"
#include "utils.h"
#include "string.h"

/* Synthetic machine: 4 GiB of RAM at 0x10000000000. */
#define DRAM      0x10000000000UL
#define DRAM_SIZE 0x100000000UL
#define AURA      0x10008000000UL
#define CTXR_D_LO 0x10008000000UL
#define CTXR_D_HI 0x10010000000UL
#define HEAP_TOP  0x10012000000UL
#define HEAP_LIM  0x10018000000UL /* aura's heap may grow to here */
#define HOST_TOP  0x10020000000UL /* the launcher's heap, above the limit */
#define MTE       0x100f0000000UL
#define WIN_LO    0x10040000000UL
#define WIN_HI    0x100f0000000UL
#define AIC_BASE  0x2a0000000UL
#define AIC_SIZE  0x100000UL
#define QSPI_ADT  0x2b0000000UL
#define UART0     0x2c0000000UL
#define J804_QSPI 0x331118000UL
#define PMGR      0x2d0000000UL /* a node with more than 32 regs, like J804 pmgr-child */
#define S2_HW     (1UL | (1UL << 10) | (3UL << 8) | (3UL << 6) | (0xfUL << 2))
#define S2_RO     (1UL | (1UL << 10) | (3UL << 8) | (1UL << 6) | (0xfUL << 2))
#define S2_HOOK   (1UL << 50)
#define S2_PROXY  (2UL << 50)

u64 fake_mpidr = 0x10100, fake_hcr = 0x80001UL, fake_cntfrq = 1000000000;
u64 fake_actlr12 = 0xc00; /* the EL1 view Part C (h) read on every core type */
static int actlr12_writes;
void fake_msr_SYS_ACTLR_EL12(u64 value) { fake_actlr12 = value; actlr12_writes++; }
u32 board_id = 0x22, chip_id = 0x8152;
char *fake_base = (char *)AURA, *fake_rodata_end = (char *)(AURA + 0x120000);
int boot_cpu_idx = 6;
static unsigned long alive_mask = 0xfbf; /* the 11 secondaries; cpu6 boots */
bool smp_is_alive(int cpu) { return cpu >= 0 && cpu < 32 && (alive_mask >> cpu) & 1; }
struct aic fake_aic = {AIC_BASE, 3, 0x960}, *aic = &fake_aic;
struct boot_args cur_boot_args;
static bool ctxr_ok = true, qspi_present = true;
static u64 ctxr_hi = CTXR_D_HI;
static int pmgr_regs = 40;
static u64 heap_lim = HEAP_LIM;

uintptr_t heapblock_high_water(void) { return HEAP_TOP; }
uintptr_t heapblock_limit(void) { return heap_lim; }
bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end)
{
    if (range != CTXR_RANGE_D || !ctxr_ok)
        return false;
    *start = CTXR_D_LO;
    *end = ctxr_hi;
    return true;
}

/* Fake ADT: node ids are small integers; properties live in a byte buffer. */
enum { N_ROOT = 1, N_ARMIO, N_QSPI, N_SEP, N_WDT, N_AIC, N_I2C3, N_UART0, N_ADMAC, N_PMGR, N_CO,
       N_MM };
static const char *names[] = {"", "", "arm-io", "qspi", "sep", "wdt", "aic", "i2c3", "uart0",
                              "admac-base-ns", "pmgr-child", "carveout-memory-map", "memory-map"};
static const u64 regs[][2] = {
    [N_QSPI] = {QSPI_ADT, 0x8000},   [N_SEP] = {0x2a8000000, 0x74000},
    [N_WDT] = {0x2b8000000, 0x4000}, [N_AIC] = {AIC_BASE, AIC_SIZE},
    [N_I2C3] = {0x2b4000000, 0x4000}, [N_UART0] = {UART0, 0x4000},
    [N_ADMAC] = {0x2b6000000, 0x44000},
};
static u8 propbuf[4096];
void *adt = propbuf;
static int co_first, co_count, mm_first, mm_count, prop_end = 16;
static int add_prop(const char *name, u64 base, u64 size)
{
    int off = prop_end;
    struct adt_property *p = (struct adt_property *)(propbuf + off);
    strncpy(p->name, name, sizeof(p->name));
    p->size = 16;
    memcpy(p->value, &base, 8);
    memcpy(p->value + 8, &size, 8);
    prop_end += sizeof(*p) + 16;
    return off;
}
int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io")) return N_ARMIO;
    if (!strcmp(path, "/chosen/carveout-memory-map")) return N_CO;
    if (!strcmp(path, "/chosen/memory-map")) return N_MM;
    return -1;
}
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    if (strcmp(path, "/arm-io/aic")) return -1;
    offsets[0] = N_ARMIO; offsets[1] = N_AIC; offsets[2] = 0;
    return N_AIC;
}
const char *adt_get_name(const void *a, int n) { (void)a; return names[n]; }
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)prop;
    if (path[2]) return -1;
    if (path[1] == N_PMGR) {
        if (idx >= pmgr_regs) return -1;
        /* Past 40, empty entries: a long node must hit the walk's cap, not the denylist's. */
        *addr = PMGR + (u64)idx * 0x10000; *size = idx < 40 ? 0x4000 : 0;
        return 0;
    }
    if (idx) return -1;
    *addr = regs[path[1]][0]; *size = regs[path[1]][1];
    return 0;
}
int adt_get_child_count(const void *a, int n) { (void)a; return n == N_ARMIO ? (qspi_present ? 8 : 7) : 0; }
int adt_first_child_offset(const void *a, int n) { (void)a; (void)n; return qspi_present ? N_QSPI : N_SEP; }
int adt_next_sibling_offset(const void *a, int n) { (void)a; return n + 1; }
int adt_get_property_count(const void *a, int n) { (void)a; return n == N_CO ? co_count : n == N_MM ? mm_count : 0; }
int adt_first_property_offset(const void *a, int n) { (void)a; return n == N_CO ? co_first : mm_first; }
int adt_next_property_offset(const void *a, int off) { (void)a; return off + sizeof(struct adt_property) + 16; }
const void *adt_getprop(const void *a, int n, const char *name, u32 *len)
{
    int count = adt_get_property_count(a, n), off = adt_first_property_offset(a, n);
    for (int i = 0; i < count; i++, off = adt_next_property_offset(a, off)) {
        const struct adt_property *p = ADT_PROP(a, off);
        if (!strcmp(p->name, name)) { *len = p->size; return p->value; }
    }
    return NULL;
}

/* Fake stage 2: a list of leaves. */
struct leaf { u64 ipa, size, pte; int level; };
static struct leaf leaves[16];
static int nleaves;
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    for (int i = 0; i < nleaves; i++)
        if (!fn(ctx, leaves[i].ipa, leaves[i].size, leaves[i].pte, leaves[i].level))
            return;
}

static void build_adt(void)
{
    prop_end = 16;
    co_first = prop_end;
    add_prop("region-id-24", DRAM, DRAM_SIZE);         /* the usable RAM itself */
    add_prop("region-id-353", 0x10006000000, 0x1e000000); /* aura and its heaps */
    add_prop("region-id-60", 0x10030000000, 0x4000000);
    add_prop("region-id-218", MTE, DRAM + DRAM_SIZE - MTE);
    add_prop("region-id-4", DRAM + DRAM_SIZE, 0x2000000);
    co_count = 5;
    mm_first = prop_end;
    add_prop("BootArgs", 0x10007000000, 0x4000);
    mm_count = 1;
}

static void boot(void)
{
    cur_boot_args.phys_base = DRAM;
    cur_boot_args.mem_size = DRAM_SIZE;
    cur_boot_args.virt_base = 0xfffffe0000000000UL;
    cur_boot_args.devtree = (void *)(0xfffffe0000000000UL + 0x7004000UL);
    cur_boot_args.devtree_size = 0x80000;
    cur_boot_args.video.base = 0x10180000000UL;
    cur_boot_args.video.stride = 0x4000;
    cur_boot_args.video.height = 0x400;
    build_adt();
}

static u64 map(u64 ipa, u64 to, u64 size)
{
    u64 v = to;
    return hv_t8152_map_filter(ipa, &v, size) ? v : 0xdead;
}

int main(void)
{
    boot();
    /* Not armed: the filter is transparent and every entry is refused. */
    assert(map(J804_QSPI, J804_QSPI | S2_HW, 0x4000) == (J804_QSPI | S2_HW));
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) < 0);

    /* Preflight refusals, one at a time. */
    ctxr_ok = false;
    assert(hv_t8152_preflight() < 0 && !hv_t8152_policy_armed());
    ctxr_ok = true;
    board_id = 0x24;
    assert(hv_t8152_preflight() < 0);
    board_id = 0x22;
    fake_mpidr = 0x0;
    assert(hv_t8152_preflight() < 0);
    fake_mpidr = 0x10100;
    qspi_present = false;
    assert(hv_t8152_preflight() < 0);
    qspi_present = true;
    fake_hcr |= HCR_FWB;
    assert(hv_t8152_preflight() < 0);
    fake_hcr &= ~HCR_FWB;
    aic = NULL;
    assert(hv_t8152_preflight() < 0);
    aic = &fake_aic;
    pmgr_regs = 1025; /* more reg entries than the walk takes: refuse, never truncate */
    assert(hv_t8152_preflight() < 0);
    pmgr_regs = 40;
    /* B3: all 12 CPUs live in aura's dispatch loop, nothing else. */
    puts("LIVE CPU CHECK");
    alive_mask = 0xfbf & ~BIT(9);   /* one M core missing */
    assert(hv_t8152_preflight() < 0 && !hv_t8152_policy_armed());
    alive_mask = 0;                 /* secondaries never started: only the boot CPU */
    assert(hv_t8152_preflight() < 0);
    alive_mask = 0xfbf | BIT(12);   /* a spin-table entry past cpu11 */
    assert(hv_t8152_preflight() < 0);
    alive_mask = 0xfbf;
    /*
     * Part A: EL2 on the secondaries faulted on the last page of the CTXR exec window
     * (an address size fault at level 2, cause unknown).  Aura's code must end at or below
     * the exec window base, ALIGN_UP(AURA + 16 MiB, 2 MiB) = 0x10009000000, and below the
     * 32 MiB block holding CTXR_D's last page (0x1000e000000 here).
     */
    puts("CODE PLACEMENT CHECK");
    fake_rodata_end = (char *)(0x10009000000UL + 0x10);  /* runs into the exec window */
    assert(hv_t8152_preflight() < 0);
    fake_rodata_end = (char *)(CTXR_D_HI + 0x10000);     /* runs past CTXR_D */
    assert(hv_t8152_preflight() < 0);
    fake_rodata_end = (char *)(CTXR_D_HI - 0x4000 + 0x10); /* the old rule's limit: refused now */
    assert(hv_t8152_preflight() < 0);
    fake_rodata_end = (char *)0x10009000000UL;           /* ends exactly at the window base */
    assert(hv_t8152_preflight() == 0);
    fake_rodata_end = (char *)(AURA + 0x120000);
    ctxr_hi = 0x1000a001000UL; /* small CTXR_D, UPR + 4 KiB unaligned: last page 0x10009ffc000 */
    assert(hv_t8152_preflight() < 0);  /* its 32 MiB block starts at aura's base */
    ctxr_hi = CTXR_D_HI;
    assert(hv_t8152_preflight() == 0 && hv_t8152_policy_armed());

    /* Guest RAM window. */
    assert(hv_t8152_set_guest_ram(WIN_LO + 0x4000, 0x40000000, HOST_TOP) < 0);  /* not 64 KiB aligned */
    assert(hv_t8152_set_guest_ram(WIN_LO, 0x1000000, HOST_TOP) < 0);             /* below 256 MiB */
    assert(hv_t8152_set_guest_ram(0x10020000000, 0x20000000, HOST_TOP) < 0);     /* region-id-353 */
    assert(hv_t8152_set_guest_ram(0x10028000000, 0x10000000, HOST_TOP) < 0);     /* region-id-60 */
    assert(hv_t8152_set_guest_ram(WIN_LO, MTE - WIN_LO + 0x10000, HOST_TOP) < 0);/* MTE tag storage */
    assert(hv_t8152_set_guest_ram(0x10008000000, 0x10000000, HOST_TOP) < 0);     /* aura / CTXR_D */
    assert(hv_t8152_set_guest_ram(WIN_LO, DRAM + DRAM_SIZE + 0x10000 - WIN_LO, HOST_TOP) < 0); /* past RAM */
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, 0) < 0);          /* no host heap top */
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, WIN_LO + 0x10000) < 0); /* host heap */
    heap_lim = 0;
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) < 0);   /* unbounded heap */
    heap_lim = WIN_LO + 0x4000;
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) < 0);   /* heap limit */
    heap_lim = HEAP_LIM;
    u64 b, s;
    assert(!hv_t8152_guest_ram(&b, &s));
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) == 0);
    assert(hv_t8152_guest_ram(&b, &s) && b == WIN_LO && s == WIN_HI - WIN_LO);
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) < 0);       /* once per session */

    /* Map-time filter. */
    assert(map(J804_QSPI, J804_QSPI | S2_HW, 0x4000) == 0xdead);        /* fixed NVRAM copy */
    assert(map(QSPI_ADT, QSPI_ADT | S2_HW, 0x4000) == 0xdead);          /* ADT qspi */
    assert(map(0x2b4000000, 0x2b4000000 | S2_HW, 0x4000) == 0xdead);    /* i2c3 (amps) */
    assert(map(0x2b6000000, 0x2b6000000 | S2_HW, 0x4000) == 0xdead);    /* admac */
    assert(map(0x2a8000000, 0x2a8000000 | S2_HW, 0x4000) == 0xdead);    /* sep */
    assert(map(0x2b8000000, 0x2b8000000 | S2_HW, 0x4000) == 0xdead);    /* wdt */
    assert(map(UART0, UART0 | S2_HW, 0x4000) == 0xdead);                /* not allowlisted */
    assert(map(MTE, MTE | S2_HW, 0x4000) == 0xdead);                    /* MTE tag storage */
    assert(map(AURA, AURA | S2_HW, 0x4000) == 0xdead);                  /* aura */
    assert(map(WIN_LO, (WIN_LO + 0x4000) | S2_HW, 0x4000) == 0xdead);   /* not identity */
    assert(map(WIN_HI - 0x4000, (WIN_HI - 0x4000) | S2_HW, 0x8000) == 0xdead); /* runs past */
    assert(map(WIN_LO, WIN_LO | S2_HW, WIN_HI - WIN_LO) == (WIN_LO | S2_HW));
    u64 aic_pte = map(AIC_BASE, AIC_BASE | S2_HW, AIC_SIZE);
    assert(aic_pte != 0xdead && ((aic_pte >> 2) & 0xf) == 0);            /* Device-nGnRnE */
    assert(map(0x2c0001000, (u64)0x1234 | S2_HOOK, 0x1000) == ((u64)0x1234 | S2_HOOK));
    assert(map(J804_QSPI, (7UL << 2) | S2_PROXY, 0x4000) == 0xdead);              /* host hook on NVRAM */
    assert(map(UART0, (7UL << 2) | S2_PROXY, 0x4000) == 0xdead);   /* any host hook: PA = IPA */
    assert(map(0x3f0000000, (7UL << 2) | S2_PROXY, 0x4000) == 0xdead);
    /* Reg entries past the 32nd still reach the denylist (J804 pmgr-child has 64). */
    puts("PMGR TAIL CHECK");
    assert(map(PMGR + 34 * 0x10000, (PMGR + 34 * 0x10000) | S2_HW, 0x4000) == 0xdead);
    assert(map(J804_QSPI, 0, 0x4000) == 0);                               /* unmap always ok */
    assert(map(J804_QSPI, J804_QSPI, 0x4000) == 0xdead);                 /* sw map onto NVRAM */

    /* Exceptions: inside RAM, never the denylist, read-only where asked. */
    assert(hv_t8152_allow(MTE, MTE, 0x4000, false, "bad") < 0);
    assert(hv_t8152_allow(J804_QSPI, J804_QSPI, 0x4000, false, "bad") < 0);
    assert(hv_t8152_allow(0x10011000000, 0x10011000000, 0x8000, false, "shadow") == 0);
    assert(hv_t8152_allow(0x5300000000, 0x10007004000, 0x80000, true, "adt") == 0);
    assert(map(0x10011000000, 0x10011000000 | S2_HW, 0x8000) != 0xdead);
    assert(map(0x5300000000, 0x10007004000 | S2_HW, 0x80000) == 0xdead); /* RO mapped RW */
    assert(map(0x5300000000, 0x10007004000 | S2_RO, 0x80000) != 0xdead);
    assert(map(0x5300004000, 0x10007004000 | S2_RO, 0x4000) == 0xdead);  /* wrong IPA */

    /* HCR filter. */
    assert(hv_t8152_hcr_filter(HCR_FWB) == (HCR_TWI | HCR_TWE));

    /* Pre-entry walk. */
    u64 hcr_ok = HCR_VM | HCR_TSC | HCR_TWI | HCR_TWE;
    fake_hcr = hcr_ok;
    leaves[nleaves++] = (struct leaf){WIN_LO, 0x2000000, WIN_LO | S2_HW | 2, 2};
    leaves[nleaves++] = (struct leaf){AIC_BASE, 0x4000, AIC_BASE | (S2_HW & ~(0xfUL << 2)) | 2, 3};
    leaves[nleaves++] = (struct leaf){0x10011000000, 0x4000, 0x10011000000 | S2_HW | 2, 3};
    leaves[nleaves++] = (struct leaf){0x5300000000, 0x4000, 0x10007004000 | S2_RO | 2, 3};
    leaves[nleaves++] = (struct leaf){UART0, 4, 0x1234 | S2_HOOK | 2, 4};
    assert(hv_t8152_verify(WIN_LO + 0x1000800, WIN_LO + 0x2000000, hcr_ok, true) == 0);
    assert(hv_t8152_verify(AURA, WIN_LO + 0x2000000, hcr_ok, true) < 0);       /* entry */
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, false) < 0);        /* no shadow */
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok & ~HCR_TWI, true) < 0); /* TWI */
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok & ~HCR_TWE, true) < 0); /* TWE */
    fake_hcr = hcr_ok & ~HCR_TWE;
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);
    fake_hcr = hcr_ok;
    fake_hcr = hcr_ok | HCR_FWB;
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);
    fake_hcr = hcr_ok;
    leaves[nleaves++] = (struct leaf){AIC_BASE + 0x4000, 0x4000, (AIC_BASE + 0x4000) | S2_HW | 2, 3};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);         /* AIC normal */
    leaves[nleaves - 1] = (struct leaf){J804_QSPI, 0x4000, J804_QSPI | S2_HW | 2, 3};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);         /* NVRAM */
    leaves[nleaves - 1] = (struct leaf){J804_QSPI, 4, (9UL << 2) | S2_PROXY, 4};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);         /* host hook */
    leaves[nleaves - 1] = (struct leaf){UART0, 4, (9UL << 2) | S2_PROXY, 4};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);         /* any host hook */
    leaves[nleaves - 1] = (struct leaf){0x2000000000, 0x2000000, 0x10006000000 | S2_HW | 2, 2};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);         /* aura RAM */
    nleaves--;
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) == 0);
    alive_mask = 0xfbf & ~BIT(0);   /* a CPU left the spin table after hv_init() */
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) < 0);
    alive_mask = 0xfbf;
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO, hcr_ok, true) == 0);

    /*
     * B2, Part C (h): the guest's ACTLR_EL1 goes through ACTLR_EL12 only.  The write of
     * that register is not yet measured on the M6, so restoring the value it already holds
     * must issue no write at all; a different value must be written exactly once.
     */
    puts("ACTLR CHECK");
    assert(hv_t8152_guest_actlr_read() == 0xc00);
    hv_t8152_guest_actlr_write(0xc00);
    assert(actlr12_writes == 0 && fake_actlr12 == 0xc00);
    hv_t8152_guest_actlr_write(0xc80);
    assert(actlr12_writes == 1 && fake_actlr12 == 0xc80);
    assert(hv_t8152_guest_actlr_read() == 0xc80);
    hv_t8152_guest_actlr_write(0xc80);
    assert(actlr12_writes == 1);
    puts("T8152 POLICY OK");
    return 0;
}
"""


def test_t8152_carrier_policy(tmp_path):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler")
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    for name, text in STUBS.items():
        (stubs / name).write_text(text)
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    exe = tmp_path / "policy"
    # Compile byte-identical copies next to the stand-ins, so that the production
    # file's own "hv.h" resolves to the stub rather than the carrier's header.
    for name in ("hv_t8152.c", "hv_t8152.h"):
        shutil.copyfile(REPO / "src/hv/windows" / name, stubs / name)
    source = stubs / "hv_t8152.c"
    cmd = [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-parameter", "-iquote", str(stubs), str(source), str(harness),
           "-o", str(exe)]
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert "T8152 POLICY OK" in result.stdout
    assert "ACTLR CHECK" in result.stdout
    # Every refusal says why.
    assert "CTXR_D is not armed" in result.stdout
    assert "is not J804" in result.stdout
    assert "lacks /arm-io/qspi" in result.stdout
    assert "refusing stage-2 map" in result.stdout
    assert "region-id-218 (MTE tag storage)" in result.stdout
    assert "qspi NOR/NVRAM (J804 fixed)" in result.stdout
    assert "MMIO passthrough must be Device-nGnRnE" in result.stdout
    assert "stage 2 verified" in result.stdout
    # Findings from review: no truncated reg walk, no host hooks, the whole heap span.
    assert "pmgr-child has more than 1024 reg entries" in result.stdout
    tail = result.stdout.split("PMGR TAIL CHECK", 1)[1].splitlines()[1]
    assert tail.endswith(": pmgr-child"), tail
    assert "refusing a host hook at IPA 0x2c0000000" in result.stdout
    assert "host (proxy) hook: the host would access PA = IPA" in result.stdout
    assert "did not report the top of its heap" in result.stdout
    assert "heap has no limit" in result.stdout
    assert "overlaps aura and its heaps" in result.stdout
    # B3 and Part A: the 12-live-CPU refusal and aura's code placement.
    live = result.stdout.split("LIVE CPU CHECK", 1)[1]
    place = live.split("CODE PLACEMENT CHECK", 1)[1]
    assert "only 11 of 12 CPUs are live in aura's dispatch loop (not live: cpu9)" in live
    assert "only 1 of 12 CPUs are live" in live
    assert "(not live: cpu0 cpu1 cpu2 cpu3 cpu4 cpu5 cpu7 cpu8 cpu9 cpu10 cpu11)" in live
    assert "1 spin-table entries beyond cpu11 are live" in live
    assert "a second start does nothing, reload the carrier" in live
    assert ("aura's code [0x10008000000, 0x10009004000) must lie inside CTXR_D "
            "[0x10008000000, 0x10010000000) and end at or below 0x10009000000: the CTXR exec "
            "window base 0x10009000000") in place
    assert "the 32 MiB block 0x1000e000000 that holds CTXR_D's last page" in place
    assert ("end at or below 0x10008000000: the CTXR exec window base 0x10009000000 "
            "(EL2 on the secondaries faulted on the window's last page in Part A) and the "
            "32 MiB block 0x10008000000") in place
    assert "code ends 0x10009000000 at or below 0x10009000000" in place
    assert "all 12 CPUs live in aura's dispatch loop" in live
    assert "only 11 of 12 CPUs are live in aura's dispatch loop (not live: cpu0)" in live


def test_exec_window_constants_match_ctxr_exec():
    policy = (REPO / "src/hv/windows/hv_t8152.c").read_text()
    ctxr = (REPO / "src/ctxr_exec.c").read_text()
    assert "#define T8152_EXEC_SLOT  (16UL << 20)" in policy
    assert "#define T8152_EXEC_ALIGN (2UL << 20)" in policy
    assert "#define CTXR_EXEC_AURA_SLOT   ((u64)16 * SZ_1M)" in ctxr
    assert "#define CTXR_EXEC_ALIGN       ((u64)2 * SZ_1M)" in ctxr


def test_t8152_guest_actlr_is_architectural_only():
    """B2, Part C (h): on T8152 a write to the implementation-defined ACTLR_EL12 alias UNDEFs
    at EL2 on every core type.  The M6 CPU parts select the architectural ACTLR_EL12, and the
    carrier's only alias accesses sit behind a T8152 early return."""
    chickens = (REPO / "src/chickens.c").read_text()
    m6 = chickens.split("const struct midr_part_features features_m6 = {", 1)[1].split("};", 1)[0]
    assert ".actlr_el2 = true," in m6
    windows = REPO / "src/hv/windows"
    for path in sorted(windows.glob("*.[ch]")):
        text = path.read_text()
        if path.name == "hv.c":
            continue
        assert "SYS_IMP_APL_ACTLR_EL12" not in text, path.name
        assert "SYS_ACTLR_EL12" not in text or path.name == "hv_t8152.c", path.name
    hv = (windows / "hv.c").read_text()
    assert hv.count("SYS_IMP_APL_ACTLR_EL12") == 2
    for name in ("u64 hv_guest_actlr_read(void)\n{", "void hv_guest_actlr_write(u64 value)\n{"):
        body = hv.split(name, 1)[1].split("\n}\n", 1)[0]
        assert body.lstrip().startswith("if (chip_id == T8152)"), name
        assert "SYS_IMP_APL_ACTLR_EL12" in body
    assert hv.count("hv_guest_actlr_read()") >= 1 and hv.count("hv_guest_actlr_write(") >= 2
