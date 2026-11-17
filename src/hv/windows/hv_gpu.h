/* SPDX-License-Identifier: MIT */

#ifndef HV_GPU_H
#define HV_GPU_H

#include "types.h"

/*
 * G0 (windows-m6/WINDOWS-GPU-PLAN.md, windows-m6/G0-CARRIER-CONTRACT.md): the carrier side of
 * GPU passthrough for the J804 Windows guest, logging only.  Off unless the launcher asks
 * (--windows-gpu admit|log); with it off nothing here runs and the guest sees no change.
 *
 *   ADMIT  once per carrier session, after guest RAM is set and before guest entry: the
 *          admission port (gpu_t8152_admit.c) against the live ADT and both TEXT identities;
 *          the G0 range table; the firmware __TEXT segments onto the stage-2 denylist; the four
 *          UAT regions (RW) and four DATA regions (RO) registered as identity exceptions (not
 *          mapped); the GPU pool recorded (inside guest RAM, which stays normal identity RAM);
 *          the read-only handoff page built in carrier memory and mapped at IPA = PA.
 *   HOOK   logging hooks (read-as-zero, write-ignored, counted per class, the first accesses
 *          printed) over the whole sgx register window and the post-office page.  No access
 *          reaches the hardware.
 *
 * Not in this file yet (each needs its own owner-present session): PSD power-on (3, 82, 81),
 * the CPU_CONTROL guard (RUN is already set after power-on; the carrier refuses writes that
 * clear or toggle it), iop-vbar pinning by value, mailbox forwarding, measured sgx passthrough,
 * the GPU IRQ aliases (hv_aic_alias_t8152_gpu), and mapping the firmware classes.
 */

/* P_HV_WINDOWS_CAPS2 bit 2: G0 GPU admission and logging hooks (this file, P_HV_WINDOWS_GPU). */
#define HV_WINDOWS_CAPS2_GPU BIT(2)

#define HV_GPU_ERROR ((u64) - 1)
/* Pool: 32 MiB aligned (one level-2 block with the 16 KiB granule), at most half guest RAM. */
#define HV_GPU_POOL_ALIGN (32UL << 20)
#define HV_GPU_LOG_LIMIT  32

/* P_HV_WINDOWS_GPU args[0]. */
enum hv_gpu_op {
    HV_GPU_OP_ADMIT = 1,  /* args[1] = pool base, args[2] = pool size */
    HV_GPU_OP_HOOK = 2,   /* logging hooks on the sgx window and the post-office page */
    HV_GPU_OP_STATUS = 3, /* args[1] = hv_gpu_status index */
};

enum hv_gpu_status {
    HV_GPU_S_STATE = 0,         /* handoff state bits (GPU_HS_*) */
    HV_GPU_S_HANDOFF_IPA = 1,   /* 0 before ADMIT succeeded */
    HV_GPU_S_HANDOFF_SIZE = 2,
    HV_GPU_S_RANGES = 3,        /* entries in the range table */
    HV_GPU_S_ACCESSES = 4,      /* guest accesses the hooks took */
    HV_GPU_S_MAILBOX_GFX = 5,   /* of those, inside a GFX post-office window */
    HV_GPU_S_CLASS = 0x100,     /* + enum gpu_class: accesses per class */
    HV_GPU_S_RANGE = 0x200,     /* + 4 * i + {0 base, 1 size, 2 class | tag << 16, 3 flags} */
};

/* ADMIT / HOOK results (negative; 0 is success). */
enum hv_gpu_err {
    HV_GPU_E_STATE = -1,   /* not T8152, policy not armed, no guest RAM, guest entered, ADMIT
                              already ran this session (passed or refused), HOOK before ADMIT
                              or twice */
    HV_GPU_E_POOL = -2,    /* the pool is not 32 MiB aligned inside guest RAM (nothing spent) */
    HV_GPU_E_ADMIT = -3,   /* the admission refused (the console says why) */
    HV_GPU_E_RANGES = -4,  /* the range table does not fit or breaks a rule */
    HV_GPU_E_POLICY = -5,  /* the stage-2 policy refused a class, or a map/hook failed */
    HV_GPU_E_HANDOFF = -6, /* no memory for the handoff page, or it does not fit */
};

/* P_HV_WINDOWS_GPU. */
u64 hv_gpu_proxy(u64 op, u64 a1, u64 a2);

/*
 * The EL2 fatal report's GPU part (guest SError attribution): the session state, the handoff
 * page and pool, how many admitted firmware ranges there are (registered identity exceptions,
 * never mapped in G0), and the logging hooks' hit count with the last access.  Recorded values
 * only, no locks, no MMIO.
 */
void hv_gpu_fatal_report(void);

#ifdef HV_GPU_HOST_TEST
void hv_gpu_host_new_session(void);
bool hv_gpu_host_read_text(u64 phys, void *buf, u64 size);
bool hv_gpu_host_hook(u64 addr, u64 *val, bool write, int width);
const void *hv_gpu_host_handoff(void);
#endif

#endif
