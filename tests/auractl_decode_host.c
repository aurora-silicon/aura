/* SPDX-License-Identifier: MIT */

/*
 * Host unit test for auractl's pure register decode + ring (src/sptm_virt_auractl.c with
 * AURACTL_HOST defined, which drops the HV/EL2 glue). Covers the ABI handshake, ARM range
 * validation, the IDLE->ARMED->RUNNING->STOPPED state machine, the record stream / DUMP / FIFO pop
 * order and count, and DROPPED on ring wrap. No hardware -- this is the bulk of the correctness
 * proof. Modelled on tests/stage1_config_host.c.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Minimal firmware types the pure part of sptm_virt_auractl.c needs (see src/types.h). */
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define AURACTL_HOST 1
#include "../src/sptm_virt_auractl.c"

/* --- small helpers over the pure decoder --- */

static u64 rd(struct auractl_dev *d, u32 off, int width)
{
    u64 v = 0;
    auractl_decode(d, off, &v, false, width);
    return v;
}

static void wr(struct auractl_dev *d, u32 off, u64 v, int width)
{
    auractl_decode(d, off, &v, true, width);
}

/* Issue a command via the register file. */
static void cmd(struct auractl_dev *d, u32 op)
{
    wr(d, AURACTL_REG_CMD, op, 2);
}

static u32 state(struct auractl_dev *d)
{
    return (u32)(rd(d, AURACTL_REG_STATUS, 2) & AURACTL_STATE_MASK);
}

static u32 err(struct auractl_dev *d)
{
    return (u32)((rd(d, AURACTL_REG_STATUS, 2) & AURACTL_ERR_MASK) >> AURACTL_ERR_SH);
}

/* A base inside the device-MMIO aperture, clear of the control page. */
#define OK_BASE 0x210000000UL
#define OK_SIZE 0x4000UL

static void arm(struct auractl_dev *d, u64 base, u64 size)
{
    wr(d, AURACTL_REG_ARG0, base, 3);
    wr(d, AURACTL_REG_ARG1, size, 3);
    cmd(d, AURACTL_CMD_ARM);
}

