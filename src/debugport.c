/* SPDX-License-Identifier: GPL-2.0-only */

#include "debugport.h"
#include "adt.h"
#include "soc.h"
#include "string.h"
#include "tps6598x.h"
#include "utils.h"

/*
 * A Mac mini has one debug (DFU) USB-C port. Its port controller is the one
 * with "uart-hpm-rids" in the ADT, or hpm0 when no port controller has that
 * property, the same port tps6598x_enable_debugusb() uses. A debug cable
 * shows up as a USB host: plug present, data connection, and this Mac as the
 * UFP. On J873g the KIS proxy only answers on the debug port, so a cable in
 * any other port just looks dead. Warn about it on the boot console.
 *
 * SPMI port controllers (J873g) report a new partner only some time after
 * they enter S0, so the ports are put in S0 early and read at least
 * DEBUGPORT_SETTLE_US later.
 */
#define DEBUGPORT_SETTLE_US 2000000
#define DEBUGPORT_MAX_HOSTS 4
#define DEBUGPORT_PATH_LEN  64

struct debugport_scan {
    char uart_path[DEBUGPORT_PATH_LEN];
    char hpm0_path[DEBUGPORT_PATH_LEN];
    const char *debug_path;
    char hosts[DEBUGPORT_MAX_HOSTS][DEBUGPORT_PATH_LEN];
    int host_count;
    bool debug_host;
};

static u64 powerup_ticks;
static bool powered_up, spmi_ports;

static bool debugport_is_mac_mini(void)
{
    int node = adt_path_offset(adt, "/product");
    u32 len = 0;
    const char *name = node < 0 ? NULL : adt_getprop(adt, node, "product-name", &len);

    return name && len >= 8 && !strncmp(name, "Mac mini", 8);
}

/* The port's name as the ADT gives it ("back-middle"), else its path. */
static const char *debugport_location(const char *hpm_path)
{
    int node = adt_path_offset(adt, hpm_path);
    u32 len = 0;
    const char *loc = node < 0 ? NULL : adt_getprop(adt, node, "port-location", &len);

    return loc && len && !loc[len - 1] ? loc : hpm_path;
}

static void debugport_copy(char *dst, const char *src)
{
    strncpy(dst, src, DEBUGPORT_PATH_LEN - 1);
    dst[DEBUGPORT_PATH_LEN - 1] = 0;
}

static bool debugport_find(char *hpm_path, void *data)
{
    struct debugport_scan *scan = data;
    int node = adt_path_offset(adt, hpm_path);
    size_t len = strlen(hpm_path);

    if (!scan->uart_path[0] && node >= 0 && adt_getprop(adt, node, "uart-hpm-rids", NULL))
        debugport_copy(scan->uart_path, hpm_path);
    if (len >= 4 && !strcmp(hpm_path + len - 4, "hpm0"))
        debugport_copy(scan->hpm0_path, hpm_path);
    /* Collect only; no port controller is opened. */
    return false;
}

static bool debugport_every_port(char *hpm_path, void *data)
{
    UNUSED(hpm_path);
    UNUSED(data);
    return true;
}

static int debugport_powerup_one(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    UNUSED(data);
    if (strstr(hpm_path, "spmi"))
        spmi_ports = true;
    if (tps6598x_powerup(tps) < 0)
        printf("debug port: %s did not enter S0\n", debugport_location(hpm_path));
    return HPM_ACTION_CONTINUE;
}

static int debugport_read_one(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    struct debugport_scan *scan = data;
    u32 status, data_status;

    if (tps6598x_read_status(tps, &status, &data_status) < 0) {
        printf("debug port: cannot read the %s port controller\n", debugport_location(hpm_path));
        return HPM_ACTION_CONTINUE;
    }
    if (!(status & TPS6598X_STATUS_PLUG_PRESENT) || !(data_status & TPS6598X_DATA_CONNECTION) ||
        !(data_status & TPS6598X_DATA_ROLE_UFP))
        return HPM_ACTION_CONTINUE;

    if (!strcmp(hpm_path, scan->debug_path))
        scan->debug_host = true;
    else if (scan->host_count < DEBUGPORT_MAX_HOSTS)
        debugport_copy(scan->hosts[scan->host_count++], hpm_path);
    return HPM_ACTION_CONTINUE;
}

void debugport_powerup(void)
{
    if (!debugport_is_mac_mini())
        return;
    tps6598x_foreach_hpm(debugport_every_port, debugport_powerup_one, NULL);
    powerup_ticks = get_ticks();
    powered_up = true;
}

void debugport_check(void)
{
    struct debugport_scan scan;

    if (!debugport_is_mac_mini())
        return;

    memset(&scan, 0, sizeof(scan));
    tps6598x_foreach_hpm(debugport_find, NULL, &scan);
    scan.debug_path = scan.uart_path[0] ? scan.uart_path : scan.hpm0_path;
    if (!scan.debug_path[0]) {
        printf("debug port: this Mac mini's debug (DFU) port is unknown\n");
        return;
    }

    if (spmi_ports && powered_up) {
        u64 elapsed = ticks_to_usecs(get_ticks() - powerup_ticks);
        if (elapsed < DEBUGPORT_SETTLE_US)
            udelay(DEBUGPORT_SETTLE_US - elapsed);
    }
    tps6598x_foreach_hpm(debugport_every_port, debugport_read_one, &scan);

    const char *debug_loc = debugport_location(scan.debug_path);
    if (scan.debug_host)
        printf("debug port: host connected to the %s debug port\n", debug_loc);

    for (int i = 0; i < scan.host_count; i++) {
        printf("\n!!! WARNING: debug cable in the wrong USB-C port !!!\n");
        printf("A USB host is plugged into the %s port.\n", debugport_location(scan.hosts[i]));
        printf("This Mac mini's debug (DFU) port is %s.\n", debug_loc);
        printf(chip_id == T8152 ? "KIS and the aura proxy only work there.\n"
                                : "DFU and serial debugging need that port.\n");
        printf("Move the cable to the %s port.\n\n", debug_loc);
    }
}
