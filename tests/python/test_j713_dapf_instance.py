# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production T8132 selector and consumer power/count/error paths."""
import json
from pathlib import Path
import subprocess


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
#define T8132 0x8132
#define T8140 0x8140
#define PMGR_FLAG_VIRTUAL 0x10
#define PMGR_DEVICE_ID 0xffffU
#define PMGR_DIE_ID 0xffff0000U
#define PMGR_PS_ACTUAL 0xf0U
#define PMGR_PS_ACTIVE 15
#define FIELD_GET(mask,val) (((val)&(mask)) >> __builtin_ctz(mask))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define SZ_16K (1 << 14)
static int chip_id = T8132, pmgr_initialized = 1;
static void *adt;
struct node {
 const char *name;
 const u8 *instance, *config;
 u32 instance_len, config_len;
 const u32 *gates;
 u32 ngates;
 u64 base[3], size[3];
 int nregs;
};
static struct node nodes[3];
static unsigned requested, power_requested, consumer_enables, dart_enables, disables;
static unsigned filter_writes, dart_gate_queries, instance_reads;
static bool handoff, consumer_active, enable_error, dart_gates;
static const u32 fake_gate = 126;
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
 if(!strcmp(key,"clock-gates")) {
  if(id<=3) {dart_gate_queries++;if(!dart_gates)return NULL;if(len)*len=4;return &fake_gate;}
  if(len)*len=n->ngates*4;return n->gates;
 }
 if(!strcmp(key,"instance")) {instance_reads++;if(len)*len=n->instance_len;return n->instance;}
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
static int pmgr_adt_power_enable(const char *p) {
 int id=find(p);assert(id>0);
 if(id<=3) {dart_enables++;return 0;}
 consumer_enables++;if(enable_error)return -1;consumer_active=true;return 0;
}
static int pmgr_adt_power_disable(const char *p){(void)p;disables++;return 0;}
struct pmgr_device {unsigned flags;};
static const struct pmgr_device device;
static int pmgr_find_device(unsigned id,const struct pmgr_device **out)
{assert(id==126||id==127);*out=&device;return 0;}
static uintptr_t pmgr_device_get_addr(unsigned die,const struct pmgr_device *dev)
{assert(!die&&dev==&device);return 0x1000;}
static u32 read32(u64 addr){assert(addr==0x1000);return consumer_active?0xf0:0;}
static u64 read64(u64 addr){(void)addr;assert(chip_id!=T8132);return 0;}
static void write32(u64 addr,u32 value){(void)addr;(void)value;filter_writes++;assert(chip_id!=T8132);}
static void write64(u64 addr,u64 value){(void)addr;(void)value;filter_writes++;assert(chip_id!=T8132);}
static int fdt_path_offset(const void *dt,const char *p){(void)dt;(void)p;return 1;}
static const void *fdt_getprop(const void *dt,int id,const char *key,void *len)
{(void)dt;(void)id;(void)key;(void)len;return handoff?&fake_gate:NULL;}
static int fdt_stringlist_search(const void *dt,int id,const char *key,const char *value) {
 (void)dt;(void)id;int n=find(value);if(n<4)return -1;
 return ((!strcmp(key,"linux-enablement-mac,dapf-handoff")?requested:power_requested)&(1U<<(n-4)))?0:-1;
}
static int fdt_node_check_compatible(const void *dt,int node,const char *compat)
{(void)dt;(void)node;(void)compat;return 0;}
'''

TESTS = r'''
static void reset(void) {
 memcpy(nodes,original,sizeof(nodes));chip_id=T8132;requested=7;power_requested=4;
 consumer_enables=dart_enables=disables=filter_writes=dart_gate_queries=instance_reads=0;
 handoff=true;consumer_active=enable_error=dart_gates=false;
}
static void preserved(void) {assert(!dart_enables&&!disables&&!filter_writes&&!dart_gate_queries);}
static void rejected(void) {assert(dapf_init("/arm-io/dart-pmp",1)<0);preserved();}
int main(void) {
 /* Actual PMP metadata has no DAPF. Retain its independent consumer power. */
 reset();assert(dapf_init_j713((void *)1)==1);
 assert(consumer_enables==1&&consumer_active&&instance_reads==1);preserved();
 reset();assert(!dapf_init("/arm-io/dart-pmp",1));preserved();
 assert(dapf_init("/arm-io/dart-pmp",0)<0);preserved();
 assert(dapf_init("/arm-io/dart-pmp",-1)<0);preserved();
 assert(dapf_init("/arm-io/dart-pmp",2)<0);preserved();
 /* Direct/proxy-equivalent calls cannot admit the known old genuine writer. */
 reset();dart_gates=true;assert(dapf_init("/arm-io/dart-aop",1)<0);preserved();
 assert(dapf_init("/arm-io/dart-mtp",1)<0);preserved();
 assert(dapf_init("/arm-io/dart-aop",2)<0);preserved();
 struct dapf_instance scratch[4];
 /* Missing, empty, partial and ambiguous records never mean absence. */
 reset();nodes[2].instance=NULL;rejected();
 for(unsigned len=0;len<48;len++) {
  if(len==32)continue;
  reset();nodes[2].instance_len=len;rejected();
 }
 reset();memcpy(scratch,nodes[2].instance,32);nodes[2].instance=(const u8 *)scratch;
 scratch[1]=scratch[0];rejected();
 for(unsigned duplicate=0;duplicate<2;duplicate++) {
  reset();memcpy(scratch,nodes[2].instance,32);scratch[2]=scratch[duplicate];
  nodes[2].instance=(const u8 *)scratch;nodes[2].instance_len=48;nodes[2].nregs=3;
  nodes[2].base[2]=0x390000000;nodes[2].size[2]=nodes[2].size[duplicate];rejected();
 }
 reset();memcpy(scratch,nodes[2].instance,32);nodes[2].instance=(const u8 *)scratch;
 scratch[0].type=0x12345678;rejected();
 reset();memcpy(scratch,nodes[2].instance,32);nodes[2].instance=(const u8 *)scratch;
 scratch[0].type=DAPF_INSTANCE_GAPF;scratch[0].instance=1;rejected();
 /* Every described tuple must exist, even after the selected tuple. */
 reset();nodes[2].nregs=1;rejected();
 reset();memcpy(scratch,nodes[2].instance,32);scratch[2]=scratch[1];scratch[2].instance=1;
 nodes[2].instance=(const u8 *)scratch;nodes[2].instance_len=48;rejected();
 reset();nodes[2].base[1]++;rejected();
 reset();nodes[2].size[1]=0x8000;rejected();
 reset();nodes[2].size[0]=0;rejected();
 reset();nodes[2].base[0]=UINT64_MAX-0x100;rejected();
 /* Tuple position follows the type record; there is no fixed reg1 fallback. */
 reset();memcpy(scratch,nodes[2].instance,32);struct dapf_instance swap=scratch[0];
 scratch[0]=scratch[1];scratch[1]=swap;nodes[2].instance=(const u8 *)scratch;
 u64 base=nodes[2].base[0],size=nodes[2].size[0];
 nodes[2].base[0]=nodes[2].base[1];nodes[2].size[0]=nodes[2].size[1];
 nodes[2].base[1]=base;nodes[2].size[1]=size;
 assert(!dapf_init("/arm-io/dart-pmp",0));preserved();
 assert(dapf_init("/arm-io/dart-pmp",1)<0);preserved();
 /* Actual J713 caller return count/error semantics, including failed power. */
 reset();enable_error=true;assert(dapf_init_j713((void *)1)<0);
 assert(consumer_enables==1&&!consumer_active&&!instance_reads);preserved();
 reset();power_requested=0;assert(!dapf_init_j713((void *)1));
 assert(!consumer_enables&&!instance_reads);preserved();
 reset();power_requested=0;consumer_active=true;assert(dapf_init_j713((void *)1)==1);
 assert(!consumer_enables&&instance_reads==1);preserved();
 reset();nodes[2].instance_len=31;assert(dapf_init_j713((void *)1)<0);
 assert(consumer_enables==1&&consumer_active);preserved();
 reset();memcpy(scratch,nodes[2].instance,32);scratch[1].type=DAPF_INSTANCE_DAPF;
 nodes[2].instance=(const u8 *)scratch;assert(dapf_init_j713((void *)1)<0);
 assert(consumer_enables==1&&consumer_active);preserved();
 reset();handoff=false;nodes[2].instance_len=31;assert(!dapf_init_j713((void *)1));
 assert(!consumer_enables&&!instance_reads);preserved();
 /* The central T8132 branch is the only change to the legacy flow. */
 reset();chip_id=0x8110;dart_gates=true;assert(!dapf_init("/arm-io/dart-aop",1));
 assert(filter_writes==5*(nodes[0].config_len/52)&&dart_enables==1&&disables==1);
 puts("DAPF: actual PMP preservation/power, inactive AOP/MTP, typed refusal, malformed/duplicate metadata, direct calls, caller count/errors and legacy pass");
 return 0;
}
'''


def test_j713_instance_and_power_flow(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/dapf.c").read_text()
    production = source[source.index("struct dapf_t8020_config {"):]
    pmgr = block((repo / "src/pmgr.c").read_text(), "int pmgr_adt_is_powered(")
    fixture = json.loads((repo / "tests/data/j713-dapf-instance.json").read_text())
    declarations, entries = [], []
    for i, node in enumerate(fixture["nodes"]):
        for label, key in [("inst", "instance_hex"), ("cfg", "filter_hex")]:
            raw = bytes.fromhex(node[key])
            declarations.append(f'static const u8 {label}{i}[] = {{{",".join(map(str, raw))}}};')
        raw = bytes.fromhex(node["consumer_clock_gate_hex"])
        gates = [int.from_bytes(raw[j:j+4], "little") for j in range(0, len(raw), 4)]
        declarations.append(f'static const u32 gates{i}[] = {{{",".join(map(str, gates)) or "0"}}};')
        bases = ",".join(r["base"] for r in node["reg"])
        sizes = ",".join(r["size"] for r in node["reg"])
        entries.append(f'{{"{node["name"]}",inst{i},cfg{i},sizeof(inst{i}),sizeof(cfg{i}),'
                       f'gates{i},{len(gates)},{{{bases}}},{{{sizes}}},{len(node["reg"])}}}')
    inputs = "\n".join(declarations) + '\nstatic const struct node original[3]={' + ",".join(entries) + '};\n'
    window = block((repo / "src/dapf.h").read_text(), "struct dapf_window {") + ";\n"
    harness = PREFIX + window + pmgr + "\n" + production + inputs + TESTS
    cfile, binary = tmp_path / "dapf-instance.c", tmp_path / "dapf-instance"

    def run(text, negative=False, label="production"):
        cfile.write_text(text)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-misleading-indentation",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert (result.returncode != 0) if negative else (result.returncode == 0), (label, result.stderr)

    run(harness)
    controls = [
        ("central admission", 'if (chip_id == T8132) {\n        u32 instance;', 'if (false) {\n        u32 instance;'),
        ("partial records", 'length % sizeof(*instances) ||', 'false ||'),
        ("duplicate records", 'instances[i].type == instances[j].type &&', 'false &&'),
        ("false absence", '!has_dapf && instances[index].type == DAPF_INSTANCE_GAPF',
         '(!has_dapf || true) && instances[index].type == DAPF_INSTANCE_GAPF'),
        ("resource bounds", '(base & 0x3fff) || size != 0x4000', 'false'),
        ("consumer power", 'if (pmgr_adt_power_enable(entry->consumer) < 0)', 'if (false)'),
        ("caller count", 'count += 1;', 'count += 0;'),
        ("inactive consumer", 'if (powered <= 0)', 'if (powered <= 0 && false)'),
    ]
    for title, old, new in controls:
        bad = harness.replace(old, new, 1)
        assert bad != harness, title
        run(bad, True, title)
