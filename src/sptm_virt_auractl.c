/* SPDX-License-Identifier: MIT */

/*
 * auractl: a synthetic in-guest MMIO device for tetherless tracing control (SPTM_VIRT_AURACTL).
 *
 * aura exposes one 16 KiB page at a reserved guest IPA (AURACTL_MMIO_BASE), installed with
 * hv_map_hook/SPTE_HOOK, so every guest access to it traps to the hypervisor. The page is NOT
 * backed by RAM or any Apple block; the control hook implements the register file in
 * sptm_virt_auractl_abi.h from aura's own state. A tool running inside the guest arms MMIO trace
 * ranges, starts/stops recording and dumps the trace through this register file -- with no debug
 * cable and without taking a USB port from macOS.
 *
 * ARM'd ranges share one generic trace hook (auractl_trace_hook): each performs the guest's own
 * access for real with sptm_virt_mmio_rw() and, while RUNNING, appends a record to a ring in
 * aura's carve-out. DUMP latches the ring and the guest pops it through FIFO_DATA (4 x u64 per
 * record, oldest-first). The HV never writes guest memory from EL2, so no core ever has to park
 * and there is no cache-coherence or DMA question. Development builds only, like the rest of
 * SPTM_VIRT.
 *
 * The register decode, the ARM-range validation and the ring are a pure state machine
 * (auractl_decode / auractl_record) with no HV/EL2 dependency, so they host-unit-test exactly like
 * sptm_virt_compute_layout. The host test compiles this file with AURACTL_HOST defined, which
 * drops the HV glue at the bottom.
 */

#include "sptm_virt_auractl_abi.h"

#ifndef AURACTL_HOST
#include "exception.h"
#include "hv.h"
#include "malloc.h"
#include "sptm_virt.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#endif

/*
 * Caps. One range at a time is armed by the tool in practice, but the device holds up to
 * AURACTL_RANGES_MAX so a trace can span a few register windows; the total hooked size is capped
 * so a mistyped size cannot ask the HV to split half the aperture into L3 pages.
 */
#define AURACTL_RANGES_MAX  8
#define AURACTL_RANGE_ALIGN 0x4000UL    /* 16 KiB: the page size the HV hooks at */
#define AURACTL_SIZE_CAP    (4UL << 20) /* 4 MiB total across all armed ranges */

/*
 * The guest device-MMIO aperture ARM accepts a range inside. On Apple SoCs the SoC register blocks
 * (PMGR, the GPU/ASC windows, the USB controllers the operator wants to trace) sit in a low MMIO
 * aperture, below DRAM (which is based high, at 32 GiB or above); aura's own carve-out lives in
 * DRAM. So a "below DRAM, above the first SoC block" window check rejects both RAM and the carve-
 * out. The bounds are deliberately wide -- auractl only observes the guest's own accesses, never
 * injects a write, so the guard rail is "a device address, not RAM", not an exact allow-list. The
 * exact per-SoC bounds are confirmed against the live guest ADT before the hardware proof (the
 * tool reads the USB controller's reg from the guest IORegistry and arms that).
 */
#define AURACTL_DEV_WINDOW_BASE 0x200000000UL /* 8 GiB: at/above the first SoC register block */
#define AURACTL_DEV_WINDOW_END  0x800000000UL /* 32 GiB: DRAM base; the aperture ends below it */

/* The control page's own extent, excluded from ARM so a range cannot trace the controller. */
#define AURACTL_SELF_BASE AURACTL_MMIO_BASE
#define AURACTL_SELF_END  (AURACTL_MMIO_BASE + AURACTL_PAGE_SIZE)

struct auractl_range {
    u64 base; /* guest PA, 16 KiB aligned */
    u64 size; /* bytes, 16 KiB multiple */
};

struct auractl_rec {
    u64 t;    /* CNTPCT_EL0 at the access */
    u64 meta; /* pa[47:0] | width[50:48] | write[51] | cpu[55:52], see AURACTL_META */
    u64 val;  /* value written, or read */
    u64 pc;   /* guest PC (ELR) at the access */
};

