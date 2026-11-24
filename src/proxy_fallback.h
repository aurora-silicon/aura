/* SPDX-License-Identifier: MIT */

#ifndef PROXY_FALLBACK_H
#define PROXY_FALLBACK_H

#include "types.h"

/* How long a trial (or m1n1.wdt=keep) stage that would stop in the proxy waits for a host */
#define PROXY_FALLBACK_HOST_MS 15000

/*
 * A stage that would stop in the proxy with nobody to drive it calls this first, with the reason.
 * On an aura-try trial, or with m1n1.wdt=keep, it waits PROXY_FALLBACK_HOST_MS for a host and
 * reboots if none came. It returns when the proxy should run: true if a host attached in the
 * window (it has a carrier already), false if this is neither (the proxy runs as before).
 */
bool proxy_fallback(const char *why);

#endif
