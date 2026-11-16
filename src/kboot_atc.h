/* SPDX-License-Identifier: MIT */

#ifndef __ATC_H__
#define __ATC_H__

#include "types.h"

int kboot_setup_atc(void *dt);
/* 0: completed/no handoff; 1: safely left disabled; -1: invalid ownership tree. */
int kboot_handoff_atc0(void *dt, int capacity, bool safe);
bool kboot_j614s_hdmi_ready(void);

#endif