int main(void)
{
    struct auractl_rec ring[8];
    struct auractl_dev dev;
    struct auractl_dev *d = &dev;

    memset(&dev, 0, sizeof dev);
    d->ring = ring;
    d->nrec_cap = 8;
    d->state = AURACTL_STATE_IDLE;

    /* --- handshake --- */
    assert(rd(d, AURACTL_REG_MAGIC, 2) == AURACTL_MAGIC);
    assert(rd(d, AURACTL_REG_VERSION, 2) == AURACTL_VERSION);
    u32 caps = (u32)rd(d, AURACTL_REG_CAPS, 2);
    assert((caps & AURACTL_CAPS_RANGES_MASK) == AURACTL_RANGES_MAX);
    assert(caps & AURACTL_CAPS_FIFO);
    assert(((caps >> AURACTL_CAPS_RECBYTES_SH) & 0xff) == AURACTL_REC_BYTES);

    /* --- ARG0/ARG1 read back as u64 --- */
    wr(d, AURACTL_REG_ARG0, 0x1122334455667788UL, 3);
    wr(d, AURACTL_REG_ARG1, 0x99aabbccddeeff00UL, 3);
    assert(rd(d, AURACTL_REG_ARG0, 3) == 0x1122334455667788UL);
    assert(rd(d, AURACTL_REG_ARG1, 3) == 0x99aabbccddeeff00UL);

    /* --- ARM validation: each bad range sets the right err and leaves state IDLE --- */
    assert(state(d) == AURACTL_STATE_IDLE);

    arm(d, OK_BASE + 1, OK_SIZE); /* misaligned base */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    arm(d, OK_BASE, OK_SIZE + 1); /* misaligned size */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    arm(d, OK_BASE, 0); /* zero size */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    arm(d, 0x40000000UL, OK_SIZE); /* RAM / below the device aperture */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    arm(d, AURACTL_DEV_WINDOW_END, OK_SIZE); /* at/above the aperture top */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    arm(d, AURACTL_MMIO_BASE, OK_SIZE); /* the controller's own page */
    assert(err(d) == AURACTL_ERR_BADRANGE && state(d) == AURACTL_STATE_IDLE);

    assert(rd(d, AURACTL_REG_NRANGES, 2) == 0);

    /* --- a valid ARM: ARMED, NRANGES bumps --- */
    arm(d, OK_BASE, OK_SIZE);
    assert(err(d) == AURACTL_ERR_NONE && state(d) == AURACTL_STATE_ARMED);
    assert(rd(d, AURACTL_REG_NRANGES, 2) == 1);
    assert(d->pending_hook && d->pending_range.base == OK_BASE && d->pending_range.size == OK_SIZE);

    /* a second, adjacent range is accepted (abuts, does not overlap) */
    arm(d, OK_BASE + OK_SIZE, OK_SIZE);
    assert(err(d) == AURACTL_ERR_NONE && rd(d, AURACTL_REG_NRANGES, 2) == 2);

    /* overlap is rejected and leaves state/NRANGES unchanged */
    arm(d, OK_BASE, OK_SIZE);
    assert(err(d) == AURACTL_ERR_OVERLAP && state(d) == AURACTL_STATE_ARMED);
    assert(rd(d, AURACTL_REG_NRANGES, 2) == 2);

    /* over the size cap is rejected (one range larger than AURACTL_SIZE_CAP) */
    arm(d, OK_BASE + 0x100000UL, AURACTL_SIZE_CAP + OK_SIZE);
    assert(err(d) == AURACTL_ERR_FULL && rd(d, AURACTL_REG_NRANGES, 2) == 2);

    /* READBACK loads range 0 into ARG0/ARG1 */
    wr(d, AURACTL_REG_RANGE_IDX, 0, 2);
    cmd(d, AURACTL_CMD_READBACK);
    assert(err(d) == AURACTL_ERR_NONE);
    assert(rd(d, AURACTL_REG_ARG0, 3) == OK_BASE && rd(d, AURACTL_REG_ARG1, 3) == OK_SIZE);

    /* READBACK of an out-of-range index errors */
    wr(d, AURACTL_REG_RANGE_IDX, 99, 2);
    cmd(d, AURACTL_CMD_READBACK);
    assert(err(d) == AURACTL_ERR_BADRANGE);

    /* --- RANGES_MAX cap: fill the table (2 armed; each tiny, under the size cap) --- */
    for (u32 i = (u32)rd(d, AURACTL_REG_NRANGES, 2); i < AURACTL_RANGES_MAX; i++)
        arm(d, OK_BASE + 0x200000UL + (u64)i * OK_SIZE, OK_SIZE);
    assert(rd(d, AURACTL_REG_NRANGES, 2) == AURACTL_RANGES_MAX && err(d) == AURACTL_ERR_NONE);
    arm(d, OK_BASE + 0x400000UL, OK_SIZE); /* one too many */
    assert(err(d) == AURACTL_ERR_FULL && rd(d, AURACTL_REG_NRANGES, 2) == AURACTL_RANGES_MAX);

    /* --- state machine --- */
    cmd(d, AURACTL_CMD_STOP); /* illegal: not RUNNING */
    assert(err(d) == AURACTL_ERR_BADSTATE && state(d) == AURACTL_STATE_ARMED);

    cmd(d, AURACTL_CMD_START);
    assert(err(d) == AURACTL_ERR_NONE && state(d) == AURACTL_STATE_RUNNING);

    cmd(d, AURACTL_CMD_DUMP); /* illegal: RUNNING */
    assert(err(d) == AURACTL_ERR_BADSTATE && state(d) == AURACTL_STATE_RUNNING);

    cmd(d, AURACTL_CMD_ARM); /* illegal: cannot arm while RUNNING */
    assert(err(d) == AURACTL_ERR_BADSTATE && state(d) == AURACTL_STATE_RUNNING);

    cmd(d, AURACTL_CMD_STOP);
    assert(err(d) == AURACTL_ERR_NONE && state(d) == AURACTL_STATE_STOPPED);

    cmd(d, 42); /* unknown opcode */
    assert(err(d) == AURACTL_ERR_BADCMD && state(d) == AURACTL_STATE_STOPPED);

    cmd(d, AURACTL_CMD_DISARM);
    assert(err(d) == AURACTL_ERR_NONE && state(d) == AURACTL_STATE_IDLE);
    assert(rd(d, AURACTL_REG_NRANGES, 2) == 0);

    /* START with no armed range is illegal */
    cmd(d, AURACTL_CMD_START);
    assert(err(d) == AURACTL_ERR_BADSTATE && state(d) == AURACTL_STATE_IDLE);

    /* --- record stream + DUMP pop order (oldest-first) and count --- */
    cmd(d, AURACTL_CMD_RESET);
    assert(state(d) == AURACTL_STATE_IDLE && rd(d, AURACTL_REG_NREC, 3) == 0);
    arm(d, OK_BASE, OK_SIZE);
    cmd(d, AURACTL_CMD_START);

    const u64 N = 5; /* fits the 8-record ring, no wrap */
    for (u64 i = 0; i < N; i++)
        auractl_record(d, 1000 + i, 2000 + i, 3000 + i, 4000 + i);
    assert(rd(d, AURACTL_REG_NREC, 3) == N);
    assert(rd(d, AURACTL_REG_DROPPED, 3) == 0);

    cmd(d, AURACTL_CMD_STOP);
    cmd(d, AURACTL_CMD_DUMP);
    assert(err(d) == AURACTL_ERR_NONE);
    assert(rd(d, AURACTL_REG_FIFO_AVAIL, 3) == N);

    for (u64 i = 0; i < N; i++) {
        assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 1000 + i); /* t */
        assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 2000 + i); /* meta */
        assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 3000 + i); /* val */
        assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 4000 + i); /* pc */
        assert(rd(d, AURACTL_REG_FIFO_AVAIL, 3) == N - 1 - i);
    }
    assert(rd(d, AURACTL_REG_FIFO_AVAIL, 3) == 0);
    assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 0); /* drained */

    /* --- DROPPED: dump in flight, then overfill so the oldest unread records are lost --- */
    cmd(d, AURACTL_CMD_RESET);
    arm(d, OK_BASE, OK_SIZE);
    cmd(d, AURACTL_CMD_START);
    for (u64 i = 0; i < 8; i++) /* fill the 8-record ring exactly */
        auractl_record(d, 100 + i, 0, 0, 0);
    cmd(d, AURACTL_CMD_STOP);
    cmd(d, AURACTL_CMD_DUMP);
    assert(rd(d, AURACTL_REG_FIFO_AVAIL, 3) == 8 && rd(d, AURACTL_REG_DROPPED, 3) == 0);

    /* Resume and write 3 more; those overwrite the 3 oldest (t=100,101,102) before they were
     * popped. */
    cmd(d, AURACTL_CMD_START);
    for (u64 i = 0; i < 3; i++)
        auractl_record(d, 200 + i, 0, 0, 0);
    assert(rd(d, AURACTL_REG_DROPPED, 3) == 3);
    /* The surviving dump window is now the newest records only: oldest live t == 103. */
    assert(rd(d, AURACTL_REG_FIFO_DATA, 3) == 103);

    printf(
        "PASS: auractl handshake, ARM validation, state machine, DUMP order/count and DROPPED\n");
    return 0;
}
