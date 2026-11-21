/* SPDX-License-Identifier: MIT */

#ifndef PROXY_FALLBACK_H
#define PROXY_FALLBACK_H

#include "types.h"

/*
 * At most this many self-reboots in a row before a stage stops in the proxy: the chain's
 * failed-boot count (aura.failed-boots=) bounds them.
 */
#define PROXY_FALLBACK_REBOOTS 2

/* How long a stage that falls back waits for a host before it reboots */
#define PROXY_FALLBACK_HOST_MS 15000

enum proxy_fallback_action {
    /* Stop in the proxy, as before */
    PROXY_FALLBACK_PROXY,
    /* An aura-try trial: reboot to the configured image, the trial marker is spent */
    PROXY_FALLBACK_REBOOT_TRIAL,
    /* Reboot, and let iBoot count it: the chain's failed-boot count is under the limit */
    PROXY_FALLBACK_REBOOT_COUNTED,
};

enum proxy_fallback_action proxy_fallback_decide(bool trial, bool start_failed, bool count_passed,
                                                 u8 failed_boots);

/*
 * A stage that would stop in the proxy calls this first, with the reason and whether something
 * failed to start (as opposed to an image with nothing to start). It waits a bounded time for a
 * host and reboots if none came and the decision above allows it. It returns, with the chain left
 * (pmu_chain_leave()), when the proxy should run: true if a host attached in the window (it has a
 * carrier already), false if no reboot is allowed.
 */
bool proxy_fallback(const char *why, bool start_failed);

#endif
