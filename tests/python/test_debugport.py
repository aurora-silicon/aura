# SPDX-License-Identifier: GPL-2.0-only
"""The Mac mini debug port check, run against modeled port controllers.

It finds the debug port by "uart-hpm-rids" (else hpm0), warns only for a USB
host on another port, waits out the SPMI settle time after S0, and does
nothing on a machine that is not a Mac mini.
"""
from pathlib import Path
import subprocess


def test_debugport_warns_for_the_wrong_port(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/debugport.c").read_text()
    source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
    harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define UNUSED(x) (void)(x)
#define T8152 0x8152
#define TPS6598X_STATUS_PLUG_PRESENT BIT(0)
#define TPS6598X_DATA_CONNECTION BIT(0)
#define TPS6598X_DATA_ROLE_UFP BIT(7)
#define HPM_ACTION_CONTINUE 0
typedef struct tps6598x_dev tps6598x_dev_t;
typedef bool(hpm_match_t)(char *hpm_path, void *data);
typedef int(hpm_action_t)(char *hpm_path, tps6598x_dev_t *tps, void *data);
static void *adt;
static int chip_id = T8152;
static bool mini = true, uart_on_hpm1 = true;
static const char *ports[] = {"/arm-io/nub-spmi-a0/hpm0", "/arm-io/nub-spmi-a0/hpm1",
                              "/arm-io/nub-spmi-a0/hpm2"};
static const char *locations[] = {"back-left", "back-middle", "back-right"};
static u32 data_status[3];
static int powerups, reads;
static u64 now, waited;
static char out[4096];
static int printf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t len = strlen(out);
    int n = vsnprintf(out + len, sizeof(out) - len, fmt, ap);
    va_end(ap);
    return n;
}
#define printf printf_
static int adt_path_offset(const void *a, const char *path)
{
    if (!strcmp(path, "/product")) return 100;
    for (int i = 0; i < 3; i++)
        if (!strcmp(path, ports[i])) return i;
    return -1;
}
static const void *adt_getprop(const void *a, int node, const char *prop, u32 *len)
{
    static const char product[] = "Mac mini";
    static const char rids[4];
    if (node == 100 && !strcmp(prop, "product-name") && mini) {
        if (len) *len = sizeof(product);
        return product;
    }
    if (node >= 0 && node < 3 && !strcmp(prop, "port-location")) {
        if (len) *len = strlen(locations[node]) + 1;
        return locations[node];
    }
    if (node == 1 && uart_on_hpm1 && !strcmp(prop, "uart-hpm-rids")) {
        if (len) *len = sizeof(rids);
        return rids;
    }
    return NULL;
}
static int tps6598x_foreach_hpm(hpm_match_t *match, hpm_action_t *action, void *data)
{
    for (int i = 0; i < 3; i++) {
        char path[64];
        strcpy(path, ports[i]);
        if (match(path, data) && action)
            action(path, (tps6598x_dev_t *)&ports[i], data);
    }
    return 0;
}
static int tps6598x_powerup(tps6598x_dev_t *tps) { powerups++; return 0; }
static int tps6598x_read_status(tps6598x_dev_t *tps, u32 *status, u32 *ds)
{
    int i = (const char **)tps - ports;
    assert(powerups == 3);
    reads++;
    *ds = data_status[i];
    *status = data_status[i] ? TPS6598X_STATUS_PLUG_PRESENT : 0;
    return 0;
}
static u64 get_ticks(void) { return now; }
static u64 ticks_to_usecs(u64 t) { return t; }
static void udelay(u64 us) { waited += us; now += us; }
''' + source + r'''
static void reset(void)
{
    mini = uart_on_hpm1 = true;
    memset(data_status, 0, sizeof(data_status));
    powerups = reads = 0;
    now = waited = 0;
    out[0] = 0;
}
int main(void)
{
    /* Host on the debug port (hpm1 by uart-hpm-rids): no warning */
    reset();
    data_status[1] = 0x90000091;
    debugport_powerup();
    now += 500000;
    debugport_check();
    assert(powerups == 3 && reads == 3 && waited == 1500000);
    assert(strstr(out, "host connected to the back-middle debug port"));
    assert(!strstr(out, "WARNING"));

    /* Host elsewhere, a DP adapter (this Mac is the host) is not a host */
    reset();
    data_status[0] = 0x90000091;
    data_status[2] = 0x80008511;
    debugport_powerup();
    now += 3000000;
    debugport_check();
    assert(waited == 0);
    assert(strstr(out, "plugged into the back-left port"));
    assert(strstr(out, "debug (DFU) port is back-middle"));
    assert(!strstr(out, "back-right port"));
    assert(strstr(out, "KIS and the aura proxy only work there"));

    /* Without uart-hpm-rids the debug port is hpm0 */
    reset();
    uart_on_hpm1 = false;
    data_status[0] = 0x90000091;
    debugport_powerup();
    debugport_check();
    assert(strstr(out, "back-left debug port") && !strstr(out, "WARNING"));

    /* Not a Mac mini: nothing at all */
    reset();
    mini = false;
    data_status[0] = 0x90000091;
    debugport_powerup();
    debugport_check();
    assert(!powerups && !reads && !out[0]);
    return 0;
}
'''
    binary = tmp_path / "debugport"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
