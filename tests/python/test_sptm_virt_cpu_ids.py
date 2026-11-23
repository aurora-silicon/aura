# SPDX-License-Identifier: MIT
"""SPTM-virt keeps per-core state for every cpu-id below MAX_CPUS, 32 to 34 included.

Three pieces of SPTM-virt are compiled unchanged into host programs: the S2R trace's walk of
/cpus (src/sptm_virt_s2r_soc.c), the stop-mode and go-mode core masks (src/sptm_virt_s2r.c) and the
DCC transport's per-core cookie and errno (src/sptm_virt_ctl.c). The T6050 Ultra's cpu-ids reach
34, so each is driven with cpu-ids on both sides of 32.

AURA_SPTM_SOURCE names another directory holding the three files (the negative controls use it).
"""
import os
from pathlib import Path
import re
import subprocess

import pytest

from host_compiler import host_cc

REPO = Path(__file__).resolve().parents[2]
SOURCES = Path(os.environ.get("AURA_SPTM_SOURCE", REPO / "src"))


def defines(path, *names):
    text = (REPO / path).read_text()
    out = []
    for name in names:
        match = re.search(r"^#define " + re.escape(name) + r"\b(?:.*\\\n)*.*$", text, re.M)
        assert match, name
        out.append(match.group(0))
    return "\n".join(out)


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def line(text, pattern):
    match = re.search(pattern, text, re.M)
    assert match, pattern
    return match.group(0)


