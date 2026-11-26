/* SPDX-License-Identifier: MIT */
/*
 * Host shim for src/sptm_virt_mmio.c (tests/run-mmio-trace.sh): aura's headers reduced to what
 * the tracer uses, on aarch64 Linux user space under qemu-aarch64. "Physical" addresses are this
 * process's own: the fake device registers are an anonymous mapping at a fixed low address.
 */
#ifndef MMT_SHIM_H
#define MMT_SHIM_H

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

#define BIT(n)           (1UL << (n))
#define GENMASK(h, l)    (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define min(a, b)        ((a) < (b) ? (a) : (b))
#define AURA_XTEXT       __attribute__((noinline))
#define PTE_VALID        BIT(0)
#define SPTM_VIRT_MMIO   BIT(13)
#define mrs(reg)         (++shim_clock)

typedef int spinlock_t;
#define DECLARE_SPINLOCK(n) spinlock_t n = 0
static inline void spin_lock(spinlock_t *l)
{
    if ((*l)++) {
        fprintf(stderr, "lock taken twice\n");
        exit(2);
    }
}
static inline void spin_unlock(spinlock_t *l)
{
    (*l)--;
}

struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
};

extern u64 shim_clock;
extern int shim_cpu;
extern u64 sptm_virt_flags;
extern u64 sptm_virt_s2r_nrec;
extern volatile u64 sptm_virt_s2r_head;
extern void *adt;
static inline int smp_id(void)
{
    return shim_cpu;
}

typedef bool hv_hook_t(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
u64 hv_pt_walk(u64 addr);
bool sptm_virt_rg_owns(u64 page);
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width);
bool sptm_virt_s2r_ring_ensure(void);
volatile u64 *sptm_virt_s2r_rec_ext(u64 t, u64 meta_hi, u64 pa, int width, bool write, bool marker,
                                    u64 val, u64 pc);
void sptm_virt_mmio_setup(void);

int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *pa, u64 *size);
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
