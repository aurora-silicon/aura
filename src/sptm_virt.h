/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_H
#define SPTM_VIRT_H

#include "../build/build_cfg.h"

#include "types.h"

/*
 * Virtualized SPTM: aura runs Apple's stock SPTM, TXM, the cL4 secure kernel and XNU as a
 * FEAT_NV2 guest at virtual EL2 on T8140, with their guarded world (GXF/SPRR) running natively
 * one level below aura's own GL2. Development builds only (SPTM_VIRT=1); the host side is
 * proxyclient/tools/run_guest_sptm.py. See docs/sptm-nv2-virt.md.
 */

/*
 * The physical split between SPTM and aura. SPTM owns the managed-RAM window iBoot would hand it;
 * aura's heap and per-CPU pages sit in a carve-out at the top of that window, below the fixed
 * high-firmware regions (SAPT, the GPU handoff), and SPTM is told its window ends below it.
 */
struct sptm_virt_layout {
    u64 ram_base;     /* DRAM base (physBase rounded down to 4 GiB) */
    u64 ram_top;      /* ram_base + mem_size_actual */
    u64 managed_base; /* start of SPTM's managed RAM */
    u64 managed_end;  /* end of SPTM's managed RAM (== aura_base) */
    u64 aura_base;    /* aura's carve-out */
    u64 aura_top;
    u64 fw_high;   /* lowest fixed high-firmware PA, 0 if none */
    u64 aura_size; /* bytes reserved for aura */
};

/* Pure carve-out math (unit-tested on the host). 0 on success, negative if the values cannot
 * satisfy the constraints. */
int sptm_virt_compute_layout(u64 phys_base, u64 mem_size, u64 ram_base, u64 ram_top, u64 fw_high,
                             u64 aura_size, struct sptm_virt_layout *out);

/* Where the host may place the images: SPTM's managed window, shortened for aura. */
struct sptm_virt_placement {
    u64 managed_base, managed_end;
};

#ifdef SPTM_VIRT

bool sptm_virt_requested(void);
bool sptm_virt_heap_window(u64 *base, u64 *limit);

/* Host entry points, called over the proxy. */
int sptm_virt_place(struct sptm_virt_placement *out);
int sptm_virt_load(u64 sptm_entry, u64 handoff);
void sptm_virt_flush(u64 pa, u64 size);
void sptm_virt_enter(void) __attribute__((noreturn));

/* nv2.c: the last step of every guest entry on a core. */
void sptm_virt_cpu_arm(void);
void sptm_virt_amx_ctl_el2_write(u64 v);
u64 sptm_virt_amx_ctl_el2_read(void);

/* exception.c */
bool sptm_virt_peek_fault(bool in_gl);
bool sptm_virt_gl_sync(u64 *regs, u64 esr, u64 elr, u64 spsr);
bool sptm_virt_el1_wfx(u64 *regs, u64 esr);

#else

static inline bool sptm_virt_heap_window(u64 *base, u64 *limit)
{
    (void)base;
    (void)limit;
    return false;
}
static inline bool sptm_virt_peek_fault(bool in_gl)
{
    (void)in_gl;
    return false;
}

#endif

#endif
