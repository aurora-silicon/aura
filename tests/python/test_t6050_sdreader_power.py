# SPDX-License-Identifier: MIT
"""Exercise ADT-selected reader power and the shared SMC function decoder.

Missing properties must allow link adoption. Bad lengths, opcodes and providers
must refuse power-up. Transport failures must prevent the settle delay and close
any opened SMC session. Valid calls must preserve ADT keys and argument words.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc


ROOT = Path(__file__).resolve().parents[2]


def test_sdreader_power(tmp_path):
    smc = (ROOT / "src/smc.c").read_text()
    pcie = (ROOT / "src/pcie_t6050.c").read_text()
    decoder = smc[smc.index("struct smc_adt_function {"):]
    helper = pcie[pcie.index("static int sdreader_power("):pcie.index("/* B-G for one port")]
    port_up = pcie[pcie.index("static int port_up("):pcie.index("/* Renumber the up ports")]
    assert "TRY(sdreader_power(pp));\n    if (read32" in port_up

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
static int opens, writes, closes, delays, bits;
static bool present, provider, init_fails, write_fails;
static u32 function[5], len, key, expected_delay;
static u64 values[2];

#define BRIDGE "/arm-io/apcie0/pci-bridge3"
#define FAIL(...) do { return -1; } while (0)
#define ADT_GETPROP(a,n,p,v) get_phandle(n, p, v)
#define ADT_FOREACH_CHILD(a,n) for ((n) = 3; (n) <= 4; (n)++)
static int get_phandle(int n, const char *p, u32 *v)
{
    assert(n == 1 && !strcmp(p, "AAPL,phandle"));
    *v = 42;
    return provider ? 4 : -1;
}
static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/smc/iop-smc-nub/smc-pmu")) return provider ? 1 : -1;
    if (!strcmp(path, BRIDGE)) return 2;
    if (!strcmp(path, BRIDGE "/card-reader")) return 4;
    return -1;
}
static const char *adt_get_name(void *a, int n)
{
    (void)a;
    assert(n == 4);
    return "card-reader";
}
static const void *adt_getprop(void *a, int n, const char *p, u32 *size)
{
    (void)a;
    if (!strcmp(p, "function-sd_pwr_en")) {
        if (n != 4) return NULL;
    } else {
        assert(n == 2 && !strcmp(p, "function-reset"));
    }
    if (!present) return NULL;
    if (size) *size = len;
    return function;
}
static smc_dev_t *smc_init(void) { opens++; return init_fails ? NULL : &opens; }
static int record_write(smc_dev_t *s, u32 k, u64 v, int width)
{
    assert(s && writes < 2);
    key = k;
    bits = width;
    values[writes++] = v;
    return write_fails ? -1 : 0;
}
static int smc_write_u32(smc_dev_t *s, u32 k, u32 v) { return record_write(s, k, v, 32); }
static int smc_write_u64(smc_dev_t *s, u32 k, u64 v) { return record_write(s, k, v, 64); }
static void smc_shutdown(smc_dev_t *s) { assert(s); closes++; }
static void mdelay(u32 ms)
{
    assert(ms == expected_delay && writes == delays + 1);
    assert(closes == (ms == 150));
    delays++;
}
''' + decoder + helper + r'''
static void setup(bool wide)
{
    const u32 args[] = {42, 0, 0x53645077, 0x100, 0x234567};
    memcpy(function, args, sizeof(args));
    memcpy(&function[1], wide ? "8WKp" : "4WKp", 4);
    len = wide ? 20 : 16;
    opens = writes = closes = delays = 0;
    present = provider = true;
    init_fails = write_fails = false;
    expected_delay = 150;
}
static void check_quiet(void)
{
    assert(!opens && !writes && !closes && !delays);
}
int main(void)
{
    struct port_regs pp = { BRIDGE };

    setup(true);
    present = false;
    assert(sdreader_power(&pp) == 1);
    assert(smc_adt_function_set(BRIDGE "/missing", "function-sd_pwr_en", 1) == 1);
    check_quiet();

    for (int mode = 0; mode < 5; mode++) {
        setup(true);
        if (mode == 0) len = 8;
        if (mode == 1) len = 16;
        if (mode == 2) function[1] ^= 1;
        if (mode == 3) function[0] ^= 1;
        if (mode == 4) provider = false;
        assert(sdreader_power(&pp) == -1);
        check_quiet();
    }

    setup(true);
    init_fails = true;
    assert(sdreader_power(&pp) == -1);
    assert(opens == 1 && !writes && !closes && !delays);

    setup(true);
    write_fails = true;
    assert(sdreader_power(&pp) == -1);
    assert(opens == 1 && writes == 1 && closes == 1 && !delays);

    for (int wide = 0; wide <= 1; wide++) {
        setup(wide);
        assert(sdreader_power(&pp) == 0);
        assert(opens == 1 && writes == 1 && closes == 1 && delays == 1);
        assert(bits == (wide ? 64 : 32) && key == function[2]);
        assert(values[0] == (wide ? 0x0023456700000101ULL : 0x101));

        setup(wide);
        assert(smc_adt_function_set(BRIDGE "/card-reader", "function-sd_pwr_en", 0) == 0);
        assert(opens == 1 && writes == 1 && closes == 1 && !delays);
        assert(values[0] == (wide ? 0x0023456700000100ULL : 0x100));

        setup(wide);
        expected_delay = 50;
        assert(smc_adt_function_reset(BRIDGE, 50) == 0);
        assert(opens == 1 && writes == 2 && closes == 1 && delays == 2);
        assert(bits == (wide ? 64 : 32) && key == function[2]);
        assert(values[0] == (wide ? 0x0023456700000100ULL : 0x100));
        assert(values[1] == values[0] + 1);
    }

    setup(true);
    len = 16;
    assert(smc_adt_function_reset(BRIDGE, 50) == 1);
    check_quiet();
    return 0;
}
'''
    binary = tmp_path / "sdreader-power"
    subprocess.run([*host_cc(), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
