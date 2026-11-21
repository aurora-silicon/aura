#!/usr/bin/env python3
"""Exercise the production read-only inherited-controller predicate."""
from pathlib import Path
import os
import shlex
import subprocess
import re
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=False)
source = (root / 'src/usb.c').read_text()
start = source.index('bool usb_atc0_quiescent(')
body = source[start:source.index('\n}', start) + 2]
assert 'write32' not in body and 'set32' not in body and 'clear32' not in body
assert 'dwc_size < DWC3_DALEPENA + 4' in source
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1ULL << (n))
#define GENMASK(msb,lsb) ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define PACKED __attribute__((packed))
typedef uint32_t u32;
typedef uint64_t u64;
#include "usb_dwc3_regs_host.h"
#define T6050 0x6050
#define PIPEHANDLER_MUX_CTRL 0xc
#define PIPEHANDLER_AON_GEN 0x1c
#define PIPEHANDLER_AON_GEN_DWC3_RESET_N BIT(0)
static unsigned chip_id=T6050;
static void *adt;
static bool board=true, used, layout=true;
static int powered=1;
static int phy_powered=1;
static unsigned reads;
static u32 aon=1, mux=0x42, mode=DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE);
static u32 run, halt=DWC3_DSTS_DEVCTRLHLT, events, endpoints;
struct usb_t6050_regs { u64 phy, phy_size, digital, pipe, pipe_size, dwc; };
static bool adt_is_compatible(void *a, int n, const char *s) { (void)a; (void)n; (void)s; return board; }
static bool usb_atc0_used(void) { return used; }
static int pmgr_adt_is_powered(const char *p) { return strstr(p,"atc-phy0") ? phy_powered : powered; }
static int usb_t6050_get_regs(struct usb_t6050_regs *r) { r->dwc=0x100000; r->pipe=0x200000; return layout ? 0 : -1; }
static u32 read32(uintptr_t address) {
    assert(powered == 1 && phy_powered == 1 && layout && board && !used && chip_id == T6050);
    reads++;
    if (address == 0x200000 + PIPEHANDLER_AON_GEN) return aon;
    if (address == 0x200000 + PIPEHANDLER_MUX_CTRL) return mux;
    if (address == 0x100000 + DWC3_GCTL) return mode;
    if (address == 0x100000 + DWC3_DCTL) return run;
    if (address == 0x100000 + DWC3_DSTS) return halt;
    if (address == 0x100000 + DWC3_DEVTEN) return events;
    if (address == 0x100000 + DWC3_DALEPENA) return endpoints;
    assert(0); return 0;
}
'''
cases = r'''
static unsigned count;
static void check(bool expected, bool may_read) {
    reads=0; assert(usb_atc0_quiescent() == expected);
    assert(may_read || !reads); count++;
}
int main(void) {
    powered=-1; check(false,false); powered=0; check(false,false); powered=1;
    phy_powered=-1; check(false,false); phy_powered=0; check(false,false); phy_powered=1;
    layout=false; check(false,false); layout=true;
    board=false; check(false,false); board=true;
    chip_id=0x8103; check(false,false); chip_id=T6050;
    used=true; check(false,false); used=false;
    aon=0; check(true,true); aon=1;
    check(true,true);
    run=DWC3_DCTL_RUN_STOP; check(false,true); run=0;
    halt=0; check(false,true); halt=DWC3_DSTS_DEVCTRLHLT;
    events=1; check(false,true); events=0;
    endpoints=1; check(false,true); endpoints=0;
    mux=0x11; check(false,true); mux=0x22; check(true,true); mux=0x42;
    mode=DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_HOST); check(false,true);
    mode=DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG); check(false,true);
    printf("PASS: %u inherited-controller states, unpowered MMIO refused\n",count);
}
'''
c = out / 'probe.c'
# Keep the production register definitions, but use the host C ABI's pointer
# and size typedefs. Firmware types.h intentionally supplies its own ABI.
(out / 'usb_dwc3_regs_host.h').write_text(
    (root / 'src/usb_dwc3_regs.h').read_text().replace('#include "types.h"', ''))
for name in ('PIPEHANDLER_MUX_CTRL', 'PIPEHANDLER_AON_GEN', 'PIPEHANDLER_AON_GEN_DWC3_RESET_N'):
    actual = re.search(r'^#define ' + name + r'\s+([^\n]+)', source, re.M)
    expected = re.search(r'^#define ' + name + r'\s+([^\n]+)', prefix, re.M)
    assert actual and expected
    a, b = actual[1].strip().lower(), expected[1].strip().lower()
    assert int(a, 0) == int(b, 0) if a.startswith('0x') else a == b
c.write_text(prefix + body + '\n' + cases)
exe = out / 'probe'
subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'src'), str(c), '-o', str(exe)], check=True)
run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
(out / 'RESULT.log').write_text(run.stdout + run.stderr)
print(run.stdout, end='')
