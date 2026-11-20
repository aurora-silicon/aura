# SPDX-License-Identifier: GPL-2.0-only
"""A post-M4 ANS that a Stage 2 couldn't start from off: reboot instead of stopping in the proxy,
while the failed-boot count the earlier stages handed on (aura.failed-boots=) is below the limit.
Each stage reads iBoot's count before it clears it and hands on the larger of its own and the one
it was handed; without a handed-on count nothing bounds the reboots, so the proxy stays."""

from pathlib import Path
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

# name: (own read ok, own count byte, var handed on or None, expected decision, expected next var)
CASES = {
    "first-failure": (True, 0x00, "0", "reboot", "aura.failed-boots=0"),
    "second-failure": (True, 0x00, "1", "reboot", "aura.failed-boots=1"),
    "limit-reached": (True, 0x00, "2", "proxy", "aura.failed-boots=2"),
    "own-count-larger": (True, 0x32, "0", "proxy", "aura.failed-boots=2"),
    "panic-nibble-ignored": (True, 0x70, "1", "reboot", "aura.failed-boots=1"),
    "no-var-older-stage-1": (True, 0x00, None, "proxy", "aura.failed-boots=0"),
    "no-var-no-record": (False, 0x00, None, "proxy", "none"),
    "malformed-var": (True, 0x00, "1x", "proxy", "aura.failed-boots=0"),
    "out-of-range-var": (True, 0x00, "16", "proxy", "aura.failed-boots=0"),
    "empty-var": (True, 0x00, "", "proxy", "aura.failed-boots=0"),
}


def _between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


@pytest.mark.parametrize("case", sorted(CASES))
def test_nvme_start_reboot(tmp_path, case):
    read_ok, counts, var, expected, next_var = CASES[case]
    header = (ROOT / "src/pmu_counter.h").read_text()
    payload = (ROOT / "src/payload.c").read_text()
    pmu = (ROOT / "src/pmu_counter.c").read_text()
    helpers = _between(header, "/* The failed-boot count is", "/* The chain's")
    chain = pmu[pmu.index("/*\n * Every aura stage clears iBoot's count"):]
    policy = _between(payload, "/*\n * Reboot after a failed start", "int payload_run(void)")
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
static bool read_ok = @READ_OK@;
static u8 counts = @COUNTS@;
static int rebooted;
static int pmu_read_boot_record(u8 *c, u8 *error_stage)
{
    if (!read_ok)
        return -1;
    *c = counts;
    *error_stage = 0x2f;
    return 0;
}
static void flush_and_reboot(void) { rebooted = 1; }
@HELPERS@
@CHAIN@
@POLICY@
int main(void)
{
    const char *var = @VAR@;
    pmu_snapshot_failed_boots();
    if (var)
        pmu_take_failed_boots_var(var);
    const char *next = pmu_failed_boots_var();
    payload_nvme_start_failed();
    printf("%s %s\n", rebooted ? "reboot" : "proxy", next ? next : "none");
    return 0;
}
'''
    for key, val in {
        "@READ_OK@": str(read_ok).lower(), "@COUNTS@": hex(counts),
        "@VAR@": f'"{var}"' if var is not None else "NULL",
        "@HELPERS@": helpers, "@CHAIN@": chain.replace("\nvoid ", "\nstatic void ").replace(
            "\nbool ", "\nstatic bool ").replace("\nconst char *pmu", "\nstatic const char *pmu"),
        "@POLICY@": policy,
    }.items():
        harness = harness.replace(key, val)
    src = tmp_path / "t.c"
    src.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", str(src),
                    "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1].split() == [expected, next_var]