COMMON = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* unsigned long matches the %lx the source prints with, on Linux and macOS alike. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#define UNUSED(x) ((void)(x))
static int cur_cpu;
static int smp_id(void) { return cur_cpu; }
static char console[4096];
static void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void console_printf(const char *fmt, ...)
{
    va_list ap;
    size_t used = strlen(console);
    va_start(ap, fmt);
    vsnprintf(console + used, sizeof(console) - used, fmt, ap);
    va_end(ap);
}
''' + "\n".join((
    defines("src/types.h", "BIT", "MASK", "GENMASK"),
    defines("src/utils.h", "ARRAY_SIZE"),
    defines("src/smp_asm.h", "MAX_CPUS"),
))


def build(tmp_path_factory, name, source):
    tmp = tmp_path_factory.mktemp(name)
    (tmp / f"{name}.c").write_text(source)
    binary = tmp / name
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-fno-omit-frame-pointer", str(tmp / f"{name}.c"), "-o", str(binary)],
        check=True)
    return binary


def run(binary, *args):
    out = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr
    return out.stdout


# ---- the S2R trace's /cpus walk --------------------------------------------------------------

TRACE_HARNESS = COMMON + r'''
#include <stdlib.h>
typedef long s64;
/* /cpus as a table: node 1000, its children 1001 onwards. */
static struct cpu {
    bool has_id;
    u32 id;
    u64 impl[2], acc[2], cpm[2];
} cpus[80];
static int n_cpus = -1;
static const void *adt = cpus;
static int adt_path_offset(const void *adt, const char *path)
{
    return !strcmp(path, "/cpus") && n_cpus >= 0 ? 1000 : -1;
}
static int adt_get_child_count(const void *adt, int node) { return node == 1000 ? n_cpus : 0; }
static int adt_first_child_offset(const void *adt, int node) { return node + 1; }
static int adt_next_sibling_offset(const void *adt, int node) { return node + 1; }
static int adt_getprop_copy(const void *adt, int node, const char *name, void *out, size_t len)
{
    const struct cpu *c = &cpus[node - 1001];
    const void *src = NULL;
    size_t have = 16;
    assert(node > 1000 && node <= 1000 + n_cpus);
    if (!strcmp(name, "cpu-id") && c->has_id) { src = &c->id; have = 4; }
    else if (!strcmp(name, "cpu-impl-reg")) src = c->impl;
    else if (!strcmp(name, "acc-impl-reg")) src = c->acc;
    else if (!strcmp(name, "cpm-impl-reg")) src = c->cpm;
    if (!src || have != len)
        return -1;
    memcpy(out, src, len);
    return len;
}
static int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *a, u64 *s)
{
    return -1;
}
''' + defines("src/adt.h", "ADT_GETPROP", "ADT_GETPROP_ARRAY", "ADT_FOREACH_CHILD") + "\n@CODE@\n" + r'''
/* cpu-ids as arguments; "x" is a core with no cpu-id. Four cores to a cluster. */
int main(int argc, char **argv)
{
    static struct s2r_soc soc;
    n_cpus = argc - 1;
    for (int i = 0; i < n_cpus; i++) {
        struct cpu *c = &cpus[i];
        c->has_id = strcmp(argv[i + 1], "x");
        c->id = strtoul(argv[i + 1], NULL, 0);
        c->impl[0] = 0x210000000UL + 0x100000UL * i;
        c->impl[1] = 0x4000;
        c->acc[0] = 0x200000000UL + 0x1000000UL * (i / 4);
        c->acc[1] = 0x8000;
        c->cpm[0] = c->acc[0] + 0x800000;
        c->cpm[1] = 0x4000;
    }
    int rc = s2r_trace_cpus(adt, &soc);
    printf("%d %d %u\n", rc, soc.n_trace, soc.trace_pages);
    return 0;
}
'''


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    soc = (SOURCES / "sptm_virt_s2r_soc.c").read_text()
    header = (REPO / "src/sptm_virt_s2r_soc.h").read_text()
    code = "\n".join((
        between(header, "#define S2R_LINK_NODES", "/*\n * Fill *out from the ADT."),
        between(soc, "/* A traced range, exactly as the ADT gives it",
                "/*\n * A PMGR whose sleep is the S2R timer's start alone"),
    ))
    return build(tmp_path_factory, "s2r-trace-cpus", TRACE_HARNESS.replace("@CODE@", code))


def trace_cpus(trace, ids):
    return tuple(int(x) for x in run(trace, *ids).split())


# 30 cores whose cpu-ids reach 34, as the J775D's do (the ids themselves are made up here).
ULTRA_IDS = [n for n in range(35) if n not in (6, 7, 14, 15, 22)]


def test_trace_takes_cpu_ids_up_to_34(trace):
    assert len(ULTRA_IDS) == 30 and max(ULTRA_IDS) == 34
    # every core's window, and one ACC and one CPM window (three pages) for each cluster of four
    assert trace_cpus(trace, ULTRA_IDS) == (0, 30 + 2 * 8, 30 + 3 * 8)


@pytest.mark.parametrize("cpu_id", [0, 31, 32, 33, 34, 63])
def test_trace_takes_any_cpu_id_below_max_cpus(trace, cpu_id):
    assert trace_cpus(trace, [cpu_id]) == (0, 3, 4)
    assert trace_cpus(trace, [1, cpu_id, 2])[0] == 0


@pytest.mark.parametrize("ids", [
    [64], [0, 1, 64], [65], [0xffffffff], [0x100000000 - 64],
    # the same cpu-id twice, below and above 32
    [0, 0], [5, 31, 5], [32, 32], [33, 0, 34, 33], [63, 1, 63],
    # a core with no cpu-id, and a tree with no core
    ["x"], [32, "x"], [],
])
def test_trace_refuses_bad_cpu_ids(trace, ids):
    assert trace_cpus(trace, ids)[0] == -1


# ---- the S2R core masks -----------------------------------------------------------------------

S2R_HARNESS = COMMON + r'''
typedef struct spmi_dev spmi_dev_t;
static jmp_buf parked;
static int wfis, twi_arms, proxy_runs, crumb_writes, markers;
static u8 crumb[3];
static u64 host_ELR_EL2, host_CNTP_CTL_EL0 = 1, host_CNTV_CTL_EL0 = 1, deep_sleep;
static u64 sptm_virt_s2r_head = 77;
static u32 sptm_virt_s2r_crumbs;
static bool s2r_go;
static volatile u64 s2r_sleep_head;
static volatile int s2r_timer_cpu = -1;
#define HCR_TWI             BIT(13)
#define START_HV            3
#define SAFE_MRS_FAILED     0xbadUL
#define SAFE_MRS(enc)       (deep_sleep)
#define mrs(reg)            (host_##reg)
#define msr(reg, val)       (host_##reg = (val))
#define sysop(op)           host_sysop(op)
#define printf(...)         console_printf(__VA_ARGS__)
struct uartproxy_msg_start { u32 reason, code; void *info; };
/* A core that waits for ever in wfe has parked: back to the test. */
static void host_sysop(const char *op)
{
    if (!strcmp(op, "wfe"))
        longjmp(parked, 1);
    wfis += !strcmp(op, "wfi");
}
static void nv2_hcr_update(u64 set, u64 clear) { assert(set == HCR_TWI && !clear); twi_arms++; }
static void s2r_clean(volatile void *p, size_t n) {}
static void s2r_marker(const char *what, u64 val) { markers++; }
static void s2r_console_to_aon(void) {}
static void iodev_console_flush(void) {}
static void s2r_console_off(void) {}
static void sptm_virt_s2r_dump_prio(u64 a, u64 b) {}
static void sptm_virt_park_others(void) {}
static void uartproxy_run(struct uartproxy_msg_start *start)
{
    assert(start->reason == START_HV && start->code == 0x53325221);
    proxy_runs++;
}
static void spmi_ext_write_long(spmi_dev_t *dev, u8 addr, u32 reg, const u8 *buf, size_t len)
{
    assert(addr == 0xe && reg == 0x830d && len == sizeof(crumb));
    memcpy(crumb, buf, len);
    crumb_writes++;
}
@CODE@
#undef printf

