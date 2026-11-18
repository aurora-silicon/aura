/* SPDX-License-Identifier: MIT */

/*
 * D5 carrier glue: the AHCI disk's NVMe backend.  See hv_ahci_nvme.h for the contract and
 * windows-m6/D5-NVME-DESIGN.md for the reasoning.  Written on this branch; no third-party
 * source.  The pure parts (ahci_nvme.c, gpt_check.c) are host-tested; so is this file, against
 * aura's real nvme.c and a simulated controller (tests/e2e/windows_nvme_sim.c).
 */

#include "hv_ahci_nvme.h"
#include "ahci_model.h"
#include "ahci_nvme.h"
#include "gpt_check.h"
#include "hv.h"
#include "hv_fatal.h"
#include "hv_t8152.h"
#include "malloc.h"
#include "nvme.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

#define NSID 1
/*
 * Longest wait for the controller lock: one command's longest timeout (nvme.c: a 10 s flush)
 * plus margin.  The close and reset paths wait as long: they first stop the owner issuing more
 * I/O commands (nvme_abort_io), so the lock comes free within the command in flight, and only a
 * CPU that is really stuck makes them shut the controller down without the lock.
 */
#define LOCK_WAIT_US       12000000
#define RESET_LOCK_WAIT_US LOCK_WAIT_US
#define POOL_ALLOC         ((AHCI_NVME_POOL_BYTES + 0x3fffUL) & ~0x3fffUL)

static struct ahci_nvme core;
static enum hv_ahci_nvme_state state;
static struct hv_ahci_nvme_req req;
static u8 *pool;
static bool pool_reserved;
static bool ever_attached;
static u64 found_first, found_last, inner_bytes;
static u64 longest_ticks, longest_flush_ticks;
static u64 reported;
static volatile bool closing;
/* P_REBOOT "FORCE" after a FAILED close: reset even if the final shutdown attempt fails. */
static bool force_reset;
/* A close shut the controller down without the lock: that CPU may still touch the queues. */
static bool closed_without_lock;

/*
 * Activity record for the fatal report (hv_fatal.h seqlock): the backend's last NVMe command,
 * written by the CPU that holds the controller lock.  `done` is 0 while the command is in
 * flight.  A guest SError, a panic or a watchdog bark then says whether the controller was
 * busy, with what and on which CPU.
 */
static struct {
    u64 seq;
    u64 n;    /* NVMe commands issued by the backend */
    u64 op;   /* 'R' read, 'W' write, 'F' flush */
    u64 cpu;
    u64 block, count;
    u64 start, done; /* CNTPCT at issue and at completion (0: in flight) */
    u64 ok;
} act;

static void act_begin(char op, u64 block, u64 count, u64 start)
{
    hv_fatal_seq_begin(&act.seq);
    act.n++;
    act.op = (u64)op;
    act.cpu = smp_id();
    act.block = block;
    act.count = count;
    act.start = start;
    act.done = 0;
    act.ok = 0;
    hv_fatal_seq_end(&act.seq);
}

static void act_end(bool ok)
{
    hv_fatal_seq_begin(&act.seq);
    act.done = mrs(CNTPCT_EL0);
    act.ok = ok;
    hv_fatal_seq_end(&act.seq);
}

static u64 ticks_to_us(u64 t)
{
    u64 f = mrs(CNTFRQ_EL0);
    return f ? t * 1000000 / f : 0;
}

/* ---- the device: aura's nvme.c, one command at a time under the controller lock ---------- */

static void timed(u64 start, bool flush)
{
    u64 took = mrs(CNTPCT_EL0) - start;
    if (took > longest_ticks)
        longest_ticks = took;
    if (flush && took > longest_flush_ticks)
        longest_flush_ticks = took;
}

