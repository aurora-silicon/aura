/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_SNAP_H
#define SPTM_VIRT_SNAP_H

/*
 * GPU firmware snapshots (src/sptm_virt_snap.c): at a GPU doorbell, before the write is performed,
 * the firmware's page tables and the objects the doorbell names are copied, through the firmware's
 * own translation, into a buffer in aura's carve-out the host reads at a park
 * (proxyclient/tools/dump_snapshot.py). The layout below is the host's too.
 */

#ifndef SNAP_HOST
#include "types.h"
#endif

#define SNAP_MAGIC   0x31504e5341525541UL /* "AURASNP1" */
#define SNAP_VERSION 1

/* The buffer: this header, then records, each a struct snap_rec and len bytes (padded to 16). */
struct snap_hdr {
    u64 magic;
    u32 version, hdr_bytes;
    u64 capacity, used; /* bytes, the header included */
    u32 nrec, nsnap;
    u32 flags;    /* SNAP_F_* */
    u32 skipped;  /* snapshots not taken: over the budget, or the buffer full */
    u64 watch[4]; /* sptm_virt_snap_watch's words, from the first snapshot that read them */
    u32 watch_ok; /* bit i: watch[i] was read */
    u32 pad;
    u64 watch_at[4]; /* what watch[i] is: its chain index | bytes << 16 | offset << 32 */
    u64 reserved;
};

#define SNAP_F_FULL BIT(0) /* a record didn't fit */

struct snap_rec {
    u32 kind;
    u32 len; /* data bytes after this header */
    u64 va, pa, attrs;
    u64 a, b, c, d;
};

enum {
    SNAP_SNAP = 1, /* va doorbell word, pa its second word, attrs guest pc, a CNTPCT,
                      b cpu | ep << 8 | match << 32, c the MMIO ring's head, d root VA */
    SNAP_REGION,   /* va index (tables, shared-l2, ttbs, handoff), pa, a size, c pages refused */
    SNAP_WALK,     /* va ttbr1, pa leaves, attrs tables refused << 32 | SNAP_W_*, a..d TTBAT[0..3];
                      data: runs of {va, va_end, pa, attrs} */
    SNAP_OBJ,      /* va, pa and attrs of its first page, a parent (object index, ~0 a root),
                      b offset in the parent, c depth, d object index | root slot << 32 */
    SNAP_END,      /* va snapshot index, a objects, b CNTPCT ticks taken, c SNAP_E_*, d refused */
    SNAP_CHAIN,    /* va, pa, attrs at its end, a chain index | status << 32, b the step that
                      failed, c/d its name */
};

#define SNAP_W_TTBAT_FALLBACK BIT(0) /* no valid TTBAT entry: the tables region's first page */
#define SNAP_W_TRUNCATED      BIT(1) /* the buffer filled before the walk ended */

#define SNAP_E_FULL     BIT(0) /* the buffer filled */
#define SNAP_E_OBJ_MAX  BIT(1) /* the object limit was reached */
#define SNAP_E_REFUSED  BIT(2) /* a PA outside guest DRAM was never read */
#define SNAP_E_NO_TABLE BIT(3) /* no firmware page tables: nothing translated */

/* What a snapshot takes after the first, which takes everything (sptm_virt_snap_parts). */
#define SNAP_PART_REGIONS BIT(0)
#define SNAP_PART_WALK    BIT(1)
#define SNAP_PART_OBJECTS BIT(2)
#define SNAP_PART_CHAINS  BIT(3)

/* A chain: from the snapshot's InitData root, read a pointer at each step's offset (or, with
 * SNAP_STEP_ADD, add the offset), then copy len bytes there. */
#define SNAP_CHAINS      24
#define SNAP_CHAIN_STEPS 6
#define SNAP_STEP_ADD    BIT(63)
struct snap_chain {
    char name[16];
    u64 len; /* 0: unused */
    u32 nsteps, pad;
    u64 step[SNAP_CHAIN_STEPS];
};
enum { SNAP_C_OK, SNAP_C_UNMAPPED, SNAP_C_SHORT };

struct snap_watch {
    u32 chain; /* 1 + the chain whose copy holds the word; 0: unused */
    u32 bytes; /* 4 or 8 */
    u64 off;
};

struct snap_kick {
    u64 t, value, value_hi, pc, ring_head;
    u32 cpu, ep, match;
};

/* Set by the host before entry (run_guest_sptm.py) or by the GPU hook's setup. */
extern u64 sptm_virt_snap_region[4][2]; /* {pa, size} */
extern u64 sptm_virt_snap_roots[4];     /* firmware VAs for the objects; 0: from the doorbell */
extern struct snap_chain sptm_virt_snap_chains[SNAP_CHAINS];
extern struct snap_watch sptm_virt_snap_watch[4];
extern u32 sptm_virt_snap_parts; /* SNAP_PART_* of every snapshot after the first */
extern u32 sptm_virt_snap_max;   /* snapshots in all */

void sptm_virt_snap_init(void *buf, u64 bytes);
/* One snapshot; false if none was taken (the budget, or no room). */
bool sptm_virt_snap_take(const struct snap_kick *k);

/* From the platform: may [pa, pa + len) be read (normal RAM aura maps, never MMIO)? And where. */
bool sptm_virt_snap_pa_ok(u64 pa, u64 len);
const void *sptm_virt_snap_pa(u64 pa);
u64 sptm_virt_snap_ticks(void);

#endif
