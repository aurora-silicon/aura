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
int pcie_ans_power_off(void);
/* Back on after pcie_ans_power_off(), CPU stopped */
int pcie_ans_power_on(void);

/*
 * Endpoint reset timing every port bring-up shares.
 *
 * PCIE_CEM_T_PVPERL_MS: PCIe CEM r5.0 2.2, T_PVPERL: the endpoint's power stable at least 100 ms
 * before PERST# is deasserted. Taken after an SMC function-reset releases the endpoint.
 *
 * PCIE_RESET_TO_CONFIG_MS: PCIe Base r6.0 6.6.1: with a port faster than 5 GT/s, at least 100 ms
 * after link training completes before the first Configuration Request. The ADT bridge states it
 * too, as perst-to-config (ms); pcie_adt_perst_to_config_ms() takes the longer of the two.
 *
 * PCIE_RESET_READY_MS: PCIe Base r6.0 6.6.1: a function completes Configuration Requests
 * successfully within 1.0 s of a Conventional Reset. Until then it answers with Request Retry
 * Status (vendor ID 0x0001 when software visibility is on) or not at all (all ones).
 */
#define PCIE_CEM_T_PVPERL_MS    100
#define PCIE_RESET_TO_CONFIG_MS 100
#define PCIE_RESET_READY_MS     1000

/* The ADT bridge's perst-to-config in ms, at least PCIE_RESET_TO_CONFIG_MS */
u32 pcie_adt_perst_to_config_ms(const char *bridge);
/* Wait up to PCIE_RESET_READY_MS for the function at `cfg` to answer config space; *id gets its
 * vendor/device word. -1, and a log line, if it never does. */
int pcie_wait_config_ready(u64 cfg, u32 *id);

/* The MMIO pad register of the ADT bridge's function-perst GPIO on /arm-io/gpio0 */
int pcie_adt_perst_gpio(const char *bridge, u64 *addr);
/*
 * Refclk running to PERST# deasserted: the ADT bridge's t-refclk-to-perst (us; 100 on every apcie
 * bridge, the CEM T_PERST-CLK minimum), but never less than 1 ms, the time every Aura bring-up has
 * used since its first trace. Nothing shorter has been tried.
 */
#define APCIE_REFCLK_TO_PERST_MIN_US 1000

/* Disable optional radio PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);
/* Post-M4: copy the APCIe tables from the ADT to the DT's PCIe host and its port. */
int pcie_apcie_tables_fdt(void *dt, int capacity);

#endif
