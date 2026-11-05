/* SPDX-License-Identifier: MIT */

#ifndef __ATC_H__
#define __ATC_H__

#include "types.h"

int kboot_setup_atc(void *dt);
bool kboot_j614s_hdmi_ready(void);

#endif