/* SPDX-License-Identifier: MIT */

#ifndef PSCI_H
#define PSCI_H

#include "types.h"

/*
 * Service an SMC trapped from an EL1 OS (HCR_EL2.TSC) as a PSCI/SMCCC call.
 * regs[0..3] are x0..x3 on entry and the SMCCC result on return. Always
 * returns true: an unknown function yields PSCI NOT_SUPPORTED, never a reboot.
 * The caller advances ELR past the SMC. Some calls (CPU_OFF, SYSTEM_RESET) do
 * not return.
 */
bool psci_handle_smc(u64 *regs);

#endif
