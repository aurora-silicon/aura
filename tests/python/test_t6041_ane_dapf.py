# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production T6040/T6041 ANE DAPF writer on the J616s dart-ane0."""
from pathlib import Path
import struct
import subprocess

from test_j713_dapf_instance import block

# J616s ADT /arm-io/dart-ane0 (26.6.2): reg entries as Linux physical
# addresses, the instance tags naming them, and dapf-instance-0.
REGS = [(0x485800000, 0xc000), (0x485820000, 0xc000), (0x485840000, 0xc000),
        (0x485810000, 0x4000), (0x500578000, 0x4000)]
INSTANCE = bytes.fromhex(
    "5452414400000000444152544c4c540054524144010000004441525442524400"
    "545241440200000044415254425752004650414400000000444150464c4c5400"
    "465041470000000050535f50524f5400")
RANGES = [(0x5022803c0, 0x5022803c3), (0x50228c000, 0x50228c01b),
          (0x581668000, 0x581668003), (0x28a668000, 0x28a668003)]


def record(start, end):
    # start, end, r20, unk1, r4, unk2[5], unk3, r0_hi, r0_lo, unk4: 52 bytes
    return struct.pack("<QQIII5IBBBB", start, end, 1, 0, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0)


def carray(name, raw):
    return f"static const u8 {name}[] = {{{','.join(map(str, raw))}}};\n"


TYPES = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
'''

PREFIX = r'''
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
#define T6040 0x6040
#define T6041 0x6041
#define T8132 0x8132
#define T8140 0x8140
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define SZ_16K (1 << 14)
static int chip_id = T6040;
static void *adt;
static u8 instance[sizeof(instance0)];
static const u32 gates[] = {404};
static int ane_node, powered, enables, disables, enable_error;
static unsigned filter_writes;
static int ranges_len;
static u8 ranges[sizeof(ranges0)];
static int adt_path_offset(const void *tree,const char *p)
{(void)tree;return !strcmp(p,"/arm-io/dart-ane0")?1:-1;}
static int adt_path_offset_trace(const void *tree,const char *p,int *trace)
{int n=adt_path_offset(tree,p);trace[0]=n;return n;}
static const void *adt_getprop(const void *tree,int id,const char *key,u32 *len) {
 (void)tree;assert(id==1);
 if(!strcmp(key,"instance")){*len=sizeof(instance);return instance;}
 if(!strcmp(key,"dapf-instance-0")){if(len)*len=sizeof(config0);return config0;}
 if(!strcmp(key,"clock-gates")){if(len)*len=sizeof(gates);return gates;}
 return NULL;
}
static int adt_get_reg(const void *tree,int *trace,const char *key,int index,u64 *base,u64 *size) {
 (void)tree;(void)key;assert(trace[0]==1);
 if(index<0||index>=(int)(sizeof(regs)/sizeof(regs[0])))return -1;
 *base=regs[index][0];if(size)*size=regs[index][1];return 0;
}
static bool adt_is_compatible(const void *tree,int id,const char *compat)
{(void)tree;(void)id;return !strcmp(compat,"dart,t8110");}
static int pmgr_adt_power_enable(const char *p)
{assert(!strcmp(p,"/arm-io/dart-ane0"));enables++;if(!enable_error)powered=1;return enable_error;}
static int pmgr_adt_power_disable(const char *p)
{assert(!strcmp(p,"/arm-io/dart-ane0"));disables++;powered=0;return 0;}
static int pmgr_adt_is_powered(const char *p){(void)p;return powered;}
/*
 * The FPAD bank, and nothing else: any access outside it (the BRD/BWR
 * +0x10000 hole that reset the J616s) fails the test. Like the hardware,
 * r0 keeps only its low byte and an end its upper bits; stuck_slice's r20
 * ignores writes.
 */