static const int ids[] = {0, 5, 31, 32, 33, 34, 63};
#define N_IDS ((int)ARRAY_SIZE(ids))
/* A core's last instruction before power-off: true if the core parked there. */
static bool wfi_parks(int cpu, bool *handled)
{
    cur_cpu = cpu;
    if (setjmp(parked))
        return true;
    *handled = sptm_virt_s2r_wfi(NULL, 0);
    return false;
}

static void stop_mode(void)
{
    bool handled;
    u64 all = 0;

    /* Before a core sleeps its WFIs are the guest's own. */
    for (int i = 0; i < N_IDS; i++) {
        host_ELR_EL2 = 0x1000;
        assert(!wfi_parks(ids[i], &handled) && !handled && host_ELR_EL2 == 0x1000);
    }
    /* Each core arms the WFI trap once; the first one is the core that commits. */
    for (int i = 0; i < N_IDS; i++) {
        cur_cpu = ids[i];
        s2r_core_sleeps();
        assert(twi_arms == i + 1 && markers == i + 1);
        s2r_core_sleeps();
        assert(twi_arms == i + 1 && markers == i + 1);
        all |= BIT(ids[i]);
        assert(s2r_twi_mask == all);
        assert(s2r_timer_cpu == ids[0]);
    }
    assert(all == 0x8000000780000021UL);
    /* A core that never slept is still not armed, whatever its cpu-id shares with one that did. */
    host_ELR_EL2 = 0x1000;
    assert(!wfi_parks(1, &handled) && !handled && host_ELR_EL2 == 0x1000);
    assert(!wfi_parks(35, &handled) && !handled && host_ELR_EL2 == 0x1000);
    for (int i = 0; i < N_IDS; i++) {
        /* a WFE is not a WFI */
        cur_cpu = ids[i];
        host_ELR_EL2 = 0x1000;
        assert(!sptm_virt_s2r_wfi(NULL, 1) && host_ELR_EL2 == 0x1000);
        /* an idle WFI, or a core with no deep-sleep register, is stepped over */
        deep_sleep = 0;
        assert(!wfi_parks(ids[i], &handled) && handled && host_ELR_EL2 == 0x1004);
        deep_sleep = SAFE_MRS_FAILED;
        assert(!wfi_parks(ids[i], &handled) && handled && host_ELR_EL2 == 0x1008);
    }
    /* The deep-sleep WFI parks every core but the one that commits. */
    deep_sleep = 1;
    u64 parked_now = 0;
    for (int i = 1; i < N_IDS; i++) {
        assert(wfi_parks(ids[i], &handled));
        parked_now |= BIT(ids[i]);
        assert(s2r_parked_mask == parked_now);
    }
    assert(!proxy_runs && !wfis);
    console[0] = 0;
    assert(wfi_parks(ids[0], &handled) && proxy_runs == 1);
    assert(s2r_parked_mask == parked_now && parked_now == 0x8000000780000020UL);
    assert(strstr(console, "final WFI on the S2R-timer core cpu0; parked 0x8000000780000020; 77 "
                           "records."));
    assert(!crumb_writes && !wfis);
}

