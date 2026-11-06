/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WINDOWS_N1_H
#define WINDOWS_N1_H
#include "types.h"
#define HV_WINDOWS_N1_DESCRIPTOR_SIZE 128
#define HV_WINDOWS_N1_READ_CALL       0x4e3152443332ULL /* "N1RD32" */
#define HV_WINDOWS_N1_READ_ACK        0x4e3141434b31ULL /* "N1ACK1" */
#define HV_WINDOWS_N1_READ_VERSION    1
enum hv_windows_n1_read_status {
    HV_WINDOWS_N1_READ_OK = 0,
    HV_WINDOWS_N1_READ_UNAVAILABLE = -1,
    HV_WINDOWS_N1_READ_INVALID = -2,
    HV_WINDOWS_N1_READ_FAULT = -3,
};
/* Explicit pre-guest operation. -1 rejected/held; -2 cannot prove drain.
 * On failure the descriptor is cleared. No automatic call from hv_init. */
int hv_windows_n1_prepare(void *descriptor, size_t size);
/* HVC #0: x0=CALL, x1=physical address, x2=version, x3=0.
 * Return x0=ACK, x1=status, x2=u32 value (zero on failure), x3=version.
 * Caller holds the HV lock. Unknown magic leaves all registers unchanged. */
bool hv_windows_n1_census_hvc(u64 regs[4]);
#endif
