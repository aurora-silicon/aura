/* SPDX-License-Identifier: MIT */
/*
 * The guest-side entry: how a process hands its request block to the guest's kernel, which
 * issues aura's hvc with it (src/auractl_abi.h). It is the guest's own, not part of this tree;
 * entry_none.c, the only one here, refuses every call. Build another with `make ENTRY=<name>`
 * from entry_<name>.c.
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
