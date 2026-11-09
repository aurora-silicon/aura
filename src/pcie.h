/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef PCIE_H
#define PCIE_H

int pcie_prepare_fdt(void *dt);
int pcie_handoff_fdt(void *dt);
int pcie_init(void);
int pcie_shutdown(void);
/* T6050 apcie0 port 2 (pcie_t6050.c): 0 up, -1 stopped (sticky for this boot) */
int pcie_t6050_init(void);
/* Disable an untrained optional port's root port and DART in the handed-over tree */
int pcie_t6050_handoff_fdt(void *dt);
int pcie_ans_power_off(void);
/* Back on after pcie_ans_power_off(), CPU stopped */
int pcie_ans_power_on(void);

/* Disable optional J713 PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);

#endif
