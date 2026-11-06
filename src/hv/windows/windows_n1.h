/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WINDOWS_N1_H
#define WINDOWS_N1_H
#include "types.h"
#define HV_WINDOWS_N1_DESCRIPTOR_SIZE 128
/* Explicit pre-guest operation. -1 rejected/held; -2 cannot prove drain.
 * On failure the descriptor is cleared. No automatic call from hv_init. */
int hv_windows_n1_prepare(void *descriptor, size_t size);
#endif
