/* SPDX-License-Identifier: MIT */

#ifndef MONITOR_H
#define MONITOR_H

#include "types.h"

/* Set before the OS handoff: every CPU enters the OS at EL1 under the monitor. */
extern bool monitor_active;
/* The boot CPU's CNTFRQ_EL0, which every CPU gets before it enters the OS. */
extern u64 monitor_cntfrq;

bool monitor_required(void);
void monitor_enter_os(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));

#endif
