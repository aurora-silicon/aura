# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production T8132 DAPF writer and window merge on the J713 AOP DART."""
import json
from pathlib import Path
import re
import subprocess

from test_j713_dapf_instance import block

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the m1n1 host fixture requires an LP64 host");
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
#define T8132 0x8132
#define T8140 0x8140
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define SZ_16K (1 << 14)
static int chip_id = T8132;
static void *adt;
struct node {
 const char *name;
 const u8 *instance, *config;
 u32 instance_len, config_len;
 u64 base[3], size[3];
 int nregs;
};
static struct node nodes[3];
static int find(const char *p) {
 for(int i=0;i<3;i++) {
  char path[40];snprintf(path,sizeof(path),"/arm-io/dart-%s",nodes[i].name);
  if(!strcmp(p,path))return i+1;
  snprintf(path,sizeof(path),"/arm-io/%s",nodes[i].name);
  if(!strcmp(p,path))return i+4;
 }
 return -1;
}
static int adt_path_offset(const void *tree,const char *p){(void)tree;return find(p);}
static int adt_path_offset_trace(const void *tree,const char *p,int *trace) {
 int n=adt_path_offset(tree,p);trace[0]=n;return n;
}
static const void *adt_getprop(const void *tree,int id,const char *key,u32 *len) {
 (void)tree;assert(id>=1&&id<=6);struct node *n=&nodes[(id-1)%3];
 /* Every consumer is always-on (no clock gates), like the J713 AOP. */
 if(!strcmp(key,"clock-gates"))return NULL;
 if(!strcmp(key,"instance")) {if(len)*len=n->instance_len;return n->instance;}
 if(!strcmp(key,"dapf-instance-0")) {if(len)*len=n->config_len;return n->config;}
 return NULL;
}
static int adt_get_reg(const void *tree,int *trace,const char *key,int index,u64 *base,u64 *size) {
 (void)tree;(void)key;assert(trace[0]>=1&&trace[0]<=3);struct node *n=&nodes[trace[0]-1];
 if(index<0||index>=n->nregs)return -1;
 *base=n->base[index];if(size)*size=n->size[index];return 0;
}
static bool adt_is_compatible(const void *tree,int id,const char *compat)
{(void)tree;(void)id;return !strcmp(compat,"dart,t8110");}
static int pmgr_adt_power_enable(const char *p){(void)p;return 0;}
static int pmgr_adt_power_disable(const char *p){(void)p;return 0;}
static int pmgr_adt_is_powered(const char *p){(void)p;return 1;}
/*
 * The AOP DART's DAPF bank. Like the hardware, r0 keeps only its low byte and
 * an end its upper bits; stuck_slice's r20 ignores writes.
 */
