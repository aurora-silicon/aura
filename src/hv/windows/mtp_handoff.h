/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_MTP_HANDOFF_H
#define HV_WINDOWS_MTP_HANDOFF_H

#include "types.h"

bool hv_windows_mtp_prepare(void);
bool hv_windows_mtp_map_guest(void);
bool hv_windows_mtp_stop(void);
void hv_windows_mtp_poll(void);
void hv_windows_mtp_report(void);

#endif
