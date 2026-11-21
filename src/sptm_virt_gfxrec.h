/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_GFXREC_H
#define SPTM_VIRT_GFXREC_H

/*
 * The GPU/PMP record (src/sptm_virt_gfxrec.c): from the first InitData doorbell for a host-set
 * window, every mailbox message on the GPU's and the PMP's ASCs, both ways, and the guest's
 * writes to a few GPU and PMGR registers, each as one record in a buffer in aura's carve-out. The
 * guest runs on: the record only reads what each trapped access carried once it was performed.
 * The host reads the buffer at a park (proxyclient/aura/hvgfxrec.py). The layout below is the
 * host's too.
 */

#ifndef GFXREC_HOST
#include "types.h"
#endif

#define GFXREC_MAGIC   0x3152504741525541UL /* "AURAGPR1" */
#define GFXREC_VERSION 1
#define GFXREC_SRCS    4 /* the windows below; the PMGR's PS words are listed apart */
#define GFXREC_PS_MAX  8
#define GFXREC_SGX_W   4

/* Where a record's access went (meta's src). */
enum {
    GFXREC_SRC_GFX_ASC,  /* /arm-io/gfx-asc reg 0: its mailbox */
    GFXREC_SRC_GFX1_ASC, /* /arm-io/gfx1-asc reg 0 (none on the J773g) */
    GFXREC_SRC_PMP_ASC,  /* /arm-io/pmp reg 0: its mailbox */
    GFXREC_SRC_SGX,      /* /arm-io/sgx reg 0: the writes in sgx_w */
    GFXREC_SRC_PMGR,     /* a GFX/AGX/SGX device's PMGR power-state word: its writes */
};

/* An ASC's mailbox, from its reg 0: the message to it (endpoint word at +8), and its own. */
#define GFXREC_A2I 0x8800
#define GFXREC_I2A 0x8830

enum {
    GFXREC_OFF,       /* no record in this boot */
    GFXREC_ARMED,     /* waiting for the doorbell */
    GFXREC_ON,        /* recording */
    GFXREC_DONE_TIME, /* the window ended: everything in it is here */
    GFXREC_DONE_FULL, /* the buffer filled at t_done: what came after it is lost */
};

struct gfxrec_rec {
    u64 t;    /* CNTPCT_EL0 once the access was performed; 0: still being written */
    u64 val;  /* the value written or read; a pair's first word */
    u64 hi;   /* a pair's second word (a mailbox message's endpoint word), else 0 */
    u64 meta; /* pa [47:0] | src [51:48] | write [52] | width [55:53] | cpu [59:56] */
};

/* The buffer: this header, then cap records. */
struct gfxrec_hdr {
    u64 magic;
    u32 version, hdr_bytes;
    u32 rec_bytes, state; /* GFXREC_* */
    u64 cap;              /* records the buffer holds */
    u64 claimed;          /* slots taken: past cap, each one a record lost */
    u64 t_start;          /* CNTPCT at the doorbell, before it was performed */
    u64 t_end;            /* the window's end: nothing at or after it is recorded */
    u64 t_done;           /* the first access at or past t_end, or the first one that didn't fit */
    u64 cntfrq, window_ms;
    u64 trig_val, trig_hi, trig_pc; /* the doorbell and the guest PC that wrote it */
    u32 trig_cpu, n_ps;
    u64 base[GFXREC_SRCS], size[GFXREC_SRCS]; /* each window; size 0: absent */
    u32 sgx_w[GFXREC_SGX_W][2];               /* the sgx writes taken: {offset, bytes} */
    u64 ps[GFXREC_PS_MAX];                    /* the PS words */
    char ps_name[GFXREC_PS_MAX][16];          /* their devices */
    u32 ps_trapped; /* bit i: the guest's accesses to ps[i]'s page trap (else none is recorded) */
    u32 pad;
    u64 reserved[13];
};