static bool dev_read(void *opaque, u64 block, u32 count, void *buf)
{
    UNUSED(opaque);
    if (!nvme_lock_acquire(LOCK_WAIT_US))
        return false;
    u64 start = mrs(CNTPCT_EL0);
    act_begin('R', block, count, start);
    bool ok = !closing && nvme_read_blocks(NSID, block, buf, count);
    act_end(ok);
    timed(start, false);
    nvme_lock_release();
    return ok;
}

static bool dev_write(void *opaque, u64 block, u32 count, const void *buf)
{
    UNUSED(opaque);
    if (!nvme_lock_acquire(LOCK_WAIT_US))
        return false;
    u64 start = mrs(CNTPCT_EL0);
    act_begin('W', block, count, start);
    bool ok = !closing && nvme_write_blocks(NSID, block, buf, count);
    act_end(ok);
    timed(start, false);
    nvme_lock_release();
    return ok;
}

static bool dev_flush(void *opaque)
{
    UNUSED(opaque);
    if (!nvme_lock_acquire(LOCK_WAIT_US))
        return false;
    u64 start = mrs(CNTPCT_EL0);
    act_begin('F', 0, 0, start);
    bool ok = !closing && nvme_flush(NSID);
    act_end(ok);
    timed(start, true);
    nvme_lock_release();
    return ok;
}

/* GPT reads at attach, before the fence: whole namespace, 4 KiB blocks. */
static bool gpt_read(void *opaque, u64 lba, u32 count, void *buf)
{
    UNUSED(opaque);
    return nvme_read_blocks(NSID, lba, buf, count);
}

static const char *const state_names[] = {"DETACHED", "ATTACHED", "RUNNING", "CLEAN", "FAILED"};

static const char *state_name(enum hv_ahci_nvme_state s)
{
    return (unsigned)s < ARRAY_SIZE(state_names) ? state_names[s] : "?";
}

/* ---- attach ------------------------------------------------------------------------------ */

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* Identify Namespace (NVMe base spec, Identify Namespace data structure): NSZE, FLBAS, LBAF. */
static bool identify(u64 *nsze, u32 *lba_bytes)
{
    u8 *id = pool + 2 * AHCI_NVME_WINDOW;
    memset(id, 0, AHCI_NVME_BLOCK);
    if (!nvme_identify_ns(NSID, id))
        return false;
    *nsze = (u64)le32(id) | (u64)le32(id + 4) << 32;
    u32 fmt = id[26] & 0xf;
    u32 lbads = (le32(id + 128 + 4 * fmt) >> 16) & 0xff;
    *lba_bytes = lbads >= 9 && lbads <= 16 ? 1u << lbads : 0;
    return true;
}

static bool outside_ram(u64 start, u64 end, bool have_ram, u64 ram_lo, u64 ram_hi)
{
    return !have_ram || end <= ram_lo || start >= ram_hi;
}

static void refuse_shutdown(void)
{
    nvme_clear_fence();
    nvme_request_normal_shutdown();
    if (!nvme_shutdown())
        printf("HV: T8152: AHCI NVMe: the controller did not shut down after the refusal "
               "(nothing was written; a reset is safe)\n");
}

