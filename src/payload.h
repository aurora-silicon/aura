/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __PAYLOAD_H__
#define __PAYLOAD_H__

#include "types.h"

bool payload_logo(void **custom_128, void **custom_256);

int payload_run(void);
bool payload_start_failed(void);
int payload_boot_storage(const char *spec);

#endif
