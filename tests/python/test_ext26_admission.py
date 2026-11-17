# SPDX-License-Identifier: MIT
"""Native26 opt-in and inherited page ownership, executing production helpers."""
from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def test_native26_admission(tmp_path):
    src = (ROOT / 'src/kboot.c').read_text()

    def function(name, kind):
        start = src.index('static ' + kind + ' ' + name + '(')
        return src[start:src.index('\n}', start) + 2]

    page_type = src[src.index('struct dt_ext26_pages {'):src.index('\n};', src.index('struct dt_ext26_pages {'))+3]
    adt = (ROOT / 'src/adt.h').read_text()
    start = adt.index('struct adt_segment_ranges {')
    seg_type = adt[start:adt.index('} PACKED;', start)+len('} PACKED;')]
    run_c(tmp_path, 'native26-admission', r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define SZ_16K 16384
#define ALIGN_UP(v,a) (((v)+(a)-1)&~((a)-1))
static void *dt;
static const char *ext26_step;
static char ext26_detail[256];
''' + seg_type + page_type + function('dt_ext26_page', 'int') +
          function('dt_t8122_dcpext_armed', 'bool') + function('dt_ext26_retain_cpu', 'int') + function('dt_ext26_publish', 'int') + function('dt_ext26_result', 'void') + function('dt_display_transaction', 'int') + r'''
static int reject_transaction(unsigned int n) {
 (void)n; int c=fdt_path_offset(dt,"/chosen");
 assert(!fdt_setprop_string(dt,c,"partial-test-property","uncommitted"));
 ext26_step="fixture-rollback"; return -17;
}
int main(void) {
 unsigned char blob[8192]; dt=blob;
 assert(!fdt_create_empty_tree(dt,sizeof(blob)));
 int aliases=fdt_add_subnode(dt,0,"aliases"); assert(aliases>=0);
 assert(!fdt_setprop_string(dt,aliases,"dcp","/panel"));
 int panel=fdt_add_subnode(dt,0,"panel"); assert(panel>=0);
 int chosen=fdt_add_subnode(dt,0,"chosen"); assert(chosen>=0);
 assert(!dt_t8122_dcpext_armed());
 assert(!fdt_setprop_string(dt,chosen,"asahi,t8122-dcpext","1"));
 assert(!dt_t8122_dcpext_armed());
 panel=fdt_path_offset(dt,"dcp");
 assert(!fdt_setprop_u32(dt,panel,"apple,t8122-handoff",1));
 assert(dt_t8122_dcpext_armed());
 assert(!fdt_setprop_u32(dt,panel,"apple,j613-25g83-mapping-handoff",1));
 assert(!dt_t8122_dcpext_armed()); /* Old opt-in cannot arm native26. */
 chosen=fdt_path_offset(dt,"/chosen");
 assert(!fdt_setprop_string(dt,chosen,"asahi,j613-25g83-dcpext","1"));
 assert(dt_t8122_dcpext_armed());
 const char *bad[]={"", "0", "true", "11"};
 for (unsigned i=0;i<4;i++) {
  assert(!fdt_setprop_string(dt,chosen,"asahi,j613-25g83-dcpext",bad[i]));
  assert(!dt_t8122_dcpext_armed());
 }
 struct adt_segment_ranges segments[4] = {0};
 segments[0].phys=0x100000; segments[0].remap=0x800000000; segments[0].size=32768;
 segments[2].phys=0x300000; segments[2].remap=0x800400000; segments[2].size=16384;
 struct dt_ext26_pages pages={.segments=segments};
 assert(!dt_ext26_page(0x800000000,0x100000,&pages));
 assert(!dt_ext26_page(0x800004000,0x104000,&pages));
 assert(pages.seen[0]==32768);
 assert(dt_ext26_page(0x800004000,0x108000,&pages));
 assert(dt_ext26_page(0x800008000,0x108000,&pages));
 assert(dt_ext26_page(0x800400000,0x300000,&pages)); /* OSLog is never DMA-mapped. */
 assert(pages.seen[0]==32768 && pages.seen[2]==0);
 int external=fdt_add_subnode(dt,0,"external");assert(external>=0);
 assert(!fdt_setprop_string(dt,external,"status","disabled"));
 aliases=fdt_path_offset(dt,"/aliases");assert(!fdt_setprop_string(dt,aliases,"dcpext","/external"));
 int cpu=fdt_add_subnode(dt,0,"cpu-domain"); assert(cpu>=0);
 assert(!fdt_setprop_u32(dt,cpu,"phandle",77));
 assert(!fdt_setprop_string(dt,cpu,"compatible","apple,t8122-pmgr-pwrstate"));
 assert(!fdt_setprop_string(dt,cpu,"label","dispext_cpu"));
 fdt32_t regs[]={cpu_to_fdt32(0x488),cpu_to_fdt32(4)};
 assert(!fdt_setprop(dt,cpu,"reg",regs,sizeof(regs)));
 external=fdt_path_offset(dt,"dcpext");
 assert(dt_ext26_retain_cpu("dcpext")); /* missing domain refuses */
 assert(!fdt_setprop_u32(dt,external,"power-domains",77));
 assert(!dt_ext26_retain_cpu("dcpext"));
 cpu=fdt_path_offset(dt,"/cpu-domain");
 assert(fdt_getprop(dt,cpu,"apple,firmware-owned",NULL));
 assert(!dt_ext26_retain_cpu("dcpext")); /* existing boolean updates */
 cpu=fdt_path_offset(dt,"/cpu-domain");
 assert(!fdt_setprop_string(dt,cpu,"label","other"));
 assert(dt_ext26_retain_cpu("dcpext"));
 const char *uuid="C042E95C-B9D8-3F0E-94B3-582A08AA6FDD";
 const u32 state[]={cpu_to_fdt32(1),cpu_to_fdt32(16),cpu_to_fdt32(34),cpu_to_fdt32(1)};
 assert(!dt_ext26_publish("dcpext",state,uuid));external=fdt_path_offset(dt,"dcpext");
 int compat_len; const fdt32_t *compat=fdt_getprop(dt,external,"apple,firmware-compat",&compat_len);
 assert(compat && compat_len==12 && fdt32_ld(compat)==26 && fdt32_ld(compat+1)==6 && fdt32_ld(compat+2)==2);
 ext26_step="locked-stream5-pages";dt_ext26_result(-7);
 chosen=fdt_path_offset(dt,"/chosen");
 assert(!strcmp(fdt_getprop(dt,chosen,"asahi,j613-25g83-dcpext-result",NULL),"step=locked-stream5-pages error=-7"));
 int ret=dt_display_transaction(reject_transaction,0); assert(ret==-17);
 dt_ext26_result(ret); chosen=fdt_path_offset(dt,"/chosen");
 assert(!fdt_getprop(dt,chosen,"partial-test-property",NULL));
 assert(!strcmp(fdt_getprop(dt,chosen,"asahi,j613-25g83-dcpext-result",NULL),"step=fixture-rollback error=-17"));
 strcpy(ext26_detail,"substep=L1-target l1=7 descriptor=123 page=456 bytes=4000");
 ext26_step="locked-table-collection";dt_ext26_result(-5);chosen=fdt_path_offset(dt,"/chosen");
 assert(!strcmp(fdt_getprop(dt,chosen,"asahi,j613-25g83-dcpext-result",NULL),"step=locked-table-collection error=-5 substep=L1-target l1=7 descriptor=123 page=456 bytes=4000"));
 ext26_detail[0]=0;
 external=fdt_path_offset(dt,"dcpext");
 int n;const char *published=fdt_getprop(dt,external,"apple,firmware-uuid",&n);
 assert(published && n==37 && !strcmp(published,uuid));
 const fdt32_t *marker=fdt_getprop(dt,external,"apple,j613-25g83-dcpext-handoff",&n);
 assert(marker && n==4 && fdt32_to_cpu(*marker)==1);
 assert(!strcmp(fdt_getprop(dt,external,"status",NULL),"disabled"));
 assert(!memcmp(fdt_getprop(dt,external,"apple,j613-25g83-dcpext-boot-state",&n),state,sizeof(state)) && n==16);
 assert(!fdt_delprop(dt,external,"apple,j613-25g83-dcpext-handoff"));assert(!fdt_pack(dt));
 assert(dt_ext26_publish("dcpext",state,uuid));
 assert(!fdt_getprop(dt,fdt_path_offset(dt,"dcpext"),"apple,j613-25g83-dcpext-handoff",NULL));
 return 0;
}
''')


def test_native26_reservation_reuse(tmp_path):
    src = (ROOT / 'src/kboot.c').read_text()
    start = src.index('static int dt_ext26_existing_reservation(')
    helper = src[start:src.index('\n}', start) + 2]
    run_c(tmp_path, 'native26-reservation-reuse', r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "libfdt.h"
typedef uint64_t u64;
static void *dt;
''' + helper + r'''
static void reserve(const char *name, u64 start, u64 size, bool nomap) {
 int parent = fdt_path_offset(dt, "/reserved-memory");
 int node = fdt_add_subnode(dt, parent, name); assert(node >= 0);
 fdt64_t reg[] = {cpu_to_fdt64(start), cpu_to_fdt64(size)};
 assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
 if (nomap) assert(!fdt_setprop_empty(dt, node, "no-map"));
}
int main(void) {
 unsigned char blob[4096]; dt = blob;
 assert(!fdt_create_empty_tree(dt, sizeof(blob)));
 assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
 reserve("text", 0x100000, 0x8000, true);
 reserve("data", 0x200000, 0x4000, true);
 reserve("mapped", 0x300000, 0x4000, false);
 /* Shared text reuses the exact internal reservation. */
 int text = dt_ext26_existing_reservation(0x100000, 0x8000, true);
 assert(text == fdt_path_offset(dt, "/reserved-memory/text"));
 /* Writable data or heap with an exact match is still refused. */
 assert(dt_ext26_existing_reservation(0x200000, 0x4000, false) == -FDT_ERR_INTERNAL);
 /* Partial overlaps and mapped reservations are refused even when shared. */
 assert(dt_ext26_existing_reservation(0x104000, 0x8000, true) == -FDT_ERR_INTERNAL);
 assert(dt_ext26_existing_reservation(0x300000, 0x4000, true) == -FDT_ERR_INTERNAL);
 /* No overlap leaves the caller to add its own reservation. */
 assert(dt_ext26_existing_reservation(0x400000, 0x4000, false) == -FDT_ERR_NOTFOUND);
 return 0;
}
''')
