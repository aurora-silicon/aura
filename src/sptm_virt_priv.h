/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_PRIV_H
#define SPTM_VIRT_PRIV_H

/* Shared between the sptm_virt*.c files only. */

#include "smp.h"
#include "sptm_virt.h"
#include "types.h"
#include "utils.h"

struct exc_info;

/* sptm_virt.c */
extern volatile bool sptm_virt_xnu_live;
extern volatile u64 sptm_virt_bss_canary;
extern u64 sptm_virt_flags;
extern u32 sptm_virt_cpu_entries[MAX_CPUS];
int sptm_virt_layout(struct sptm_virt_layout *out);
void sptm_virt_cpu_enters_xnu(int cpu);
extern u32 sptm_virt_wfi_watch;

/* sptm_virt_gl.c */
extern volatile bool sptm_virt_peek_active_cpu[MAX_CPUS];
extern volatile bool sptm_virt_peek_faulted_cpu[MAX_CPUS];
bool sptm_virt_peek64(u64 phys, u64 *out);
bool sptm_virt_el1_va_read64(u64 va, u64 *out);
int sptm_virt_gl_alloc(void);
u64 *sptm_virt_gl2_slot(int cpu, u16 key);
void sptm_virt_amx_host(void);
void sptm_virt_amx_child(void);

/* A system-register read that may UNDEF here: the fault is skipped (sptm_virt_peek_fault) and
 * the value reads as SAFE_MRS_FAILED. */
#define SAFE_MRS_FAILED 0xbadUL
#define SAFE_MRS(enc)                                                                              \
    ({                                                                                             \
        int _c = smp_id();                                                                         \
        u64 _v = SAFE_MRS_FAILED;                                                                  \
        sptm_virt_peek_active_cpu[_c] = true;                                                      \
        sysop("isb");                                                                              \
        __asm__ volatile("mrs %0, " enc : "+r"(_v));                                               \
        sysop("isb");                                                                              \
        sptm_virt_peek_active_cpu[_c] = false;                                                     \
        _v;                                                                                        \
    })
/* The same for a write: true if it faulted. */
#define SAFE_MSR(enc, val)                                                                         \
    ({                                                                                             \
        int _c = smp_id();                                                                         \
        u64 _w = (val);                                                                            \
        sptm_virt_peek_faulted_cpu[_c] = false;                                                    \
        sptm_virt_peek_active_cpu[_c] = true;                                                      \
        sysop("isb");                                                                              \
        __asm__ volatile("msr " enc ", %0" ::"r"(_w));                                             \
        sysop("isb");                                                                              \
        sptm_virt_peek_active_cpu[_c] = false;                                                     \
        sptm_virt_peek_faulted_cpu[_c];                                                            \
    })

/* sptm_virt_cl4.c */
bool sptm_virt_in_cl4(void);
enum {
    SPTM_WEV_GERET,
    SPTM_WEV_GEXIT,
    SPTM_WEV_CL4_ENTER,
    SPTM_WEV_CL4_SYNC,
    SPTM_WEV_CL4_FIQ,
    SPTM_WEV_CL4_ERET2,
    SPTM_WEV_CHILD_ENTER,
    SPTM_WEV_CHILD_EXIT,
};
void sptm_virt_world_ev(unsigned kind, u64 a, u64 b);
void sptm_virt_world_dump(void);
void sptm_virt_world_ring(int cpu, unsigned last, int (*pf)(const char *, ...));
void sptm_virt_world_arm(int cpu);
void sptm_virt_cl4_seed(int cpu);
void sptm_virt_cl4_world_enter(u64 target);
void sptm_virt_cl4_world_exit(void);
bool sptm_virt_cl4_reflect(u64 *regs, u64 esr);

/* sptm_virt_s2.c */
extern volatile bool sptm_virt_rg_passthrough;
void sptm_virt_s2_setup(void);
void sptm_virt_s2_enter_xnu(int cpu);
void sptm_virt_ctrr_publish_rorgn(void);
void sptm_virt_gpu_deadline(struct exc_info *ctx);
bool sptm_virt_gpu_deadline_due(void);
void sptm_virt_gpu_park_now(struct exc_info *ctx, const char *why);
void sptm_virt_report_guest(u64 *regs, u64 pc);
void sptm_virt_canary_tick(void);
void sptm_virt_fill_ctx(struct exc_info *ctx, u64 *regs, bool gl);
void sptm_virt_pac_note(const char *where);
bool sptm_virt_cpu_in_xnu(void);
bool sptm_virt_s2_dabort_gl(u64 *regs);
bool sptm_virt_rg_owns(u64 page);
void sptm_virt_rg_stop_wd2(void);
void sptm_virt_rg_disarm(void);
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width);

/* sptm_virt_s2r.c */
extern volatile bool sptm_virt_s2r_quiet;
void sptm_virt_s2r_setup(bool cdc);
void sptm_virt_s2r_wake(int cpu);
void sptm_virt_s2r_xnu_entry(int cpu, u64 target);
void sptm_virt_s2r_note(struct exc_info *ctx, u64 addr, u64 val, bool write, int width);
void sptm_virt_s2r_dump_tail(u64 n);
void sptm_virt_s2r_wd_stopped(void);
bool sptm_virt_s2r_wfi(u64 *regs, u64 esr);

/* sptm_virt_gpu.c */
void sptm_virt_gpu_setup(void);

#ifdef AURACTL
/* auractl (sptm_virt_ctl.c, src/auractl_abi.h) */
struct s2r_rec;
struct s2r_soc;
void sptm_virt_ctl_s2_setup(void); /* before the stage-2 checksum: catalogue, pre-split blocks */
void sptm_virt_ctl_guest_ram(u64 *base, u64 *size); /* sptm_virt.c */
void sptm_virt_s2_ctl_begin(void);                  /* sptm_virt_s2.c */
void sptm_virt_s2_ctl_end(bool changed);            /* changed: the new tree is the reference */
extern struct s2r_rec *sptm_virt_s2r_ring;          /* sptm_virt_s2r.c */
extern u64 sptm_virt_s2r_nrec;
extern volatile u64 sptm_virt_s2r_head;
extern volatile bool sptm_virt_s2r_ctl_paused;
const struct s2r_soc *sptm_virt_s2r_ctl_soc(void);
int sptm_virt_s2r_ctl_page(const char *what, u64 page, bool arm);
u32 sptm_virt_s2r_ctl_release_all(void);
u32 sptm_virt_s2r_ctl_count(u64 base, u64 size, u32 *ctl_pages);
const void *sptm_virt_s2r_ctl_rec(u64 seq);
void sptm_virt_s2r_marker(const char *tag, u64 arg);
#endif

#endif
