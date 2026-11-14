/* SPDX-License-Identifier: MIT */
/*
 * auractl: the guest-to-aura control ABI, version 1 (SPTM-virt builds made with AURACTL=1 only).
 *
 * The SPTM-virt guest's kernel, at virtual EL2, makes the call on behalf of one of its processes:
 *
 *     w0 = AURACTL_HVC_MAGIC, x1 = the process's address of its struct auractl_req,
 *     hvc #AURACTL_HVC_IMM
 *
 * and gets 0 or an errno back in x0. aura reads the request block and every buffer it names
 * through that process's own EL0 translation, as the process itself would, runs the command and
 * writes the outputs back. The guest decides which of its processes may reach the call; aura
 * never trusts a guest address it has not translated that way. A guest reaches the call through
 * an entry of its own; this tree ships one, the IOKit kext under tools/auractl (docs/auractl.md).
 *
 * Errno values are Darwin's, since the guest is macOS and its caller sees them as errno.
 * Shared by aura (src/sptm_virt_ctl.c) and tools/auractl/auractl.c.
 */

#ifndef AURACTL_ABI_H
#define AURACTL_ABI_H

#include <stdint.h>

#define AURACTL_HVC_IMM   0xAC71     /* the hvc immediate */
#define AURACTL_HVC_MAGIC 0x41435431 /* 'ACT1' in w0 at the hvc */
#define AURACTL_REQ_MAGIC 0x4c544341 /* 'ACTL' in auractl_req.magic */
#define AURACTL_VERSION   1

enum auractl_cmd {
    AURACTL_INFO = 0,    /* buf <- struct auractl_info */
    AURACTL_LIST = 1,    /* buf <- struct auractl_range[]; ret0 = entries in all */
    AURACTL_ARM = 2,     /* arg0 = AURACTL_SEL_*; ret0 = pages hooked */
    AURACTL_DISARM = 3,  /* same selectors; ret0 = pages released */
    AURACTL_START = 4,   /* resume recording into the ring */
    AURACTL_STOP = 5,    /* pause recording (hooks stay armed, accesses still go through) */
    AURACTL_CLEAR = 6,   /* empty the ring; ret0 = records dropped */
    AURACTL_READ = 7,    /* arg0 = first sequence number, arg1 = max records; buf <- records */
    AURACTL_MARK = 8,    /* buf = text (tag = first 8 bytes), arg0 = value; ret0 = its seq */
    AURACTL_CONSOLE = 9, /* buf <- the tail of aura's console ring; ret0 = bytes ever written */
    AURACTL_PEEK =
        10, /* arg0 = PA, arg1 = words (1..AURACTL_MAX_PEEK); buf <- u32s; ret0 = first */
    AURACTL_NCMDS
};

/* ARM/DISARM selector, in arg0 */
enum auractl_sel {
    AURACTL_SEL_NAME = 0,  /* buf = NUL-terminated catalogue name (every range of that name) */
    AURACTL_SEL_RANGE = 1, /* arg1 = physical base, arg2 = size, inside a hookable block */
    AURACTL_SEL_ALL = 2,   /* DISARM only: every range auractl armed */
};

struct auractl_req {
    uint32_t magic;   /* in: AURACTL_REQ_MAGIC */
    uint16_t version; /* in: AURACTL_VERSION; out: aura's version */
    uint16_t cmd;     /* in */
    uint64_t arg0, arg1, arg2;
    uint64_t buf;     /* in: the caller's address of the data buffer, or 0 */
    uint64_t buf_len; /* in: its size */
    uint64_t ret0, ret1, ret2;
    uint64_t buf_used; /* out: bytes aura wrote to (or read from) buf */
};

#define AURACTL_INFO_TRACE     (1u << 0) /* the ring exists (the boot's trace flags made one) */
#define AURACTL_INFO_RECORDING (1u << 1) /* records are being appended */
/* bit 2 is reserved */
#define AURACTL_INFO_ESP (1u << 3) /* an SPTM_VIRT_ESP build */

struct auractl_info {
    uint32_t version;
    uint32_t flags;     /* AURACTL_INFO_* */
    uint64_t cntfrq;    /* record timestamps are CNTPCT_EL0 ticks at this rate */
    uint64_t nrec;      /* ring capacity in records */
    uint64_t head;      /* records ever appended (the next sequence number) */
    uint32_t rec_size;  /* sizeof(struct auractl_rec) */
    uint32_t pages;     /* stage-2 pages hooked in all (trace setup and auractl) */
    uint32_t ctl_pages; /* of those, armed through auractl */
    uint32_t ncatalog;  /* catalogue entries LIST returns */
    char build[48];     /* aura's version string */
};

/* One ring record, as aura stores it (src/sptm_virt_s2r.c struct s2r_rec). */
struct auractl_rec {
    uint64_t t;    /* CNTPCT_EL0 */
    uint64_t meta; /* pa [47:0] | width [49:48] | write [50] | cpu [54:51] | marker [55] |
                      cpu bits 5:4 [57:56] */
    uint64_t val;  /* value written or read, or the marker's value */
    uint64_t pc;   /* guest PC, or the marker tag (8 chars) */
};

#define AURACTL_REC_PA(m)     ((uint64_t)((m) & 0xffffffffffffULL))
#define AURACTL_REC_WIDTH(m)  (((m) >> 48) & 3) /* log2 of bytes */
#define AURACTL_REC_WRITE(m)  (((m) >> 50) & 1)
#define AURACTL_REC_CPU(m)    ((((m) >> 51) & 15) | ((((m) >> 56) & 3) << 4))
#define AURACTL_REC_MARKER(m) (((m) >> 55) & 1)

#define AURACTL_RANGE_ARMED   (1u << 0) /* every page of the range is hooked */
#define AURACTL_RANGE_PARTIAL (1u << 1) /* some of its pages are hooked */
#define AURACTL_RANGE_FOUND   (1u << 2) /* resolved on this machine, from its ADT */

struct auractl_range {
    char name[24];
    uint64_t base;
    uint64_t size;
    uint32_t flags; /* AURACTL_RANGE_* */
    uint32_t pages_hooked;
};

_Static_assert(sizeof(struct auractl_req) == 80, "auractl_req is ABI");
_Static_assert(sizeof(struct auractl_info) == 96, "auractl_info is ABI");
_Static_assert(sizeof(struct auractl_rec) == 32, "auractl_rec is ABI");
_Static_assert(sizeof(struct auractl_range) == 48, "auractl_range is ABI");

/* Limits aura enforces on every call */
#define AURACTL_MAX_BUF   (256u << 10) /* bytes moved per call */
#define AURACTL_MAX_PAGES 64           /* pages one ARM/DISARM may change */
#define AURACTL_MAX_MARK  64           /* marker text bytes */
#define AURACTL_MAX_PEEK  64           /* words one PEEK reads */

/* Darwin errno values */
#define AURACTL_EPERM  1
#define AURACTL_ENOENT 2
#define AURACTL_E2BIG  7
#define AURACTL_EACCES 13
#define AURACTL_EFAULT 14
#define AURACTL_EBUSY  16
#define AURACTL_ENODEV 19
#define AURACTL_EINVAL 22
#define AURACTL_ENOSPC 28
#define AURACTL_ENOSYS 78

#endif
