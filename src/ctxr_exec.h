/* SPDX-License-Identifier: MIT */

#ifndef CTXR_EXEC_H
#define CTXR_EXEC_H

#include "types.h"

enum ctxr_range {
    CTXR_RANGE_A,
    CTXR_RANGE_B,
    CTXR_RANGE_C,
    CTXR_RANGE_D,
    CTRR_RANGE_C,
    CTRR_RANGE_D,
};

/* An armed range as [start, end), or false if it reads as empty, or can't be read here. */
bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end);
/* Secondaries can't execute outside the armed CTXR windows at EL2. Asks a secondary, once a boot.
 */
bool ctxr_exec_confined(void);
/* The physical window the OS runs in at EL2, false if there is none to use. */
bool ctxr_exec_window(u64 *base, u64 *size);
/* False on a confined machine with no window, or with a probe still pending, having said why. */
bool ctxr_exec_os_may_start(void);

#endif
