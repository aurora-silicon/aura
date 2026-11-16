#!/usr/bin/env python3
"""Run the production admission policy against all debug/stop/DMA combinations."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=False)


def function(text, name):
    start = text.index(name + '(')
    start = text.rfind('\n', 0, start) + 1
    return text[start:text.index('\n}', start) + 2]


kboot = (root / 'src/kboot.c').read_text()
usb = (root / 'src/usb.c').read_text()
main = (root / 'src/main.c').read_text()
# A cleanup can unregister USB0 before main's later stop, and display failure
# can return to proxy. These ordering invariants are part of the contract.
assert main.index('kboot_atc0_reserved()') < main.index('usb_cdc_cleanup();')
assert main.index('kboot_finalize_atc0(') > main.index('display_shutdown(DCP_SLEEP_IF_EXTERNAL)')
assert main.index('kboot_finalize_atc0(') < main.index('mmu_shutdown();')
assert function(usb, 'usb_iodev_bringup').index('atc0_controller_used = true') < function(usb, 'usb_iodev_bringup').index('usb_dart_init(')
assert function(usb, 'usb_t6050_init').index('atc0_controller_used = true') < function(usb, 'usb_t6050_init').index('usb_t6050_get_regs(')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define DEBUGUSB_PROXY 1
typedef unsigned long u64;
struct usb_dwc3_dma_range { unsigned long base, size; };
static bool debug, console, configured, used, host, overflow;
static size_t retained;
static void *dt = (void *)0x1000;
static int dt_bufsize = 65536;
static struct { bool entry; uintptr_t args[4]; } next_stage;
static unsigned calls;
static bool allowed;
static int helper_result;
static const char *kboot_serial_console_reason(void) { return configured ? "config" : NULL; }
static bool aura_debug_enabled(void) { return debug; }
static bool aura_console_enabled(void) { return console; }
static bool usb_atc0_used(void) { return used; }
static bool dockchannel_uart_host_seen(void) { return host; }
static int kboot_hibernate_handoff_size(const void *p) { return p == (void *)0x3000 ? 32768 : 0; }
static size_t usb_dwc3_retained(const struct usb_dwc3_dma_range **p, bool *o) { *p = NULL; *o = overflow; return retained; }
static int kboot_handoff_atc0(void *p, int size, bool safe) {
    assert((p == dt && size == dt_bufsize) || (p == (void *)0x3000 && size == 32768));
    calls++; allowed = safe; return helper_result;
}
static void dc_cvac_range(void *p, size_t len) { (void)p; assert(len > 0); }
static int fdt_totalsize(const void *p) { (void)p; return 100; }
#define sysop(x) ((void)0)
'''
cases = r'''
int main(void) {
    unsigned count = 0;
    next_stage.entry = true; next_stage.args[0] = (uintptr_t)dt;
    for (unsigned bits = 0; bits < 512; bits++) {
        debug=bits&1; console=bits&2; configured=bits&4; used=bits&8; host=bits&16;
        bool stopped=bits&32, captured=bits&64;
        retained=!!(bits&128); overflow=bits&256;
        calls=0;
        assert(!kboot_finalize_atc0(stopped, captured));
        assert(calls == 1);
        assert(allowed == (stopped && !captured && !debug && !console && !configured && !used && !host && !retained && !overflow));
        count++;
    }
    debug=console=configured=used=host=overflow=false; retained=0;
    calls=0; next_stage.args[0] = 0xdead;
    assert(!kboot_finalize_atc0(true, false) && calls == 0);
    next_stage.args[0] = 0x3000;
    assert(!kboot_finalize_atc0(true, false) && calls == 1 && allowed);
    next_stage.entry=false; calls=0;
    assert(!kboot_finalize_atc0(true, false) && calls == 0);
    next_stage.entry=true; next_stage.args[0]=(uintptr_t)dt; helper_result=-1;
    assert(kboot_finalize_atc0(true, false) < 0);
    printf("PASS: %u production policy combinations + kernel/hibernate identity and failure gates\n", count);
}
'''
c = out / 'policy.c'
c.write_text(prefix + function(kboot, 'kboot_atc0_reserved') + '\n' +
             function(kboot, 'kboot_finalize_atc0') + '\n' + cases)
exe = out / 'policy'
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
(out / 'RESULT.log').write_text(run.stdout + run.stderr)
print(run.stdout, end='')
