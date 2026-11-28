/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "wdt.h"
#include "adt.h"
#include "memory.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

#define WDT_WD2_COUNT 0x20
#define WDT_WD2_BITE  0x24
#define WDT_WD2_BARK  0x28
#define WDT_WD2_CTL   0x2c

#define WDT_CTL_IRQ_EN     BIT(0)
#define WDT_CTL_IRQ_STATUS BIT(1)
#define WDT_CTL_RESET_EN   BIT(2)

#define WDT_RECONFIG_MASK (3u << 4)
#define WDT_RECONFIG_WD2  (1u << 4)

static u64 wdt_base = 0;
static u64 wdt_reconfig = 0;

static void wdt_set_addrs(u64 primary, u64 reconfig)
{
    wdt_reconfig = reconfig;
    dc_cvac_range(&wdt_reconfig, sizeof(wdt_reconfig));
    sysop("dmb ishst");
    wdt_base = primary;
    dc_cvac_range(&wdt_base, sizeof(wdt_base));
}

static bool wdt_has_simple_reconfig(int node, int *path, u64 primary_size, u64 *reconfig)
{
    u64 addr, size;
    u32 version;

    if (primary_size < 0x30 || ADT_GETPROP(adt, node, "wdt-version", &version) < 0 || version < 3 ||
        !adt_getprop(adt, node, "simple-reconfig-wdog-support", NULL) ||
        adt_get_reg(adt, path, "reg", 3, &addr, &size) || (addr & 3) || size != 4)
        return false;

    *reconfig = addr;
    return true;
}

bool wdt_simple_reconfig_reg(u64 *primary_out, u64 *reconfig_out)
{
    int path[8];
    u64 primary = 0, primary_size = 0, reconfig = 0;
    int node;

    if (!adt)
        return false;
    node = adt_path_offset_trace(adt, "/arm-io/wdt", path);
    if (node < 0 || adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) || (primary & 3) ||
        !wdt_has_simple_reconfig(node, path, primary_size, &reconfig))
        return false;

    if (primary_out)
        *primary_out = primary;
    if (reconfig_out)
        *reconfig_out = reconfig;
    return true;
}

