/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * D5: the AHCI disk's NVMe backend, pure part.  See ahci_nvme.h.  Written on this branch from
 * the design (windows-m6/D5-NVME-DESIGN.md); no third-party source.
 */

#include "ahci_nvme.h"

#define SECTOR 512u

static u8 *rwin(struct ahci_nvme *b)
{
    return b->pool;
}

static u8 *wwin(struct ahci_nvme *b)
{
    return b->pool + AHCI_NVME_WINDOW;
}

static u64 min64(u64 a, u64 b)
{
    return a < b ? a : b;
}

void ahci_nvme_init(struct ahci_nvme *b, const struct ahci_nvme_dev *dev, u8 *pool, u64 first,
                    u64 last, bool read_only, bool flush_every_write)
{
    __builtin_memset(b, 0, sizeof(*b));
    b->dev = *dev;
    b->pool = pool;
    b->first = first;
    b->last = last;
    b->sectors = (last - first + 1) * AHCI_NVME_SPB;
    b->read_only = read_only;
    b->flush_every_write = flush_every_write;
}

bool ahci_nvme_map(const struct ahci_nvme *b, u64 lba, u64 count, u64 *block0, u64 *block1)
{
    if (!count || lba >= b->sectors || count > b->sectors - lba)
        return false;
    u64 b0 = b->first + lba / AHCI_NVME_SPB;
    u64 b1 = b->first + (lba + count - 1) / AHCI_NVME_SPB;
    if (b0 < b->first || b1 > b->last || b0 > b1)
        return false;
    *block0 = b0;
    *block1 = b1;
    return true;
}

/* Guard 1: every device command stays inside the partition and uses only the pool. */
static bool guard(struct ahci_nvme *b, u64 block, u32 count, const void *buf)
{
    const u8 *p = buf;
    if (count && count <= AHCI_NVME_MAX_BLOCKS && block >= b->first && block <= b->last &&
        count - 1 <= b->last - block && p >= b->pool &&
        (u64)(p - b->pool) + (u64)count * AHCI_NVME_BLOCK <= AHCI_NVME_POOL_BYTES)
        return true;
    b->stats.guard_hits++;
    b->failed = true;
    return false;
}

static bool dev_read(struct ahci_nvme *b, u64 rel, u32 count, void *buf)
{
    u64 block = b->first + rel;
    if (b->closing || !guard(b, block, count, buf))
        return false;
    b->stats.dev_reads++;
    b->stats.blocks_read += count;
    return b->dev.read(b->dev.opaque, block, count, buf);
}

static bool dev_write(struct ahci_nvme *b, u64 rel, u32 count, const void *buf)
{
    u64 block = b->first + rel;
    if (b->closing || b->read_only || !guard(b, block, count, buf))
        return false;
    /* Before the command is issued: the exit and reset paths must know a write may be live. */
    b->wrote = true;
    b->stats.dev_writes++;
    b->stats.blocks_written += count;
    return b->dev.write(b->dev.opaque, block, count, buf);
}

static bool dev_flush(struct ahci_nvme *b)
{
    if (b->closing)
        return false;
    b->stats.dev_flushes++;
    return b->dev.flush(b->dev.opaque);
}

bool ahci_nvme_begin(void *opaque, u64 lba, u32 count, bool write)
{
    struct ahci_nvme *b = opaque;
    u64 b0, b1;

    if (b->closing || b->failed || b->active || (write && b->read_only)) {
        b->stats.refused++;
        return false;
    }
    if (!ahci_nvme_map(b, lba, count, &b0, &b1)) {
        /* The model's own range check (guard 0) runs first: reaching here is a carrier bug. */
        b->stats.guard_hits++;
        b->failed = true;
        return false;
    }
    /* The read window never outlives a command: no data is cached across commands. */
    b->rvalid = false;
    b->active = true;
    b->writing = write;
    b->cmd_lba = lba;
    b->cmd_end = lba + count;
    b->wnext = lba;
    b->wblock = lba / AHCI_NVME_SPB;
    b->wblocks = 0;
    b->stats.commands++;
    return true;
}

static bool in_command(struct ahci_nvme *b, u64 lba, u32 count)
{
    if (b->active && count && lba >= b->cmd_lba && lba < b->cmd_end && count <= b->cmd_end - lba)
        return true;
    b->stats.guard_hits++;
    b->failed = true;
    return false;
}

