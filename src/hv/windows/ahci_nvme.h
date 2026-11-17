/* SPDX-License-Identifier: MIT */

#ifndef AHCI_NVME_H
#define AHCI_NVME_H

#include "types.h"

/*
 * D5: the AHCI disk's NVMe backend, pure part (windows-m6/D5-NVME-DESIGN.md sections 4, 7.3).
 *
 * The AHCI model (ahci_model.c) speaks 512-byte sectors; the internal NVMe has 4096-byte
 * blocks.  This maps AHCI sector L to partition byte L x 512, i.e. NVMe block
 * first + L / 8 at byte (L % 8) x 512, and turns the model's per-PRD calls into whole-block
 * NVMe commands through a 256 KiB read window and a 256 KiB write window:
 *  - reads fill the read window with up to 64 blocks (one command) and copy out;
 *  - writes are staged in the write window; a block the command covers only in part (the
 *    head or the tail) is read first and overlaid (read-modify-write); full windows and, at the
 *    end of the command, the rest are written; the command completes only after every block
 *    it changed has an NVMe completion, so nothing acknowledged stays in carrier memory.
 * RMW is atomic against other AHCI commands because the model runs one command at a time
 * (single port, no NCQ, synchronous completion) under the caller's lock.
 *
 * Every device call is checked against [first, last] here (guard 1); the device layer has its
 * own fence (guard 2, nvme.c).  A guard hit marks the backend failed: every later command
 * fails.  `closing` (set by the exit and reset paths) fails new commands at once.
 *
 * Pure: no locks, no printing, no allocation.  Buffers come from the caller (the carrier's
 * bounce pool); the device sees only those, never guest memory.
 */

#define AHCI_NVME_BLOCK      4096u
#define AHCI_NVME_SPB        8u  /* 512-byte sectors per block */
#define AHCI_NVME_MAX_BLOCKS 64u /* blocks per NVMe command */
#define AHCI_NVME_WINDOW     (AHCI_NVME_MAX_BLOCKS * AHCI_NVME_BLOCK)
/* The pool: read window, write window, one RMW/scratch block. */
#define AHCI_NVME_POOL_BYTES (2 * AHCI_NVME_WINDOW + AHCI_NVME_BLOCK)

struct ahci_nvme_dev {
    /* Absolute NVMe blocks, 1..64 of them, to or from a pool buffer.  false = I/O error. */
    bool (*read)(void *opaque, u64 block, u32 count, void *buf);
    bool (*write)(void *opaque, u64 block, u32 count, const void *buf);
    bool (*flush)(void *opaque);
    void *opaque;
};

struct ahci_nvme_stats {
    u64 commands;    /* begin() calls accepted */
    u64 failed;      /* commands that ended with an error */
    u64 dev_reads;   /* NVMe read commands */
    u64 dev_writes;  /* NVMe write commands */
    u64 dev_flushes; /* NVMe flushes */
    u64 blocks_read;
    u64 blocks_written;
    u64 rmw;        /* blocks read for a partial write */
    u64 guard_hits; /* guard 1 refusals (a carrier bug) */
    u64 refused;    /* commands refused because closing or failed */
};

struct ahci_nvme {
    struct ahci_nvme_dev dev;
    u64 first, last; /* the partition, NVMe blocks, inclusive */
    u64 sectors;     /* (last - first + 1) x 8 */
    bool read_only;
    bool flush_every_write;
    u8 *pool; /* AHCI_NVME_POOL_BYTES, 4 KiB aligned */
    /* Read window: blocks [rblock, rblock + rcount). */
    bool rvalid;
    u64 rblock;
    u32 rcount;
    /* The command in progress. */
    bool active, writing;
    u64 cmd_lba, cmd_end; /* AHCI sectors [cmd_lba, cmd_end) */
    u64 wnext;            /* next AHCI sector the model must hand over */
    u64 wblock;           /* first block in the write window */
    u32 wblocks;          /* blocks staged in the write window */
    bool wprimed_head, wprimed_tail;
    bool wrote; /* a device write was issued in this backend's life */
    volatile bool closing;
    bool failed;
    struct ahci_nvme_stats stats;
};

/* Set up; nothing is read.  first <= last, pool 4 KiB aligned. */
void ahci_nvme_init(struct ahci_nvme *b, const struct ahci_nvme_dev *dev, u8 *pool, u64 first,
                    u64 last, bool read_only, bool flush_every_write);

/* struct ahci_env members (opaque = struct ahci_nvme *). */
bool ahci_nvme_begin(void *opaque, u64 lba, u32 count, bool write);
bool ahci_nvme_read(void *opaque, u64 lba, u32 count, void *dst);
bool ahci_nvme_write(void *opaque, u64 lba, u32 count, const void *src);
bool ahci_nvme_end(void *opaque, bool ok, bool write_through);
bool ahci_nvme_flush(void *opaque);

/*
 * Guard 1 on its own: true if AHCI sectors [lba, lba + count) lie in the partition, and then
 * the NVMe blocks they touch.  Exposed for the host fuzz.
 */
bool ahci_nvme_map(const struct ahci_nvme *b, u64 lba, u64 count, u64 *block0, u64 *block1);

#endif
