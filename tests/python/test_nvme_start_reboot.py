# SPDX-License-Identifier: GPL-2.0-only
"""A post-M4 ANS that a Stage 2 couldn't start from off: reboot instead of stopping in the proxy,
while iBoot's record shows fewer failed boots in a row than the limit. The boot counts itself as
failed first, so the bound holds whether or not iBoot counts the reset too; the panic count in the
high nibble is kept; chips where aura clears the record keep the proxy."""

from pathlib import Path
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

# name: (aura clears the record, record read fails, counts byte, count write fails,
#        expected decision, expected counts byte after)
CASES = {
    "first-failure": (False, False, 0x00, False, "reboot", 0x01),
    "second-failure": (False, False, 0x01, False, "reboot", 0x02),
    "limit-reached": (False, False, 0x02, False, "proxy", 0x02),
    "panics-kept": (False, False, 0x31, False, "reboot", 0x32),
    "panics-only": (False, False, 0x70, False, "reboot", 0x71),
    "saturated": (False, False, 0x0F, False, "proxy", 0x0F),
    "cleared-by-aura": (True, False, 0x00, False, "proxy", 0x00),
    "no-record": (False, True, 0x00, False, "proxy", 0x00),
    "write-fails": (False, False, 0x00, True, "proxy", 0x00),
}


def _between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


@pytest.mark.parametrize("case", sorted(CASES))
def test_nvme_start_reboot(tmp_path, case):
    cleared, read_fails, counts, write_fails, expected, after = CASES[case]
    header = (ROOT / "src/pmu_counter.h").read_text()
    payload = (ROOT / "src/payload.c").read_text()
    pmu = (ROOT / "src/pmu_counter.c").read_text()
    helpers = _between(header, "/* The failed-boot count is", "#endif")
    policy = _between(payload, "/*\n * Reboot after a failed start", "int payload_run(void)")
    counter = _between(pmu, "int pmu_count_failed_boot(u8 *before)", "\n}\n") + "\n}\n"
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef struct { int x; } spmi_dev_t;
static spmi_dev_t spmi;
static bool cleared = @CLEARED@, read_fails = @READ_FAILS@, write_fails = @WRITE_FAILS@;
static u8 scratch = @COUNTS@;
static int rebooted;
static bool pmu_counter_cleared_by_aura(void) { return cleared; }
static int pmu_find_scratchpad(char *path, size_t len, u8 *addr, u16 *scrpad)
{
    snprintf(path, len, "/arm-io/nub-spmi0");
    *addr = 0xf;
    *scrpad = 0xf800;
    return 0;
}
static spmi_dev_t *spmi_init(const char *p) { return &spmi; }
static void spmi_shutdown(spmi_dev_t *s) {}
static int spmi_ext_read_long(spmi_dev_t *s, u8 a, u16 r, u8 *v, int n)
{
    if (r != 0xf802 || n != 1)
        return -1;
    *v = scratch;
    return 0;
}
static int spmi_ext_write_long(spmi_dev_t *s, u8 a, u16 r, u8 *v, int n)
{
    if (write_fails || r != 0xf802 || n != 1)
        return -1;
    scratch = *v;
    return 0;
}
static int pmu_read_boot_record(u8 *counts, u8 *error_stage)
{
    if (read_fails)
        return -1;
    *counts = scratch;
    *error_stage = 0x2f;
    return 0;
}
static void flush_and_reboot(void) { rebooted = 1; }
#define PMU_SCRPAD_COUNTS 2
@HELPERS@
@COUNTER@
@POLICY@
int main(void)
{
    payload_nvme_start_failed();
    printf("%s 0x%02x\n", rebooted ? "reboot" : "proxy", scratch);
    return 0;
}
'''
    for key, val in {
        "@CLEARED@": str(cleared).lower(), "@READ_FAILS@": str(read_fails).lower(),
        "@WRITE_FAILS@": str(write_fails).lower(), "@COUNTS@": hex(counts),
        "@HELPERS@": helpers, "@COUNTER@": "static " + counter,
        "@POLICY@": policy,
    }.items():
        harness = harness.replace(key, val)
    src = tmp_path / "t.c"
    src.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", str(src), "-o", str(exe)],
                   check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()[-1].split()
    assert out == [expected, "0x%02x" % after]
