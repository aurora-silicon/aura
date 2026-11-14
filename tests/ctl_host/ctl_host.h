/* SPDX-License-Identifier: MIT */
/* What src/sptm_virt_ctl.c needs from aura, for tests/ctl_host/ctl_host.c. */
#ifndef CTL_HOST_H
#define CTL_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

/* src/sptm_virt_s2r_soc.h, without aura's types.h (its typedefs clash with a host libc's). */
#define TYPES_H
#include "sptm_virt_s2r_soc.h"

extern u64 host_PAR_EL1, host_CNTFRQ_EL0;
#define mrs(r)    host_##r
#define msr(r, v) (host_##r = (v))
#define sysop(x)  ctl_host_sysop(x)
void ctl_host_sysop(const char *op);

#define AURA_XTEXT
#define BIT(n)           (1UL << (n))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define min(a, b)        ((u64)(a) < (u64)(b) ? (u64)(a) : (u64)(b))
#define max(a, b)        ((u64)(a) > (u64)(b) ? (u64)(a) : (u64)(b))
#define PAR_F            BIT(0)
#define PAR_PA           (((1UL << 52) - 1) & ~0xfffUL)
#define MAIR_IDX_NORMAL  0
#define PERM_RW          0
#define T8140            0x8140

typedef struct {
    int unused;
} spinlock_t;
#define SPINLOCK_INIT {0}
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);

/* aura's image: a few pages just past the fake guest RAM, the way a real carve-out sits. */
#define _base ((u8 *)0x10000f00000UL)
#define _end  ((u8 *)0x10000f40000UL)
extern void *adt;
extern u32 chip_id;
extern struct {
    u64 phys_base;
} cur_boot_args;

int adt_path_offset_trace(const void *dt, const char *path, int *offs);
int adt_get_reg(const void *dt, const int *path, const char *prop, int idx, u64 *pa, u64 *sz);
void hv_pt_split_block(u64 addr);
void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attr, u64 perms);
void mmu_rm_mapping(u64 from, size_t size);
void ctl_host_at(u64 va, bool write);
u32 ctl_host_read32(u64 pa);

extern char con_buf[];
extern size_t con_wp;
extern const char *const aura_version;
extern volatile bool sptm_virt_s2r_quiet;
extern volatile bool sptm_virt_xnu_live;

bool sptm_virt_ctl_hvc(u64 *regs, u64 esr);
void sptm_virt_ctl_s2_setup(void);
void sptm_virt_ctl_guest_ram(u64 *base, u64 *size);
void sptm_virt_s2_ctl_begin(void);
void sptm_virt_s2_ctl_end(bool changed);
struct s2r_rec;
extern struct s2r_rec *sptm_virt_s2r_ring;
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
