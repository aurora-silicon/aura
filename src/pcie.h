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
/* T6050 apcie0 port 2 (pcie_t6050.c): 0 up, -1 stopped (sticky for this boot) */
int pcie_t6050_init(void);
/* Disable an untrained optional port's root port and DART in the handed-over tree */
int pcie_t6050_handoff_fdt(void *dt);
/* The ADT's link speed for a root port's bridge node (pcie.c): 0, or -1 when it sets none */
int pcie_adt_link_speed(int bridge, u32 *speed);
int pcie_ans_power_off(void);
/* Back on after pcie_ans_power_off(), CPU stopped */
int pcie_ans_power_on(void);

/* Disable optional radio PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);
/* Post-M4: copy the APCIe tables from the ADT to the DT's PCIe host and its port. */
int pcie_apcie_tables_fdt(void *dt, int capacity);
/* Whether the OS was handed those tables, so that it brings the APCIe block back itself */
void pcie_set_os_restores_apcie(bool restores);
/* Whether the ADT can leave a system sleep's PCIe bring-up to aura (before M4) */
bool pcie_sleep_reinit_possible(void);
/* After a system sleep, before the OS runs again: whether the complex is aura's to set up */
bool pcie_needs_sleep_reinit(void);
int pcie_reinit_after_sleep(void);

#endif
