# SPDX-License-Identifier: GPL-2.0-only
"""A post-M4 ANS that a Stage 2 couldn't start from off: reboot instead of stopping in the proxy,
while the failed-boot count the earlier stages handed on (aura.failed-boots=) is below the limit.
Each stage reads iBoot's count at start and hands on the larger of its own and the one it was
handed; without a handed-on count nothing bounds the reboots, so the proxy stays.

No stage clears the count before the chain leaves aura (pmu_chain_leave()): the multi-boot cases
below run the real stage code boot after boot against a model of iBoot's count, which goes up by
one for every boot that ends in a reset before the chain left aura."""

from pathlib import Path
import re
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


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
static bool read_ok = true;
static u8 counts;
static int rebooted, clears;
static int pmu_read_boot_record(u8 *c, u8 *error_stage)
{
    if (!read_ok)
        return -1;
    *c = counts;
    *error_stage = 0x2f;
    return 0;
}
static int pmu_clear_panic_counter(void)
{
    clears++;
    counts = 0;
    return 0;
}
static void flush_and_reboot(void) { rebooted = 1; }
@HELPERS@
@CHAIN@
@POLICY@
@MAIN@
'''

SINGLE_MAIN = r'''
int main(void)
{
    read_ok = @READ_OK@;
    counts = @COUNTS@;
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

# One aura stage per run, as main.c/payload.c order it: argv = role, iBoot's counts byte, the var
# the stage before handed on ('-' for none), the ANS start outcome. Prints how the stage ended
# ('chain <var>', 'reboot', 'proxy' or 'os') and the counts byte it left in the PMU.
STAGE_MAIN = r'''
int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    const char *role = argv[1];
    counts = (u8)strtoul(argv[2], NULL, 0);
    bool ans_ok = !strcmp(argv[4], "ok");
    if (!strcmp(role, "stage1-old")) {
        /* An older Stage 1: clears at start, hands on no count. */
        pmu_clear_panic_counter();
        printf("chain - %u\n", counts);
        return 0;
    }
    pmu_snapshot_failed_boots();
    if (!strcmp(role, "stage1")) {
        /* Chainloads the Stage 2: next_stage.chained, so no pmu_chain_leave(). */
        const char *next = pmu_failed_boots_var();
        printf("chain %s %u\n", next ? next + strlen("aura.failed-boots=") : "-", counts);
        return 0;
    }
    if (strcmp(argv[3], "-"))
        pmu_take_failed_boots_var(argv[3]);
    if (!ans_ok) {
        payload_nvme_start_failed();
        if (rebooted) {
            printf("reboot %u\n", counts);
            return 0;
        }
        /* No payload: run_actions() stops in the proxy. */
        pmu_chain_leave();
        printf("proxy %u\n", counts);
        return 0;
    }
    /* Vectoring to the kernel: not chained, so the chain leaves aura. */
    pmu_chain_leave();
    pmu_chain_leave();
    printf("os %u clears %d\n", counts, clears);
    return 0;
}
'''


def _build(tmp_path, main, subst=None):
    header = (ROOT / "src/pmu_counter.h").read_text()
    payload = (ROOT / "src/payload.c").read_text()
    pmu = (ROOT / "src/pmu_counter.c").read_text()
    helpers = _between(header, "/* The failed-boot count is", "/* The chain's")
    chain = pmu[pmu.index("/*\n * No aura stage clears iBoot's count"):]
    policy = _between(payload, "/*\n * Reboot after a failed start", "int payload_run(void)")
    harness = HARNESS
    for key, val in {
        "@HELPERS@": helpers, "@CHAIN@": chain.replace("\nvoid ", "\nstatic void ").replace(
            "\nbool ", "\nstatic bool ").replace("\nconst char *pmu", "\nstatic const char *pmu"),
        "@POLICY@": policy, "@MAIN@": main,
    }.items():
        harness = harness.replace(key, val)
    for key, val in (subst or {}).items():
        harness = harness.replace(key, val)
    src = tmp_path / "t.c"
    src.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", str(src),
                    "-o", str(exe)], check=True)
    return exe


@pytest.mark.parametrize("case", sorted(CASES))
def test_nvme_start_reboot(tmp_path, case):
    read_ok, counts, var, expected, next_var = CASES[case]
    exe = _build(tmp_path, SINGLE_MAIN, {
        "@READ_OK@": str(read_ok).lower(), "@COUNTS@": hex(counts),
        "@VAR@": f'"{var}"' if var is not None else "NULL",
    })
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1].split() == [expected, next_var]


