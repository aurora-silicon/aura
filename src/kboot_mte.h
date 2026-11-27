/* SPDX-License-Identifier: MIT */

#ifndef __KBOOT_MTE_H__
#define __KBOOT_MTE_H__

#include "types.h"

/*
 * On a CPU with MTE2: reserve the MTE tag storage in dt and lower *ram_end to its base, or log
 * why not and turn MTE off (arm64.nomte, /chosen/aurora,mte). os_end is where the initrd and the
 * device tree end; they, aura and its heap must lie below the storage.
 */
int kboot_mte_reserve(void *dt, u64 os_end, u64 dram_base, u64 dram_size, u64 ram_start,
                      u64 *ram_end);

#endif