bool ahci_nvme_read(void *opaque, u64 lba, u32 count, void *dst)
{
    struct ahci_nvme *b = opaque;
    u8 *out = dst;

    if (b->closing || b->failed || b->writing || !in_command(b, lba, count))
        return false;
    while (count) {
        u64 blk = lba / AHCI_NVME_SPB;
        if (!b->rvalid || blk < b->rblock || blk >= b->rblock + b->rcount) {
            u64 end = (b->cmd_end - 1) / AHCI_NVME_SPB;
            b->rvalid = false;
            b->rblock = blk;
            b->rcount = (u32)min64(AHCI_NVME_MAX_BLOCKS, end - blk + 1);
            if (!dev_read(b, blk, b->rcount, rwin(b)))
                return false;
            b->rvalid = true;
        }
        u64 n = min64(count, (b->rblock + b->rcount) * AHCI_NVME_SPB - lba);
        __builtin_memcpy(out, rwin(b) + (lba - b->rblock * AHCI_NVME_SPB) * SECTOR, n * SECTOR);
        out += n * SECTOR;
        lba += n;
        count -= (u32)n;
    }
    return true;
}

static bool partial(const struct ahci_nvme *b, u64 blk)
{
    bool head = blk == b->cmd_lba / AHCI_NVME_SPB && b->cmd_lba % AHCI_NVME_SPB;
    bool tail = blk == (b->cmd_end - 1) / AHCI_NVME_SPB && b->cmd_end % AHCI_NVME_SPB;
    return head || tail;
}

static bool submit(struct ahci_nvme *b)
{
    if (!b->wblocks)
        return true;
    bool ok = dev_write(b, b->wblock, b->wblocks, wwin(b));
    b->wblock += b->wblocks;
    b->wblocks = 0;
    return ok;
}

bool ahci_nvme_write(void *opaque, u64 lba, u32 count, const void *src)
{
    struct ahci_nvme *b = opaque;
    const u8 *in = src;

    if (b->closing || b->failed || !b->writing || !in_command(b, lba, count))
        return false;
    if (lba != b->wnext) {
        /* The model streams one command in LBA order: anything else is a carrier bug. */
        b->stats.guard_hits++;
        b->failed = true;
        return false;
    }
    while (count) {
        u64 blk = lba / AHCI_NVME_SPB;
        if (blk == b->wblock + b->wblocks) {
            /* A new block: make room, then read it first if the command covers it in part. */
            if (b->wblocks == AHCI_NVME_MAX_BLOCKS && !submit(b))
                return false;
            u8 *slot = wwin(b) + (u64)(blk - b->wblock) * AHCI_NVME_BLOCK;
            b->wblocks++;
            if (partial(b, blk)) {
                b->stats.rmw++;
                if (!dev_read(b, blk, 1, slot))
                    return false;
            }
        }
        u64 n = min64(count, (blk + 1) * AHCI_NVME_SPB - lba);
        u8 *dst = wwin(b) + (blk - b->wblock) * AHCI_NVME_BLOCK + (lba % AHCI_NVME_SPB) * SECTOR;
        __builtin_memcpy(dst, in, n * SECTOR);
        in += n * SECTOR;
        lba += n;
        count -= (u32)n;
        b->wnext = lba;
    }
    return true;
}

bool ahci_nvme_end(void *opaque, bool ok, bool write_through)
{
    struct ahci_nvme *b = opaque;

    if (!b->active)
        return false;
    b->active = false;
    b->rvalid = false;
    if (ok && b->writing) {
        if (b->wnext != b->cmd_end || b->closing || b->failed)
            ok = false;
        else
            ok = submit(b);
        if (ok && (write_through || b->flush_every_write))
            ok = dev_flush(b);
    } else if (ok && (b->closing || b->failed)) {
        ok = false;
    }
    /* A failed write's window is dropped: ATA leaves a failed write's range undefined. */
    b->wblocks = 0;
    b->writing = false;
    if (!ok)
        b->stats.failed++;
    return ok;
}

bool ahci_nvme_flush(void *opaque)
{
    struct ahci_nvme *b = opaque;

    if (b->closing || b->failed)
        return false;
    if (b->read_only)
        return true;
    return dev_flush(b);
}