/*
 * The whole device's state. The ring is allocated separately (by the HV glue, from aura's
 * carve-out; by the test, on its own stack/heap) so the pure decoder never allocates.
 */
struct auractl_dev {
    u32 state; /* AURACTL_STATE_* */
    u32 err;   /* AURACTL_ERR_* from the last command */

    struct auractl_range ranges[AURACTL_RANGES_MAX];
    u32 nranges;
    u64 armed_bytes; /* sum of ranges[].size, checked against AURACTL_SIZE_CAP */

    struct auractl_rec *ring; /* caller-owned; nrec_cap entries */
    u64 nrec_cap;             /* ring capacity in records */
    u64 head;                 /* total records appended (saturating counter == NREC) */
    u64 dropped;              /* records overwritten by wrap before a DUMP read them */

    u64 arg0, arg1; /* ARG0 / ARG1 */
    u32 range_idx;  /* RANGE_IDX */

    /* Dump cursor, latched by CMD_DUMP: FIFO_DATA pops [dump_lo, dump_hi) oldest-first. */
    bool dumping;
    u64 dump_lo;   /* next record index to pop (absolute, head-relative) */
    u64 dump_hi;   /* one past the last record to pop */
    u32 dump_word; /* which of the 4 u64s of the current record pops next */

    /*
     * A new range the decoder accepted on an ARM, for the HV glue to hv_map_hook. The pure decoder
     * records the range but does not touch the page tables; the caller installs the hook.
     */
    bool pending_hook;
    struct auractl_range pending_range;
};

/* The one caps word this build reports. */
static u32 auractl_caps(void)
{
    return (AURACTL_RANGES_MAX & AURACTL_CAPS_RANGES_MASK) | AURACTL_CAPS_FIFO |
           ((u32)AURACTL_REC_BYTES << AURACTL_CAPS_RECBYTES_SH);
}

static u32 auractl_status(const struct auractl_dev *d)
{
    return (d->state & AURACTL_STATE_MASK) |
           (((d->err & 0xffu) << AURACTL_ERR_SH) & AURACTL_ERR_MASK);
}

/* saturating u64 increment */
static u64 auractl_sat_inc(u64 v)
{
    return v == ~0UL ? v : v + 1;
}

/*
 * Pure ring append. Used by the real trace hook and by the host test. head is the saturating
 * record count (NREC). When the ring wraps and overwrites a record a pending DUMP has not yet read,
 * DROPPED increments.
 */
void auractl_record(struct auractl_dev *d, u64 t, u64 meta, u64 val, u64 pc)
{
    if (!d->ring || !d->nrec_cap)
        return;

    u64 slot = d->head % d->nrec_cap;
    struct auractl_rec *r = &d->ring[slot];
    r->t = t;
    r->meta = meta;
    r->val = val;
    r->pc = pc;

    /*
     * If a DUMP is in flight and this write overwrites a record still waiting to be popped, that
     * record is lost. Advance the dump floor past it and count the loss.
     */
    if (d->dumping && d->head >= d->nrec_cap) {
        u64 oldest_live = d->head - d->nrec_cap + 1; /* oldest index still in the ring after this */
        if (d->dump_lo < oldest_live) {
            d->dropped = auractl_sat_inc(d->dropped);
            d->dump_lo = oldest_live;
            d->dump_word = 0;
            if (d->dump_hi < d->dump_lo)
                d->dump_hi = d->dump_lo;
        }
    }

    d->head = auractl_sat_inc(d->head);
}

/* True if [b, b+s) overlaps an already-armed range. */
static bool auractl_overlaps(const struct auractl_dev *d, u64 b, u64 s)
{
    for (u32 i = 0; i < d->nranges; i++) {
        u64 rb = d->ranges[i].base, re = rb + d->ranges[i].size;
        if (b < re && rb < b + s)
            return true;
    }
    return false;
}

/*
 * Validate and record an ARM range from ARG0 (base) / ARG1 (size). Sets d->err and, on success,
 * records the range and leaves d->pending_hook set for the HV glue to install. Returns true on
 * success. State is left unchanged by the caller on any error.
 */