int hv_ahci_nvme_attach(u64 req_addr, bool have_ram, u64 ram_lo, u64 ram_hi, u64 *sectors)
{
    const char *why = NULL;

    if (state == HV_AHCI_NVME_ATTACHED || state == HV_AHCI_NVME_RUNNING ||
        state == HV_AHCI_NVME_FAILED) {
        printf("HV: T8152: AHCI NVMe: attach refused: the backend is %s\n", state_name(state));
        return -1;
    }
    if (!req_addr || (req_addr & 7)) {
        printf("HV: T8152: AHCI NVMe: attach refused: bad request address 0x%lx\n", req_addr);
        return -1;
    }
    memcpy(&req, (const void *)req_addr, sizeof(req));
    if (memcmp(req.magic, HV_AHCI_NVME_REQ_MAGIC, 8) || req.version != HV_AHCI_NVME_REQ_VERSION ||
        (req.flags & ~HV_AHCI_NVME_FLAGS) ||
        ((req.flags & HV_AHCI_NVME_SCRATCH) && !(req.flags & HV_AHCI_NVME_GATE)) ||
        req.first_lba > req.last_lba || req.last_lba - req.first_lba >= (1UL << 52)) {
        printf("HV: T8152: AHCI NVMe: attach refused: malformed request (magic, version, flags "
               "0x%x, LBAs [0x%lx, 0x%lx])\n",
               req.flags, req.first_lba, req.last_lba);
        return -1;
    }
    if (!(req.flags & HV_AHCI_NVME_GATE) && !have_ram) {
        printf("HV: T8152: AHCI NVMe: attach refused: no guest RAM window yet\n");
        return -1;
    }

    /* The bounce pool: carrier heap, never guest RAM, denied to stage 2. */
    if (!pool)
        pool = memalign(0x4000, POOL_ALLOC);
    if (!pool) {
        printf("HV: T8152: AHCI NVMe: attach refused: no heap for the 0x%lx-byte bounce pool\n",
               POOL_ALLOC);
        return -1;
    }
    if (!outside_ram((u64)pool, (u64)pool + POOL_ALLOC, have_ram, ram_lo, ram_hi)) {
        printf("HV: T8152: AHCI NVMe: attach refused: the bounce pool is inside guest RAM\n");
        return -1;
    }
    if (have_ram && !pool_reserved) {
        if (hv_t8152_reserve_carrier((u64)pool, (u64)pool + POOL_ALLOC,
                                     "AHCI NVMe bounce pool (D5)") < 0)
            return -1;
        pool_reserved = true;
    }

    if (!nvme_lock_acquire(LOCK_WAIT_US)) {
        printf("HV: T8152: AHCI NVMe: attach refused: CPU %d still holds the controller lock "
               "(a stuck command from an earlier session); reload the carrier\n",
               nvme_lock_owner());
        return -1;
    }
    closed_without_lock = false; /* the lock was free: no CPU is left inside a command */
    closing = false;
    if (!nvme_init()) {
        nvme_lock_release();
        printf("HV: T8152: AHCI NVMe: attach refused: nvme_init failed (the lines above say "
               "why)%s\n",
               nvme_needs_reboot() ? "; the controller needs a SoC reset (nothing was written)"
                                   : "");
        return -1;
    }
    ever_attached = true;
    u64 ranges[8][2];
    unsigned int n = 0;
    u64 nsze = 0;
    u32 lba_bytes = 0;
    struct gpt_want want;
    struct gpt_found found;
    bool at_end = false;

    if (!nvme_prepare_io()) {
        why = "cannot allocate the PRP list";
        goto refuse;
    }
    n = nvme_dma_ranges(ranges, ARRAY_SIZE(ranges));
    for (unsigned int i = 0; i < n; i++)
        if (!outside_ram(ranges[i][0], ranges[i][1], have_ram, ram_lo, ram_hi)) {
            why = "an NVMe queue or PRP list lies in guest RAM";
            goto refuse;
        }
    if (!identify(&nsze, &lba_bytes)) {
        why = "Identify Namespace failed";
        goto refuse;
    }
    if (lba_bytes != AHCI_NVME_BLOCK || nsze < 6) {
        printf("HV: T8152: AHCI NVMe: namespace 1: %lu blocks of %u bytes\n", nsze, lba_bytes);
        why = "namespace 1 does not have 4096-byte LBAs";
        goto refuse;
    }

    memcpy(want.disk_guid, req.disk_guid, 16);
    memcpy(want.part_guid, req.part_guid, 16);
    memcpy(want.type_guid, req.type_guid, 16);
    memcpy(want.name, req.name, sizeof(want.name));
    want.first = req.first_lba;
    want.last = req.last_lba;
    if (gpt_check_outer(gpt_read, NULL, nsze, AHCI_NVME_BLOCK, pool, 2 * AHCI_NVME_WINDOW, &want,
                        &found, &why) < 0)
        goto refuse;
    if (found.first != req.first_lba || found.last != req.last_lba) {
        why = "the GPT entry's LBAs changed under the check";
        goto refuse;
    }
    found_first = found.first;
    found_last = found.last;

    /* From here no read or write can leave the partition (guard 2). */
    nvme_set_fence(NSID, found_first, found_last);
    u64 part_bytes = (found_last - found_first + 1) * AHCI_NVME_BLOCK;
    u8 *first4k = pool + 2 * AHCI_NVME_WINDOW;
    if (!nvme_read_blocks(NSID, found_first, first4k, 1)) {
        why = "cannot read the partition's first block";
        goto refuse;
    }
    inner_bytes = 0;
    if (req.flags & HV_AHCI_NVME_SCRATCH) {
        /*
         * A scratch attach lets the W0 test write anywhere in the partition, so the partition
         * must really be empty: its first block all zero (no protective MBR, GPT header, or
         * file system boot sector).  An installed image is never a scratch partition.
         */
        for (u32 i = 0; i < AHCI_NVME_BLOCK; i++)
            if (first4k[i]) {
                why = "scratch attach on a partition that is not empty (its first 4 KiB are not "
                      "zero: an installed image?); use the free-tail W0 instead";
                goto refuse;
            }
    } else {
        if (gpt_check_inner(first4k, part_bytes, &at_end, &inner_bytes, &why) < 0)
            goto refuse;
        if (req.min_bytes && inner_bytes < req.min_bytes) {
            why = "the inner disk is smaller than the image the launcher expects";
            goto refuse;
        }
        if (!at_end)
            printf("HV: T8152: AHCI NVMe: warning: the inner GPT backup header is not at the "
                   "partition's end (sgdisk -e on a 512-byte loop device moves it; Windows "
                   "repairs it on its first GPT write)\n");
    }

    if ((req.flags & HV_AHCI_NVME_RW) && !nvme_multi_io_verified())
        printf("HV: T8152: AHCI NVMe: multi-block reads not verified on this ANS: every NVMe "
               "write is issued one 4 KiB block per command\n");

    struct ahci_nvme_dev dev = {dev_read, dev_write, dev_flush, NULL};
    ahci_nvme_init(&core, &dev, pool, found_first, found_last, !(req.flags & HV_AHCI_NVME_RW),
                   req.flags & HV_AHCI_NVME_FLUSH_EVERY);
    longest_ticks = longest_flush_ticks = 0;
    reported = 0;
    force_reset = false;
    nvme_poll_hook = hv_wdt_pet;
    state = HV_AHCI_NVME_ATTACHED;
    nvme_lock_release();
    *sectors = core.sectors;
    printf("HV: T8152: AHCI NVMe: attached %s%s%s: GPT entry %u of %u (by GUID, name, type and "
           "LBAs, never by number), NVMe LBAs [%lu, %lu] = %lu MiB, %lu AHCI sectors (512e), "
           "inner disk %lu MiB; fence set, pool [0x%lx, 0x%lx)\n",
           (req.flags & HV_AHCI_NVME_RW) ? "read-write" : "READ-ONLY",
           (req.flags & HV_AHCI_NVME_GATE) ? " (gate test, no guest)" : "",
           (req.flags & HV_AHCI_NVME_FLUSH_EVERY) ? ", flush after every write" : "", found.index,
           found.entries, found_first, found_last, part_bytes >> 20, core.sectors,
           inner_bytes >> 20, (u64)pool, (u64)pool + POOL_ALLOC);
    return 0;

refuse:
    printf("HV: T8152: AHCI NVMe: attach refused: %s\n", why);
    refuse_shutdown();
    nvme_lock_release();
    return -1;
}

