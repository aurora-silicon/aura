/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "wdt.h"
#include "adt.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

static u64 wdt_base = 0;

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        printf("Failed to get WDT reg property!\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", wdt_base);
    write32(wdt_base + WDT_CTL, 0);
    printf("Primary WDT disabled\n");

    // disable secondary watchdog if wdt-version is 2 or 3
    u32 wdt_version;
    if (ADT_GETPROP(adt, node, "wdt-version", &wdt_version) < 0)
        return;

    if (wdt_version == 2 || wdt_version == 3) {
        u64 wdt_2nd = 0;
        if (adt_get_reg(adt, path, "reg", 2, &wdt_2nd, NULL)) {
            printf("Failed to get WDT reg[2] property!\n");
            return;
        }

        printf("Secondary WDT register @ 0x%lx\n", wdt_2nd);
        write32(wdt_2nd, 0);
        printf("Secondary WDT disabled\n");
    }
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}

static int wdt_arm(unsigned seconds)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);
    u64 primary, primary_size, secondary, secondary_size;
    u32 version;
    if (seconds < 1 || seconds > 178 || node < 0 ||
        ADT_GETPROP(adt, node, "wdt-version", &version) < 0 || version != 2 ||
        adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) ||
        adt_get_reg(adt, path, "reg", 2, &secondary, &secondary_size) || (primary & 3) ||
        primary_size < 0x20 || secondary_size != 4 || read32(secondary))
        return -1;

    u32 alarm = seconds * 24000000u;
    write32(primary + WDT_CTL, 0);
    write32(primary + WDT_COUNT, 0);
    write32(primary + WDT_ALARM, alarm);
    write32(primary + WDT_CTL, 4);
    sysop("dsb sy");
    if (read32(primary + WDT_ALARM) != alarm || !(read32(primary + WDT_CTL) & 4))
        return -1;
    wdt_base = primary;
    return 0;
}

int wdt_arm_seconds(unsigned seconds)
{
    if (chip_id != T8140 && chip_id != T6040)
        return -1;
    return wdt_arm(seconds);
}

/*
 * A trial boot (aura-try, chainload.c) has nobody to reset it if a stage hangs, so each aura
 * stage of it disarms iBoot's watchdog as usual and then arms its own, from the ADT's version 2
 * layout, until Linux's driver finds it running and keeps it fed. The next boot takes the
 * normal path, as the trial marker is already spent.
 */
int wdt_arm_trial(void)
{
    wdt_disable();
    return wdt_arm(WDT_TRIAL_SECONDS);
}
