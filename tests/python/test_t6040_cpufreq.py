from pathlib import Path
import subprocess
import struct

from proxyclient.m1n1.proxy import M1N1Proxy


def test_t6040_low_state_controller(tmp_path):
    source = (Path(__file__).parents[2] / 'src/cpufreq_t6040.c').read_text()
    source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define BIT(n) (1ULL << (n))
#define T6040 0x6040
u32 chip_id = T6040, board_id = 6;
void *adt;
static u32 opps[] = {1260000,790,1512000,810,1800000,835};
static u64 raw[3];
static int writes, malformed, timeout_after_write;
static int adt_path_offset_trace(void *a,const char *p,int *trace) { return 1; }
static bool adt_is_compatible(void *a,int n,const char *c) { return malformed != 1; }
static const void *adt_getprop(void *a,int n,const char *p,u32 *len) {
    if (!strcmp(p,"reg")) { *len=48;return opps; }
    *len=sizeof(opps);return opps;
}
static int adt_get_reg(void *a,int *path,const char *p,int idx,u64 *base,u64 *size) {
    *base=0x210e20000ULL+idx*0x1000000ULL;*size=malformed==2?0x1000:0x2000;return 0;
}
static unsigned int index_for(u64 addr) { return (addr-0x210e20020ULL)/0x1000000ULL; }
static u64 read64(u64 addr) { return raw[index_for(addr)]; }
static void write64(u64 addr,u64 value) {
    writes++;raw[index_for(addr)]=value & ~BIT(25);
    if(timeout_after_write)raw[index_for(addr)]|=BIT(31);
}
static int poll64(u64 addr,u64 mask,u64 wanted,unsigned int us) {
    return (read64(addr)&mask)==wanted?0:-1;
}
''' + source + r'''
static void reset(void) {
    ready=false;memset(clocks,0,sizeof(clocks));
    chip_id=T6040;board_id=6;malformed=0;writes=0;timeout_after_write=0;
    opps[0]=1260000;opps[2]=1512000;
    for(int i=0;i<3;i++)raw[i]=0x400101;
}
int main(void) {
    reset();assert(cpufreq_t6040_init()==0);assert(writes==0);
    for(int i=0;i<3;i++) {
        assert(cpufreq_t6040_get_hz(i)==1260000000ULL);
        assert(cpufreq_t6040_set_pstate(i,2)==0);
        assert(raw[i]==0x400102);
        assert(cpufreq_t6040_get_hz(i)==1512000000ULL);
        assert(cpufreq_t6040_set_pstate(i,1)==0);
        assert(raw[i]==0x400101);
    }
    int before=writes;
    assert(cpufreq_t6040_set_pstate(3,1)==-1);
    assert(cpufreq_t6040_set_pstate(0,0)==-1);
    assert(cpufreq_t6040_set_pstate(0,3)==-1);
    assert(writes==before);
    reset();chip_id=0x6041;assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();board_id=5;assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();malformed=1;assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();malformed=2;assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();opps[2]=opps[0];assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();raw[0]|=BIT(31);assert(cpufreq_t6040_init()==-1);assert(writes==0);
    reset();assert(cpufreq_t6040_init()==0);raw[0]=0x400102;
    assert(cpufreq_t6040_set_pstate(0,1)==-1);assert(writes==0);
    raw[0]=0x400101;assert(cpufreq_t6040_set_pstate(0,2)==-1);assert(writes==0);
    reset();assert(cpufreq_t6040_init()==0);timeout_after_write=1;
    assert(cpufreq_t6040_set_pstate(0,2)==-1);assert(writes==1);
    timeout_after_write=0;raw[0]=0x400102;
    assert(cpufreq_t6040_set_pstate(0,1)==-1);assert(writes==1);
    assert(cpufreq_t6040_get_hz(0)==0);
    return 0;
}
'''
    path = tmp_path/'clock.c'
    path.write_text(harness)
    binary = tmp_path/'clock'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-Werror',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_cpufreq_wire_opcodes_and_signed_rejection():
    class Interface:
        requests = []
        def proxyreq(self, req, **kwargs):
            opcode,*args=struct.unpack('<7Q',req)
            self.requests.append((opcode,args[:2]))
            return struct.pack('<QqQ',opcode,0,0xffffffffffffffff if opcode==0x1302 else 1260000000)
    iface=Interface()
    p=M1N1Proxy(iface)
    assert p.cpufreq_get_cluster_hz(1)==1260000000
    assert p.cpufreq_set_cluster_pstate(1,3)==-1
    assert iface.requests==[(0x1301,[1,0]),(0x1302,[1,3])]
