# SPDX-License-Identifier: MIT
"""Run the T6034 measurement and shared probe with real libfdt and injected power faults."""
from pathlib import Path
import pytest
from test_siliconforest_handoff import definition, run_c
ROOT = Path(__file__).resolve().parents[2]

def pick(source, signature):
    a = source.index(signature)
    return source[a:source.index('\n}', a) + 2]

@pytest.mark.parametrize("board", ["apple,j514m", "apple,j516m"])
def test_scoped_measurement_restores_power_and_never_enables_gpu(tmp_path, board):
    gpu = (ROOT / 'src/kboot_gpu.c').read_text()
    shared = (ROOT / 'src/kboot_gpu_t6031.c').read_text()
    helper = (ROOT / 'src/kboot_t6031.c').read_text()
    headers = (ROOT / 'src/kboot_gpu_t8122_power.h').read_text()
    identity = headers[headers.index('enum t8122_identity_result {'):headers.index('struct t8122_power_probe_ops {')]
    headers = (ROOT / 'src/kboot_gpu_t6031.h').read_text()
    ops = headers[headers.index('struct t6031_probe_ops {'):headers.index('struct t8122_powered_identity t6031_probe_run')]
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef unsigned long u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define T6034 0x6034
#define T6031_PS_MAX 8
#define T6031_SGX_BASE 0x408000000UL
#define G15_SGX_CORE_MASK 0xe01500
struct t6031_ps { u64 addr; u32 before, after, depth; const char *name; };
struct t6031_closure { int n; struct t6031_ps ps[T6031_PS_MAX]; };
struct t6031_probe { u64 sgx_base; struct t6031_closure closure; };
static u32 chip_id;
static int enabled, restored, reads, images, fail_enable, fail_restore, fail_read, bad_window;
static void *adt;
''' + identity + ops + '''
static struct t8122_powered_identity g15_g15c_id;
struct fw_version_info;
static const struct fw_version_info *g15_g15c_fw;
''' + pick(helper, 'bool t6034_board(') + pick(helper, 'bool j516m_board(') + r'''
static int adt_path_offset_trace(void *a, const char *s, int *p) { return 0; }
static int adt_get_reg(void *a, int *p, const char *s, int n, u64 *base, u64 *size)
{ *base=bad_window ? 1 : T6031_SGX_BASE; *size=0x3fdc000; return 0; }
static int adt_path_offset(void *a, const char *s) { return 1; }
static int g15_adt_u32(int n, const char *p, u32 *v)
{ *v=!strcmp(p,"gpu-num-perf-states") ? 13 : !strcmp(p,"perf-state-table-count") ? 4 : 14; return 0; }
static int t6031_closure_get(const char *s, struct t6031_closure *c)
{ c->n=1; c->ps[0]=(struct t6031_ps){.addr=123,.before=0,.after=0}; return 0; }
static int t6031_probe_enable(void *c) { enabled++; return fail_enable; }
static int t6031_probe_active(void *c) { return 0; }
static int t6031_probe_read(void *c,u32 *v,u32 m[3],u32 *n,u32 *n2)
{ reads+=6; *v=0x07041100; *n=0x0021040a; *n2=0x40404; m[0]=0x5ed7f5fd; m[1]=0x7b; m[2]=0; return fail_read; }
static int t6031_probe_restore(void *c) { restored++; return fail_restore; }
static bool t6031_identity_valid(const struct t8122_powered_identity *id) { return true; }
''' + pick(shared, 'struct t8122_powered_identity t6031_probe_run(') + r'''
static const char *const identity_u32[] = {
 "probe-result", "id-version", "core-mask-0", "core-mask-1", "core-mask-2", "id-counts-1", "id-counts-2" };
