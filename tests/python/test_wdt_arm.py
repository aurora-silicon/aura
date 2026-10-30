"""Exercise the production watchdog arming guards and register sequence."""
from pathlib import Path
import subprocess


def test_wdt_arm_seconds(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / 'src/wdt.c').read_text()
    function = source[source.index('static int wdt_arm('):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define T8140 0x8140
#define T6040 0x6040
#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL 0x1c
#define WDT_TRIAL_SECONDS 178
#define ADT_GETPROP(a,n,k,v) (*(v)=adt_version,0)
#define sysop(x) ((void)0)
static u32 chip_id = T6040, adt_version = 2, regs[8], secondary_value;
static u64 primary = 0x1000, primary_size = 0x4000, secondary_size = 4;
static u64 wdt_base;
static unsigned writes;
static bool bad_readback;
static void *adt;
static unsigned disables;
/* The real one clears the primary control and the secondary watchdog */
static void wdt_disable(void)
{
    disables++;
    secondary_value = 0;
}
static int adt_path_offset_trace(void *a, const char *p, int *trace)
{ (void)a; (void)p; (void)trace; return 1; }
static int adt_get_reg(void *a, int *p, const char *key, int i, u64 *base, u64 *size)
{
    (void)a; (void)p; (void)key;
    *base = i == 0 ? primary : 0x2000;
    *size = i == 0 ? primary_size : secondary_size;
    return 0;
}
static u32 read32(u64 addr)
{
    if (addr == 0x2000) return secondary_value;
    assert(addr >= primary && addr < primary + 0x20);
    if (bad_readback && addr == primary + WDT_ALARM) return 0;
    return regs[(addr-primary)/4];
}
static void write32(u64 addr, u32 value)
{
    assert(addr >= primary && addr < primary + 0x20);
    regs[(addr-primary)/4] = value;
    writes++;
}
''' + function + r'''
int main(void)
{
    assert(wdt_arm_seconds(30) == 0);
    assert(regs[WDT_ALARM/4] == 720000000);
    assert(regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(writes == 4 && wdt_base == primary);
    chip_id = T8140;
    assert(wdt_arm_seconds(178) == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U);
    writes = 0;
    assert(wdt_arm_seconds(0) == -1 && writes == 0);
    assert(wdt_arm_seconds(179) == -1 && writes == 0);
    chip_id = 0x8103;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    chip_id = T6040;
    adt_version = 3;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    adt_version = 2;
    primary_size = 0x1f;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    primary_size = 0x4000;
    secondary_size = 8;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_size = 4;
    secondary_value = 1;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_value = 0;
    bad_readback = true;
    assert(wdt_arm_seconds(30) == -1);
    bad_readback = false;

    /* A trial arms on any chip with the version 2 layout, after disarming iBoot's */
    chip_id = 0x6000;
    assert(wdt_arm_seconds(30) == -1);
    secondary_value = 1;
    writes = disables = 0;
    assert(wdt_arm_trial() == 0 && disables == 1);
    assert(regs[WDT_ALARM/4] == 4272000000U && regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(writes == 4 && wdt_base == primary);
    adt_version = 3;
    writes = 0;
    assert(wdt_arm_trial() == -1 && writes == 0);
    adt_version = 2;
    return 0;
}
'''
    binary = tmp_path / 'wdt-arm'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-x', 'c', '-', '-o', str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
