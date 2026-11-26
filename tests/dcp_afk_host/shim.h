/* SPDX-License-Identifier: MIT */
/*
 * Host shim for src/sptm_virt_dcp_afk.c (tests/run-dcp-afk-trace.sh): aura's headers reduced to
 * what the tracer uses, on aarch64 Linux user space under qemu-aarch64. "Physical" addresses are
 * this process's own addresses: the fake guest RAM is a memfd mapped low, so DART PTEs (42-bit
 * PAs) can name it, and mmu_add_mapping() aliases its pages at the tracer's window VA.
 */
#ifndef DAFK_SHIM_H
#define DAFK_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef printf
#define printf(...) fprintf(stderr, __VA_ARGS__)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n)             (1UL << (n))
#define GENMASK(h, l)      (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define ALIGN_UP(x, a)     (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a)   ((x) & ~((u64)(a) - 1))
#define min(a, b)          ((a) < (b) ? (a) : (b))
#define AURA_XTEXT         __attribute__((noinline))
#define PTE_VALID          BIT(0)
#define MAIR_IDX_NORMAL_NC 1
#define PERM_RW            0
#define SPTM_VIRT_DCP_AFK  BIT(12)
#define sysop(op)          __asm__ volatile("" ::: "memory")
#define mrs(reg)           (++shim_clock)

typedef int spinlock_t;
#define DECLARE_SPINLOCK(n) spinlock_t n = 0
static inline void spin_lock(spinlock_t *l)
{
    (void)l;
}
static inline void spin_unlock(spinlock_t *l)
{
    (void)l;
}

struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
};

static inline u32 read32(u64 addr)
{
    return *(volatile u32 *)addr;
}

struct boot_args {
    u64 phys_base;
};
extern struct boot_args cur_boot_args;
extern u64 mem_size_actual;
extern u64 shim_clock;
extern u64 sptm_virt_flags;
extern u64 sptm_virt_s2r_nrec;
extern void *adt;

typedef bool hv_hook_t(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
u64 hv_pt_walk(u64 addr);
int mmu_add_mapping(u64 from, u64 to, size_t size, u8 attribute_index, u64 perms);
int mmu_rm_mapping(u64 from, size_t size);
bool sptm_virt_rg_owns(u64 page);
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width);
void sptm_virt_ctl_guest_ram(u64 *base, u64 *size);
bool sptm_virt_s2r_ring_ensure(void);
void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool write);
void sptm_virt_dcp_afk_setup(void);

int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *pa, u64 *size);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
const char *adt_get_name(const void *adt, int nodeoffset);
int adt_get_child_count(const void *adt, int nodeoffset);
int adt_first_child_offset(const void *adt, int nodeoffset);
int adt_next_sibling_offset(const void *adt, int nodeoffset);
#define ADT_FOREACH_CHILD(adt, node)                                                               \
    for (int _child_count = adt_get_child_count(adt, node); _child_count; _child_count = 0)        \
        for (node = adt_first_child_offset(adt, node); _child_count--;                             \
             node = adt_next_sibling_offset(adt, node))

#endif
