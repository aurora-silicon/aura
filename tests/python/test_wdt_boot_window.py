# SPDX-License-Identifier: GPL-2.0-only
"""Stage 1's boot watchdog: a plain boot that stops in Stage 1 or early Stage 2 must reset itself.

Ways it can fail, and the check for each:
1. A plain Stage 1 boot leaves every watchdog off (the 11-08/11-09 M2 hangs): main.c must arm it
   after disarming iBoot's, in Stage 1 builds only.
2. Stage 2 (ESP_STAGE2) arms it too: U-Boot and GRUB menus may wait for a person indefinitely.
3. A host that attaches gets reset under it: the first synced window or proxy request disarms it.
4. A host attach disarms something else: a trial watchdog or one a host armed (P_WDT_ARM) stays.
5. Arming on a layout it cannot drive: wdt_arm()'s version 2 checks refuse, nothing is written.
6. A bounded host window outlives the watchdog: both windows must be shorter than the period.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def harness(main_body):
    source = (ROOT / 'src/wdt.c').read_text()
    function = source[source.index('static int wdt_arm('):]
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define T8140 0x8140
#define T6040 0x6040
#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL 0x1c
#define WDT_TRIAL_SECONDS 178
#define WDT_BOOT_SECONDS 178
#define ADT_GETPROP(a,n,k,v) (*(v)=adt_version,0)
#define sysop(x) ((void)0)
static u32 chip_id = 0x8112, adt_version = 2, regs[8], secondary_value;
static u64 primary = 0x1000, primary_size = 0x4000, secondary_size = 4;
static u64 wdt_base;
static unsigned writes, disables;
static void *adt;
static void wdt_disable(void)
{
    disables++;
    regs[WDT_CTL/4] = 0;
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
    return regs[(addr-primary)/4];
}
static void write32(u64 addr, u32 value)
{
    regs[(addr-primary)/4] = value;
    writes++;
}
''' + function + '\nint main(void)\n{\n' + main_body + '\n    return 0;\n}\n'


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
    disable = body.index('wdt_disable();')
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
    dock = function_body(proxy, 'bool uartproxy_wait_dockchannel(')
    assert 'wdt_host_attached();' in dock


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
