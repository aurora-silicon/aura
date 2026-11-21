/* SPDX-License-Identifier: MIT */

#include "proxy_fallback.h"
#include "boot_options.h"
#include "pmu_counter.h"
#include "uartproxy.h"
#include "utils.h"
#include "wdt.h"

/*
 * Nobody may be at the Mac when a stage would stop in the proxy, and only a USB host can drive it
 * or reboot it there. Reboot instead where a reboot is known to end:
 * - An aura-try trial spent its marker on the ESP before its image was read, so the reboot lands
 *   on the configured image. This needs nothing from Stage 1 beyond the trial itself.
 * - Otherwise, when something failed to start (an image with nothing to start stops in the proxy
 *   as before), while the failed-boot count the earlier stages handed on is under the limit. iBoot
 *   counts each such reset, and no stage clears the count before the chain leaves aura, so a
 *   failure that persists ends in the proxy after PROXY_FALLBACK_REBOOTS reboots. A stage handed
 *   no count (an older Stage 1 clears it at start) can't tell a first failure from a cleared
 *   count, and keeps the proxy.
 */
enum proxy_fallback_action proxy_fallback_decide(bool trial, bool start_failed, bool count_passed,
                                                 u8 failed_boots)
{
    if (trial)
        return PROXY_FALLBACK_REBOOT_TRIAL;
    if (start_failed && count_passed && failed_boots < PROXY_FALLBACK_REBOOTS)
        return PROXY_FALLBACK_REBOOT_COUNTED;
    return PROXY_FALLBACK_PROXY;
}

bool proxy_fallback(const char *why, bool start_failed)
{
    u8 failed = 0;
    bool passed = pmu_failed_boots_passed(&failed);
    enum proxy_fallback_action action =
        proxy_fallback_decide(aura_trial_enabled(), start_failed, passed, failed);

    if (action == PROXY_FALLBACK_PROXY) {
        if (!start_failed)
            printf("%s: nothing failed to start: no reboot, proxy\n", why);
        else if (!passed)
            printf("%s: no failed-boot count from an earlier stage: no reboot, proxy\n", why);
        else
            printf("%s: %u failed boots in a row already: no reboot, proxy\n", why, failed);
        pmu_chain_leave();
        return false;
    }

    /* Shown on the panel */
    if (action == PROXY_FALLBACK_REBOOT_TRIAL)
        printf("\naura-try: %s; waiting %u s for a host, then rebooting to the configured "
               "image\n",
               why, PROXY_FALLBACK_HOST_MS / 1000);
    else
        printf("\n%s; waiting %u s for a host, then rebooting (%u failed boots in a row before, "
               "limit %u)\n",
               why, PROXY_FALLBACK_HOST_MS / 1000, failed, PROXY_FALLBACK_REBOOTS);

    if (uartproxy_wait_host(PROXY_FALLBACK_HOST_MS)) {
        if (action == PROXY_FALLBACK_REBOOT_TRIAL)
            /* Not disarmed for the host: a host that goes away must not strand the trial. */
            printf("Proxy: host attached, no reboot; the trial watchdog still runs (%u s), a host "
                   "that needs longer disarms it\n",
                   WDT_TRIAL_SECONDS);
        else
            printf("Proxy: host attached, no reboot\n");
        pmu_chain_leave();
        return true;
    }

    /*
     * A trial's reboot is a deliberate one, so clear the count first, as run_actions() does before
     * its other deliberate reboots: the configured image then starts with only this reset counted.
     */
    if (action == PROXY_FALLBACK_REBOOT_TRIAL)
        pmu_chain_leave();
    printf("No host: rebooting\n");
    flush_and_reboot();
    return false;
}