struct ahci_nvme *hv_ahci_nvme_backend(void)
{
    return state == HV_AHCI_NVME_DETACHED ? NULL : &core;
}

bool hv_ahci_nvme_attached(void)
{
    return state == HV_AHCI_NVME_ATTACHED || state == HV_AHCI_NVME_RUNNING;
}

bool hv_ahci_nvme_gate(void)
{
    return hv_ahci_nvme_attached() && (req.flags & HV_AHCI_NVME_GATE);
}

bool hv_ahci_nvme_read_only(void)
{
    return !(req.flags & HV_AHCI_NVME_RW);
}

enum hv_ahci_nvme_state hv_ahci_nvme_state(void)
{
    return state;
}

int hv_ahci_nvme_verify(void)
{
    if (state != HV_AHCI_NVME_ATTACHED || (req.flags & HV_AHCI_NVME_GATE)) {
        printf("HV: T8152: AHCI NVMe: backend is %s%s; refusing guest entry\n", state_name(state),
               (req.flags & HV_AHCI_NVME_GATE) ? " (gate attach)" : "");
        return -1;
    }
    if (!nvme_fence_active() || !nvme_session_live() || core.failed || !pool_reserved) {
        printf("HV: T8152: AHCI NVMe: fence, controller session or pool reservation missing; "
               "refusing guest entry\n");
        return -1;
    }
    return 0;
}

