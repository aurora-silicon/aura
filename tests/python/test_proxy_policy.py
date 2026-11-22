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
