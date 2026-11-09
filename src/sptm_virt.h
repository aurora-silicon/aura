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

/* Host configuration (sptm_virt_config), set over the proxy before sptm_virt_enter. */
#define SPTM_VIRT_S2R          BIT(0) /* sleep-path hooks and the trace ring */
#define SPTM_VIRT_S2R_GO       BIT(1) /* let the sleep proceed instead of stopping before it */
#define SPTM_VIRT_S2R_FULL     BIT(2) /* also hook the high-volume blocks (go mode may reset) */
#define SPTM_VIRT_S2R_COPROC   BIT(3) /* also hook the RTKit and exclave mailboxes */
#define SPTM_VIRT_S2R_DROP_USB BIT(4) /* tear down aura's CDC gadget at the sleep commit */
#define SPTM_VIRT_XNU_BT       BIT(5) /* sample XNU backtraces at GEXIT */
#define SPTM_VIRT_GPU          BIT(6) /* hook and record the GPU and PMP registers */
#define SPTM_VIRT_SAMPLE       BIT(7) /* sample the guest's PC until its XNU entry */
#define SPTM_VIRT_S2R_MEM      BIT(8) /* T6050: also hook the MCC and the coprocessor DARTs */
/* sptm_virt_s2r_cp_ansx, with COPROC on T6050 */
#define S2R_CP_ANSX_SART BIT(0) /* sart-ans reg[0-2], first page each */
#define S2R_CP_ANSX_NVME BIT(1) /* the NVMe BAR's first page (reg[9]) */
#define S2R_CP_ANSX_AUS5 BIT(2) /* apcie0 reg[10-13], the AUS5 TOP/AON windows */

#ifdef SPTM_VIRT

/* Set once the boot CPU has entered the guest; read with the MMU off by the reset path. */
extern volatile bool sptm_virt_guest_live;

bool sptm_virt_requested(void);
bool sptm_virt_mte(void);
bool sptm_virt_heap_window(u64 *base, u64 *limit);

/* Host entry points, called over the proxy. */
int sptm_virt_place(struct sptm_virt_placement *out);
int sptm_virt_load(u64 sptm_entry, u64 handoff);
void sptm_virt_flush(u64 pa, u64 size);
void sptm_virt_config(u64 flags);
void sptm_virt_enter(void) __attribute__((noreturn));

/* A core coming out of reset while the guest is live (startup.c). */
void sptm_virt_cpu_reset(void) __attribute__((noreturn));

/* nv2.c: the last step of every guest entry on a core. */
void sptm_virt_cpu_arm(void);
/* nv2.c: Apple per-level state around the switch to and from an EL1 child. */
void sptm_virt_world_child_pre_enter(void);
void sptm_virt_world_child_post_enter(void);
void sptm_virt_world_child_pre_exit(void);
void sptm_virt_world_child_post_exit(void);
u64 *sptm_virt_vel1_slot(unsigned op1, unsigned crn, unsigned crm, unsigned op2);
void sptm_virt_amx_ctl_el2_write(u64 v);
u64 sptm_virt_amx_ctl_el2_read(void);
bool sptm_virt_s2_dabort(u64 *regs);

/* exception.c */
bool sptm_virt_sample_fiq(u64 *regs);
bool sptm_virt_peek_fault(bool in_gl);
bool sptm_virt_gl_sync(u64 *regs, u64 esr, u64 elr, u64 spsr);
bool sptm_virt_el1_wfx(u64 *regs, u64 esr);
bool sptm_virt_cl4_fiq(u64 *regs);

/* The sleep path (sptm_virt_s2r.c) */
void sptm_virt_s2r_wake_trap(u64 esr, u64 elr, bool in_gl);
void sptm_virt_s2r_uart_tick(void);
bool sptm_virt_s2r_uart_byte(u8 b);
/* usb.c: the link's bring-up after a wake runs on a coroutine whose waits give the core back. */
bool sptm_virt_s2r_usb_yieldable(void);
bool sptm_virt_s2r_usb_yield(u32 us);

#else

#define sptm_virt_guest_live false

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
static inline bool sptm_virt_sample_fiq(u64 *regs)
{
    (void)regs;
    return false;
}
static inline void sptm_virt_s2r_uart_tick(void)
{
}
static inline bool sptm_virt_s2r_uart_byte(u8 b)
{
    (void)b;
    return false;
}
static inline bool sptm_virt_s2r_usb_yieldable(void)
{
    return false;
}
static inline bool sptm_virt_s2r_usb_yield(u32 us)
{
    (void)us;
    return false;
}

#endif

#endif