void hv_ahci_nvme_guest_entered(void)
{
    if (state == HV_AHCI_NVME_ATTACHED)
        state = HV_AHCI_NVME_RUNNING;
}

/* ---- close: the controller shutdown on every exit ---------------------------------------- */

/* With the lock held: flush, then the normal shutdown; the direct CC.SHN if that fails. */
static bool shutdown_locked(void)
{
    bool flushed = true;
    if (core.wrote && nvme_session_live())
        flushed = nvme_flush(NSID);
    nvme_request_normal_shutdown();
    bool down = nvme_shutdown();
    if (!down) {
        printf("HV: T8152: AHCI NVMe: normal shutdown path failed; requesting CC.SHN directly\n");
        down = nvme_shutdown_for_reset(true); /* this CPU holds the lock: it may use RTKit */
    }
    if (!flushed)
        printf("HV: T8152: AHCI NVMe: the final flush failed (the shutdown commits the cache)\n");
    return down;
}

static void finish(bool down, const char *why)
{
    nvme_clear_fence();
    nvme_poll_hook = NULL;
    bool safe = down || !core.wrote;
    state = safe ? HV_AHCI_NVME_CLEAN : HV_AHCI_NVME_FAILED;
    printf("HV: T8152: AHCI NVMe: %s: %s (%s; controller shutdown %s)\n", why, state_name(state),
           core.wrote ? "this session wrote" : "nothing written",
           down ? "confirmed" : "NOT confirmed");
    if (state == HV_AHCI_NVME_FAILED)
        printf("HV: T8152: AHCI NVMe: FAILED: do not hard reset; retry DETACH from the proxy. "
               "A reset now may lose writes Windows was told were flushed: afterwards restore "
               "the partition from the image or run chkdsk (WinPE, read-only first)\n");
}

/*
 * Get the controller for a close: stop the owner's further I/O, wait one command timeout plus
 * margin.  False if a CPU is stuck holding it.
 */
static bool close_lock(void)
{
    nvme_abort_io(true);
    if (!nvme_lock_acquire(RESET_LOCK_WAIT_US))
        return false; /* abort stays set: the stuck CPU must not start another command */
    nvme_abort_io(false);
    return true;
}

static bool shutdown_without_lock(void)
{
    printf("HV: T8152: AHCI NVMe: CPU %d did not release the controller in %u s; requesting "
           "CC.SHN directly (CSTS only; its RTKit mailbox is left alone)\n",
           nvme_lock_owner(), RESET_LOCK_WAIT_US / 1000000);
    closed_without_lock = true;
    return nvme_shutdown_for_reset(false);
}

