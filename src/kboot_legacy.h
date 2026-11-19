/* SPDX-License-Identifier: MIT */

#ifndef KBOOT_LEGACY_H
#define KBOOT_LEGACY_H

#include "types.h"

/*
 * LEGACY-ASAHI(2027-02-28): adds the pre-rename "asahi," name next to each /chosen
 * property and node a released kernel, U-Boot or userspace reads. Returns the number of
 * aliases added, or -1 if the tree ran out of space (the "aurora," names are untouched).
 */
int aurora_legacy_chosen_aliases(void *fdt);

/* True for an "aurora," /chosen property name that had an "asahi," name before */
bool aurora_legacy_wants_prop(const char *name);

/* "aurora,X" -> "asahi,X" ("aurora,aura-Y" -> "asahi,m1n1-Y"); 0, or -1 if it does not fit */
int aurora_legacy_name(const char *name, char *out, size_t size);

#endif
