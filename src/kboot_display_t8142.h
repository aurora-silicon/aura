/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KBOOT_DISPLAY_T8142_H
#define KBOOT_DISPLAY_T8142_H

const char *kboot_t8142_disp0_piodma_alias(void *dt);
int kboot_t8142_dcpext_piodma(void *dt, const char *alias);
int kboot_t8142_display_reserve(void *dt);
int kboot_t8142_display_enable(void *dt);
int kboot_t8142_dcpext_preserve(void *dt, const char *alias, const char *piodma);
int kboot_t8142_display_prune(void *dt);

#endif
