# SPDX-License-Identifier: MIT
"""An external display processor is promised to the kernel only if its DARTs can be kept."""
from pathlib import Path
import subprocess
from test_locked_dart_tables import function
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned long u64;typedef uint32_t u32;typedef uint16_t u16;typedef uint8_t u8;
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) (((~0UL)>>(63-(h)))&((~0UL)<<(l)))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctzl(m))
#define FIELD_PREP(m,v) (((u64)(v)<<__builtin_ctzl(m))&(m))
#define SZ_16K 16384
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
'''
# Two fake DARTs at these "MMIO" bases; 16 streams each.
MMIO = r'''
#define DART_A 0x319304000UL
#define DART_B 0x31930c000UL
static u32 protect[2], tcr[2][16], ttbr[2][16], sids = 0x00100010;
static int faulting = -1;
static int exc_count;
static u32 read32(uintptr_t a) {
 int d = (a & ~0x3fffUL) == DART_A ? 0 : (a & ~0x3fffUL) == DART_B ? 1 : -1;
 uintptr_t off = a & 0x3fff;
 if (d < 0) return psreg_value(a);
 if (d == faulting) exc_count++;
 if (off == DART_T8110_PROTECT) return protect[d];
 if (off == DART_T8110_PARAMS4) return sids;
 if (off >= DART_T8110_TCR_OFF && off < DART_T8110_TCR_OFF + 0x400) {
  unsigned s = (off - DART_T8110_TCR_OFF) / 4; return s < 16 ? tcr[d][s] : 0; }
 if (off >= DART_T8110_TTBR_OFF && off < DART_T8110_TTBR_OFF + 0x400) {
  unsigned s = (off - DART_T8110_TTBR_OFF) / 4; return s < 16 ? ttbr[d][s] : 0; }
 return 0;
}
'''


def dart_macros():
    s = (ROOT / "src/dart.c").read_text()
    return s, '\n'.join(l for l in s.splitlines()
                        if l.startswith('#define DART_') and not l.rstrip().endswith('\\'))


def test_dcpext_inherited_darts(tmp_path):
    dart, macros = dart_macros()
    kboot = (ROOT / "src/kboot.c").read_text()
    code = PRELUDE + r'''
#include "libfdt.h"
''' + macros + r'''
/* Power: the DARTs' pmgr power-state register (pmgr 0x2d0700000 + 0x488). */
#define PSREG 0x2d0700488UL
static u32 ps_state = 0xff;
static u32 psreg_value(uintptr_t a) { assert(a == PSREG); return ps_state; }
''' + MMIO + r'''
#define GUARD_MARK 1
#define GUARD_SILENT 2
static int exc_guard;
#define sysop(x) do { } while (0)
#define ADT_GETPROP(a, n, k, v) adt_value(k, v)
static void *dt, *adt;
static u64 dram_base, dram_size = 4 * SZ_16K;
static int adt_path_offset(void *t, const char *p) { (void)t; return strcmp(p, "/chosen") ? -1 : 1; }
static int adt_value(const char *k, u64 *v) {
 if (!strcmp(k, "dram-base")) { *v = dram_base; return 8; }
 if (!strcmp(k, "dram-size")) { *v = dram_size; return 8; }
 return -1;
}
static u64 dt_get_address(void *t, int node) {
 (void)t; int len; const fdt64_t *r = fdt_getprop(dt, node, "reg", &len);
 return r && len >= 8 ? fdt64_ld(r) : 0;
}
static int reserved, next_fault;
/* An fdt walk that fails part way (not the end of the tree) must refuse. */
static int next_node(const void *t, int off, int *depth) {
 int n = fdt_next_node(t, off, depth);
 return next_fault && n >= 0 && *depth > 0 ? -FDT_ERR_BADSTRUCTURE : n;
}
static int dt_get_or_add_reserved_mem(const char *name, const char *compat, bool nomap, u64 pa, size_t sz) {
 (void)compat; (void)nomap; assert(sz == SZ_16K && !strncmp(name, "dcpext-tables@", 14));
 assert(pa >= dram_base && pa < dram_base + dram_size); reserved++; return 1;
}
''' + function(dart, 'dart_table_in_dram') + \
        function(dart, 'dart_collect_native_t8110') + function(dart, 'dart_collect_inherited_t8110') + \
        function(kboot, 'dt_node_power') + '\n#define fdt_next_node next_node\n' + function(kboot, 'dt_dcpext_inherited_darts') + r'''
