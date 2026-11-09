# SPDX-License-Identifier: MIT
import pathlib
import subprocess
import tempfile
import unittest


class ExternalDependencyGroup(unittest.TestCase):
    def test_real_fdt_success_missing_reference_and_rollback(self):
        root = pathlib.Path(__file__).parents[2]
        source = (root / 'src/kboot.c').read_text()
        code = 'static int dt_t8140_power_paths' + source.split('static int dt_t8140_power_paths', 1)[1].split('/* A specifier occupies', 1)[0]
        handed = source[source.index('/* A display DART is published only'):source.index('static bool dt_t8140_node_enabled(')]
        prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
static unsigned char storage[8192];
static void *dt = storage;
static size_t dt_bufsize = sizeof(storage);
static int dt_get_iommu_node(int node, unsigned index) {
 int len; const fdt32_t *p=fdt_getprop(dt,node,"iommus",&len);
 if(!p || len!=8 || index) return -1;
 return fdt_node_offset_by_phandle(dt,fdt32_to_cpu(*p));
}
static int add(const char *name,unsigned phandle,int status) {
 int n=fdt_add_subnode(dt,0,name); assert(n>=0);
 assert(!fdt_setprop_u32(dt,n,"phandle",phandle));
 if(status) assert(!fdt_setprop_string(dt,n,"status","disabled"));
 return fdt_path_offset(dt,name);
}
static void fixture(int omit) {
 memset(storage,0,sizeof(storage)); assert(!fdt_create_empty_tree(dt,sizeof(storage)));
 add("dart0",1,1);add("dart1",2,!omit);add("mbox",3,1);add("external",4,1);
 /* Reservation publishes the firmware-root handoff before the group enable. */
 assert(!fdt_setprop_u32(dt,fdt_path_offset(dt,"/dart0"),"apple,firmware-root-handoff",1));
 assert(!fdt_setprop_u32(dt,fdt_path_offset(dt,"/dart1"),"apple,firmware-root-handoff",1));
 int n=fdt_path_offset(dt,"/external");
 fdt32_t iommu[]={cpu_to_fdt32(1),cpu_to_fdt32(23)};
 assert(!fdt_setprop(dt,n,"iommus",iommu,sizeof(iommu)));
 assert(!fdt_setprop_u32(dt,n,"mboxes",3));
 add("power-root",10,1);add("power-mid",11,1);add("power-leaf",12,1);
 const char *power[]={"/power-root","/power-mid","/power-leaf"};
 for(unsigned i=0;i<3;i++) { int p=fdt_path_offset(dt,power[i]); assert(!fdt_setprop_u32(dt,p,"#power-domain-cells",0)); if(i) assert(!fdt_setprop_u32(dt,p,"power-domains",9+i)); }
 n=fdt_path_offset(dt,"/external");assert(!fdt_setprop_u32(dt,n,"power-domains",12));
 int child=fdt_add_subnode(dt,n,"piodma");assert(child>=0);
 iommu[0]=cpu_to_fdt32(2);iommu[1]=cpu_to_fdt32(16);
 assert(!fdt_setprop(dt,child,"iommus",iommu,sizeof(iommu)));
 assert(!fdt_setprop_string(dt,child,"status","disabled"));
}
'''
        suffix = r'''
int main(void) {
 const char *paths[]={"/dart0","/dart1","/mbox","/external/piodma","/external","/power-leaf","/power-mid","/power-root"};
 fixture(0);char power[3][128];int external=fdt_path_offset(dt,"/external");
 assert(dt_t8140_power_paths(external,power,3)==3);
 assert(dt_t8140_power_paths(external,power,0)<0);
 assert(dt_t8140_power_paths(external,power,4)<0);
 assert(dt_t8140_power_paths(external,power,1)<0);
 assert(!dt_t8140_enable_external("/external"));
 for(unsigned i=0;i<ARRAY_SIZE(paths);i++) { int n=fdt_path_offset(dt,paths[i]); assert(n>=0); assert(!strcmp(fdt_getprop(dt,n,"status",NULL),"okay")); }
 unsigned char before[sizeof(storage)];
 fixture(0);int n=fdt_path_offset(dt,"/external");assert(!fdt_setprop_u32(dt,n,"mboxes",99));memcpy(before,dt,sizeof(storage));
 assert(dt_t8140_enable_external("/external")<0);assert(!memcmp(before,dt,sizeof(storage)));
 fixture(0);n=fdt_path_offset(dt,"/power-leaf");assert(!fdt_setprop_u32(dt,n,"power-domains",12));memcpy(before,dt,sizeof(storage));
 assert(dt_t8140_enable_external("/external")<0);assert(!memcmp(before,dt,sizeof(storage)));
 fixture(1);assert(!fdt_pack(dt));memcpy(before,dt,sizeof(storage));
 assert(dt_t8140_enable_external("/external")<0);assert(!memcmp(before,dt,sizeof(storage)));
 /* A DART without reserved firmware tables is never published. */
 for(unsigned d=0;d<4;d++){
  fixture(0);n=fdt_path_offset(dt,d&1?"/dart1":"/dart0");
  if(d<2) assert(!fdt_delprop(dt,n,"apple,firmware-root-handoff"));
  else assert(!fdt_setprop_u32(dt,n,"apple,firmware-root-handoff",2));
  memcpy(before,dt,sizeof(storage));
  assert(dt_t8140_enable_external("/external")<0);assert(!memcmp(before,dt,sizeof(storage)));
 }
 /* An always-on fabric ancestor ends the chain; its own parents are not walked. */
 for(unsigned aon=0;aon<2;aon++){
  fixture(0);add("fabric-a",20,0);add("fabric-b",21,0);add("fabric",13,0);
  n=fdt_path_offset(dt,"/fabric");assert(!fdt_setprop_u32(dt,n,"#power-domain-cells",0));
  fdt32_t two[]={cpu_to_fdt32(20),cpu_to_fdt32(21)};assert(!fdt_setprop(dt,n,"power-domains",two,sizeof(two)));
  if(aon) assert(!fdt_setprop_empty(dt,n,"apple,always-on"));
  n=fdt_path_offset(dt,"/power-root");assert(!fdt_setprop_u32(dt,n,"power-domains",13));
  external=fdt_path_offset(dt,"/external");
  if(aon){
   assert(dt_t8140_power_paths(external,power,3)==3);
   assert(!strcmp(power[2],"/power-root"));
   assert(!dt_t8140_enable_external("/external"));
   assert(!fdt_getprop(dt,fdt_path_offset(dt,"/fabric"),"status",NULL));
  } else {
   assert(dt_t8140_power_paths(external,power,3)<0);
  }
 }
 /* The controller's own first state may itself be always-on. */
 fixture(0);n=fdt_path_offset(dt,"/power-leaf");assert(!fdt_setprop_empty(dt,n,"apple,always-on"));
 external=fdt_path_offset(dt,"/external");
 assert(dt_t8140_power_paths(external,power,3)==3);
 puts("dependency group success, dangling mailbox, unreserved DART, always-on fabric end and mid-update FDT rollback PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / 'test.c'
            c.write_text(prefix + handed + code + suffix)
            exe = pathlib.Path(tmp) / 'test'
            library = root / 'src/libfdt'
            subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(library), str(c), *[str(library / name) for name in ('fdt.c', 'fdt_ro.c', 'fdt_rw.c', 'fdt_sw.c', 'fdt_wip.c', 'fdt_empty_tree.c', 'fdt_check.c')], '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