static bool auractl_arm(struct auractl_dev *d)
{
    u64 base = d->arg0, size = d->arg1;

    if (!size || (base & (AURACTL_RANGE_ALIGN - 1)) || (size & (AURACTL_RANGE_ALIGN - 1))) {
        d->err = AURACTL_ERR_BADRANGE;
        return false;
    }
    /* No wrap, and wholly inside the device-MMIO aperture (so: not RAM, not the carve-out). */
    if (base < AURACTL_DEV_WINDOW_BASE || base + size > AURACTL_DEV_WINDOW_END ||
        base + size < base) {
        d->err = AURACTL_ERR_BADRANGE;
        return false;
    }
    /* Never trace the controller's own page. */
    if (base < AURACTL_SELF_END && AURACTL_SELF_BASE < base + size) {
        d->err = AURACTL_ERR_BADRANGE;
        return false;
    }
    if (d->nranges >= AURACTL_RANGES_MAX || d->armed_bytes + size > AURACTL_SIZE_CAP) {
        d->err = AURACTL_ERR_FULL;
        return false;
    }
    if (auractl_overlaps(d, base, size)) {
        d->err = AURACTL_ERR_OVERLAP;
        return false;
    }

    d->ranges[d->nranges].base = base;
    d->ranges[d->nranges].size = size;
    d->nranges++;
    d->armed_bytes += size;
    d->pending_range.base = base;
    d->pending_range.size = size;
    d->pending_hook = true;
    d->err = AURACTL_ERR_NONE;
    return true;
}

/* RESET / DISARM: clear ranges, ring and counters. DISARM is only the range teardown + -> IDLE. */
static void auractl_clear_ranges(struct auractl_dev *d)
{
    d->nranges = 0;
    d->armed_bytes = 0;
}

static void auractl_clear_ring(struct auractl_dev *d)
{
    d->head = 0;
    d->dropped = 0;
    d->dumping = false;
    d->dump_lo = d->dump_hi = 0;
    d->dump_word = 0;
}

/* Run a CMD opcode against ARG0/ARG1, setting d->state and d->err. */
static void auractl_cmd(struct auractl_dev *d, u32 op)
{
    d->err = AURACTL_ERR_NONE;

    switch (op) {
        case AURACTL_CMD_RESET:
            auractl_clear_ranges(d);
            auractl_clear_ring(d);
            d->state = AURACTL_STATE_IDLE;
            break;

        case AURACTL_CMD_ARM:
            /* Arm while IDLE or already ARMED (add another window); never while RUNNING. */
            if (d->state != AURACTL_STATE_IDLE && d->state != AURACTL_STATE_ARMED) {
                d->err = AURACTL_ERR_BADSTATE;
                break;
            }
            if (!d->ring || !d->nrec_cap) {
                d->err = AURACTL_ERR_NOMEM;
                break;
            }
            if (auractl_arm(d))
                d->state = AURACTL_STATE_ARMED;
            break;

        case AURACTL_CMD_START:
            if (d->state != AURACTL_STATE_ARMED && d->state != AURACTL_STATE_STOPPED) {
                d->err = AURACTL_ERR_BADSTATE;
                break;
            }
            if (!d->nranges) {
                d->err = AURACTL_ERR_BADSTATE;
                break;
            }
            d->state = AURACTL_STATE_RUNNING;
            break;

        case AURACTL_CMD_STOP:
            if (d->state != AURACTL_STATE_RUNNING) {
                d->err = AURACTL_ERR_BADSTATE;
                break;
            }
            d->state = AURACTL_STATE_STOPPED;
            break;

        case AURACTL_CMD_DUMP:
            /* Latch the ring for readout. Only legal once recording has stopped. */
            if (d->state != AURACTL_STATE_STOPPED && d->state != AURACTL_STATE_ARMED) {
                d->err = AURACTL_ERR_BADSTATE;
                break;
            }
            d->dumping = true;
            d->dump_hi = d->head;
            d->dump_lo = d->head > d->nrec_cap ? d->head - d->nrec_cap : 0;
            d->dump_word = 0;
            break;

        case AURACTL_CMD_READBACK:
            if (d->range_idx >= d->nranges) {
                d->err = AURACTL_ERR_BADRANGE;
                break;
            }
            d->arg0 = d->ranges[d->range_idx].base;
            d->arg1 = d->ranges[d->range_idx].size;
            break;

        case AURACTL_CMD_DISARM:
            auractl_clear_ranges(d);
            d->state = AURACTL_STATE_IDLE;
            break;

        default:
            d->err = AURACTL_ERR_BADCMD;
            break;
    }
}

