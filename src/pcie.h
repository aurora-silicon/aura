/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef PCIE_H
#define PCIE_H

#include "types.h"

/* The J713 and the boards whose radio port matches it (J715, J604). */
bool pcie_radio_board_fdt(void *dt);
int pcie_prepare_fdt(void *dt);
int pcie_handoff_fdt(void *dt);
int pcie_init(void);
int pcie_shutdown(void);

/* Disable optional radio PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);

#endif
