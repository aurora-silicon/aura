#pragma once
#include "types.h"
/* nvme.c powers ANS through its own gates on post-M4 chips; the simulated T8152 never asks. */
static inline int pcie_ans_power_off(void) { return -1; }
static inline int pcie_ans_power_on(void) { return -1; }
