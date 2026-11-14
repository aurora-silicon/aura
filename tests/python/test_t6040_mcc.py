"""Exercise the firmware-cache initializer with rejected layouts and status reads."""
from pathlib import Path
import json
import subprocess


def test_firmware_cache_init(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/mcc.c").read_text()
    # The J773s (Mac mini, M4 Pro) values, reduced from its IODeviceTree.
    j773s = json.loads((repo / "tests/data/j773s-t6041-mcc.json").read_text())
    assert j773s["chip_id"] == "0x6040" and j773s["mcc_compatible"] == "mcc,t6041"
    assert len(j773s["apertures"]) == j773s["amcc_count"]
    fixture = "\n".join([
        f'#define J773S_BOARD_ID {j773s["board_id"]}',
        f'#define J773S_COMPAT "{j773s["root_compatible"]}"',
        f'#define J773S_REG_IDX {j773s["amcc_reg_idx"]}',
        f'#define J773S_COUNT {j773s["amcc_count"]}',
        f'#define J773S_CHANNELS {j773s["dcs_count_per_amcc"]}',
        f'#define J773S_PLANES {j773s["lock_regs_plane_count"]}',
        f'#define J773S_STRIDE {j773s["lock_regs_plane_stride"]}',
        "static const u64 j773s_base[] = {" + ", ".join(a["base"] for a in j773s["apertures"]) + "};",
        "static const u64 j773s_size[] = {" + ", ".join(a["size"] for a in j773s["apertures"]) + "};",
    ])
    helper = source[source.index("static int mcc_init_firmware_cache("):source.index("\nint mcc_init(void)")]
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define T6040 0x6040
#define MAX_MCC_INSTANCES 16
#define T8103_PLANE_STRIDE 0x40000
@FIXTURE@
static void *adt;
static u32 chip_id = T6040, board_id = 6;
static int mcc_count;
static bool mcc_initialized;
static struct {u64 plane_base,plane_stride; int plane_count,dcs_count; void *tz; bool has_cache_control;} mcc_regs[16];
static u64 count=4, channels=4, planes=4, stride=0x40000, aperture=0x200000;
static u32 reg_idx=12;
static bool is_t6041=true;
/* NULL: the J616s profile's apertures; otherwise the fixture's translated apertures. */
static const u64 *ap_base, *ap_size;
static const char *root_compat="J616sAP";
static int bad_status=-1, reads, bad_reg=-1;
static int adt_path_offset(void *a, const char *p) {return 2;}
static bool adt_is_compatible(void *a,int n,const char *p) {
 return n==0 ? !strcmp(p,root_compat) : is_t6041 && !strcmp(p,"mcc,t6041");
}
static void *adt_getprop(void *a,int n,const char *p,void *length) {return &count;}
static int getprop(void *a,int n,const char *p,u32 *v) {*v=reg_idx;return 0;}
#define ADT_GETPROP(a,n,p,v) getprop(a,n,p,v)
static int mcc_get_count(int n,const char *p,u64 *v) {
 if (!strcmp(p,"amcc-count")) *v=count;
 else if (!strcmp(p,"dcs-count-per-amcc")) *v=channels;
 else if (!strcmp(p,"plane-count")) *v=planes;
 else if (!strcmp(p,"plane-stride")) *v=stride;
 else return -1;
 return 0;
}
static int adt_get_reg(void *a,int *path,const char *p,int idx,u64 *base,u64 *size) {
 if (idx==bad_reg) return -1;
 if (ap_base) {
  assert(idx>=(int)reg_idx && idx<(int)reg_idx+J773S_COUNT);
  *base=ap_base[idx-reg_idx];*size=ap_size[idx-reg_idx];return 0;
 }
 *base=0x220000000ULL+(u64)(idx-reg_idx)*0x2000000;
 *size=aperture;return 0;
}
static u32 plane_read32(int i,int plane,u64 offset) {
 assert(offset==0x2804); assert(i>=0 && i<4);assert(plane>=0 && plane<4);
 assert(mcc_regs[i].plane_base==(ap_base ? ap_base[i] : 0x220000000ULL+(u64)i*0x2000000));
 assert(mcc_regs[i].plane_stride==stride);
 return reads++==bad_status ? 0 : 0x0c000c00;
}
''' + helper + r'''
static void reset(void) {mcc_initialized=false;reads=0;bad_status=-1;bad_reg=-1;}
int main(void) {
 int path[8]={0};
 reset();assert(mcc_init_firmware_cache(1,path)==0);assert(mcc_initialized && reads==16);
 for(int i=0;i<4;i++) assert(!mcc_regs[i].has_cache_control && !mcc_regs[i].tz);
 for(int bad=0;bad<16;bad++) {
  reset();bad_status=bad;assert(mcc_init_firmware_cache(1,path)<0);
  assert(!mcc_initialized && reads==bad+1);
 }
 reset();reg_idx=11;assert(mcc_init_firmware_cache(1,path)<0 && !reads);reg_idx=12;
 reset();board_id=7;assert(mcc_init_firmware_cache(1,path)<0 && !reads);board_id=6;
 reset();chip_id=0x6041;assert(mcc_init_firmware_cache(1,path)<0 && !reads);chip_id=T6040;
 reset();count=0;assert(mcc_init_firmware_cache(1,path)<0 && !reads);count=4;
 reset();count=17;assert(mcc_init_firmware_cache(1,path)<0 && !reads);count=4;
 reset();planes=5;assert(mcc_init_firmware_cache(1,path)<0 && !reads);planes=J773S_PLANES;
 reset();stride=0x80000;assert(mcc_init_firmware_cache(1,path)<0 && !reads);stride=J773S_STRIDE;
 reset();aperture=4;assert(mcc_init_firmware_cache(1,path)<0 && !reads);aperture=0x200000;
 reset();bad_reg=15;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 /* J614s: board-id 4 is admitted only with its root compatible. */
 reset();board_id=4;root_compat="J614sAP";assert(mcc_init_firmware_cache(1,path)==0 && reads==16);
 reset();root_compat="J773sAP";assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 /* J773s: its own board-id, root compatible, geometry and apertures from the fixture. */
 reset();board_id=J773S_BOARD_ID;root_compat=J773S_COMPAT;reg_idx=J773S_REG_IDX;count=J773S_COUNT;
 channels=J773S_CHANNELS;planes=J773S_PLANES;stride=J773S_STRIDE;ap_base=j773s_base;ap_size=j773s_size;
 assert(mcc_init_firmware_cache(1,path)==0 && mcc_initialized && reads==J773S_COUNT*J773S_PLANES);
 for(int i=0;i<J773S_COUNT;i++)
  assert(mcc_regs[i].plane_base==j773s_base[i] && mcc_regs[i].plane_stride==J773S_STRIDE &&
         mcc_regs[i].plane_count==J773S_PLANES && mcc_regs[i].dcs_count==J773S_CHANNELS);
 reset();bad_status=5;assert(mcc_init_firmware_cache(1,path)<0 && reads==6);
 reset();planes=5;assert(mcc_init_firmware_cache(1,path)<0 && !reads);planes=J773S_PLANES;
 reset();stride=0x80000;assert(mcc_init_firmware_cache(1,path)<0 && !reads);stride=J773S_STRIDE;
 reset();root_compat="J616sAP";assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 reset();board_id=3;root_compat=J773S_COMPAT;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 board_id=6;root_compat="J616sAP";ap_base=ap_size=NULL;
 reset();is_t6041=false;chip_id=0x8140;reg_idx=0;
 assert(mcc_init_firmware_cache(1,path)==0 && mcc_initialized && !reads);
 return 0;
}
'''
    harness = harness.replace("@FIXTURE@", fixture)
    cfile = tmp_path / "mcc.c"
    binary = tmp_path / "mcc-test"
    cfile.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, capture_output=True)
