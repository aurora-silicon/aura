/* SPDX-License-Identifier: MIT */

#ifndef __SMP_ASM_H__
#define __SMP_ASM_H__

#define MAX_CPUS     32
#define MAX_EL3_CPUS 4

#define SECONDARY_STACK_SIZE 0x10000

/*
 * Stage 1 is the boot object iBoot places, and its raw image runs to the end of .bss, so it takes
 * the secondary stacks from the heap (smp.c). Every other build keeps them in .bss, where the
 * stage that boots the kernel can hand them to PSCI. Include build_cfg.h before this header.
 */
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
#define SMP_HEAP_STACKS 1
#endif

#endif