static unsigned char blob[8192];
static u64 *root;
static void world(void) {
 memset(protect, 0, sizeof(protect)); memset(tcr, 0, sizeof(tcr)); memset(ttbr, 0, sizeof(ttbr));
 sids = 0x00100010; faulting = -1; exc_count = 0; reserved = 0; ps_state = 0xff; next_fault = 0;
 memset(root, 0, 4 * SZ_16K);
 assert(!fdt_create_empty_tree(blob, sizeof(blob))); dt = blob;
 int soc = fdt_add_subnode(dt, 0, "soc");
 int pm = fdt_add_subnode(dt, soc, "power-management@2d0700000");
 fdt64_t pr = cpu_to_fdt64(0x2d0700000UL);
 assert(pm >= 0 && !fdt_setprop(dt, pm, "reg", &pr, 8));
 int ps = fdt_add_subnode(dt, pm, "power-controller@488");
 fdt32_t psr[2] = {cpu_to_fdt32(0x488), cpu_to_fdt32(4)};
 assert(ps >= 0 && !fdt_setprop(dt, ps, "reg", psr, sizeof(psr)));
 assert(!fdt_setprop_string(dt, ps, "compatible", "apple,t8103-pmgr-pwrstate"));
 assert(!fdt_setprop_u32(dt, ps, "phandle", 50));
 soc = fdt_path_offset(dt, "/soc");
 const char *names[] = {"iommu@319304000", "iommu@31930c000"};
 for (int i = 0; i < 2; i++) {
  int d = fdt_add_subnode(dt, soc, names[i]);
  fdt64_t r = cpu_to_fdt64(i ? DART_B : DART_A);
  assert(d >= 0 && !fdt_setprop(dt, d, "reg", &r, 8));
  assert(!fdt_setprop_string(dt, d, "compatible", "apple,t8110-dart"));
  assert(!fdt_setprop_u32(dt, d, "phandle", i + 1));
  assert(!fdt_setprop_u32(dt, d, "power-domains", 50));
  assert(!fdt_setprop_u32(dt, d, "#iommu-cells", 1));
 }
 soc = fdt_path_offset(dt, "/soc");
 /* A node after the processor (libfdt adds a subnode first): it must not be inspected. */
 int other = fdt_add_subnode(dt, soc, "zz-other");
 fdt32_t bad[2] = {cpu_to_fdt32(99), cpu_to_fdt32(4)};
 assert(other >= 0 && !fdt_setprop(dt, other, "iommus", bad, sizeof(bad)));
 soc = fdt_path_offset(dt, "/soc");
 int ext = fdt_add_subnode(dt, soc, "dcp@31ac00000");
 fdt32_t io[2] = {cpu_to_fdt32(2), cpu_to_fdt32(5)};
 assert(!fdt_setprop(dt, ext, "iommus", io, sizeof(io)));
 int pio = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000"), "piodma");
 io[0] = cpu_to_fdt32(1); io[1] = cpu_to_fdt32(4);
 assert(!fdt_setprop(dt, pio, "iommus", io, sizeof(io)));
 int al = fdt_add_subnode(dt, 0, "aliases");
 assert(!fdt_setprop_string(dt, al, "dcpext", "/soc/dcp@31ac00000"));
}
static void lock_stream(int d, int sid) {
 protect[d] = 1; tcr[d][sid] = 1;
 ttbr[d][sid] = FIELD_PREP(DART_T8110_TTBR_ADDR, dram_base >> 14) | 1;
 root[0] = FIELD_PREP(DART_T6000_PTE_OFFSET, (dram_base + SZ_16K) >> 14) | 1;
}
int main(void) {
 root = MAP_FAILED;
 for (unsigned i = 0; i < 64; i++) {
  dram_base = 0x10000000000UL + ((u64)i << 28);
  void *p = mmap((void *)dram_base, 4 * SZ_16K, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == (void *)dram_base) { root = p; break; }
  if (p != MAP_FAILED) assert(!munmap(p, 4 * SZ_16K));
 }
 assert(root != MAP_FAILED);
 /* Unlocked DARTs: nothing to keep. */
 world(); assert(!dt_dcpext_inherited_darts("dcpext") && !reserved);
 /* Unlocked with a stream still translating, bypassing or remapping: refused. */
 world(); tcr[1][5] = 1; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); tcr[0][4] = DART_T8110_TCR_BYPASS_DART; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); tcr[0][4] = DART_T8110_TCR_REMAP_EN; assert(dt_dcpext_inherited_darts("dcpext"));
 /* DARTs whose power domain is off: no DART MMIO at all. */
 world(); ps_state = 0; faulting = 0; tcr[1][5] = 1; assert(!dt_dcpext_inherited_darts("dcpext") && !exc_count);
 /* A domain neither on nor off (e.g. clock-gated, state 4), or none described: refused. */
 world(); ps_state = 0x44; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/iommu@319304000"), "power-domains"));
 assert(dt_dcpext_inherited_darts("dcpext"));
 /* The walk fails before reaching piodma: refused, not accepted. */
 world(); next_fault = 1; assert(dt_dcpext_inherited_darts("dcpext"));
 /* Locked translating stream: its root and L2 are reserved. Both DARTs are read. */
 world(); lock_stream(0, 4); assert(!dt_dcpext_inherited_darts("dcpext") && reserved == 2);
 world(); lock_stream(1, 5); assert(!dt_dcpext_inherited_darts("dcpext") && reserved == 2);
 /* Locked, an enabled stream that bypasses or remaps, or has no root: refused. */
 world(); protect[1] = 1; tcr[1][5] = DART_T8110_TCR_BYPASS_DART; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); lock_stream(1, 5); tcr[1][5] |= DART_T8110_TCR_REMAP_EN; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); lock_stream(1, 5); ttbr[1][5] = 0; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); lock_stream(1, 5); tcr[1][9] = 1; /* a second live stream without tables */
 assert(dt_dcpext_inherited_darts("dcpext"));
 /* Locked without a stream count, an MMIO fault, a table outside DRAM: refused. */
 world(); protect[0] = 1; sids = 0; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); faulting = 1; assert(dt_dcpext_inherited_darts("dcpext"));
 world(); lock_stream(0, 4); dram_size = SZ_16K; assert(dt_dcpext_inherited_darts("dcpext"));
 dram_size = 4 * SZ_16K;
 /* A provider that is not a T8110 DART: refused. */
 world(); assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/iommu@319304000"), "compatible", "apple,t8103-dart"));
 assert(dt_dcpext_inherited_darts("dcpext"));
 /* A description that cannot be read whole fails closed. No iommus on the processor or piodma: */
 world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000"), "iommus"));
 assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000/piodma"), "iommus"));
 assert(dt_dcpext_inherited_darts("dcpext") && !reserved);
 /* ... only piodma's DART described still passes (one DART inspected). */
 world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000"), "iommus"));
 assert(!dt_dcpext_inherited_darts("dcpext"));
 /* An empty iommus, or one with a trailing cell: refused. */
 world(); assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000"), "iommus", NULL, 0));
 assert(dt_dcpext_inherited_darts("dcpext"));
 {
  fdt32_t three[3] = {cpu_to_fdt32(2), cpu_to_fdt32(5), cpu_to_fdt32(1)};
  world(); assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/dcp@31ac00000"), "iommus", three, sizeof(three)));
  assert(dt_dcpext_inherited_darts("dcpext"));
 }
 /* A provider without #iommu-cells, or with two: refused. */
 world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/iommu@319304000"), "#iommu-cells"));
 assert(dt_dcpext_inherited_darts("dcpext"));
 world(); assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/iommu@31930c000"), "#iommu-cells", 2));
 assert(dt_dcpext_inherited_darts("dcpext"));
 puts("PASS dcpext DART inspection");
 return 0;
}
'''
    run_c(tmp_path, "dcpext-darts", code)
