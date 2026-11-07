# SPDX-License-Identifier: MIT
"""Actual native collector: firmware attributes, ownership bounds and control stability."""
from pathlib import Path
import subprocess
from test_locked_dart_tables import function

def test_native_dart_tables(tmp_path):
 s=(Path(__file__).resolve().parents[2]/'src/dart.c').read_text()
 macros='\n'.join(l for l in s.splitlines() if l.startswith('#define DART_') and not l.rstrip().endswith('\\'))
 code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned long u64;typedef uint32_t u32;typedef uint8_t u8;
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) (((~0UL)>>(63-(h)))&((~0UL)<<(l)))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctzl(m))
#define FIELD_PREP(m,v) (((u64)(v)<<__builtin_ctzl(m))&(m))
#define SZ_16K 16384
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
'''+macros+r'''
static u32 tcr,ttbr,protect;static bool change;static int reads;
static u32 read32(uintptr_t a) {
 if(a==DART_T8110_PROTECT)return protect;
 if(a==DART_T8110_TCR_OFF+20)return (++reads==2 && change)?tcr^4:tcr;
 assert(a==DART_T8110_TTBR_OFF+20);return ttbr;
}
'''+function(s,'dart_table_in_dram')+function(s,'dart_collect_native_t8110')+r'''
static u64 base,*root,pages[4];static size_t count;static char detail[256];
static void setup(void) {
 memset(root,0,4*SZ_16K);tcr=1;protect=1;reads=0;change=false;count=99;
 ttbr=FIELD_PREP(DART_T8110_TTBR_ADDR,base>>14)|1;
 root[0]=FIELD_PREP(DART_T6000_PTE_OFFSET,(base+SZ_16K)>>14)|1;
}
static int collect(size_t cap){return dart_collect_native_t8110(0,5,base,4*SZ_16K,pages,cap,&count,detail,sizeof(detail));}
int main(void) {
 root=MAP_FAILED;
 for(unsigned i=0;i<64;i++) {
  base=0x10000000000UL+((u64)i<<28);
  void *p=mmap((void *)base,4*SZ_16K,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(p==(void *)base){root=p;break;}
  if(p!=MAP_FAILED)assert(!munmap(p,4*SZ_16K));
 }
 assert(root!=MAP_FAILED);
 setup();assert(!collect(4)&&count==2&&pages[0]==base&&pages[1]==base+SZ_16K);
 setup();tcr|=DART_T8110_TCR_BYPASS_DAPF|FIELD_PREP(DART_T8110_TCR_REMAP,3);
 root[0]|=DART_T6000_PTE_REALTIME;root[9]=root[0];root[10]=0x4000;
 assert(!collect(2)&&count==2); /* walker accepts these attributes; no new roots */
 setup();tcr|=DART_T8110_TCR_BYPASS_DART;assert(collect(4)==-3&&!count);
 setup();tcr|=DART_T8110_TCR_REMAP_EN;assert(collect(4)==-3&&!count);
 setup();tcr=0;assert(collect(4)==-3&&!count);
 setup();protect=0;assert(collect(4)==-3&&!count);
 setup();ttbr=0;assert(collect(4)==-3&&!count);
 setup();ttbr=FIELD_PREP(DART_T8110_TTBR_ADDR,(base+4*SZ_16K)>>14)|1;assert(collect(4)==-4&&!count);
 setup();root[0]=FIELD_PREP(DART_T6000_PTE_OFFSET,(base+4*SZ_16K)>>14)|1;assert(collect(4)==-5&&!count);assert(strstr(detail,"substep=L1-target l1=0")&&strstr(detail,"dram="));
 setup();root[0]=FIELD_PREP(DART_T6000_PTE_OFFSET,base>>14)|1;assert(collect(4)==-5&&!count);assert(strstr(detail,"substep=L1-target l1=0")&&strstr(detail,"dram="));
 setup();assert(collect(1)==-6&&!count);assert(strstr(detail,"expected-pages=2 actual-capacity=1"));
 setup();change=true;assert(collect(4)==-7&&!count);
 setup();assert(dart_collect_native_t8110(0,16,base,4*SZ_16K,pages,4,&count,detail,sizeof(detail))==-2&&!count);
 setup();assert(dart_collect_native_t8110(0,5,base,~0UL,pages,4,&count,detail,sizeof(detail))==-2&&!count);
 assert(!munmap(root,4*SZ_16K));puts("PASS native table ownership, firmware attributes, 1-TiB addresses, deduplication, capacity and refusal diagnostics");
}
'''
 p=tmp_path/'native.c';p.write_text(code);binary=tmp_path/'native'
 subprocess.run(['cc','-D_GNU_SOURCE','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