/*
 * The MMIO_PROTECTED block (by default sgx +0xe00000, 0x4000 bytes: the register-map record the
 * GPU firmware rewrites at InitData) has a record of its own: every guest access to it, reads and
 * writes, SPTM's (its GL1 traps) and XNU's, from the first the stage 2 traps (each core's XNU
 * entry) until the window above ends. Its own buffer: a block XNU polls can't starve the above.
 */
#define GFXBLK_MAGIC 0x314b4c4241525541UL /* "AURABLK1" */

struct gfxblk_rec {
    u64 t;    /* CNTPCT_EL0 once the access was performed; 0: still being written */
    u64 val;  /* the value written or read (a pair: one record per word) */
    u64 pc;   /* the guest PC */
    u64 meta; /* pa [47:0] | write [52] | width [55:53] | cpu [59:56] | gl [60] (SPTM) |
               * pair [61] (a word of a 128-bit access) */
};

struct gfxblk_hdr {
    u64 magic;
    u32 version, hdr_bytes;
    u32 rec_bytes, state; /* GFXREC_*: ON from the start */
    u64 cap, claimed;
    u64 base, size;
    u64 t_first; /* the first access recorded */
    u64 t_done;  /* when it ended: the window's end, or the first access that didn't fit */
    u64 cntfrq;
    char node[48]; /* the ADT reg that covers it, e.g. "/arm-io/sgx reg[1] +0x100000" */
};

/* Set by the host before entry (run_guest_sptm.py). */
extern u32 sptm_virt_gfxrec_ms;                               /* the window; 0: no record */
extern u32 sptm_virt_gfxrec_max;                              /* records the buffer holds */
extern u64 sptm_virt_gfxrec_blk_pa, sptm_virt_gfxrec_blk_len; /* 0: sgx +0xe00000, 0x4000 */
extern u32 sptm_virt_gfxrec_blk_max;                          /* its records */
/* For the host, at a park. */
extern struct gfxrec_hdr *sptm_virt_gfxrec_buf;
extern struct gfxblk_hdr *sptm_virt_gfxrec_blk;
/* The MMIO ring stops at the window's end too (sptm_virt_gpu.c): 0 until the doorbell. */
extern u64 sptm_virt_gfxrec_ring_stop;

/* The header in bytes (at least sizeof(struct gfxrec_hdr) + one record), ARMED. */
void sptm_virt_gfxrec_init(void *buf, u64 bytes, u64 cntfrq, u32 window_ms);
void sptm_virt_gfxrec_source(u32 src, u64 base, u64 size);
void sptm_virt_gfxrec_add_ps(u64 pa, const char *name, bool trapped);
/* At the doorbell, before it is performed: true on the one call that starts the window. */
bool sptm_virt_gfxrec_trigger(u64 t, u64 val, u64 hi, u64 pc, u32 cpu);
/* After an access in a trapped window was performed, with its time then. Takes the ones above. */
void sptm_virt_gfxrec_note(u64 addr, const u64 *val, bool write, int width, u32 cpu, u64 t);
#ifndef GFXREC_HOST
/* From the ADT (sptm_virt_gfxrec_adt.c): the /arm-io child's reg that covers [pa, pa + len) most
 * closely, as "<path> reg[i] +<offset>" ("" if none); the PS words of the PMGR devices whose names
 * hold GFX, AGX or SGX (a virtual device has none), up to max. */
void sptm_virt_gfxrec_adt_cover(u64 pa, u64 len, char *out, size_t n);
int sptm_virt_gfxrec_adt_ps(u64 *ps, char (*names)[17], int max);
#endif

/* The block's record, ON at once (bytes: at least the header and two records). */
void sptm_virt_gfxrec_blk_init(void *buf, u64 bytes, u64 cntfrq, u64 base, u64 size,
                               const char *node);
/* After an access was performed: gl, a GL1 trap (SPTM's), pc the guest's. */
void sptm_virt_gfxrec_blk_note(u64 addr, const u64 *val, bool write, int width, u32 cpu, u64 t,
                               u64 pc, bool gl);

#endif
