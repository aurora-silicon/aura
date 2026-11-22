/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "types.h"
#define PMGR_DIE_ID GENMASK(31, 28)
int pmgr_reset(int die, const char *name);