static void go_mode(void)
{
    static spmi_dev_t *const dev = (spmi_dev_t *)crumb;
    bool handled;
    u64 all = 0;

    s2r_go = true;
    s2r_crumb_dev = dev;
    sptm_virt_s2r_crumbs = 1; /* the cores' deep-sleep WFIs trap */
    deep_sleep = 1;
    for (int i = 0; i < N_IDS; i++) {
        cur_cpu = ids[N_IDS - 1 - i];
        host_CNTP_CTL_EL0 = host_CNTV_CTL_EL0 = 1;
        console[0] = 0;
        s2r_core_sleeps();
        /* the first core opens the sleep window, and only it; each one stops its EL2 timers */
        assert(!!strstr(console, "cpu63 opens the sleep window") == !i);
        assert(s2r_sleep_head == 77 && twi_arms == i + 1 && markers == i + 1);
        assert(!host_CNTP_CTL_EL0 && !host_CNTV_CTL_EL0);
        /* a second call on the same core does nothing */
        host_CNTP_CTL_EL0 = 1;
        console[0] = 0;
        s2r_core_sleeps();
        assert(host_CNTP_CTL_EL0 == 1 && twi_arms == i + 1 && markers == i + 1 && !console[0]);
        all |= BIT(cur_cpu);
        assert(s2r_twi_mask == all);
    }
    assert(s2r_timer_cpu == -1);
    /* aura executes each core's last WFI and counts the core; no core parks */
    all = 0;
    for (int i = 0; i < N_IDS; i++) {
        host_ELR_EL2 = 0x2000;
        assert(!wfi_parks(ids[i], &handled) && handled && host_ELR_EL2 == 0x2004);
        assert(wfis == i + 1);
        all |= BIT(ids[i]);
        assert(s2r_crumb_wfi == all);
    }
    assert(!crumb_writes && !proxy_runs && !s2r_parked_mask);
    /* the committing core notes the step and cores 0 to 15 in the PMU scratch's two bytes */
    s2r_timer_cpu = 5;
    assert(!wfi_parks(5, &handled) && handled && crumb_writes == 1);
    assert(crumb[0] == 0x40 && crumb[1] == 0x21 && crumb[2] == 0x00);
    s2r_crumb_wfi |= BIT(9) | BIT(15) | BIT(16);
    s2r_crumb(0x41);
    assert(crumb[0] == 0x41 && crumb[1] == 0x21 && crumb[2] == 0x82);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "stop"))
        stop_mode();
    else if (!strcmp(argv[1], "go"))
        go_mode();
    else
        return 2;
    return 0;
}
'''


@pytest.fixture(scope="module")
def s2r(tmp_path_factory):
    source = (SOURCES / "sptm_virt_s2r.c").read_text()
    code = "\n".join((
        line(source, r"^static volatile u\d+ s2r_twi_mask, s2r_parked_mask;$"),
        between(source, "static spmi_dev_t *s2r_crumb_dev;", "/* More notes:"),
        between(source, "static void s2r_core_sleeps(void)", "/* ---- Timed wake"),
        between(source, "bool sptm_virt_s2r_wfi(u64 *regs, u64 esr)",
                "static void s2r_wakerec_fallback(void);"),
    ))
    return build(tmp_path_factory, "s2r-core-masks", S2R_HARNESS.replace("@CODE@", code))


def test_stop_mode_arms_and_parks_cpu_ids_up_to_63(s2r):
    run(s2r, "stop")


def test_go_mode_counts_cpu_ids_up_to_63(s2r):
    run(s2r, "go")


# ---- the DCC transport's per-core state -------------------------------------------------------

DCC_HARNESS = COMMON + r'''
#define AURA_XTEXT
#define retain used /* not every host compiler knows the retain attribute */
#define ctl_log(...) ((void)0)
#define sysop(op)    ((void)0)
#define mrs(reg)     (host_##reg)
#define msr(reg, val) (host_##reg = (val))
static u64 host_ELR_EL2;
static u32 sptm_virt_flags;
static bool sptm_virt_xnu_live = true;
static struct { bool ready; } ctl = {true};
/* The command: answers with a number of its own for each request, and notes who was let in. */
static int calls;
static u64 last_req;
static bool last_authed;
static int sptm_virt_ctl_call(u64 req, bool authed)
{
    calls++;
    last_req = req;
    last_authed = authed;
    return 1000 + (int)(req & 0xfff);
}
''' + "\n".join((
    defines("src/sptm_virt.h", "SPTM_VIRT_CTL_DCC"),
    defines("src/auractl_abi.h", "AURACTL_ENODEV"),
)) + "\n@CODE@\n" + r'''
#define COOKIE 0x5eed5eed5eed5eedUL
#define ESR(crm, rt, read) ((2UL << 20) | (3UL << 14) | ((u64)(rt) << 5) | ((crm) << 1) | (read))
static u64 regs[31];
static void present(int cpu, u64 cookie)
{
    cur_cpu = cpu;
    regs[3] = cookie;
    assert(sptm_virt_ctl_dcc(regs, ESR(5, 3, 0)));
}
/* Ring the doorbell on `cpu` for request `req`: whether the command ran authorised. */
static bool doorbell(int cpu, u64 req)
{
    int before = calls;
    cur_cpu = cpu;
    regs[7] = (CTL_DCC_TAG << 48) | req;
    assert(sptm_virt_ctl_dcc(regs, ESR(4, 7, 0)));
    assert(calls == before + 1 && last_req == req);
    return last_authed;
}
static u64 read_errno(int cpu)
{
    cur_cpu = cpu;
    regs[9] = ~0UL;
    assert(sptm_virt_ctl_dcc(regs, ESR(4, 9, 1)));
    assert(regs[9] >> 48 == CTL_DCC_TAG);
    return regs[9] & 0xffffffffffffUL;
}

