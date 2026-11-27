/* SPDX-License-Identifier: MIT */

#ifndef __KBOOT_PMP_H__
#define __KBOOT_PMP_H__

/*
 * A DT without the pmp alias (T6050, whose PMP is pmp0): pair each enabled apple,t6000-pmp-v2
 * node of @dt with its ADT PMP by register window and call @fill(node, nub) on it to copy this
 * Mac's values, all or nothing (@dt is restored from a @dt_bufsize copy when @fill fails). Never
 * fatal: returns 0.
 */
int kboot_pmp_set_by_window(void *dt, int dt_bufsize, int (*fill)(int pmp_node, int nub_anode));

#endif
