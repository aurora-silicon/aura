/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KBOOT_DISPLAY_T8142_H
#define KBOOT_DISPLAY_T8142_H

int kboot_t8142_display_reserve(void *dt, const char *alias);
int kboot_t8142_display_enable(void *dt, const char *alias);
int kboot_t8142_display_prune(void *dt);

#endif
