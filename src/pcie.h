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
/*
 * T6050 apcie0 GP ports (pcie_t6050.c), sticky for this boot: 0 up; -1 stopped; on a board without
 * a required port a stop is PCIE_T6050_LEFT (before anything was written: left for the OS) or
 * PCIE_T6050_PARTIAL (after: the down ports go disabled), and the boot goes on.
 */
#define PCIE_T6050_LEFT    1
#define PCIE_T6050_PARTIAL 2
int pcie_t6050_init(void);
/* Describe the ports in the handed-over tree as pcie_t6050_init() left them */
int pcie_t6050_handoff_fdt(void *dt);
int pcie_ans_power_off(void);
/* Back on after pcie_ans_power_off(), CPU stopped */
int pcie_ans_power_on(void);

/* Disable optional radio PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);
/* Post-M4: copy the APCIe tables from the ADT to the DT's PCIe host and its port. */
int pcie_apcie_tables_fdt(void *dt, int capacity);

#endif