enum hv_ahci_nvme_state hv_ahci_nvme_close(const char *why)
{
    if (state != HV_AHCI_NVME_ATTACHED && state != HV_AHCI_NVME_RUNNING &&
        state != HV_AHCI_NVME_FAILED)
        return state;
    closing = true;
    core.closing = true;
    hv_ahci_nvme_report(true);
    bool down;
    if (close_lock()) {
        down = shutdown_locked();
        nvme_lock_release();
    } else {
        down = shutdown_without_lock();
    }
    finish(down, why);
    return state;
}

bool hv_ahci_nvme_reset_close(void)
{
    if (!ever_attached) {
        /*
         * No backend session ever started (an attach whose nvme_init failed included).  The
         * carrier wrote nothing to NVMe then, so a reset is safe even when the controller cannot
         * be shut down (nvme_init's out_preserve after a failed second ANS wake).
         */
        if (nvme_shutdown())
            return true;
        if (nvme_has_written())
            return false;
        printf("HV: T8152: AHCI NVMe: no NVMe shutdown possible, but this carrier wrote nothing "
               "to NVMe: resetting\n");
        return true;
    }
    if (state == HV_AHCI_NVME_CLEAN || state == HV_AHCI_NVME_DETACHED)
        return nvme_shutdown() || !nvme_session_live();
    __atomic_store_n(&closing, true, __ATOMIC_RELEASE);
    core.closing = true;
    bool down;
    if (nvme_lock_owner() == smp_id()) {
        /* A fault inside nvme.c on this CPU: its state is unknown, do not reuse the queues. */
        printf("HV: T8152: AHCI NVMe: reset from inside the controller lock\n");
        down = nvme_shutdown_for_reset(true);
    } else if (close_lock()) {
        down = shutdown_locked();
        nvme_lock_release();
    } else {
        down = shutdown_without_lock();
    }
    finish(down, "reset");
    if (state == HV_AHCI_NVME_FAILED && force_reset) {
        printf("HV: T8152: AHCI NVMe: FORCE: resetting although the controller did NOT confirm "
               "its shutdown. Writes of this session may be lost or reordered: restore the "
               "Windows partition from the image, or check it (chkdsk from WinPE), before the "
               "next read-write session\n");
        return true;
    }
    return state == HV_AHCI_NVME_CLEAN;
}

void hv_ahci_nvme_force_reset(void)
{
    if (state == HV_AHCI_NVME_FAILED)
        force_reset = true;
}

bool hv_ahci_nvme_owns_controller(void)
{
    if (state == HV_AHCI_NVME_ATTACHED || state == HV_AHCI_NVME_RUNNING ||
        state == HV_AHCI_NVME_FAILED)
        return true;
    /* After a close without the lock, until the stuck CPU lets go of the queues. */
    int owner = nvme_lock_owner();
    return closed_without_lock && owner >= 0 && owner != smp_id();
}

bool hv_ahci_nvme_reboot_allowed(bool force)
{
    if (state == HV_AHCI_NVME_RUNNING) {
        printf("HV: T8152: AHCI NVMe: reboot refused: a guest session owns the disk (exit the "
               "guest first: its exit closes the disk)\n");
        return false;
    }
    if (state == HV_AHCI_NVME_FAILED && !force) {
        printf("HV: T8152: AHCI NVMe: reboot refused: the last close was not confirmed "
               "(FAILED); retry DETACH, or P_REBOOT \"FORCE\" (one more shutdown attempt, then "
               "a reset even without it: the partition then needs a restore or chkdsk)\n");
        return false;
    }
    return true;
}

/* ---- status, test, reports --------------------------------------------------------------- */