def _iboot_counts_reset(counts):
    """iBoot counts a boot that ended in a reset before an OS cleared the count."""
    return (counts & 0xf0) | min(15, (counts & 0xf) + 1)


def _run_chain(exe, stage1, outcomes, counts=0, max_boots=20):
    """Boot until a boot reaches an OS or the proxy. outcomes[i] is boot i's ANS start ('ok' or
    'fail'; the last one repeats). Returns (ends of each boot, counts byte left in the PMU)."""
    ends = []
    for boot in range(max_boots):
        outcome = outcomes[min(boot, len(outcomes) - 1)]
        s1 = subprocess.run([str(exe), stage1, str(counts), "-", "-"], check=True,
                            capture_output=True, text=True).stdout.splitlines()[-1].split()
        assert s1[0] == "chain"
        var, counts = s1[1], int(s1[2])
        s2 = subprocess.run([str(exe), "stage2", str(counts), var, outcome], check=True,
                            capture_output=True, text=True).stdout.splitlines()[-1].split()
        ends.append(s2[0])
        counts = int(s2[1])
        if s2[0] == "reboot":
            counts = _iboot_counts_reset(counts)
            continue
        if s2[0] == "os":
            assert s2[2:] == ["clears", "1"]
        return ends, counts
    pytest.fail(f"no OS or proxy after {max_boots} boots: {ends}")


# name: (Stage 1, ANS outcomes per boot, counts byte at the first boot, ends, counts byte left)
MULTI_BOOT = {
    # Dj's case on 3b1237db: a failure that persists must end in the proxy after exactly 2 reboots.
    "persistent-failure": ("stage1", ["fail"], 0x00, ["reboot", "reboot", "proxy"], 0),
    "single-failure": ("stage1", ["fail", "ok"], 0x00, ["reboot", "os"], 0),
    "two-failures": ("stage1", ["fail", "fail", "ok"], 0x00, ["reboot", "reboot", "os"], 0),
    "normal-boot": ("stage1", ["ok"], 0x00, ["os"], 0),
    # A boot that failed before (a test kernel that hung) is one of the boots in a row.
    "after-a-failed-boot": ("stage1", ["fail"], 0x01, ["reboot", "proxy"], 0),
    "panic-count-kept-until-os": ("stage1", ["fail", "ok"], 0x30, ["reboot", "os"], 0),
    # An older Stage 1 clears at start and hands on no count: the proxy, as before.
    "older-stage-1": ("stage1-old", ["fail"], 0x00, ["proxy"], 0),
    "older-stage-1-normal": ("stage1-old", ["ok"], 0x01, ["os"], 0),
}


@pytest.mark.parametrize("case", sorted(MULTI_BOOT))
def test_nvme_start_reboot_multi_boot(tmp_path, case):
    stage1, outcomes, counts, ends, left = MULTI_BOOT[case]
    exe = _build(tmp_path, STAGE_MAIN)
    assert _run_chain(exe, stage1, outcomes, counts) == (ends, left)


def test_counts_cleared_only_where_the_chain_leaves_aura():
    """main.c clears no count at start; every proxy and deliberate reboot in run_actions() and
    the handoff leaves the chain first; vectoring leaves it unless the next stage is chained."""
    main = (ROOT / "src/main.c").read_text()
    assert "pmu_clear_panic_counter" not in main
    start = main[main.index("void aura_main(void)"):]
    assert "pmu_snapshot_failed_boots();" in start[:start.index("run_actions();")]

    run_actions = _between(main, "void run_actions(void)", "\n}\n")
    handoff = start[start.index("next_stage:"):]
    host_proxy = _between(main, "static void stage1_run_host_proxy(void)", "\n}\n")
    for name, body in (("run_actions", run_actions), ("handoff", handoff),
                       ("stage1_run_host_proxy", host_proxy)):
        for m in re.finditer(r"uartproxy_run(_presynced)?\(|flush_and_reboot\(\)", body):
            before = body[:m.start()]
            last_leave = before.rfind("pmu_chain_leave();")
            assert last_leave >= 0 and m.start() - last_leave < 120, \
                f"{name}: {m.group(0)} at {m.start()} without pmu_chain_leave() right before it"

    vec = handoff[:handoff.index("Preparing to run next stage")]
    assert re.search(r"if \(!next_stage\.chained\)\s+pmu_chain_leave\(\);", vec)

    chainload = (ROOT / "src/chainload.c").read_text()
    assert "next_stage.chained = true;" in chainload
    for path in ("src/kboot.c", "src/proxy.c"):
        text = (ROOT / path).read_text()
        assert text.count("next_stage.restore_logo =") == text.count("next_stage.chained = false;")
