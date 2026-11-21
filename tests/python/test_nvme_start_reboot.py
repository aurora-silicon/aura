# SPDX-License-Identifier: GPL-2.0-only
"""A stage that would stop in the proxy with no host (a post-M4 ANS that a Stage 2 couldn't start
from off, no payload, a failed handoff): proxy_fallback() reboots instead, while the failed-boot
count the earlier stages handed on (aura.failed-boots=) is below the limit, and always on an aura-try
trial, whose marker is spent so the reboot lands on the configured image.
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

# name: (own read ok, own count byte, var handed on or None, trial, host attaches in the window,
#        expected decision, expected next var, PMU clears)
CASES = {
    "first-failure": (True, 0x00, "0", False, False, "reboot", "aura.failed-boots=0", 0),
    "second-failure": (True, 0x00, "1", False, False, "reboot", "aura.failed-boots=1", 0),
    "limit-reached": (True, 0x00, "2", False, False, "proxy", "aura.failed-boots=2", 1),
    "own-count-larger": (True, 0x32, "0", False, False, "proxy", "aura.failed-boots=2", 1),
    "panic-nibble-ignored": (True, 0x70, "1", False, False, "reboot", "aura.failed-boots=1", 0),
    "no-var-older-stage-1": (True, 0x00, None, False, False, "proxy", "aura.failed-boots=0", 1),
    "no-var-no-record": (False, 0x00, None, False, False, "proxy", "none", 1),
    "malformed-var": (True, 0x00, "1x", False, False, "proxy", "aura.failed-boots=0", 1),
    "out-of-range-var": (True, 0x00, "16", False, False, "proxy", "aura.failed-boots=0", 1),
    "empty-var": (True, 0x00, "", False, False, "proxy", "aura.failed-boots=0", 1),
    # A host that attaches in the window keeps the proxy, and the chain leaves aura.
    "host-attaches": (True, 0x00, "0", False, True, "proxy", "aura.failed-boots=0", 1),
    # An aura-try trial reboots with or without a count (an older Stage 1 hands on none), and at
    # or over the limit: the marker is spent, so the reboot lands on the configured image. The
    # reboot is deliberate: the count is cleared first.
    "trial-older-stage-1": (True, 0x00, None, True, False, "reboot", "aura.failed-boots=0", 1),
    "trial-no-record": (False, 0x00, None, True, False, "reboot", "none", 1),
    "trial-limit-reached": (True, 0x00, "2", True, False, "reboot", "aura.failed-boots=2", 1),
    "trial-host-attaches": (True, 0x00, None, True, True, "proxy", "aura.failed-boots=0", 1),
}
# Cases where nothing failed to start (an image with nothing to start): the proxy as before, on a
# count too; a trial still reboots. Same tuple, start_failed false.
NOTHING_TO_START = {
    "nothing-to-start-first": (True, 0x00, "0", False, False, "proxy", "aura.failed-boots=0", 1),
    "nothing-to-start-trial": (True, 0x00, None, True, False, "reboot", "aura.failed-boots=0", 1),
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
static bool trial, host, start_failed = true;
#define WDT_TRIAL_SECONDS 178
static bool aura_trial_enabled(void) { return trial; }
static bool uartproxy_wait_host(unsigned int timeout_ms) { (void)timeout_ms; return host; }
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
    trial = @TRIAL@;
    host = @HOST@;
    start_failed = @FAILED@;
    pmu_snapshot_failed_boots();
    if (var)
        pmu_take_failed_boots_var(var);
    const char *next = pmu_failed_boots_var();
    proxy_fallback("nvme", start_failed);
    printf("%s %s %d\n", rebooted ? "reboot" : "proxy", next ? next : "none", clears);
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
    bool handoff_fail = !strcmp(argv[4], "handoff");
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
    trial = !strcmp(role, "trial2");
    if (strcmp(argv[3], "-"))
        pmu_take_failed_boots_var(argv[3]);
    if (handoff_fail) {
        /* The payload is ready (not chained) and its NVMe or display handoff fails, in main.c's
         * order: @LEAVE_FIRST@ is 1 if the chain leaves aura before the handoffs. */
        if (@LEAVE_FIRST@)
            pmu_chain_leave();
        proxy_fallback("handoff", true);
        printf("%s %u\n", rebooted ? "reboot" : "proxy", counts);
        return 0;
    }
    if (!ans_ok) {
        /* No payload: run_actions() falls back, then stops in the proxy. */
        proxy_fallback("nvme", start_failed);
        printf("%s %u\n", rebooted ? "reboot" : "proxy", counts);
        return 0;
    }
    /* Vectoring to the kernel: not chained, so the chain leaves aura. */
    pmu_chain_leave();
    pmu_chain_leave();
    printf("os %u clears %d\n", counts, clears);
    return 0;
}
'''


def _leave_first():
    """1 if main.c leaves the chain (non-chained next stage) before the NVMe handoff."""
    main = (ROOT / "src/main.c").read_text()
    handoff = main[main.index("next_stage:"):]
    leave = re.search(r"if \(!next_stage\.chained\)\s+pmu_chain_leave\(\);", handoff)
    return int(leave.start() < handoff.index("nvme_shutdown()"))


def _build(tmp_path, main, subst=None):
    header = (ROOT / "src/pmu_counter.h").read_text()
    fallback_h = (ROOT / "src/proxy_fallback.h").read_text()
    fallback = (ROOT / "src/proxy_fallback.c").read_text()
    pmu = (ROOT / "src/pmu_counter.c").read_text()
    helpers = _between(header, "/* The failed-boot count is", "/* The chain's") + \
        _between(fallback_h, "/*\n * At most this many", "enum proxy_fallback_action proxy_fallback_decide")
    chain = pmu[pmu.index("/*\n * No aura stage clears iBoot's count"):]
    policy = fallback[fallback.index("/*\n * Nobody may be at the Mac"):].replace(
        "\nenum proxy_fallback_action proxy_fallback_decide", "\nstatic enum proxy_fallback_action proxy_fallback_decide").replace(
        "\nvoid proxy_fallback(", "\nstatic void proxy_fallback(")
    harness = HARNESS
    for key, val in {
        "@HELPERS@": helpers, "@CHAIN@": chain.replace("\nvoid ", "\nstatic void ").replace(
            "\nbool ", "\nstatic bool ").replace("\nconst char *pmu", "\nstatic const char *pmu"),
        "@POLICY@": policy, "@MAIN@": main,
    }.items():
        harness = harness.replace(key, val)
    for key, val in (subst or {}).items():
        harness = harness.replace(key, val)
    harness = harness.replace("@LEAVE_FIRST@", str(_leave_first()))
    src = tmp_path / "t.c"
    src.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", str(src),
                    "-o", str(exe)], check=True)
    return exe


@pytest.mark.parametrize("case", sorted(CASES) + sorted(NOTHING_TO_START))
def test_nvme_start_reboot(tmp_path, case):
    failed = case in CASES
    read_ok, counts, var, trial, host, expected, next_var, clears = \
        CASES[case] if failed else NOTHING_TO_START[case]
    exe = _build(tmp_path, SINGLE_MAIN, {
        "@READ_OK@": str(read_ok).lower(), "@COUNTS@": hex(counts),
        "@VAR@": f'"{var}"' if var is not None else "NULL",
        "@TRIAL@": str(trial).lower(), "@HOST@": str(host).lower(),
        "@FAILED@": str(failed).lower(),
    })
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1].split() == [expected, next_var, str(clears)]


def _iboot_counts_reset(counts):
    """iBoot counts a boot that ended in a reset before an OS cleared the count."""
    return (counts & 0xf0) | min(15, (counts & 0xf) + 1)


def _run_chain(exe, stage1, outcomes, counts=0, max_boots=20, first_stage2="stage2"):
    """Boot until a boot reaches an OS or the proxy. outcomes[i] is boot i's ANS start ('ok' or
    'fail'; the last one repeats). Returns (ends of each boot, counts byte left in the PMU)."""
    ends = []
    for boot in range(max_boots):
        outcome = outcomes[min(boot, len(outcomes) - 1)]
        s1 = subprocess.run([str(exe), stage1, str(counts), "-", "-"], check=True,
                            capture_output=True, text=True).stdout.splitlines()[-1].split()
        assert s1[0] == "chain"
        var, counts = s1[1], int(s1[2])
        role = first_stage2 if boot == 0 else "stage2"
        s2 = subprocess.run([str(exe), role, str(counts), var, outcome], check=True,
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
    # A persistent NVMe or display handoff failure on a non-chained next stage (review of fb9b62f2):
    # the count must still be there when the handoff fails, so it ends in the proxy.
    "persistent-handoff-failure": ("stage1", ["handoff"], 0x00, ["reboot", "reboot", "proxy"], 0),
    "single-handoff-failure": ("stage1", ["handoff", "ok"], 0x00, ["reboot", "os"], 0),
}


# An aura-try trial on the first boot: (Stage 1, outcomes, counts byte, ends, counts byte left).
# The trial's reboot clears the count first, and iBoot counts the reset itself, so a configured
# image that also fails gets one reboot of its own before the proxy.
TRIAL_CHAINS = {
    "trial-fails-configured-ok": ("stage1", ["fail", "ok"], 0x00, ["reboot", "os"], 0),
    "trial-fails-configured-fails": ("stage1", ["fail"], 0x00, ["reboot", "reboot", "proxy"], 0),
    "trial-after-failed-boots": ("stage1", ["fail", "ok"], 0x03, ["reboot", "os"], 0),
    "trial-ok": ("stage1", ["ok"], 0x00, ["os"], 0),
    # An older Stage 1 (b10 on the Neo) hands on no count: the trial still reboots, the
    # configured image then keeps the proxy as before.
    "trial-older-stage-1": ("stage1-old", ["fail", "ok"], 0x00, ["reboot", "os"], 0),
    "trial-older-stage-1-configured-fails": ("stage1-old", ["fail"], 0x00, ["reboot", "proxy"], 0),
}


@pytest.mark.parametrize("case", sorted(TRIAL_CHAINS))
def test_trial_chain(tmp_path, case):
    stage1, outcomes, counts, ends, left = TRIAL_CHAINS[case]
    exe = _build(tmp_path, STAGE_MAIN)
    assert _run_chain(exe, stage1, outcomes, counts, first_stage2="trial2") == (ends, left)


@pytest.mark.parametrize("case", sorted(MULTI_BOOT))
def test_nvme_start_reboot_multi_boot(tmp_path, case):
    stage1, outcomes, counts, ends, left = MULTI_BOOT[case]
    exe = _build(tmp_path, STAGE_MAIN)
    assert _run_chain(exe, stage1, outcomes, counts) == (ends, left)


def test_counts_cleared_only_where_the_chain_leaves_aura():
    """main.c clears no count at start; every proxy and deliberate reboot in run_actions() and
    the handoff leaves the chain first (proxy_fallback() leaves it before it returns to the proxy);
    vectoring leaves it unless the next stage is chained."""
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
            leave = before.rfind("pmu_chain_leave();")
            fallback = before.rfind("proxy_fallback(")
            last = max(leave, fallback)
            gap = body[last:m.start()] if last >= 0 else ""
            # proxy_fallback() may sit before the proxy's USB bring-up; nothing in between ends
            # the stage another way.
            near = m.start() - last < (600 if fallback > leave else 120)
            assert last >= 0 and near and "flush_and_reboot" not in gap[1:] and \
                "return" not in gap, \
                f"{name}: {m.group(0)} at {m.start()} without pmu_chain_leave() or " \
                "proxy_fallback() right before it"

    # The chain leaves aura only after both handoffs: a handoff that fails must still find the
    # count it reboots on (a persistent NVMe or display handoff failure: reboot, reboot, proxy).
    leave = re.search(r"if \(!next_stage\.chained\)\s+pmu_chain_leave\(\);", handoff)
    assert leave
    assert handoff.index('proxy_fallback("NVMe handoff failed"') < leave.start()
    assert handoff.index('proxy_fallback("Display handoff failed"') < leave.start()
    assert leave.start() < handoff.index("Vectoring to next stage")

    chainload = (ROOT / "src/chainload.c").read_text()
    assert "next_stage.chained = true;" in chainload
    for path in ("src/kboot.c", "src/proxy.c"):
        text = (ROOT / path).read_text()
        assert text.count("next_stage.restore_logo =") == text.count("next_stage.chained = false;")