static u8 bank[0x4000];
static u64 bank_base;
static bool writable;
static int stuck_slice = -1;
static unsigned filter_writes;
static u64 offset(u64 addr,unsigned width) {
 assert(addr>=bank_base&&addr+width<=bank_base+sizeof(bank));return addr-bank_base;
}
static u32 read32(u64 addr){u32 v;memcpy(&v,bank+offset(addr,4),4);return v;}
static u64 read64(u64 addr){u64 v;memcpy(&v,bank+offset(addr,8),8);return v;}
static void write32(u64 addr,u32 value) {
 u64 off=offset(addr,4);filter_writes++;assert(writable);
 if(off%0x40==0x00)value&=0xff;
 if(off%0x40==0x20&&(int)(off/0x40)==stuck_slice)return;
 memcpy(bank+off,&value,4);
}
static void write64(u64 addr,u64 value) {
 u64 off=offset(addr,8);filter_writes++;assert(writable);
 if(off%0x40==0x10)value&=~3UL;
 memcpy(bank+off,&value,8);
}
static int fdt_path_offset(const void *dt,const char *p){(void)dt;(void)p;return 1;}
static const void *fdt_getprop(const void *dt,int id,const char *key,void *len)
{(void)dt;(void)id;(void)key;(void)len;return &bank;}
/* The J713 DT requests the AOP, MTP and PMP and powers the PMP; the MTP stays inactive here. */
static int fdt_stringlist_search(const void *dt,int id,const char *key,const char *value)
{(void)dt;(void)id;(void)key;return !strcmp(value,"/arm-io/aop")||!strcmp(value,"/arm-io/pmp")?0:-1;}
static int fdt_node_check_compatible(const void *dt,int node,const char *compat)
{(void)dt;(void)node;(void)compat;return 0;}
'''

TESTS = r'''
static u8 before[sizeof(bank)];
static void reset(void) {
 memcpy(nodes,original,sizeof(nodes));chip_id=T8132;writable=false;stuck_slice=-1;filter_writes=0;
 bank_base=nodes[0].base[1];
 /* Whatever iBoot left: anything but the ADT's slices. */
 for(unsigned i=0;i<sizeof(bank);i+=4) {u32 v=0x5a000000|i;memcpy(bank+i,&v,4);}
 for(unsigned s=0;s<sizeof(bank)/0x40;s++) {bank[s*0x40]=0x11;bank[s*0x40+1]=bank[s*0x40+2]=bank[s*0x40+3]=0;bank[s*0x40+0x10]&=~3;}
 memcpy(before,bank,sizeof(bank));
}
static void untouched(void){assert(!filter_writes&&!memcmp(bank,before,sizeof(bank)));}
static int program(void) {writable=true;int ret=dapf_init_t8132_halted("/arm-io/dart-aop",1);writable=false;return ret;}
int main(void) {
 const struct dapf_t8110_config *cfg;
 unsigned count;
 /* The ADT's slices land in register order and read back. */
 reset();cfg=(const void *)nodes[0].config;count=nodes[0].config_len/sizeof(*cfg);
 assert(count==16&&!program()&&filter_writes==5*count);
 for(unsigned i=0;i<count;i++) {
  u64 s=bank_base+0x40*i;
  assert(read64(s+0x08)==cfg[i].start&&read64(s+0x10)==(cfg[i].end&~3UL));
  assert(read32(s+0x04)==cfg[i].r4&&read32(s+0x20)==cfg[i].r20);
  assert(read32(s)==(u32)((cfg[i].r0_hi<<4)|cfg[i].r0_lo));
 }
 assert(!memcmp(bank+0x40*count,before+0x40*count,sizeof(bank)-0x40*count));
 /* A slice that does not read back restores every slice. */
 for(int stuck=0;stuck<16;stuck+=5) {
  reset();stuck_slice=stuck;assert(program()<0&&filter_writes==2*5*count);
  assert(!memcmp(bank,before,sizeof(bank)));
 }
 /* No writer for another SoC, a GAPF-only DART, a legacy caller or bad records. */
 reset();chip_id=T8140;assert(program()<0);untouched();
 reset();writable=true;assert(dapf_init_t8132_halted("/arm-io/dart-pmp",1)<0);untouched();
 reset();assert(dapf_init("/arm-io/dart-aop",1)<0);untouched();
 static u8 config[17*52];
 reset();nodes[0].config_len=51;assert(program()<0);untouched();
 reset();memcpy(config,nodes[0].config,16*52);memcpy(config+16*52,nodes[0].config,52);
 nodes[0].config=config;nodes[0].config_len=17*52;assert(program()<0);untouched();
 reset();memcpy(config,nodes[0].config,16*52);nodes[0].config=config;
 u64 start=1;memcpy(config+3*52+8,&start,8);assert(program()<0);untouched();
 /* The J713 caller leaves the AOP to kboot's handoff, even when it is active. */
 reset();assert(dapf_init_j713(&bank)==1);untouched();
 /* The windows, merged to whole pages; a short array or a wrong DART fails. */
 reset();struct dapf_window w[16];int n=dapf_t8132_windows("/arm-io/dart-aop",1,w,16);
 assert(n>0);for(int i=0;i<n;i++)printf("W %lx %lx\n",w[i].start,w[i].end);
 assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,n-1)<0);
 assert(dapf_t8132_windows("/arm-io/dart-pmp",1,w,16)<0);
 chip_id=T8140;assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,16)<0);
 untouched();
 /* A slice that bridges two windows, and one that lands before a window it overlaps.
  * The last two sets grow a window that then reaches the next one. */
 static const u64 spans[][2]={{0x10000,0x13fff},{0x20000,0x23fff},{0xc000,0x2ffff},
                               {0x80000,0x80fff},{0x7c000,0x80000},
                               {0x4000,0x7fff},{0xc000,0xffff},{0x4000,0xffff},
                               {0x10000,0x13fff},{0x20000,0x23fff},{0x12000,0x20fff}};
 for(unsigned i=0;i<11;i++) {
  memcpy(config+52*i,nodes[0].config,52);memcpy(config+52*i,spans[i],8);memcpy(config+52*i+8,spans[i]+1,8);
 }
 reset();nodes[0].config=config;nodes[0].config_len=3*52;
 assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,16)==1&&w[0].start==0xc000&&w[0].end==0x30000);
 reset();nodes[0].config=config+3*52;nodes[0].config_len=2*52;
 assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,16)==1&&w[0].start==0x7c000&&w[0].end==0x84000);
 reset();nodes[0].config=config+5*52;nodes[0].config_len=3*52;
 assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,16)==1&&w[0].start==0x4000&&w[0].end==0x10000);
 reset();nodes[0].config=config+8*52;nodes[0].config_len=3*52;
 assert(dapf_t8132_windows("/arm-io/dart-aop",1,w,16)==1&&w[0].start==0x10000&&w[0].end==0x24000);
 puts("AOP DAPF: writer, read-back, restore, refusals, J713 caller and windows pass");
 return 0;
}
'''


def expected_windows(config):
    windows = []
    for i in range(0, len(config), 52):
        start = int.from_bytes(config[i:i + 8], "little")
        end = int.from_bytes(config[i + 8:i + 16], "little")
        windows.append((start & ~0x3fff, (end + 1 + 0x3fff) & ~0x3fff))
    merged = []
    for lo, hi in sorted(windows):
        if merged and lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def test_j713_aop_dapf(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/dapf.c").read_text()
    production = source[source.index("struct dapf_t8020_config {"):]
    window = block((repo / "src/dapf.h").read_text(), "struct dapf_window {") + ";\n"
    fixture = json.loads((repo / "tests/data/j713-dapf-instance.json").read_text())
    declarations, entries = [], []
    for i, node in enumerate(fixture["nodes"]):
        for label, key in [("inst", "instance_hex"), ("cfg", "filter_hex")]:
            raw = bytes.fromhex(node[key])
            declarations.append(f'static const u8 {label}{i}[] = {{{",".join(map(str, raw))}}};')
        bases = ",".join(r["base"] for r in node["reg"])
        sizes = ",".join(r["size"] for r in node["reg"])
        entries.append(f'{{"{node["name"]}",inst{i},cfg{i},sizeof(inst{i}),sizeof(cfg{i}),'
                       f'{{{bases}}},{{{sizes}}},{len(node["reg"])}}}')
    inputs = "\n".join(declarations) + '\nstatic const struct node original[3]={' + ",".join(entries) + '};\n'
    harness = PREFIX + window + production + inputs + TESTS
    aop = next(n for n in fixture["nodes"] if n["name"] == "aop")
    expected = expected_windows(bytes.fromhex(aop["filter_hex"]))
    cfile, binary = tmp_path / "aop-dapf.c", tmp_path / "aop-dapf"

    def run(text):
        cfile.write_text(text)
        if subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-parameter", "-Wno-unused-function",
                           str(cfile), "-o", str(binary)]).returncode:
            return False
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        windows = [(int(a, 16), int(b, 16))
                   for a, b in re.findall(r"^W ([0-9a-f]+) ([0-9a-f]+)$", result.stdout, re.M)]
        return result.returncode == 0 and windows == expected

    assert run(harness)
    controls = [
        ("read-back", "(read32(slice + 0x00) & 0xff) != want[i].r0 || read32(slice + 0x20) != want[i].r20",
         "(read32(slice + 0x00) & 0xff) != want[i].r0"),
        ("restore", "dapf_t8132_put(base + 0x40 * j, &had[j]);", ";"),
        ("end mask", "(read64(slice + 0x10) & ~3UL) != (want[i].end & ~3UL)",
         "read64(slice + 0x10) != want[i].end"),
        ("slice limit", "length / sizeof(*config) > T8132_DAPF_SLICES", "false"),
        ("slice order", "config[i].end < config[i].start", "false"),
        ("merge", "if (pos > 0 && window.start <= out[pos - 1].end) {", "if (false) {"),
        ("grown merge", "while (pos + 1 < used && out[pos + 1].start <= out[pos].end) {",
         "while (false) {"),
        ("page round", "ALIGN_UP(config[i].end + 1, SZ_16K)", "config[i].end + 1"),
        ("caller skip", 'if (!strcmp(entry->consumer, "/arm-io/aop")) {', "if (false) {"),
    ]
    for title, old, new in controls:
        bad = harness.replace(old, new, 1)
        assert bad != harness, title
        assert not run(bad), title