static const int ids[] = {0, 1, 30, 31, 32, 33, 34, 62, 63};
#define N_IDS ((int)ARRAY_SIZE(ids))

int main(void)
{
    /* Without the boot flag the trap is not aura's. */
    regs[3] = COOKIE;
    host_ELR_EL2 = 0x4000;
    assert(!sptm_virt_ctl_dcc(regs, ESR(5, 3, 0)) && host_ELR_EL2 == 0x4000);
    sptm_virt_flags = SPTM_VIRT_CTL_DCC;
    /* Other debug registers are left for the guest's own handler. */
    assert(!sptm_virt_ctl_dcc(regs, ESR(4, 3, 0) | (1UL << 17)) && host_ELR_EL2 == 0x4000);
    assert(!sptm_virt_ctl_dcc(regs, ESR(2, 3, 0)) && host_ELR_EL2 == 0x4000);
    /* A handled access is stepped over; MDCCSR reads as TX full. */
    assert(sptm_virt_ctl_dcc(regs, ESR(1, 4, 1)) && regs[4] == 1u << 29 && host_ELR_EL2 == 0x4004);

    /* No cookie set: presenting 0, or anything else, authorises nothing. */
    present(32, 0);
    assert(!doorbell(32, 1));
    present(32, COOKIE);
    assert(!doorbell(32, 1));
    sptm_virt_ctl_set_cookie(COOKIE);

    for (int a = 0; a < N_IDS; a++) {
        /* The cookie a core presents authorises that core's next doorbell, and only that one. */
        present(ids[a], COOKIE);
        assert(doorbell(ids[a], 2));
        assert(!doorbell(ids[a], 2));
        present(ids[a], COOKIE ^ 1);
        assert(!doorbell(ids[a], 2));
        for (int b = 0; b < N_IDS; b++) {
            if (a == b)
                continue;
            /* Another core's doorbell neither takes it nor uses it up. */
            present(ids[a], COOKIE);
            assert(!doorbell(ids[b], 3));
            assert(doorbell(ids[a], 3));
            /* Nor does a wrong cookie from another core replace it. */
            present(ids[a], COOKIE);
            present(ids[b], 0x1234);
            assert(doorbell(ids[a], 3));
            assert(!doorbell(ids[b], 3));
        }
    }
    /* Each core reads back the errno of its own last doorbell. */
    for (int a = 0; a < N_IDS; a++)
        doorbell(ids[a], 0x100 + a);
    for (int a = N_IDS - 1; a >= 0; a--)
        assert(read_errno(ids[a]) == 1000 + 0x100 + (u64)a);
    /* Stray DCC traffic (no tag) runs nothing and leaves the errno, but uses the cookie up. */
    present(33, COOKIE);
    cur_cpu = 33;
    regs[7] = 0x1234;
    assert(sptm_virt_ctl_dcc(regs, ESR(4, 7, 0)));
    assert(read_errno(33) == 1000 + 0x100 + 5);
    assert(!doorbell(33, 4));
    /* Before the guest is live a doorbell answers ENODEV, on that core only. */
    ctl.ready = false;
    int before = calls;
    cur_cpu = 34;
    regs[7] = (CTL_DCC_TAG << 48) | 5;
    assert(sptm_virt_ctl_dcc(regs, ESR(4, 7, 0)) && calls == before);
    assert(read_errno(34) == AURACTL_ENODEV && read_errno(33) == 1004);
    ctl.ready = true;
    /* Register 31 is the zero register: a read writes nothing, a write is 0. */
    cur_cpu = 32;
    assert(sptm_virt_ctl_dcc(regs, ESR(4, 31, 1)) && sptm_virt_ctl_dcc(regs, ESR(5, 31, 1)));
    present(32, COOKIE);
    assert(sptm_virt_ctl_dcc(regs, ESR(5, 31, 0)));
    assert(!doorbell(32, 6));
    /* A cpu-id past the table shares its last slot rather than writing outside it. */
    present(MAX_CPUS, COOKIE);
    assert(doorbell(MAX_CPUS + 40, 7));
    doorbell(1000, 8);
    assert(read_errno(MAX_CPUS - 1) == 1008);
    return 0;
}
'''


@pytest.fixture(scope="module")
def dcc(tmp_path_factory):
    source = (SOURCES / "sptm_virt_ctl.c").read_text()
    code = between(source, "#define CTL_DCC_TAG", "#endif /* !CTL_HOST */")
    return build(tmp_path_factory, "ctl-dcc-per-core", DCC_HARNESS.replace("@CODE@", code))


def test_dcc_cookie_and_errno_are_per_core(dcc):
    run(dcc)