u64 hv_ahci_nvme_status(u64 index)
{
    const struct ahci_nvme_stats *s = &core.stats;
    switch (index) {
        case HV_AHCI_NVME_S_STATE:
            return state;
        case HV_AHCI_NVME_S_FLAGS:
            return req.flags;
        case HV_AHCI_NVME_S_COMMANDS:
            return s->commands;
        case HV_AHCI_NVME_S_DEV_COMMANDS:
            return s->dev_reads + s->dev_writes + s->dev_flushes;
        case HV_AHCI_NVME_S_LONGEST_US:
            return ticks_to_us(longest_ticks);
        case HV_AHCI_NVME_S_FLUSHES:
            return s->dev_flushes;
        case HV_AHCI_NVME_S_RMW:
            return s->rmw;
        case HV_AHCI_NVME_S_GUARD_HITS:
            return s->guard_hits + nvme_fence_refusals();
        case HV_AHCI_NVME_S_FIRST:
            return found_first;
        case HV_AHCI_NVME_S_LAST:
            return found_last;
        case HV_AHCI_NVME_S_WROTE:
            return core.wrote;
        case HV_AHCI_NVME_S_FAILED_COMMANDS:
            return s->failed;
        case HV_AHCI_NVME_S_BLOCKS_READ:
            return s->blocks_read;
        case HV_AHCI_NVME_S_BLOCKS_WRITTEN:
            return s->blocks_written;
        case HV_AHCI_NVME_S_INNER_BYTES:
            return inner_bytes;
        case HV_AHCI_NVME_S_LONGEST_FLUSH_US:
            return ticks_to_us(longest_flush_ticks);
        default:
            return (u64)-1;
    }
}

/* splitmix64 of (seed, block, word): the launcher's nvme_test_pattern() is the same. */
u64 hv_ahci_nvme_pattern(u64 seed, u64 rel_block, u32 word)
{
    u64 z = seed ^ (rel_block * 0x9e3779b97f4a7c15UL) ^ ((u64)word << 32 | word);
    z += 0x9e3779b97f4a7c15UL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
    return z ^ (z >> 31);
}

u64 hv_ahci_nvme_test(u64 rel_block, u64 count, u64 seed, u64 op)
{
    const u64 err = (u64)-1;
    if (state != HV_AHCI_NVME_ATTACHED || !(req.flags & HV_AHCI_NVME_GATE)) {
        printf("HV: T8152: AHCI NVMe: test refused: only on a gate attach, before any guest\n");
        return err;
    }
    u64 blocks = found_last - found_first + 1;
    if (op != HV_AHCI_NVME_TEST_FLUSH && (!count || count > AHCI_NVME_MAX_BLOCKS ||
                                          rel_block >= blocks || count > blocks - rel_block)) {
        printf("HV: T8152: AHCI NVMe: test refused: blocks [%lu, +%lu) outside the partition's "
               "%lu or more than 64\n",
               rel_block, count, blocks);
        return err;
    }
    /*
     * Test writes never touch a disk image: on a scratch attach (an empty partition) anywhere,
     * otherwise only past the inner disk's end (its backup GPT header), the tail that is free
     * until the backup header is moved to the partition's end or Windows extends into it.
     */
    /* Rounded up: a 4 KiB block holding any byte of the inner disk is not tail. */
    u64 first_free = (inner_bytes + AHCI_NVME_BLOCK - 1) / AHCI_NVME_BLOCK;
    bool tail = inner_bytes && rel_block >= first_free;
    if (op == HV_AHCI_NVME_TEST_WRITE &&
        (!(req.flags & HV_AHCI_NVME_RW) || (!(req.flags & HV_AHCI_NVME_SCRATCH) && !tail))) {
        printf("HV: T8152: AHCI NVMe: test write refused: only read-write, and only on a scratch "
               "attach or past the inner disk's end (block %lu)\n",
               first_free);
        return err;
    }
    u64 *w = (u64 *)(void *)(pool + AHCI_NVME_WINDOW);
    u64 words = count * (AHCI_NVME_BLOCK / 8);
    u64 result = 0;
    bool ok;
    switch (op) {
        case HV_AHCI_NVME_TEST_WRITE:
            for (u64 i = 0; i < words; i++)
                w[i] = hv_ahci_nvme_pattern(seed, rel_block + i / 512, (u32)(i % 512));
            core.wrote = true;
            ok = dev_write(NULL, found_first + rel_block, (u32)count, w);
            break;
        case HV_AHCI_NVME_TEST_VERIFY:
            ok = dev_read(NULL, found_first + rel_block, (u32)count, w);
            for (u64 i = 0; ok && i < words; i++)
                if (w[i] != hv_ahci_nvme_pattern(seed, rel_block + i / 512, (u32)(i % 512)))
                    result++;
            break;
        case HV_AHCI_NVME_TEST_FLUSH:
            ok = dev_flush(NULL);
            break;
        case HV_AHCI_NVME_TEST_CRC:
            ok = dev_read(NULL, found_first + rel_block, (u32)count, w);
            if (ok)
                result = ahci_crc32(0, w, count * AHCI_NVME_BLOCK);
            break;
        default:
            ok = false;
    }
    if (!ok) {
        printf("HV: T8152: AHCI NVMe: test op %lu at block %lu (+%lu) failed\n", op, rel_block,
               count);
        return err;
    }
    return result;
}

