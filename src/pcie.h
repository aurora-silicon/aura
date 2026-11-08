/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef PCIE_H
#define PCIE_H

int pcie_prepare_fdt(void *dt);
int pcie_handoff_fdt(void *dt);
int pcie_init(void);
int pcie_shutdown(void);

/* Disable optional J713 PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);
/* Post-M4: copy the APCIe tables from the ADT to the DT's PCIe host and its port. */
int pcie_apcie_tables_fdt(void *dt);

#endif
