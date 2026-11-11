# SPDX-License-Identifier: MIT
"""Execute the production SD power helper with hostile ADT and SMC responses.

Missing nodes/properties, wrong lengths/provider/function/arguments and failed
SMC initialization/writes must refuse power-up without a settling delay.
Only the validated function may issue the exact enable payload. This does not
model PCIe training, optional-card boot, or electrical card insertion/removal.
"""
from pathlib import Path
import subprocess

from host_compiler import host_cc


def test_sdreader_power(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/pcie_t6050.c").read_text()
    helper = source[source.index("static int sdreader_power("):source.index("/* B-G for one port")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int smc_dev_t;
struct port_regs { char bridge[40]; };
static void *adt;
static int mode, opens, writes, closes, delays;
static u32 function[5];
#define FAIL(...) do { return -1; } while (0)
#define ADT_GETPROP(a,n,p,v) get_phandle(v)
static int get_phandle(u32 *v) { *v = 42; return mode == 5 ? -1 : 4; }
static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/smc/iop-smc-nub/smc-pmu")) return mode == 1 ? -1 : 1;
    assert(!strcmp(path, "/arm-io/apcie0/pci-bridge1/pcie-sdreader"));
    return mode == 2 ? -1 : 2;
}
static void *adt_getprop(void *a, int n, const char *p, u32 *len)
{
    (void)a; assert(n == 2 && !strcmp(p, "function-sd_pwr_en"));
    *len = mode == 4 ? 16 : sizeof(function);
    return mode == 3 ? NULL : function;
}
static smc_dev_t *smc_init(void) { opens++; return mode == 11 ? NULL : &opens; }
static int smc_write_u64(smc_dev_t *s, u32 key, u64 value)
{
    assert(s && key == 0x7063494f && value == 0x0019000000000001ULL);
    writes++; return mode == 12 ? -1 : 0;
}
static void smc_shutdown(smc_dev_t *s) { assert(s); closes++; }
static void mdelay(int ms) { assert(ms == 150 && writes == 1 && closes == 1); delays++; }
''' + helper + r'''
int main(void)
{
    struct port_regs pp = { "/arm-io/apcie0/pci-bridge1" };
    for (mode = 0; mode <= 12; mode++) {
        u32 valid[] = {42, 0, 0x7063494f, 0, 0x190000};
        memcpy(&valid[1], "8WKp", 4);
        memcpy(function, valid, sizeof(valid));
        if (mode >= 6 && mode <= 10) function[mode - 6] ^= 1;
        opens = writes = closes = delays = 0;
        int ret = sdreader_power(&pp);
        assert((ret == 0) == (mode == 0));
        assert(opens == (mode == 0 || mode >= 11));
        assert(writes == (mode == 0 || mode == 12));
        assert(closes == writes);
        assert(delays == (mode == 0));
    }
    return 0;
}
'''
    binary = tmp_path / "sdreader-power"
    subprocess.run([*host_cc(), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
