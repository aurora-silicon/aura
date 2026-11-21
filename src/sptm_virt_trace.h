/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_TRACE_H
#define SPTM_VIRT_TRACE_H

/*
 * Record-only traces of the guest's MMIO accesses to host-named blocks (AURA_TRACE_BLKS) and of
 * the IRQs it takes through the AIC (AURA_IRQ_TRACE), each into a ring in aura's carve-out
 * (src/sptm_virt_trace.c). The host reads and drains the rings at a park
 * (proxyclient/aura/hvtrace.py). The layout below is the host's too.
 */

#ifndef HVTRACE_HOST
#include "types.h"
#endif

#define HVTRACE_MAGIC   0x3143525441525541UL /* "AURATRC1" */
#define HVTRACE_VERSION 1
#define HVTRACE_BLKS    8  /* named blocks */
#define HVTRACE_RNGS    16 /* page ranges, each in one block */
#define HVTRACE_PAGES   32 /* 16 KiB pages hooked in all */

#define HVTRACE_KIND_BLK 1
#define HVTRACE_KIND_IRQ 2
/* An IRQ ring record's block: a read of the event register that took an IRQ, or any other access
 * to the event register's page. */
#define HVTRACE_IRQ_EVENT 0
#define HVTRACE_IRQ_OTHER 1

struct hvtrace_rec {
    u64 t;    /* CNTPCT_EL0 once the access was performed; 0: not (yet) written */
    u64 val;  /* the value written or read; a 128-bit access is two records, low word first */
    u64 pc;   /* the guest PC */
    u64 meta; /* pa [47:0] | width [50:48] | write [51] | cpu [56:52] | blk [60:57] |
               * gl [61] (a GL1 trap: SPTM's) | hi [62] (a 128-bit access's second word) */
};

struct hvtrace_rng {
    u64 pa;
    u32 len; /* bytes, whole 16 KiB pages */
    u32 blk;
};

/* What the host sets before entry (run_guest_sptm.py). */
struct hvtrace_cfg {
    u32 n_rng, n_blk;
    u32 blk_max, irq_max; /* each ring's records; 0: the default */
    u32 irq;              /* AURA_IRQ_TRACE */
    u32 print_max;        /* print the first n accesses of each block on the console */
    struct hvtrace_rng rng[HVTRACE_RNGS];
    char name[HVTRACE_BLKS][16];
};

/* A ring: this header, then cap records; record i is in slot i % cap. */
struct hvtrace_hdr {
    u64 magic;
    u32 version, hdr_bytes;
    u32 rec_bytes, kind; /* HVTRACE_KIND_* */
    u64 cap;
    u64 head;   /* records claimed: [tail, head) are in the buffer */
    u64 tail;   /* records the host has read and drained */
    u64 lost;   /* accesses not recorded: cap records were undrained */
    u64 drains; /* the host's drains */
    u64 cntfrq;
    u64 t_setup;   /* CNTPCT at setup */
    u64 irq_event; /* the IRQ ring: the AIC event register's PA */
    u64 irq_zero;  /* the IRQ ring: its reads that returned 0 (no IRQ), only counted */
    u32 irq_max;   /* the IRQ ring: the AIC's IRQs per die (a flat number: die * this + num) */
    u32 n_blk, n_rng, print_max;
    u32 full; /* the ring filled since the last drain (said once on the console) */
    u32 pad;
    u64 blk_seen[HVTRACE_BLKS]; /* accesses per block, recorded or lost */
    u64 blk_lost[HVTRACE_BLKS];
    u64 blk_base[HVTRACE_BLKS]; /* the block's lowest page: offsets are from it */
    struct hvtrace_rng rng[HVTRACE_RNGS];
    char name[HVTRACE_BLKS][16];
    u64 reserved[41];
};

/* Set by the host before entry. */
extern struct hvtrace_cfg sptm_virt_trace_cfg;
/* For the host, at a park. */
extern struct hvtrace_hdr *sptm_virt_trace_blk, *sptm_virt_trace_irq;

/* The host's block list: NULL if it can be hooked as it is, else why not. */
const char *sptm_virt_trace_check(const struct hvtrace_cfg *c);
/* A ring of cap records in bytes (at least the header and two records), empty. */
void sptm_virt_trace_ring_init(struct hvtrace_hdr *h, u64 bytes, u32 kind, u64 cntfrq, u64 t);
/* An access performed: one record (a 128-bit access two), or counted lost if the ring is full. */
void sptm_virt_trace_note(struct hvtrace_hdr *h, u64 addr, const u64 *val, bool write, int width,
                          u32 blk, u64 pc, bool gl, u32 cpu, u64 t);
/* From the host at a park: [tail, upto) has been read; free it. 0, or < 0 refused. */
int sptm_virt_trace_drain(struct hvtrace_hdr *h, u64 upto);
/* The hooks (struct exc_info's elr is the guest PC). */
struct exc_info;
bool sptm_virt_trace_blk_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
bool sptm_virt_trace_irq_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
/* sptm_virt_s2_setup, after every other hook and before the tree is summed. */
void sptm_virt_trace_setup(void);
#ifndef HVTRACE_HOST
/* sptm_virt_trace_adt.c: the debug link's devices, from its PHYs' phandles (false: no port found,
 * and every USB-C port's devices are refused); then the refused device a range overlaps, or NULL.
 */
bool sptm_virt_trace_adt_link(const u32 *phys, int n_phys);
const char *sptm_virt_trace_adt_refused(u64 pa, u64 len);
#endif

#endif
