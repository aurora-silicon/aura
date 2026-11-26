# SPDX-License-Identifier: GPL-2.0-only
"""Stage 1's boot watchdog: a plain boot that stops in Stage 1 or early Stage 2 must reset itself.

Ways it can fail, and the check for each:
1. A plain Stage 1 boot leaves every watchdog off (the 11-08/11-09 M2 hangs): main.c must arm it
   after disarming iBoot's, in Stage 1 builds only.
2. Stage 2 (ESP_STAGE2) arms it too: U-Boot and GRUB menus may wait for a person indefinitely.
3. A host that attaches gets reset under it: the first synced window or proxy request disarms it.
4. A host attach disarms something else: a trial watchdog or one a host armed (P_WDT_ARM) stays.
5. Arming on a layout it cannot drive: wdt_arm()'s ADT checks refuse, nothing is written.
6. A bounded host window outlives the watchdog: both windows must be shorter than the period.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def harness(main_body):
    source = (ROOT / 'src/wdt.c').read_text()
    body = source[source.index('#define WDT_COUNT'):]
    # Replace wdt_disable with a counting wrapper by renaming the real one.
    body = body.replace('void wdt_disable(void)', 'static void real_wdt_disable(void)')
    return r'''
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
struct wdt_guard {
    bool borrowed;
    u64 base;
    u32 ctl, count, alarm;
};
static void dc_cvac_range(const void *p, size_t n)
{ (void)p; (void)n; }
static u32 chip_id = 0x8112, adt_version = 2, regs[12], secondary_value, reconfig_value;
static u64 primary = 0x1000, primary_size = 0x4000, secondary_size = 4, reconfig_size = 4;
static bool has_simple_reconfig;
/* m1n1.wdt=keep is not given: wdt_disable_at_start() is wdt_disable() */
static bool boot_wdt_keep_enabled(void) { return false; }
static unsigned writes, disables;
static int adt_dummy;
static void *adt = &adt_dummy;
static void real_wdt_disable(void);
static void wdt_disable(void)
{
    disables++;
    real_wdt_disable();
}
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
        *base = 0x3000;
        if (size) *size = reconfig_size;
        return 0;
    }
    return -1;
}
static u32 read32(u64 addr)
{
    if (addr == 0x2000) return secondary_value;
    if (addr == 0x3000) return reconfig_value;
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
    regs[(addr - primary) / 4] = value;
    writes++;
}
''' + body + '\nint main(void)\n{\n' + main_body + '\n    return 0;\n}\n'


def run(tmp_path, body):
    binary = tmp_path / 'wdt-boot'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-x', 'c', '-', '-o', str(binary)], input=harness(body), text=True,
                   check=True)
    subprocess.run([str(binary)], check=True)


def test_boot_watchdog_arms_on_any_v2_chip_and_a_host_disarms_it(tmp_path):
    run(tmp_path, r'''
    /* T8112 (M2) is neither T8140 nor T6040: the boot watchdog must still arm */
    assert(wdt_arm_boot() == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U && regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    disables = 0;
    wdt_host_attached();
    assert(disables == 1 && regs[WDT_CTL/4] == 0);
    /* once only: later requests leave whatever the host does alone */
    wdt_host_attached();
    assert(disables == 1);
    /* a host-armed watchdog survives further requests */
    chip_id = T8140;
    assert(wdt_arm_seconds(30) == 0);
    wdt_host_attached();
    assert(disables == 1 && regs[WDT_CTL/4] == 4);
    /* T6040 shape: wdt-version 2 with simple-reconfig-wdog-support stays on WD1 */
    chip_id = T6040;
    adt_version = 2;
    has_simple_reconfig = true;
    regs[WDT_WD2_CTL/4] = 0;
    reconfig_value = 0x55u;
    assert(wdt_arm_boot() == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U && regs[WDT_CTL/4] == 4);
    assert(regs[WDT_WD2_CTL/4] == 0 && reconfig_value == 0x55u);
    disables = 0;
    wdt_host_attached();
    assert(disables == 1 && regs[WDT_CTL/4] == 0);
''')


def test_boot_watchdog_arms_wd2_on_v3_simple_reconfig_and_host_disarms_it(tmp_path):
    run(tmp_path, r'''
    adt_version = 3;
    has_simple_reconfig = true;
    assert(wdt_arm_boot() == 0);
    assert(regs[WDT_WD2_BARK/4] == 4272000000U && regs[WDT_WD2_CTL/4] == 1);
    assert((reconfig_value & 0x30u) == 0x10u);
    disables = 0;
    wdt_host_attached();
    assert(disables == 1 && regs[WDT_WD2_CTL/4] == 0);
''')


def test_trial_watchdog_is_not_disarmed_by_a_host(tmp_path):
    run(tmp_path, r'''
    assert(wdt_arm_boot() == 0);
    assert(wdt_arm_trial() == 0);
    disables = 0;
    wdt_host_attached();
    assert(disables == 0 && regs[WDT_CTL/4] == 4);
''')


def test_boot_watchdog_refuses_layouts_it_cannot_drive(tmp_path):
    run(tmp_path, r'''
    adt_version = 3;
    has_simple_reconfig = false;
    writes = disables = 0;
    assert(wdt_arm_boot() == -1 && writes == 0);
    wdt_host_attached();
    assert(disables == 0);
    adt_version = 2;
    secondary_value = 1;
    assert(wdt_arm_boot() == -1 && writes == 0);
''')


def function_body(source, signature):
    start = source.index(signature)
    depth = 0
    for i in range(source.index('{', start), len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(signature)


def test_stage1_arms_it_after_disarming_iboot_and_stage2_does_not():
    body = function_body((ROOT / 'src/main.c').read_text(), 'void aura_main(void)')
    disable = body.index('wdt_disable_at_start();')
    arm = body.index('wdt_arm_boot()')
    assert disable < arm < body.index('run_actions();')
    guard = body.rindex('#if', 0, arm)
    assert re.match(r'#if defined\(CHAINLOADING\) && !defined\(ESP_STAGE2\)', body[guard:])
    assert body.index('#endif', guard) > arm


def test_a_host_disarms_it_as_soon_as_it_is_seen():
    proxy = (ROOT / 'src/uartproxy.c').read_text()
    run_body = function_body(proxy, 'int uartproxy_run(struct uartproxy_msg_start *start)')
    checked = run_body.index('!= request.checksum')
    assert checked < run_body.index('wdt_host_attached();') < run_body.index('switch (request.type)')
    wait = function_body(proxy, 'enum stage1_proxy_result uartproxy_wait_stage1(')
    assert wait.count('wdt_host_attached();') == 2


def test_host_windows_are_shorter_than_the_watchdog():
    config = (ROOT / 'src/stage1_config.h').read_text()
    header = (ROOT / 'src/wdt.h').read_text()
    period = int(re.search(r'#define WDT_BOOT_SECONDS (\d+)', header).group(1))
    for name in ('STAGE1_PROXY_REQUEST_MS', 'STAGE1_RECOVERY_WINDOW_MS'):
        ms = int(re.search(r'#define %s (\d+)' % name, config).group(1))
        assert ms < period * 1000 // 2, name
    # a Stage 1 config window (stage1_config.c caps window_ms) runs under it as well
    cap = int(re.search(r'config\.window_ms > (\d+)', (ROOT / 'src/stage1_config.c').read_text()).group(1))
    assert cap < period * 1000 - 30_000