/* Pop one u64 of the dump stream. Returns 0 once the FIFO is drained. */
static u64 auractl_fifo_pop(struct auractl_dev *d)
{
    if (!d->dumping || d->dump_lo >= d->dump_hi || !d->ring || !d->nrec_cap)
        return 0;

    struct auractl_rec *r = &d->ring[d->dump_lo % d->nrec_cap];
    u64 out;
    switch (d->dump_word) {
        case 0:
            out = r->t;
            break;
        case 1:
            out = r->meta;
            break;
        case 2:
            out = r->val;
            break;
        default:
            out = r->pc;
            break;
    }
    if (++d->dump_word == 4) {
        d->dump_word = 0;
        d->dump_lo++;
    }
    return out;
}

/* u64 records still waiting to be popped (whole records, not words). */
static u64 auractl_fifo_avail(const struct auractl_dev *d)
{
    if (!d->dumping || d->dump_lo >= d->dump_hi)
        return 0;
    return d->dump_hi - d->dump_lo;
}

/*
 * The pure register handler. off is the byte offset from the page base; width is log2(bytes) as the
 * HV passes it (2 == 4 bytes, 3 == 8 bytes). On a read, *val is filled; on a write, *val is the
 * value the guest wrote. Unknown/misaligned offsets read as 0 and ignore writes.
 */
