/* SPDX-License-Identifier: MIT */

#ifndef CTXR_EXEC_H
#define CTXR_EXEC_H

#include "types.h"

/* Secondaries can't execute outside the armed CTXR windows at EL2. Asks a secondary, once a boot.
 */
bool ctxr_exec_confined(void);
/* The physical window the OS runs in at EL2, false if there is none to use. */
bool ctxr_exec_window(u64 *base, u64 *size);
/* False on a confined machine with no window, having said why: don't enter the OS. */
bool ctxr_exec_os_may_start(void);

#endif