void hv_ahci_nvme_report(bool final)
{
    if (state == HV_AHCI_NVME_DETACHED)
        return;
    const struct ahci_nvme_stats *s = &core.stats;
    u64 total = s->commands + s->dev_flushes;
    if (!final && total == reported)
        return;
    reported = total;
    printf("HV: T8152: AHCI NVMe: %lu commands (%lu failed, %lu refused); NVMe %lu reads, %lu "
           "writes, %lu flushes; %lu MiB read, %lu MiB written; %lu RMW blocks; longest NVMe "
           "command %lu us, longest flush %lu us; guard hits %lu\n",
           s->commands, s->failed, s->refused, s->dev_reads, s->dev_writes, s->dev_flushes,
           s->blocks_read >> 8, s->blocks_written >> 8, s->rmw, ticks_to_us(longest_ticks),
           ticks_to_us(longest_flush_ticks), s->guard_hits + nvme_fence_refusals());
}

static u64 fatal_age_us(u64 now, u64 then, u64 freq)
{
    if (!then || then > now || !freq)
        return 0;
    return (now - then) * 1000000 / freq;
}

void hv_ahci_nvme_fatal_report(u64 now, u64 freq)
{
    if (state == HV_AHCI_NVME_DETACHED)
        return;
    printf("HV: fatal: AHCI NVMe backend %s, partition LBAs [%lu, %lu], %s, wrote %d, "
           "controller lock owner %d\n",
           state_name(state), found_first, found_last,
           (req.flags & HV_AHCI_NVME_RW) ? "read-write" : "read-only", core.wrote,
           nvme_lock_owner());
    const struct ahci_nvme_stats *s = &core.stats;
    printf("HV: fatal: AHCI NVMe activity: %lu AHCI commands (%lu failed, %lu refused), %lu "
           "NVMe commands; closing %d, fence %s, session %s\n",
           s->commands, s->failed, s->refused, act.n, (int)closing,
           nvme_fence_active() ? "set" : "clear", nvme_session_live() ? "live" : "down");
    typeof(act) a;
    u64 seq = hv_fatal_read(&act.seq, &act, &a, sizeof(a));
    if (!a.n) {
        printf("HV: fatal: AHCI NVMe last command: none issued by the backend\n");
        return;
    }
    const char *op = a.op == 'R' ? "read" : a.op == 'W' ? "write" : "flush";
    printf("HV: fatal: AHCI NVMe last command #%lu: %s", a.n, op);
    if (a.op != 'F')
        printf(" block %lu (partition +%lu), %lu block(s)", a.block,
               a.block >= found_first ? a.block - found_first : 0, a.count);
    printf(" on cpu %lu, issued %lu us ago, ", a.cpu, fatal_age_us(now, a.start, freq));
    if (seq == ~0UL || (seq & 1))
        printf("record being rewritten (a command is starting or ending)\n");
    else if (!a.done)
        printf("IN FLIGHT (no completion yet)\n");
    else
        printf("completed in %lu us, %s\n", fatal_age_us(a.done, a.start, freq),
               a.ok ? "ok" : "FAILED");
}