void auractl_decode(struct auractl_dev *d, u32 off, u64 *val, bool write, int width)
{
    if (!write)
        *val = 0;

    switch (off) {
        case AURACTL_REG_MAGIC:
            if (!write)
                *val = AURACTL_MAGIC;
            break;
        case AURACTL_REG_VERSION:
            if (!write)
                *val = AURACTL_VERSION;
            break;
        case AURACTL_REG_CAPS:
            if (!write)
                *val = auractl_caps();
            break;
        case AURACTL_REG_ARG0:
            if (write)
                d->arg0 = *val;
            else
                *val = d->arg0;
            break;
        case AURACTL_REG_ARG1:
            if (write)
                d->arg1 = *val;
            else
                *val = d->arg1;
            break;
        case AURACTL_REG_CMD:
            if (write)
                auractl_cmd(d, (u32)*val);
            break;
        case AURACTL_REG_STATUS:
            if (!write)
                *val = auractl_status(d);
            break;
        case AURACTL_REG_NREC:
            if (!write)
                *val = d->head;
            break;
        case AURACTL_REG_DROPPED:
            if (!write)
                *val = d->dropped;
            break;
        case AURACTL_REG_FIFO_DATA:
            if (!write)
                *val = auractl_fifo_pop(d);
            break;
        case AURACTL_REG_FIFO_AVAIL:
            if (!write)
                *val = auractl_fifo_avail(d);
            break;
        case AURACTL_REG_RANGE_IDX:
            if (write)
                d->range_idx = (u32)*val;
            else
                *val = d->range_idx;
            break;
        case AURACTL_REG_NRANGES:
            if (!write)
                *val = d->nranges;
            break;
        default:
            /* Unmapped register: reads 0, writes ignored. Keep (void) so -Wunused is quiet. */
            (void)width;
            break;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* HV glue: compiled only in the firmware, dropped from the host unit test. */
/* ---------------------------------------------------------------------------------------------- */

#ifndef AURACTL_HOST

#define AURACTL_RING_BYTES (16UL << 20) /* same 16 MiB ring budget as the GPU tracer */

static struct auractl_dev auractl;

/* Install the trace hook for a range the decoder just accepted. */
static bool auractl_trace_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

static void auractl_install_pending(void)
{
    if (!auractl.pending_hook)
        return;
    auractl.pending_hook = false;
    u64 base = auractl.pending_range.base, size = auractl.pending_range.size;
    if (hv_map_hook(base, auractl_trace_hook, size) < 0)
        printf("SPTM-virt[auractl]: cannot hook 0x%lx+0x%lx\n", base, size);
    else
        printf("SPTM-virt[auractl]: armed trace 0x%lx+0x%lx\n", base, size);
}

/*
 * The shared hook for every ARM'd range. Mirrors gpu_hook: perform the guest's own access for real
 * with sptm_virt_mmio_rw() (including the width==4 LDP/STP pair the ASC mailboxes use, and the
 * width>4 reject), then, while RUNNING, record it. No console print -- the trace path is tetherless
 * and must stay cheap.
 */
static bool auractl_trace_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    bool running = auractl.state == AURACTL_STATE_RUNNING;
    u64 now = running ? mrs(CNTPCT_EL0) : 0;
    u64 pc = ctx ? ctx->elr : 0;
    int cpu = smp_id();

    if (width == 4) {
        /* A trapped LDP/STP pair (a 128-bit access): two 64-bit halves. */
        if (write)
            __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
        else
            __asm__ volatile("ldp %0, %1, [%2]"
                             : "=r"(val[0]), "=r"(val[1])
                             : "r"(addr)
                             : "memory");
        if (running) {
            auractl_record(&auractl, now, AURACTL_META(addr, 3, write, cpu), val[0], pc);
            auractl_record(&auractl, now, AURACTL_META(addr + 8, 3, write, cpu), val[1], pc);
        }
    } else if (width > 4) {
        printf("SPTM-virt[auractl]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n",
               1u << width, addr, pc);
        return false;
    } else {
        sptm_virt_mmio_rw(addr, val, write, width);
        if (running)
            auractl_record(&auractl, now, AURACTL_META(addr, width, write, cpu), *val, pc);
    }
    return true;
}

/*
 * The control page's hook. Translate the trapped IPA to a register offset and run the pure decoder,
 * then install any range an ARM just accepted. A one-line print on each CMD write is fine (rare);
 * the trace path above stays silent.
 */
static bool auractl_ctl_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    (void)ctx;
    if (width > 3) {
        /* The register file is u32/u64 only; a wider access is a tool bug. */
        if (!write)
            val[0] = 0;
        return true;
    }

    u32 off = (u32)(addr - AURACTL_MMIO_BASE);
    auractl_decode(&auractl, off, val, write, width);

    if (write && off == AURACTL_REG_CMD) {
        auractl_install_pending();
        printf(
            "SPTM-virt[auractl]: cmd %lu -> state %u err %u (nranges %u, nrec %lu, dropped %lu)\n",
            *val, auractl.state, auractl.err, auractl.nranges, auractl.head, auractl.dropped);
    }
    return true;
}

void sptm_virt_auractl_setup(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_AURACTL))
        return;

    auractl.ring = memalign(0x4000, AURACTL_RING_BYTES);
    if (!auractl.ring) {
        printf("SPTM-virt[auractl]: no memory for the ring; auractl off\n");
        return;
    }
    memset(auractl.ring, 0, AURACTL_RING_BYTES);
    auractl.nrec_cap = AURACTL_RING_BYTES / sizeof(struct auractl_rec);
    auractl.state = AURACTL_STATE_IDLE;

    if (hv_map_hook(AURACTL_MMIO_BASE, auractl_ctl_hook, AURACTL_PAGE_SIZE) < 0) {
        printf("SPTM-virt[auractl]: cannot map the control page at 0x%lx\n", AURACTL_MMIO_BASE);
        auractl.ring = NULL;
        return;
    }
    printf("SPTM-virt[auractl]: control page at 0x%lx, ring 0x%lx (%lu records), up to %d ranges\n",
           AURACTL_MMIO_BASE, (u64)auractl.ring, auractl.nrec_cap, AURACTL_RANGES_MAX);
}

#endif /* !AURACTL_HOST */
