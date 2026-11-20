/* SPDX-License-Identifier: MIT */

#ifndef __SMP_ASM_H__
#define __SMP_ASM_H__

/* Per-CPU tables are indexed by ADT cpu-id, which runs to 34 on T6050 Ultra (30 CPUs). */
#define MAX_CPUS     64
#define MAX_EL3_CPUS 4

/* One per CPU present, not per cpu-id (smp.c) */
#define SECONDARY_STACKS     32
#define SECONDARY_STACK_SIZE 0x10000

/*
 * Stage 1 is the boot object iBoot places, and its raw image runs to the end of .bss, so it takes
 * the secondary stacks from the heap (smp.c). Every other build keeps them in .bss. Include
 * build_cfg.h before this header.
 */
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
#define SMP_HEAP_STACKS 1
#endif

#endif