static u8 bank[0x4000];
static const u64 bank_base = 0x485810000;
static int stuck_slice = -1;
static u64 offset(u64 addr,unsigned width) {
 assert(powered==1);assert(addr>=bank_base&&addr+width<=bank_base+sizeof(bank));return addr-bank_base;
}
static u32 read32(u64 addr){u32 v;memcpy(&v,bank+offset(addr,4),4);return v;}
static u64 read64(u64 addr){u64 v;memcpy(&v,bank+offset(addr,8),8);return v;}
static void write32(u64 addr,u32 value) {
 u64 off=offset(addr,4);filter_writes++;
 if(off%0x40==0x00)value&=0xff;
 if(off%0x40==0x20&&(int)(off/0x40)==stuck_slice)return;
 memcpy(bank+off,&value,4);
}
static void write64(u64 addr,u64 value) {
 u64 off=offset(addr,8);filter_writes++;
 if(off%0x40==0x10)value&=~3UL;
 memcpy(bank+off,&value,8);
}
static int fdt_path_offset(const void *dt,const char *p){(void)dt;(void)p;return -1;}
static const void *fdt_getprop(const void *dt,int node,const char *key,int *len) {
 (void)dt;assert(node==ane_node);
 if(strcmp(key,"apple,dapf-ranges")||ranges_len<0)return NULL;
 *len=ranges_len;return ranges;
}
static int fdt_stringlist_search(const void *dt,int id,const char *key,const char *value)
{(void)dt;(void)id;(void)key;(void)value;return -1;}
static int fdt_node_check_compatible(const void *dt,int node,const char *compat)
{(void)dt;(void)node;(void)compat;return 1;}
static int fdt_node_offset_by_compatible(const void *dt,int start,const char *compat)
{(void)dt;assert(start==-1&&!strcmp(compat,"apple,t6041-ane"));return ane_node;}
'''

TESTS = r'''
static u8 before[sizeof(bank)];
static void reset(void) {
 chip_id=T6040;ane_node=7;powered=0;enables=disables=enable_error=0;filter_writes=0;stuck_slice=-1;
 memcpy(instance,instance0,sizeof(instance));memcpy(ranges,ranges0,sizeof(ranges));ranges_len=sizeof(ranges);
 /* Whatever iBoot left: anything but the ADT's records. */
 for(unsigned i=0;i<sizeof(bank);i+=4){u32 v=0x5a000000|i;memcpy(bank+i,&v,4);}
 for(unsigned s=0;s<sizeof(bank)/0x40;s++){bank[s*0x40]=0x11;bank[s*0x40+1]=bank[s*0x40+2]=bank[s*0x40+3]=0;bank[s*0x40+0x10]&=~3;}
 memcpy(before,bank,sizeof(bank));
}
static void untouched(void){assert(!filter_writes&&!enables&&!memcmp(bank,before,sizeof(bank)));}
static void programmed(void) {
 const struct dapf_t8110_config *c=(const void *)config0;
 for(unsigned i=0;i<sizeof(config0)/sizeof(*c);i++) {
  u64 s=0x40*i;u32 r0,r4,r20;u64 start,end;
  memcpy(&r0,bank+s,4);memcpy(&r4,bank+s+4,4);memcpy(&start,bank+s+8,8);
  memcpy(&end,bank+s+0x10,8);memcpy(&r20,bank+s+0x20,4);
  assert(r0==0x31&&r4==0&&start==c[i].start&&end==(c[i].end&~3UL)&&r20==1);
 }
 /* Slices past the ADT's are as iBoot left them. */
 assert(!memcmp(bank+0x40*(sizeof(config0)/sizeof(*c)),before+0x40*(sizeof(config0)/sizeof(*c)),
                sizeof(bank)-0x40*(sizeof(config0)/sizeof(*c))));
}
int main(void) {
 /* The J616s (T6040) and the M4 Max (T6041): programmed, read back, power left on. */
 reset();assert(dapf_init_t6041_ane((void *)1)==0);programmed();assert(enables==1&&!disables&&powered==1);
 reset();chip_id=T6041;assert(dapf_init_t6041_ane((void *)1)==0);programmed();
 /* Already powered by iBoot: same result. */
 reset();powered=1;assert(dapf_init_t6041_ane((void *)1)==0);programmed();assert(!disables&&powered==1);
 /* T8140 (its Linux driver owns the bank) and every other chip: untouched. */
 reset();chip_id=T8140;assert(dapf_init_t6041_ane((void *)1)==0);untouched();
 reset();chip_id=T8132;assert(dapf_init_t6041_ane((void *)1)==0);untouched();
 /* No ANE node, or no DT: nothing to hand over. */
 reset();ane_node=-1;assert(dapf_init_t6041_ane((void *)1)==0);untouched();
 reset();assert(dapf_init_t6041_ane(NULL)==0);untouched();
 /* The DT ranges the OS verifies against must be the ADT's, record for record. */
 reset();ranges[16*2+15]^=4;assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 reset();ranges[16*3+7]^=1;assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 reset();ranges_len-=16;assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 reset();ranges_len=-1;assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 /* Only the FPAD-tagged entry is a bank: none, or two, is refused. */
 reset();memcpy(instance+48,"TRAD",4);assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 reset();memcpy(instance+64,"FPAD",4);assert(dapf_init_t6041_ane((void *)1)<0);untouched();
 /* Power failure: nothing written. */
 reset();enable_error=-1;assert(dapf_init_t6041_ane((void *)1)<0);assert(!filter_writes&&!memcmp(bank,before,sizeof(bank)));
 /* A record that does not read back restores the bank, and powers it down if it was off. */
 reset();stuck_slice=2;assert(dapf_init_t6041_ane((void *)1)<0);
 assert(!memcmp(bank,before,sizeof(bank))&&disables==1&&powered==0);
 reset();powered=1;stuck_slice=1;assert(dapf_init_t6041_ane((void *)1)<0);
 assert(!memcmp(bank,before,sizeof(bank))&&!disables&&powered==1);
 puts("ANE DAPF: T6040/T6041 programmed and read back, T8140 untouched, ranges/instance/power/read-back refusals");
 return 0;
}
'''


def test_t6041_ane_dapf(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/dapf.c").read_text()
    production = source[source.index("struct dapf_t8020_config {"):]
    window = block((repo / "src/dapf.h").read_text(), "struct dapf_window {") + ";\n"
    config = b"".join(record(s, e) for s, e in RANGES)
    ranges = b"".join(struct.pack(">QQ", s, e) for s, e in RANGES)
    inputs = (carray("instance0", INSTANCE) + carray("config0", config) +
              carray("ranges0", ranges) +
              "static const u64 regs[][2] = {" +
              ",".join(f"{{{b:#x},{s:#x}}}" for b, s in REGS) + "};\n")
    harness = TYPES + inputs + PREFIX + window + production + TESTS
    cfile, binary = tmp_path / "ane-dapf.c", tmp_path / "ane-dapf"

    def run(text):
        cfile.write_text(text)
        if subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-parameter", "-Wno-unused-function",
                           str(cfile), "-o", str(binary)]).returncode:
            return None
        return subprocess.run([str(binary)], capture_output=True, text=True).returncode == 0

    assert run(harness)
    controls = [
        ("chip gate", "if (chip_id != T6040 && chip_id != T6041)\n        return 0;",
         "if (false)\n        return 0;"),
        ("range check", "dapf_be64(ranges + 16 * i + 8) != config[i].end", "false"),
        ("range length", "(u32)len != count * 16", "false"),
        ("FPAD only", "if (instances[i].type != DAPF_INSTANCE_DAPF)\n            continue;",
         "if (false)\n            continue;"),
        ("single bank", "if (index >= 0) {\n            printf(\"dapf: %s has more than one",
         "if (false) {\n            printf(\"dapf: %s has more than one"),
        ("power on", "if (gates && pmgr_adt_power_enable(path) < 0) {", "if (false) {"),
        ("power down on failure", "if (gates && powered != 1 && pmgr_adt_power_disable(path))",
         "if (gates && powered != 1 && false)"),
    ]
    for title, old, new in controls:
        bad = harness.replace(old, new, 1)
        assert bad != harness, title
        assert run(bad) is False, title
