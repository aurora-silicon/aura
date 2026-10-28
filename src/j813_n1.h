/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef J813_N1_H
#define J813_N1_H

/* Train the J813 N1 PCIe link before Linux; disables the ECAM host on failure. */
int j813_n1_prepare(void *dt);

#endif
