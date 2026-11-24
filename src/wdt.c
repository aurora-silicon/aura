/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "wdt.h"
#include "adt.h"
#include "memory.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

#define WDT_CTL_RESET_EN BIT(2)

/* The block's third watchdog, which an OS arms where it doesn't use the primary */
#define WDT2_CTL        0x2c
#define WDT2_CTL_ENABLE BIT(0)

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
    /* A fault before wdt_disable() (early init) must still reset the Mac, not spin */
    if (!wdt_base && adt) {
        int path[8];
        u64 base = 0;
        if (adt_path_offset_trace(adt, "/arm-io/wdt", path) >= 0 &&
            !adt_get_reg(adt, path, "reg", 0, &base, NULL))
            wdt_base = base;
    }
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}

/*
 * The ADT's version 2 layout that wdt_s2r_guard_enter() needs: the primary block (both watchdogs
 * an OS may arm) and the secondary. The primary's base is kept for wdt_reboot() and cleaned to
 * memory, so that a wake, with the caches off, can still reset the SoC through it.
 */
bool wdt_s2r_guard_supported(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);
    u64 primary, primary_size, secondary, secondary_size;
    u32 version;

    if (node < 0 || ADT_GETPROP(adt, node, "wdt-version", &version) < 0 || version != 2 ||
        adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) || (primary & 3) ||
        primary_size < WDT2_CTL + 4 ||
        adt_get_reg(adt, path, "reg", 2, &secondary, &secondary_size) || secondary_size != 4)
        return false;
    wdt_base = primary;
    dc_cvac_range(&wdt_base, sizeof(wdt_base));
    return true;
}

/*
 * A dead-man for a step on the way back from a system sleep, with the OS live. The watchdogs
 * are read before anything is written. If the OS left one of the two it uses armed (the
 * primary, as Linux's T8103 sleep does, or the third), that covers the step and nothing is
 * written.
 * Otherwise, on the ADT's version 2 layout with the secondary watchdog off (wdt_arm()'s
 * conditions), the primary is borrowed: armed for the given time and read back. -1: the step
 * has no watchdog and must not run.
 */
int wdt_s2r_guard_enter(struct wdt_guard *guard, unsigned seconds)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);
    u64 primary, primary_size, secondary, secondary_size;
    u32 version;

    guard->borrowed = false;
    if (seconds < 1 || seconds > 178 || node < 0 ||
        adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) || (primary & 3) ||
        primary_size < WDT2_CTL + 4)
        return -1;

    guard->base = primary;
    guard->ctl = read32(primary + WDT_CTL);
    guard->count = read32(primary + WDT_COUNT);
    guard->alarm = read32(primary + WDT_ALARM);
    if ((guard->ctl & WDT_CTL_RESET_EN) || (read32(primary + WDT2_CTL) & WDT2_CTL_ENABLE))
        return 0;

    if (ADT_GETPROP(adt, node, "wdt-version", &version) < 0 || version != 2 ||
        adt_get_reg(adt, path, "reg", 2, &secondary, &secondary_size) || secondary_size != 4 ||
        read32(secondary))
        return -1;

    u32 alarm = seconds * 24000000u;
    guard->borrowed = true;
    write32(primary + WDT_CTL, 0);
    write32(primary + WDT_COUNT, 0);
    write32(primary + WDT_ALARM, alarm);
    write32(primary + WDT_CTL, WDT_CTL_RESET_EN);
    sysop("dsb sy");
    if (read32(primary + WDT_ALARM) != alarm || !(read32(primary + WDT_CTL) & WDT_CTL_RESET_EN))
        return -1;
    return 0;
}

/* A borrowed primary watchdog back as it was read; the secondary is never touched. */
void wdt_s2r_guard_exit(const struct wdt_guard *guard)
{
    if (!guard->borrowed)
        return;
    write32(guard->base + WDT_CTL, 0);
    write32(guard->base + WDT_COUNT, guard->count);
    write32(guard->base + WDT_ALARM, guard->alarm);
    write32(guard->base + WDT_CTL, guard->ctl);
    sysop("dsb sy");
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

/*
 * A host's request (P_WDT_ARM). wdt_arm() takes only the ADT's version 2 layout, whose register
 * sequence it reads back, so this needs no list of chips: one with another layout refuses.
 */
int wdt_arm_seconds(unsigned seconds)
{
    return wdt_arm(seconds);
}

/*
 * A trial boot (aura-try, chainload.c) has nobody to reset it if a stage hangs, so each aura
 * stage of it disarms iBoot's watchdog as usual and then arms its own, from the ADT's version 2
 * layout, until Linux's driver finds it running and keeps it fed. The next boot takes the
 * normal path, as the trial marker is already spent.
 */
static bool wdt_boot_armed;

int wdt_arm_trial(void)
{
    wdt_boot_armed = false;
    wdt_disable();
    return wdt_arm(WDT_TRIAL_SECONDS);
}

/*
 * A plain boot disarms iBoot's watchdog as soon as Stage 1 starts, and nothing re-arms one until
 * Linux, so a Stage 1 or early Stage 2 that stops (an NVMe command that never completes, a failed
 * read that drops it into the proxy with no host) stays on screen until someone holds the power
 * button. Stage 1 arms the primary watchdog for its longest period instead. The first host it
 * sees (a synced window or proxy request) disarms it, so a debugging session is never reset
 * under it; Stage 2's own wdt_disable() at its start disarms it on the normal path. Its reads
 * take well under a second, and its bounded host windows are 10-100 s.
 */
int wdt_arm_boot(void)
{
    if (wdt_arm(WDT_BOOT_SECONDS))
        return -1;
    wdt_boot_armed = true;
    return 0;
}

bool wdt_boot_watchdog_armed(void)
{
    return wdt_boot_armed;
}

/* Only the boot watchdog: a trial's or a host-armed one is not ours to disarm. */
void wdt_host_attached(void)
{
    if (!wdt_boot_armed)
        return;
    wdt_boot_armed = false;
    wdt_disable();
    printf("WDT: host attached, boot watchdog off\n");
}