static u32 wdt_arm_wd2(u64 primary, u64 reconfig, u32 ticks)
{
    u32 r3 = read32(reconfig);
    write32(primary + WDT_WD2_CTL, 0);
    write32(primary + WDT_WD2_BITE, 0xffffffffu);
    write32(primary + WDT_WD2_COUNT, 0);
    write32(primary + WDT_WD2_BARK, ticks);
    write32(primary + WDT_WD2_CTL, WDT_CTL_IRQ_STATUS);
    sysop("dsb sy");
    write32(reconfig, (r3 & ~WDT_RECONFIG_MASK) | WDT_RECONFIG_WD2);
    sysop("dsb sy");
    write32(primary + WDT_WD2_CTL, WDT_CTL_IRQ_EN);
    write32(primary + WDT_WD2_COUNT, 0);
    sysop("dsb sy");
    return r3;
}

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    u64 primary = 0, primary_size = 0;
    if (adt_get_reg(adt, path, "reg", 0, &primary, &primary_size)) {
        printf("Failed to get WDT reg property!\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", primary);
    write32(primary + WDT_CTL, 0);
    u64 reconfig = 0;
    if (wdt_has_simple_reconfig(node, path, primary_size, &reconfig)) {
        write32(primary + WDT_WD2_CTL, 0);
        wdt_set_addrs(primary, reconfig);
    } else {
        wdt_set_addrs(primary, 0);
    }
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
        u64 base = 0, size = 0, reconfig = 0;
        int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);
        if (node >= 0 && !adt_get_reg(adt, path, "reg", 0, &base, &size)) {
            wdt_set_addrs(base,
                          wdt_has_simple_reconfig(node, path, size, &reconfig) ? reconfig : 0);
        }
    }
    if (!wdt_base)
        return;

    if (wdt_reconfig) {
        wdt_arm_wd2(wdt_base, wdt_reconfig, 0x100000);
        return;
    }

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, WDT_CTL_RESET_EN);
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
    u64 primary, primary_size, secondary, secondary_size, reconfig = 0;
    u32 version;

    if (node < 0 || ADT_GETPROP(adt, node, "wdt-version", &version) < 0 || version != 2 ||
        adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) || (primary & 3) ||
        primary_size < WDT_WD2_CTL + 4 ||
        adt_get_reg(adt, path, "reg", 2, &secondary, &secondary_size) || secondary_size != 4)
        return false;
    wdt_set_addrs(primary,
                  wdt_has_simple_reconfig(node, path, primary_size, &reconfig) ? reconfig : 0);
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
        primary_size < WDT_WD2_CTL + 4)
        return -1;

    guard->base = primary;
    guard->ctl = read32(primary + WDT_CTL);
    guard->count = read32(primary + WDT_COUNT);
    guard->alarm = read32(primary + WDT_ALARM);
    if ((guard->ctl & WDT_CTL_RESET_EN) || (read32(primary + WDT_WD2_CTL) & WDT_CTL_IRQ_EN))
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
    u64 primary, primary_size, secondary, secondary_size, reconfig = 0;
    u32 version;
    if (seconds < 1 || seconds > 178 || node < 0 ||
        ADT_GETPROP(adt, node, "wdt-version", &version) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &primary, &primary_size) ||
        adt_get_reg(adt, path, "reg", 2, &secondary, &secondary_size) || (primary & 3) ||
        primary_size < 0x20 || secondary_size != 4 || read32(secondary))
        return -1;

    u32 alarm = seconds * 24000000u;

    /*
     * On wdt-version >= 3 machines reporting simple-reconfig-wdog-support with a 4-byte reg[3],
     * arm WD2 in bark-IRQ mode with reg[3] bits 5:4 = 01 so expiry takes the simple-reconfig
     * cold-reset path instead of stalling on RESET_EN. Machines with wdt-version 2 stay on WD1
     * RESET_EN so existing kernels (which pet WD1) keep working even when the ADT also lists
     * simple-reconfig-wdog-support.
     */
    if (wdt_has_simple_reconfig(node, path, primary_size, &reconfig)) {
        u32 prev_r3 = wdt_arm_wd2(primary, reconfig, alarm);
        if (read32(primary + WDT_WD2_BARK) != alarm ||
            !(read32(primary + WDT_WD2_CTL) & WDT_CTL_IRQ_EN) ||
            (read32(reconfig) & WDT_RECONFIG_MASK) != WDT_RECONFIG_WD2) {
            write32(primary + WDT_WD2_CTL, 0);
            write32(reconfig, prev_r3);
            sysop("dsb sy");
            return -1;
        }
        wdt_set_addrs(primary, reconfig);
        return 0;
    }

    if (version != 2)
        return -1;

    write32(primary + WDT_CTL, 0);
    write32(primary + WDT_COUNT, 0);
    write32(primary + WDT_ALARM, alarm);
    write32(primary + WDT_CTL, WDT_CTL_RESET_EN);
    sysop("dsb sy");
    if (read32(primary + WDT_ALARM) != alarm || !(read32(primary + WDT_CTL) & WDT_CTL_RESET_EN)) {
        write32(primary + WDT_CTL, 0);
        sysop("dsb sy");
        return -1;
    }
    wdt_set_addrs(primary, 0);
    return 0;
}

/*
 * A host's request (P_WDT_ARM). wdt_arm() selects the channel from the ADT (WD2 bark-IRQ +
 * simple-reconfig when wdt-version >= 3 with simple-reconfig-wdog-support and reg[3], otherwise
 * WD1 RESET_EN on wdt-version 2) and reads back what it wrote; other layouts refuse.
 */
static bool wdt_boot_armed;

int wdt_arm_seconds(unsigned seconds)
{
    int ret = wdt_arm(seconds);

    /*
     * The hardware now runs this period, not the boot watchdog's: a host that attaches later
     * must not turn off a watchdog it did not arm (usb_cdc's CDC transition watchdog on T8140).
     */
    if (!ret)
        wdt_boot_armed = false;
    return ret;
}

/*
 * A trial boot (aura-try, chainload.c) has nobody to reset it if a stage hangs, so each aura
 * stage of it disarms iBoot's watchdog as usual and then arms its own from the ADT layout until
 * Linux's driver finds it running and keeps it fed. The next boot takes the normal path, as the
 * trial marker is already spent.
 */

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
 * button. Stage 1 arms the watchdog for its longest period instead. The first host it sees (a
 * synced window or proxy request) disarms it, so a debugging session is never reset under it;
 * Stage 2's own wdt_disable() at its start disarms it on the normal path. Its reads take well
 * under a second, and its bounded host windows are 10-100 s.
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

/* A host has synced with this stage: a later stop in the proxy must not arm the boot watchdog. */
static bool wdt_host_seen;

bool wdt_host_was_attached(void)
{
    return wdt_host_seen;
}

/* Only the boot watchdog: a trial's or a host-armed one is not ours to disarm. */
void wdt_host_attached(void)
{
    wdt_host_seen = true;
    if (!wdt_boot_armed)
        return;
    wdt_boot_armed = false;
    wdt_disable();
    printf("WDT: host attached, boot watchdog off\n");
}
