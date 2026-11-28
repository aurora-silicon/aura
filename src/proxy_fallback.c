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
 * A plain boot that stops in the proxy is in the same place. Stage 1 runs its boot watchdog for
 * that (wdt_arm_boot()). A Stage 2 used to disarm it at its start, so one that could not start its
 * payload stayed in the proxy until someone held the power button or reset it over USB-C (on the
 * M4 MacBook Air: an ANS that a warm boot's start from off could not bring up, about one warm boot
 * in eight; a later boot brings it up). An ESP Stage 2 now runs it from its start (main.c), and a
 * stop that finds it off arms it again, unless a host has attached already, this is a debug boot
 * or a BRINGUP build (which never hands off and stays in the proxy by design). It then stops in
 * the proxy as before. The first proxy request from a host turns it off (wdt_host_attached()); a
 * host that only holds the port open and sends nothing for that long is reset like an absent one.
 * Without a host the Mac restarts and the next boot tries again.
 */
bool proxy_fallback(const char *why)
{
    if (!aura_trial_enabled()) {
#ifndef BRINGUP
#ifdef ESP_STAGE2
        /* It runs from Stage 2's start (main.c); arm it here only if something turned it off. */
        bool keep = wdt_host_was_attached() || aura_debug_enabled();
        bool arm = !wdt_boot_watchdog_armed();
#else
        /* Stage 1's own boot watchdog runs, and its own stop explains it. */
        bool keep = wdt_boot_watchdog_armed() || wdt_host_was_attached() || aura_debug_enabled();
        bool arm = true;
#endif
        if (keep)
            return false;
        if (arm && wdt_arm_boot()) {
            printf("%s; no boot watchdog, staying in the proxy\n", why);
            return false;
        }
        /*
         * Shown on the panel. Like Stage 1's own stop, this repeats on every boot that fails the
         * same way: no count survives the reset for this stage to read (the failed storage holds
         * none and Stage 1 clears iBoot's failed-boot count), so the owner ends it with a host
         * or the power button.
         */
        printf("\n%s. Without a host, this Mac restarts by itself within %u s to try again.\n"
               "  A host that sends a proxy request in that time stops the restarts.\n"
               "  Hold the power button to shut down, then hold it again to choose another "
               "system.\n",
               why, WDT_BOOT_SECONDS);
#endif
        return false;
    }

    /*
     * A Stage 1 that took the trial marker still runs its boot watchdog, and the first host turns
     * that one off (wdt_host_attached()). Make it the trial watchdog, with a fresh period, before a
     * host can attach, so a host that goes away cannot strand the trial. Without a watchdog, a
     * host window could, so reboot now instead.
     */
    if (wdt_boot_watchdog_armed() && wdt_arm_trial()) {
        printf("\naura-try: %s; no trial watchdog, rebooting to the configured image\n", why);
        flush_and_reboot();
    }

    /* Shown on the panel */
    printf("\naura-try: %s; waiting %u s for a host, then rebooting to the configured image\n", why,
           PROXY_FALLBACK_HOST_MS / 1000);

    if (uartproxy_wait_host(PROXY_FALLBACK_HOST_MS)) {
        /* Not disarmed for the host: a host that goes away must not strand the trial. */
        printf("Proxy: host attached, no reboot; the trial watchdog still runs (%u s), a host "
               "that needs longer disarms it\n",
               WDT_TRIAL_SECONDS);
        return true;
    }

    printf("No host: rebooting\n");
    flush_and_reboot();
}