''' + pick(shared, 'int t6031_identity_node_named(') + definition(shared, 'set_u32') + pick(shared, 'int t6031_identity_publish_named(') + r'''
static const struct fw_version_info *dt_t6031_gpu_image_named(void *d, const char *n)
{ images++; return NULL; }
''' + definition(gpu, 'dt_measure_gpu_t6034') + r'''
static unsigned char dt[8192];
static void reset(const char *board, const char *soc)
{
 enabled=restored=reads=images=fail_enable=fail_restore=fail_read=bad_window=0; chip_id=T6034;
 assert(!fdt_create_empty_tree(dt,sizeof(dt)));
 char compat[128]; size_t n=strlen(board)+1; memcpy(compat,board,n); memcpy(compat+n,soc,strlen(soc)+1);
 assert(!fdt_setprop(dt,0,"compatible",compat,n+strlen(soc)+1));
 assert(fdt_add_subnode(dt,0,"chosen")>=0);
 int gpu=fdt_add_subnode(dt,0,"gpu"); assert(gpu>=0);
 assert(!fdt_setprop_string(dt,gpu,"compatible","apple,agx-t6034"));
 assert(!fdt_setprop_string(dt,gpu,"status","disabled"));
 int aliases=fdt_add_subnode(dt,0,"aliases");assert(aliases>=0);
 assert(!fdt_setprop_string(dt,aliases,"gpu","/gpu"));
}
static int result(void)
{
 int n=fdt_path_offset(dt,"/chosen/asahi,t6034-gpu-powered-identity");assert(n>=0);
 const fdt32_t *v=fdt_getprop(dt,n,"probe-result",NULL);assert(v);return fdt32_ld(v);
}
static void stays_disabled(void)
{ assert(!strcmp(fdt_getprop(dt,fdt_path_offset(dt,"/gpu"),"status",NULL),"disabled")); }
int main(void)
{
 reset("apple,j514m","apple,t6034"); assert(!dt_measure_gpu_t6034(dt));
 assert(enabled==1 && restored==1 && reads==6 && images==1 && result()==T8122_ID_PASS);stays_disabled();
 for(int fault=0;fault<3;fault++) {
  reset("apple,j514m","apple,t6034"); fail_enable=fault==0;fail_read=fault==1;fail_restore=fault==2;
  assert(!dt_measure_gpu_t6034(dt));assert(enabled==1 && restored==1);
  assert(result()==(fault==0 ? T8122_ID_POWER_FAILED : fault==1 ? T8122_ID_READ_FAULT : T8122_ID_POWER_OFF_FAILED));
  assert(reads==(fault==0 ? 0 : 6));stays_disabled();
 }
 const char *bad_board[]={"apple,j293","apple,j514c","apple,j999"};
 for(unsigned i=0;i<ARRAY_SIZE(bad_board);i++) {
  reset(bad_board[i],"apple,t6034");assert(!dt_measure_gpu_t6034(dt));assert(!enabled && !reads && !images);
 }
 reset("apple,j514m","apple,t6031");assert(!dt_measure_gpu_t6034(dt));assert(!enabled);
 reset("apple,j514m","apple,t6034");chip_id=0x6031;assert(!dt_measure_gpu_t6034(dt));assert(!enabled);
 const char *values[]={"0","","true","01","1x"};
 for(unsigned j=0;j<ARRAY_SIZE(values);j++) {
  reset("apple,j514m","apple,t6034");assert(!fdt_setprop_string(dt,fdt_path_offset(dt,"/chosen"),"asahi,t6034-gpu",values[j]));
  assert(!dt_measure_gpu_t6034(dt));assert(!enabled && !restored && !reads && !images);stays_disabled();
 }
 /* m3-defaults=0 withholds only the default: an owner's own GPU line of 1 still measures */
 reset("apple,j514m","apple,t6034");
 assert(!fdt_setprop_string(dt,fdt_path_offset(dt,"/chosen"),"asahi,m3-defaults","0"));
 assert(!fdt_setprop_string(dt,fdt_path_offset(dt,"/chosen"),"asahi,t6034-gpu","1"));
 assert(!dt_measure_gpu_t6034(dt));
 assert(enabled==1 && restored==1 && reads==6 && images==1 && result()==T8122_ID_PASS);stays_disabled();
 reset("apple,j514m","apple,t6034");bad_window=1;assert(!dt_measure_gpu_t6034(dt));assert(!enabled && !reads);
 return 0;
}
'''
    fixture, main = code.split('int main(void)', 1)
    code = fixture + 'int main(void)' + main.replace('apple,j514m', board)
    run_c(tmp_path, 't6034-measurement', code)
    boot = (ROOT / 'src/kboot.c').read_text()
    assert 'if (!(chip_id == T6034 && chosen_lost) && dt_set_gpu(dt))' in boot

@pytest.mark.parametrize("board", ["apple,j514m", "apple,j516m"])
def test_normal_t6034_selector_keeps_owner_optouts_before_any_probe(tmp_path, board):
    gpu = (ROOT / 'src/kboot_gpu.c').read_text()
    boards = (ROOT / 'src/kboot_t6031.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "libfdt.h"
#define T6034 0x6034
static int chip_id=T6034;
static int probes, setups, disables;
struct fw_version_info;
enum { T8122_ID_NOT_RUN };
struct t8122_powered_identity { int result; };
static struct t8122_powered_identity g15_g15c_id;
static const struct fw_version_info *g15_g15c_fw;
static const struct { const char *optin; } g15_soc_t6034={"asahi,t6034-gpu"};
static void g15_disable(void *d) { disables++; }
static int dt_measure_gpu_t6034(void *d) { probes++; return 0; }
static int dt_set_gpu_g15(void *d,const void *soc) { assert(soc==&g15_soc_t6034);setups++;return 0; }
''' + pick(boards, 'bool t6034_board(') + pick(boards, 'bool j516m_board(') + pick(gpu, 'static bool g15_optin(') + definition(gpu, 'dt_set_gpu_t6034') + r'''
int main(void)
{
 unsigned char d[2048];assert(!fdt_create_empty_tree(d,sizeof(d)));
 const char comp[]="apple,j514m\0apple,t6034";
 assert(!fdt_setprop(d,0,"compatible",comp,sizeof(comp)));
 int n=fdt_add_subnode(d,0,"chosen");assert(n>=0);
 assert(!dt_set_gpu_t6034(d));assert(!probes && !setups && disables==1);
 const char *no[]={"0","","true","01"};
 for(unsigned i=0;i<sizeof(no)/sizeof(no[0]);i++) {
  assert(!fdt_setprop_string(d,n,"asahi,t6034-gpu",no[i]));
  assert(!dt_set_gpu_t6034(d));assert(!probes && !setups);
 }
 assert(!fdt_setprop_string(d,n,"asahi,t6034-gpu","1"));
 assert(!dt_set_gpu_t6034(d));assert(probes==1 && setups==1);
 chip_id=0x6031;assert(!dt_set_gpu_t6034(d));assert(probes==1 && setups==1);
 chip_id=T6034;const char wrong[]="apple,j999\0apple,t6034";
 assert(!fdt_setprop(d,0,"compatible",wrong,sizeof(wrong)));
 assert(!dt_set_gpu_t6034(d));assert(probes==1 && setups==1);
 return 0;
}
'''
    fixture, main = code.split('int main(void)', 1)
    code = fixture + 'int main(void)' + main.replace('apple,j514m', board)
    run_c(tmp_path, 't6034-selector', code)
