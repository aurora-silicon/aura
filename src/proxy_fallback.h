/* SPDX-License-Identifier: MIT */

#ifndef PROXY_FALLBACK_H
#define PROXY_FALLBACK_H

#include "types.h"

/* How long an aura-try trial that would stop in the proxy waits for a host before it reboots */
#define PROXY_FALLBACK_HOST_MS 15000

/*
 * A stage that would stop in the proxy with nobody to drive it calls this first, with the reason.
 * On an aura-try trial it waits PROXY_FALLBACK_HOST_MS for a host and reboots if none came. It
 * returns when the proxy should run: true if a host attached in the window (it has a carrier
 * already), false if this is not a trial (the proxy runs as before).
 */
bool proxy_fallback(const char *why);

#endif
