"""Exercise the production watchdog arming, disable and reboot paths."""
from pathlib import Path
import subprocess


def test_wdt_arm_seconds(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / 'src/wdt.c').read_text()
    body = source[source.index('#define WDT_COUNT'):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x) (1u << (x))
#define T8140 0x8140
#define T6040 0x6040
#define WDT_TRIAL_SECONDS 178
#define WDT_BOOT_SECONDS 178
#define ADT_GETPROP(a,n,k,v) (*(v)=adt_version,0)
#define sysop(x) ((void)0)
static u32 chip_id = T6040, adt_version = 2, regs[12], secondary_value, reconfig_value;
static u64 primary = 0x1000, primary_size = 0x4000, secondary_size = 4, reconfig_size = 4;
static bool has_simple_reconfig, has_reg3 = true;
static unsigned writes;
static bool bad_readback;
static int adt_dummy;
static void *adt = &adt_dummy;
static int adt_path_offset_trace(void *a, const char *p, int *trace)
{ (void)a; (void)p; (void)trace; return 1; }
static const void *adt_getprop(void *a, int node, const char *key, u32 *len)
{
    (void)a; (void)node; (void)len;
    if (!strcmp(key, "simple-reconfig-wdog-support"))
        return has_simple_reconfig ? (const void *)1 : NULL;
    return NULL;
}
static int adt_get_reg(void *a, int *p, const char *key, int i, u64 *base, u64 *size)
{
    (void)a; (void)p; (void)key;
    if (i == 0) {
        *base = primary;
        if (size) *size = primary_size;
        return 0;
    }
    if (i == 2) {
        *base = 0x2000;
        if (size) *size = secondary_size;
        return 0;
    }
    if (i == 3) {
        if (!has_reg3) return -1;
        *base = 0x3000;
        if (size) *size = reconfig_size;
        return 0;
    }
    return -1;
}
static u32 read32(u64 addr)
{
    if (addr == 0x2000) return secondary_value;
    if (addr == 0x3000) return bad_readback ? 0 : reconfig_value;
    assert(addr >= primary && addr < primary + 0x30);
    if (bad_readback && (addr == primary + 0x14 || addr == primary + 0x28)) return 0;
    return regs[(addr - primary) / 4];
}
static void write32(u64 addr, u32 value)
{
    if (addr == 0x2000) {
        secondary_value = value;
        writes++;
        return;
    }
    if (addr == 0x3000) {
        reconfig_value = value;
        writes++;
        return;
    }
    assert(addr >= primary && addr < primary + 0x30);
    regs[(addr - primary) / 4] = value;
    writes++;
}
''' + body + r'''
int main(void)
{
    /* wdt-version 2: WD1 RESET_EN path unchanged */
    assert(wdt_arm_seconds(30) == 0);
    assert(regs[WDT_ALARM/4] == 720000000);
    assert(regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(writes == 4 && wdt_base == primary && wdt_reconfig == 0);
    chip_id = T8140;
    assert(wdt_arm_seconds(178) == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U);
    writes = 0;
    assert(wdt_arm_seconds(0) == -1 && writes == 0);
    assert(wdt_arm_seconds(179) == -1 && writes == 0);
    chip_id = 0x8103;
    assert(wdt_arm_seconds(30) == 0 && writes == 4);

    /* wdt-version 3 WITHOUT simple-reconfig-wdog-support refuses */
    writes = 0;
    adt_version = 3;
    has_simple_reconfig = false;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);

    /* wdt-version 3 WITH simple-reconfig-wdog-support + reg[3]: WD2 bark-IRQ + reg[3]=0x10 */
    has_simple_reconfig = true;
    reconfig_value = 0x000000a5u; /* unrelated bits preserved, bits [5:4] -> 01 */
    writes = 0;
    assert(wdt_arm_seconds(30) == 0);
    assert(regs[WDT_WD2_BITE/4] == 0xffffffffu);
    assert(regs[WDT_WD2_BARK/4] == 720000000u);
    assert(regs[WDT_WD2_CTL/4] == 1 && regs[WDT_WD2_COUNT/4] == 0);
    assert(reconfig_value == 0x00000095u);
    assert(wdt_reconfig == 0x3000);

    /* wdt_disable clears WD1, WD2, and reg[2] */
    regs[WDT_CTL/4] = 4;
    secondary_value = 1;
    wdt_disable();
    assert(regs[WDT_CTL/4] == 0 && regs[WDT_WD2_CTL/4] == 0 && secondary_value == 0);

    /* wdt_reboot uses WD2 + reg[3] even on an early fault before wdt_disable() */
    wdt_base = 0;
    wdt_reconfig = 0;
    reconfig_value = 0;
    wdt_reboot();
    assert(regs[WDT_WD2_BARK/4] == 0x100000u && regs[WDT_WD2_BITE/4] == 0xffffffffu);
    assert(regs[WDT_WD2_CTL/4] == 1 && reconfig_value == 0x10u);

    /* Missing reg[3], bad reg[3] size, or short reg[0] refuses on simple-reconfig */
    writes = 0;
    has_reg3 = false;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    has_reg3 = true;
    reconfig_size = 8;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    reconfig_size = 4;
    primary_size = 0x20;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    primary_size = 0x4000;
    /* Read-back failure clears WD2_CTL so the watchdog is never left armed */
    bad_readback = true;
    assert(wdt_arm_seconds(30) == -1 && regs[WDT_WD2_CTL/4] == 0);
    bad_readback = false;

    /* Trial arms WD2 on v3 simple-reconfig after disarming iBoot's */
    secondary_value = 1;
    writes = 0;
    assert(wdt_arm_trial() == 0);
    assert(regs[WDT_WD2_BARK/4] == 4272000000U && regs[WDT_WD2_CTL/4] == 1);
    assert((reconfig_value & 0x30u) == 0x10u && secondary_value == 0);

    /* Back to v2 checks */
    adt_version = 2;
    has_simple_reconfig = false;
    primary_size = 0x1f;
    writes = 0;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    primary_size = 0x4000;
    secondary_size = 8;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_size = 4;
    secondary_value = 1;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_value = 0;
    bad_readback = true;
    assert(wdt_arm_seconds(30) == -1 && regs[WDT_CTL/4] == 0);
    bad_readback = false;

    /* Trial arms WD1 on v2 */
    chip_id = 0x6000;
    secondary_value = 1;
    writes = 0;
    assert(wdt_arm_trial() == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U && regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(secondary_value == 0 && wdt_base == primary && wdt_reconfig == 0);

    /* Early reboot on v2 uses WD1 */
    wdt_base = 0;
    wdt_reboot();
    assert(regs[WDT_ALARM/4] == 0x100000u && regs[WDT_CTL/4] == 4);
    return 0;
}
'''
    binary = tmp_path / 'wdt-arm'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-x', 'c', '-', '-o', str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
