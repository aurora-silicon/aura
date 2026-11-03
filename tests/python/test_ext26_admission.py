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
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define SZ_16K 16384
#define ALIGN_UP(v,a) (((v)+(a)-1)&~((a)-1))
static void *dt;
''' + seg_type + page_type + function('dt_ext26_page', 'int') +
          function('dt_t8122_dcpext_armed', 'bool') + r'''
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
 return 0;
}
''')
