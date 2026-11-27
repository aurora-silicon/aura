/* SPDX-License-Identifier: MIT */
/*
 * The guest-side entry: how a process hands its request block to the guest's kernel, which
 * issues aura's hvc with it (src/auractl_abi.h). `make ENTRY=<name>` picks entry_<name>.c:
 * `iokit` (entry_iokit.c) goes through the AuractlDevice kext in this tree; `xnu`
 * (entry_xnu.c) calls an acct(2) the host patched into the hvc, with no kext; `none`
 * (entry_none.c) refuses every call, so only `auractl print` works. A guest with a different
 * kernel path can add its own entry_<name>.c.
 */
#ifndef AURACTL_ENTRY_H
#define AURACTL_ENTRY_H

#include "../../src/auractl_abi.h"

#define AURACTL_ENTRY_NONE (-1) /* no entry in this build */

/* 0, aura's errno for the call (AURACTL_E*), or AURACTL_ENTRY_NONE. req and every buffer it names
 * are resident and writable in this process. */
int auractl_enter(struct auractl_req *req);

/* For `auractl status`. */
const char *auractl_entry_name(void);

#endif
