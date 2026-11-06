/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef J813_N1_H
#define J813_N1_H

#include "types.h"

/* Train the J813 N1 PCIe link before Linux; disables the ECAM host on failure. */
int j813_n1_prepare(void *dt);

/* Explicit Windows handoff only; caller validates the live ADT/resources. */
int j813_n1_validate_windows(void);
int j813_n1_prepare_fabric_windows(void);
/* port_ready permits full drain only after known port/root setup completed. */
int j813_n1_finish_windows(bool *port_ready);

#endif
