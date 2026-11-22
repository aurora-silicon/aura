# SPDX-License-Identifier: GPL-2.0-only
"""When an aura stage may give a USB host the proxy (src/main.c, docs/secure-boot.md)."""

from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "src/main.c").read_text()


def build_and_run(tmp_path, name, harness):
    binary = tmp_path / name
    subprocess.run(
        host_cc() + ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                     "-Wno-unused-variable", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


J873G_STUBS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
static void *adt;
static u32 board_id = 0x24;
static const char *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    const char *v = !strcmp(name, "model") ? "Mac18,5" : "J873g";
    *len = strlen(v) + 1;
    return v;
}
#define panic(...) assert(!"panic")
static void mmu_init(void) {}
static void wdt_disable(void) {}
static int smp_init(void) { return 0; }
static void debugport_powerup(void) {}
static int pmgr_init(void) { return 0; }
static void j873_usb_prep(void) {}
static void j873_n1_power(void) {}
static void j873_pcie_prep(void) {}
static void debugport_check(void) {}
static void j873_usb_wake_hpms(void) {}
static void mdelay(int ms) {}

/* The boot under test */
static bool proxy_arg, allowed, unrequested_allowed, payload_ok;
static int payloads, proxies, reboots;
static jmp_buf out;
static bool aura_proxy_enabled(void) { return proxy_arg; }
static bool secboot_host_proxy_allowed(void) { return allowed; }
static bool secboot_unrequested_proxy_allowed(void) { return unrequested_allowed; }
static int payload_run(void) { payloads++; return payload_ok ? 0 : -1; }
static int uartproxy_run(void *start) { proxies++; longjmp(out, 1); }
static void flush_and_reboot(void) { reboots++; longjmp(out, 1); }
'''


def j873g_source():
    start = MAIN.index("static void j873g_debugport_check(void)")
    return MAIN[start:MAIN.index("void aura_main(void)", start)]


def test_j873g_enforcing_stage_never_runs_the_proxy(tmp_path):
    harness = J873G_STUBS + j873g_source() + r'''
static void boot(bool arg, bool may, bool ok)
{
    proxy_arg = arg;
    allowed = unrequested_allowed = may;
    payload_ok = ok;
    payloads = proxies = reboots = 0;
    if (!setjmp(out))
        j873g_run_proxy();
}

int main(void)
{
    /* Enforcing (or secure): aura-proxy does not skip the payloads ... */
    boot(true, false, true);
    assert(payloads == 1 && !proxies && !reboots);
    /* ... and a payload that fails reboots instead of falling back to the proxy. */
    boot(true, false, false);
    assert(payloads == 1 && !proxies && reboots == 1);
    boot(false, false, false);
    assert(payloads == 1 && !proxies && reboots == 1);

    /* A stage that may give the proxy keeps aura-proxy and the fallback. */
    boot(true, true, true);
    assert(!payloads && proxies == 1 && !reboots);
    boot(false, true, false);
    assert(payloads == 1 && proxies == 1 && !reboots);
    boot(false, true, true);
    assert(payloads == 1 && !proxies && !reboots);
    return 0;
}
'''
    build_and_run(tmp_path, "j873g-proxy-policy", harness)


STAGE1_STUBS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
/* The real window and payload decisions */
#include "stage1_proxy.c"
#define CHAINLOADING 1
#define STAGE1_PROXY_REQUEST_MS 15000
#define STAGE1_RECOVERY_WINDOW_MS 10000
#define WDT_BOOT_SECONDS 178
#define T8140 0x8140
#define panic(...) assert(!"panic")
static struct { void *entry; } next_stage;
static void *adt;
static int chip_id;
static bool stage1_chained;
static void fb_set_active(bool on) {}
static void mdelay(int ms) {}
static void usb_init(void) {}
static void usb_iodev_init(void) {}
static bool aura_debug_enabled(void) { return false; }
static bool aura_trial_enabled(void) { return false; }
static bool usb_cdc_ready(void) { return true; }
static bool usb_cdc_owns_carrier(void) { return true; }
static const char *stage1_config_target(void) { return "uuid;aurora/stage2.bin"; }
static bool wdt_boot_watchdog_armed(void) { return true; }

/* The boot under test: a Stage 1 that does not enforce. */
static bool lab, failed_boot, proxy_arg;
static unsigned config_window_ms;
static int windows, proxies, reboots, payloads;
static unsigned last_window_ms;
static jmp_buf out;
static bool secboot_enforcing(void) { return false; }
static bool secboot_host_proxy_allowed(void) { return true; }
static bool secboot_unrequested_proxy_allowed(void) { return lab; }
static int pmu_read_boot_record(unsigned char *counts, unsigned char *stage)
{
    *counts = failed_boot ? 1 : 0;
    *stage = 0x10;
    return 0;
}
static unsigned stage1_config_window_ms(void) { return config_window_ms; }
static bool aura_proxy_enabled(void) { return proxy_arg; }
/* No host ever syncs. */
static enum stage1_proxy_result uartproxy_wait_stage1(unsigned ms)
{
    windows++;
    last_window_ms = ms;
    return STAGE1_PROXY_TIMEOUT;
}
static void uartproxy_run_presynced(void) {}
static int uartproxy_run(void *start) { proxies++; longjmp(out, 1); }
static void flush_and_reboot(void) { reboots++; longjmp(out, 1); }
static int payload_run(void) { payloads++; return -1; }
'''


def stage1_source():
    start = MAIN.index("/* Stage 1 runs at most one proxy window per boot")
    return MAIN[start:MAIN.index("static void j873g_debugport_check(void)", start)]


def test_release_stage1_gives_no_host_an_unrequested_proxy(tmp_path):
    harness = STAGE1_STUBS + stage1_source() + r'''
static void reset(bool is_lab, bool failed, bool arg, unsigned window)
{
    lab = is_lab;
    failed_boot = failed;
    proxy_arg = arg;
    config_window_ms = window;
    stage1_window_done = false;
    stage1_window_result = STAGE1_PROXY_TIMEOUT;
    windows = proxies = reboots = payloads = 0;
    last_window_ms = 0;
    stage1_read_boot_record();
}

static void run(void)
{
    if (!setjmp(out))
        run_actions();
}

int main(void)
{
    /* A release Stage 1 after a boot that stopped early: no recovery window. */
    reset(false, true, false, 0);
    assert(!stage1_recovery_window() && windows == 0);
    /* No payload: it reboots, it does not fall back to the proxy. */
    reset(false, false, false, 0);
    run();
    assert(payloads == 1 && reboots == 1 && proxies == 0);

    /* Windows the owner asks for stay: aura-proxy and the configured window. */
    reset(false, false, true, 0);
    run();
    assert(windows == 1 && last_window_ms == STAGE1_PROXY_REQUEST_MS && proxies == 0);
    reset(false, false, false, 3000);
    run();
    assert(windows == 1 && last_window_ms == 3000 && proxies == 0);

    /* A lab Stage 1 keeps the recovery window and the proxy fallback. */
    reset(true, true, false, 0);
    assert(!stage1_recovery_window() && windows == 1 &&
           last_window_ms == STAGE1_RECOVERY_WINDOW_MS);
    reset(true, false, false, 0);
    run();
    assert(payloads == 1 && reboots == 0 && proxies == 1);
    return 0;
}
'''
    binary = tmp_path / "stage1-proxy-policy"
    subprocess.run(
        host_cc() + ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                     "-Wno-unused-variable", "-I", str(ROOT / "src"), "-DSTAGE1_PROXY_HOST_TEST",
                     "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
