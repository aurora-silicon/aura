/* SPDX-License-Identifier: MIT */

#include "proxy_fallback.h"
#include "boot_options.h"
#include "uartproxy.h"
#include "utils.h"
#include "wdt.h"

/*
 * Nobody may be at the Mac when an aura-try trial would stop in the proxy, and only a host can
 * drive it or reboot it there; the trial watchdog is the only other way out. The trial spent its
 * marker on the ESP before its image was read, so a reboot lands on the configured image. This
 * needs nothing from Stage 1 beyond the trial itself.
 *
 * A stage booted with m1n1.wdt=keep has the same fallback behind it in a first stage that is not
 * aura: a reset before the OS counts as a failed boot, and that stage then boots its other slot.
 * It reboots after the same host window, whether or not the watchdog is still armed (a power
 * cycle does not count as a failed boot). Any other stage stops in the proxy as before.
 */
bool proxy_fallback(const char *why)
{
    bool trial = aura_trial_enabled();

    if (!trial && !boot_wdt_keep_enabled())
        return false;

    /*
     * A Stage 1 that took the trial marker still runs its boot watchdog, and the first host turns
     * that one off (wdt_host_attached()). Make it the trial watchdog, with a fresh period, before a
     * host can attach, so a host that goes away cannot strand the trial. Without a watchdog, a
     * host window could, so reboot now instead.
     */
    if (trial && wdt_boot_watchdog_armed() && wdt_arm_trial()) {
        printf("\naura-try: %s; no trial watchdog, rebooting to the configured image\n", why);
        flush_and_reboot();
    }

    /* Shown on the panel */
    if (trial)
        printf("\naura-try: %s; waiting %u s for a host, then rebooting to the configured image\n",
               why, PROXY_FALLBACK_HOST_MS / 1000);
    else
        printf("\nm1n1.wdt=keep: %s; waiting %u s for a host, then rebooting into the first "
               "stage's fallback\n",
               why, PROXY_FALLBACK_HOST_MS / 1000);

    if (uartproxy_wait_host(PROXY_FALLBACK_HOST_MS)) {
        /* Not disarmed for the host: a host that goes away must not strand the boot. */
        if (trial)
            printf("Proxy: host attached, no reboot; the trial watchdog still runs (%u s), a host "
                   "that needs longer disarms it\n",
                   WDT_TRIAL_SECONDS);
        else
            printf("Proxy: host attached, no reboot; a watchdog the first stage armed still runs, "
                   "a host that needs longer disarms it\n");
        return true;
    }

    printf("No host: rebooting\n");
    flush_and_reboot();
}
