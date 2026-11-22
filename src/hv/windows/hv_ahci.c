/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * D1 carrier glue for the AHCI disk model.  See hv_ahci.h for the contract and
 * windows-m6/D1-AHCI-DESIGN.md for the reasoning.  Written on this branch; no third-party
 * source.  The device model itself (ahci_model.c) is pure and host-tested.
 */

#include "hv_ahci.h"
#include "ahci_model.h"
#include "ahci_nvme.h"
#include "hv.h"
#include "hv_ahci_nvme.h"
#include "hv_fatal.h"
#include "hv_t8152.h"
#include "hv_vdev_spi.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

/* One-shot log lines. */
enum {
    LOG_WIDTH = BIT(0),
    LOG_EVENT = BIT(1),
    LOG_SPAN = BIT(2),
};

static bool ahci_enabled;
static bool attached;
/* D5: the disk is an internal NVMe partition (hv_ahci_nvme.c), not the DRAM store. */
static bool nvme_mode;
static bool entered;
static bool exited;
static int exit_status;
static u64 ram_lo, ram_hi;
static u64 store_base, disk_bytes, window_top;
static struct hv_ahci_store_desc *desc;
static struct ahci_model model;
static struct ahci_env env;
static DECLARE_SPINLOCK(ahci_lock);
static u32 logged;
static u32 events_logged;
/* Report counters: the longest single register access (one command batch) under the lock. */
static u64 longest_ticks;
static u64 report_next;
static u64 reported_commands;

/*
 * What the fatal report prints (hv_fatal.h): the access being handled now (or last), and the
 * last access that ran a command -- its CPU, the command FIS, the disk range and the last
 * TRACE_SPANS guest spans the model checked (command list, command table, PRDT entries, data
 * buffers).  Written under ahci_lock, seqlocked for the reporting CPU, which takes no lock.
 */
#define TRACE_SPANS 8
struct ahci_trace {
    u64 seq;
    u64 cpu, cntpct, off, val, write, width;
    u64 commands, slot;
    u8 fis[20];
    u64 io_calls, io_lba, io_sectors, io_write;
    u64 spans;
    struct {
        u64 ipa, len, write, ok;
    } span[TRACE_SPANS];
};
static struct ahci_trace trace_access, trace_command;

_Static_assert(sizeof(struct hv_ahci_store_desc) <= HV_AHCI_DESC_SIZE, "descriptor size");
_Static_assert(HV_AHCI_INTID == HV_VDEV_SPI_AHCI, "one SPI number");
_Static_assert(HV_AHCI_ABAR_SIZE == AHCI_MODEL_ABAR_SIZE, "one ABAR size");

void hv_ahci_enable(void)
{
    ahci_enabled = true;
}

bool hv_ahci_enabled(void)
{
    return ahci_enabled;
}

/* ---- the model's environment ------------------------------------------------------------- */

/*
 * Guest RAM is identity mapped (IPA = PA, hv_t8152.c) and EL2 maps all DRAM as normal
 * cacheable memory, so after the range check the IPA is the pointer.  The span must lie
 * wholly inside [ram_lo, ram_hi): never the store, its descriptor, aura or MMIO.
 */
static void trace_span(u64 ipa, u64 len, bool write, bool ok)
{
    struct ahci_trace *t = &trace_access;
    u64 i = t->spans++ % TRACE_SPANS;
    t->span[i].ipa = ipa;
    t->span[i].len = len;
    t->span[i].write = write;
    t->span[i].ok = ok;
}

static void trace_io(u64 lba, u32 count, bool write)
{
    struct ahci_trace *t = &trace_access;
    if (!t->io_calls++) {
        t->io_lba = lba;
        t->io_sectors = 0;
        t->io_write = write;
    }
    t->io_sectors += count;
}

static void *env_span(void *opaque, u64 ipa, u64 len, bool write)
{
    UNUSED(opaque);
    if (!len || ipa < ram_lo || ipa >= ram_hi || len > ram_hi - ipa) {
        trace_span(ipa, len, write, false);
        return NULL;
    }
    trace_span(ipa, len, write, true);
    return (void *)ipa;
}

/* Bulk copy: 8-byte aligned pieces with the assembly loops, the rest byte by byte. */
static void copy(void *dst, const void *src, u64 len)
{
    if (!(((u64)dst | (u64)src) & 7)) {
        u64 bulk = len & ~15UL;
        if (bulk)
            memcpy128(dst, (void *)src, bulk);
        dst = (u8 *)dst + bulk;
        src = (const u8 *)src + bulk;
        len -= bulk;
    }
    if (len)
        memcpy(dst, src, len);
}

static bool env_read(void *opaque, u64 lba, u32 count, void *dst)
{
    UNUSED(opaque);
    u64 bytes = (u64)count * AHCI_MODEL_SECTOR;
    trace_io(lba, count, false);
    if (lba > disk_bytes / AHCI_MODEL_SECTOR || bytes > disk_bytes - lba * AHCI_MODEL_SECTOR)
        return false;
    copy(dst, (const void *)(store_base + lba * AHCI_MODEL_SECTOR), bytes);
    return true;
}

static bool env_write(void *opaque, u64 lba, u32 count, const void *src)
{
    UNUSED(opaque);
    u64 bytes = (u64)count * AHCI_MODEL_SECTOR;
    trace_io(lba, count, true);
    if (lba > disk_bytes / AHCI_MODEL_SECTOR || bytes > disk_bytes - lba * AHCI_MODEL_SECTOR)
        return false;
    copy((void *)(store_base + lba * AHCI_MODEL_SECTOR), src, bytes);
    return true;
}

/* The RAM store is coherent DRAM: completing the stores is the whole flush. */
static bool env_flush(void *opaque)
{
    UNUSED(opaque);
    sysop("dsb sy");
    return true;
}

/* D5: the NVMe backend, traced like the RAM store for the fatal report. */
static bool env_nvme_read(void *opaque, u64 lba, u32 count, void *dst)
{
    trace_io(lba, count, false);
    return ahci_nvme_read(opaque, lba, count, dst);
}

static bool env_nvme_write(void *opaque, u64 lba, u32 count, const void *src)
{
    trace_io(lba, count, true);
    return ahci_nvme_write(opaque, lba, count, src);
}

static void env_irq(void *opaque, bool level)
{
    UNUSED(opaque);
    hv_t8152_gic_spi_level(HV_AHCI_INTID, level);
}

static void env_command(void *opaque, u32 slot, const u8 *fis)
{
    UNUSED(opaque);
    trace_access.commands++;
    trace_access.slot = slot;
    memcpy(trace_access.fis, fis, sizeof(trace_access.fis));
}

static void env_log(void *opaque, const char *event, u64 a, u64 b)
{
    UNUSED(opaque);
    /* At most 16 lines for the whole session: the guest must not flood the console. */
    if (events_logged >= 16)
        return;
    events_logged++;
    printf("HV: T8152: AHCI: %s (0x%lx, 0x%lx)\n", event, a, b);
}

/* ---- the ABAR hook ----------------------------------------------------------------------- */

static bool hv_ahci_mmio(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - HV_AHCI_ABAR;
    bool ok = true;
    u64 start = mrs(CNTPCT_EL0);

    spin_lock(&ahci_lock);
    struct ahci_trace *t = &trace_access;
    hv_fatal_seq_begin(&t->seq);
    t->cpu = smp_id();
    t->cntpct = start;
    t->off = off;
    t->val = write ? val[0] : 0;
    t->write = write;
    t->width = width;
    t->commands = t->io_calls = t->spans = 0;
    if (width == 4 && !(off & 15)) {
        /* LDP/STP of two X registers: two 64-bit accesses. */
        ok = ahci_model_mmio(&model, off, 3, &val[0], write) &&
             ahci_model_mmio(&model, off + 8, 3, &val[1], write);
    } else if (width >= 0 && width <= 3) {
        ok = ahci_model_mmio(&model, off, (u32)width, val, write);
    } else {
        ok = false;
        if (!write)
            for (int i = 0; i < (1 << (width - 3)) && i < HV_MAX_RW_WORDS; i++)
                val[i] = 0;
    }
    u64 took = mrs(CNTPCT_EL0) - start;
    if (took > longest_ticks)
        longest_ticks = took;
    if (t->commands || t->io_calls || t->spans) {
        u64 seq = trace_command.seq;
        hv_fatal_seq_begin(&trace_command.seq);
        trace_command = *t;
        trace_command.seq = seq + 1;
        hv_fatal_seq_end(&trace_command.seq);
    }
    hv_fatal_seq_end(&t->seq);
    spin_unlock(&ahci_lock);

    if (!ok && !(logged & LOG_WIDTH)) {
        logged |= LOG_WIDTH;
        printf("HV: T8152: AHCI: %s of %d bytes at ABAR+0x%lx is unaligned or too wide: "
               "%s (logged once)\n",
               write ? "write" : "read", 1 << width, off, write ? "ignored" : "reads 0");
    }
    return true; /* RAZ/WI, never a guest abort */
}

/* ---- the store --------------------------------------------------------------------------- */

static void desc_seal(void)
{
    desc->desc_crc32 =
        ahci_crc32(0, desc, __builtin_offsetof(struct hv_ahci_store_desc, desc_crc32));
    sysop("dsb sy");
}

static void desc_state(u32 state)
{
    desc->state = state;
    desc->alive_cntpct = mrs(CNTPCT_EL0);
    desc_seal();
}

static u32 disk_crc(void)
{
    return ahci_crc32(0, (const void *)store_base, disk_bytes);
}

static const char *const state_names[] = {"EMPTY", "LOADING", "READY", "RUNNING", "SEALED"};

static const char *state_name(u32 state)
{
    return state < ARRAY_SIZE(state_names) ? state_names[state] : "unknown";
}

/* A descriptor header with this carrier's magic, version, layout and a matching CRC. */
static bool desc_valid(const struct hv_ahci_store_desc *d, bool verbose)
{
    if (memcmp(d->magic, HV_AHCI_DESC_MAGIC, 8) || d->version != HV_AHCI_VERSION ||
        d->header_size != __builtin_offsetof(struct hv_ahci_store_desc, desc_crc32)) {
        if (verbose)
            printf("HV: T8152: AHCI: keep refused: no store descriptor at 0x%lx "
                   "(magic/version)\n",
                   (u64)d);
        return false;
    }
    u32 crc = ahci_crc32(0, d, __builtin_offsetof(struct hv_ahci_store_desc, desc_crc32));
    if (crc != d->desc_crc32) {
        if (verbose)
            printf("HV: T8152: AHCI: keep refused: descriptor CRC 0x%x, recorded 0x%x\n", crc,
                   d->desc_crc32);
        return false;
    }
    return true;
}

/* The host note (launcher-written, informational): printable ASCII up to the first NUL. */
static void print_note(const struct hv_ahci_store_desc *d)
{
    char note[sizeof(d->host_note)];
    unsigned int n = 0;
    while (n < sizeof(note) - 1 && d->host_note[n] >= 0x20 && d->host_note[n] < 0x7f) {
        note[n] = (char)d->host_note[n];
        n++;
    }
    note[n] = 0;
    printf("HV: T8152: AHCI: store note: %s\n", n ? note : "(none)");
}

/* A kept store's descriptor: everything must match, nothing is trusted. */
static int desc_check(u64 flags)
{
    const struct hv_ahci_store_desc *d = desc;
    if (!desc_valid(d, true))
        return -1;
    if (d->window_top != window_top || d->store_base != store_base || d->disk_bytes != disk_bytes) {
        printf("HV: T8152: AHCI: keep refused: descriptor geometry [0x%lx +0x%lx, top 0x%lx) "
               "differs from the request [0x%lx +0x%lx, top 0x%lx)\n",
               d->store_base, d->disk_bytes, d->window_top, store_base, disk_bytes, window_top);
        return -1;
    }
    if (d->state == HV_AHCI_READY || d->state == HV_AHCI_SEALED) {
        u32 now = disk_crc();
        if (now != d->disk_crc32) {
            printf("HV: T8152: AHCI: keep refused: disk CRC-32 0x%08x, sealed 0x%08x (the store "
                   "changed since generation %u)\n",
                   now, d->disk_crc32, d->generation);
            return -1;
        }
        printf("HV: T8152: AHCI: kept %s store generation %u, disk CRC-32 0x%08x verified\n",
               state_name(d->state), d->generation, now);
        return 0;
    }
    if (d->state == HV_AHCI_RUNNING && (flags & HV_AHCI_ATTACH_ACCEPT_UNSEALED)) {
        /*
         * Nothing proves an unsealed disk intact; at least refuse one from before a SoC
         * reset: the counter restarts then, and DRAM (or what iBoot and aura wrote into it)
         * cannot be trusted.  A reset that took longer than the old session's uptime is not
         * caught: the launcher's help says keep-unsealed is unsafe after any hard reset.
         */
        u64 now = mrs(CNTPCT_EL0);
        if (now < d->alive_cntpct) {
            printf("HV: T8152: AHCI: keep refused: the system counter (0x%lx) is below the "
                   "store's last-alive stamp (0x%lx): the machine was reset since the store was "
                   "written, so its unsealed contents are not trusted\n",
                   now, d->alive_cntpct);
            return -1;
        }
        printf("HV: T8152: AHCI: WARNING: keeping an UNSEALED store (generation %u, last exit "
               "%d, last alive %lu s ago): its contents cannot be verified; the guest sees an "
               "unclean shutdown\n",
               d->generation, d->last_exit,
               (now - d->alive_cntpct) / (mrs(CNTFRQ_EL0) ? mrs(CNTFRQ_EL0) : 1));
        return 0;
    }
    printf("HV: T8152: AHCI: keep refused: store state %s%s\n", state_name(d->state),
           d->state == HV_AHCI_RUNNING ? " (not sealed: the last run did not end with a PSCI "
                                         "exit; --windows-ahci-keep-unsealed accepts it)"
                                       : "");
    return -1;
}

static u64 attach(u64 base, u64 top, u64 bytes, u64 flags)
{
    u64 ram_size;

    if (!ahci_enabled || chip_id != T8152) {
        printf("HV: T8152: AHCI: attach refused: the AHCI configure flag is not set\n");
        return HV_AHCI_ERROR;
    }
    if (attached || entered || hv_ahci_nvme_attached()) {
        printf("HV: T8152: AHCI: attach refused: a store is already attached or the guest "
               "started\n");
        return HV_AHCI_ERROR;
    }
    if (flags & ~(u64)(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_ACCEPT_UNSEALED |
                       HV_AHCI_ATTACH_OVERWRITE) ||
        ((flags & HV_AHCI_ATTACH_KEEP) && (flags & HV_AHCI_ATTACH_OVERWRITE)) ||
        ((flags & HV_AHCI_ATTACH_ACCEPT_UNSEALED) && !(flags & HV_AHCI_ATTACH_KEEP))) {
        printf("HV: T8152: AHCI: attach refused: bad flags 0x%lx\n", flags);
        return HV_AHCI_ERROR;
    }
    if (!hv_t8152_guest_ram(&ram_lo, &ram_size)) {
        printf("HV: T8152: AHCI: attach refused: no guest RAM window yet (load the guest "
               "first)\n");
        return HV_AHCI_ERROR;
    }
    ram_hi = ram_lo + ram_size;
    if (ram_size < HV_AHCI_MIN_GUEST) {
        printf("HV: T8152: AHCI: attach refused: guest RAM [0x%lx, 0x%lx) is below 4 GiB\n", ram_lo,
               ram_hi);
        return HV_AHCI_ERROR;
    }
    if (base != ram_hi) {
        printf("HV: T8152: AHCI: attach refused: the store must start at the guest RAM top "
               "0x%lx, not 0x%lx\n",
               ram_hi, base);
        return HV_AHCI_ERROR;
    }
    if ((base & (HV_AHCI_STORE_ALIGN - 1)) || (top & (HV_AHCI_DESC_SIZE - 1)) || top <= base ||
        top - base < HV_AHCI_DESC_SIZE + HV_AHCI_MIN_DISK || (bytes % AHCI_MODEL_SECTOR) ||
        bytes < HV_AHCI_MIN_DISK || bytes > top - base - HV_AHCI_DESC_SIZE) {
        printf("HV: T8152: AHCI: attach refused: store [0x%lx, 0x%lx) with a 0x%lx-byte disk: "
               "base must be 32 MiB aligned, top 64 KiB aligned, the disk a multiple of 512, at "
               "least 1 MiB, and below the 64 KiB descriptor\n",
               base, top, bytes);
        return HV_AHCI_ERROR;
    }
    if (hv_t8152_reserve_store(base, top) < 0)
        return HV_AHCI_ERROR;

    store_base = base;
    window_top = top;
    disk_bytes = bytes;
    desc = (struct hv_ahci_store_desc *)(top - HV_AHCI_DESC_SIZE);

    if (flags & HV_AHCI_ATTACH_KEEP) {
        if (desc_valid(desc, false))
            print_note(desc);
        if (desc_check(flags) < 0)
            return HV_AHCI_ERROR; /* the denylist entry stays: harmless, the store is unused */
        if (desc->state == HV_AHCI_RUNNING) {
            /*
             * Accepted unsealed: record what it holds now, so READY carries a real CRC.  If
             * entry then fails, the next keep verifies this CRC instead of locking the store
             * out (READY is always CRC-checked).
             */
            desc->disk_crc32 = disk_crc();
            printf("HV: T8152: AHCI: unsealed store recorded as READY, disk CRC-32 0x%08x\n",
                   desc->disk_crc32);
        }
        desc_state(HV_AHCI_READY);
    } else {
        /* A fresh attach zeroes the store: never over a disk someone may still want. */
        if (desc_valid(desc, false) && desc->window_top == top &&
            (desc->state == HV_AHCI_READY || desc->state == HV_AHCI_RUNNING ||
             desc->state == HV_AHCI_SEALED)) {
            print_note(desc);
            if (!(flags & HV_AHCI_ATTACH_OVERWRITE)) {
                printf("HV: T8152: AHCI: attach refused: a %s store (generation %u, disk "
                       "[0x%lx +0x%lx)) is at this window's top; keep it, or pass "
                       "--windows-ahci-overwrite to replace it\n",
                       state_name(desc->state), desc->generation, desc->store_base,
                       desc->disk_bytes);
                return HV_AHCI_ERROR;
            }
            printf("HV: T8152: AHCI: overwriting the %s store (generation %u) on request\n",
                   state_name(desc->state), desc->generation);
        }
        /* Fresh: disk, slack and descriptor all zero (DRAM held anything before). */
        memset64((void *)base, 0, top - base);
        memcpy(desc->magic, HV_AHCI_DESC_MAGIC, 8);
        desc->version = HV_AHCI_VERSION;
        desc->header_size = __builtin_offsetof(struct hv_ahci_store_desc, desc_crc32);
        desc->window_top = top;
        desc->store_base = base;
        desc->disk_bytes = bytes;
        desc_state(HV_AHCI_LOADING);
    }

    env = (struct ahci_env){.guest_span = env_span,
                            .read = env_read,
                            .write = env_write,
                            .flush = env_flush,
                            .irq_level = env_irq,
                            .log = env_log,
                            .opaque = NULL,
                            .sectors = bytes / AHCI_MODEL_SECTOR,
                            .command = env_command};
    ahci_model_init(&model, &env);
    if (hv_map_hook(HV_AHCI_ABAR, hv_ahci_mmio, HV_AHCI_ABAR_SIZE) < 0) {
        printf("HV: T8152: AHCI: attach refused: cannot hook the ABAR at 0x%lx\n", HV_AHCI_ABAR);
        return HV_AHCI_ERROR;
    }
    attached = true;
    printf("HV: T8152: AHCI: %s store: disk [0x%lx, 0x%lx) %lu MiB, descriptor 0x%lx, guest RAM "
           "[0x%lx, 0x%lx); ABAR 0x%lx hooked, SPI %u\n",
           (flags & HV_AHCI_ATTACH_KEEP) ? "kept" : "fresh", base, base + bytes, bytes >> 20,
           (u64)desc, ram_lo, ram_hi, HV_AHCI_ABAR, HV_AHCI_INTID);
    return 0;
}

static u64 seal_upload(u64 host_crc)
{
    if (!attached || nvme_mode || entered || desc->state != HV_AHCI_LOADING) {
        printf("HV: T8152: AHCI: seal refused: no fresh store is loading\n");
        return HV_AHCI_ERROR;
    }
    u32 crc = disk_crc();
    if (crc != (u32)host_crc) {
        printf("HV: T8152: AHCI: upload CRC-32 mismatch: store 0x%08x, host 0x%08x; the guest "
               "will not start\n",
               crc, (u32)host_crc);
        return HV_AHCI_ERROR;
    }
    desc->disk_crc32 = crc;
    desc_state(HV_AHCI_READY);
    printf("HV: T8152: AHCI: upload verified, disk CRC-32 0x%08x\n", crc);
    return crc;
}

static u64 seal_exit(void)
{
    if (!attached || nvme_mode || !entered || !exited || desc->state != HV_AHCI_RUNNING) {
        printf("HV: T8152: AHCI: seal refused: the guest has not run and exited\n");
        return HV_AHCI_ERROR;
    }
    if (exit_status != 1 && exit_status != 2) {
        printf("HV: T8152: AHCI: seal refused: the guest did not end with PSCI SYSTEM_OFF or "
               "SYSTEM_RESET (exit %d); the store stays RUNNING (unsealed)\n",
               exit_status);
        return HV_AHCI_ERROR;
    }
    u32 crc = disk_crc();
    desc->disk_crc32 = crc;
    desc->generation++;
    desc_state(HV_AHCI_SEALED);
    printf("HV: T8152: AHCI: store sealed: generation %u, disk CRC-32 0x%08x\n", desc->generation,
           crc);
    return crc | (u64)desc->generation << 32;
}

/*
 * D5: the disk is a GPT partition of the internal NVMe.  Guest RAM is the whole window (no
 * store).  A gate attach (R0/W0) needs no configure flag and installs no disk.
 */
static u64 attach_nvme(u64 req_addr)
{
    u64 lo = 0, size = 0, sectors = 0;
    bool have_ram = hv_t8152_guest_ram(&lo, &size);
    struct hv_ahci_nvme_req r;

    if (!req_addr || (req_addr & 7)) {
        printf("HV: T8152: AHCI NVMe: attach refused: bad request address 0x%lx\n", req_addr);
        return HV_AHCI_ERROR;
    }
    /* One copy: what is checked here is what the backend gets. */
    memcpy(&r, (const void *)req_addr, sizeof(r));
    bool gate = r.flags & HV_AHCI_NVME_GATE;

    if (chip_id != T8152 || (!gate && !ahci_enabled)) {
        printf("HV: T8152: AHCI NVMe: attach refused: the AHCI configure flag is not set\n");
        return HV_AHCI_ERROR;
    }
    if (attached || entered) {
        printf("HV: T8152: AHCI NVMe: attach refused: a disk is already attached or the guest "
               "started\n");
        return HV_AHCI_ERROR;
    }
    if (!gate && (!have_ram || size < HV_AHCI_MIN_GUEST)) {
        printf("HV: T8152: AHCI NVMe: attach refused: guest RAM is not set or below 4 GiB\n");
        return HV_AHCI_ERROR;
    }
    if (hv_ahci_nvme_attach((u64)&r, have_ram, lo, lo + size, &sectors) < 0)
        return HV_AHCI_ERROR;
    if (gate)
        return 0;
    ram_lo = lo;
    ram_hi = lo + size;
    bool ro = hv_ahci_nvme_read_only();
    env = (struct ahci_env){.guest_span = env_span,
                            .read = env_nvme_read,
                            .write = env_nvme_write,
                            .flush = ahci_nvme_flush,
                            .irq_level = env_irq,
                            .log = env_log,
                            .opaque = hv_ahci_nvme_backend(),
                            .sectors = sectors,
                            .command = env_command,
                            .begin = ahci_nvme_begin,
                            .end = ahci_nvme_end,
                            .phys_log2 = 3,
                            .read_only = ro,
                            .serial = "AURA-AHCI-NVME000001",
                            .firmware = "D5.0",
                            .model_name = "Aura carrier NVMe disk"};
    ahci_model_init(&model, &env);
    if (hv_map_hook(HV_AHCI_ABAR, hv_ahci_mmio, HV_AHCI_ABAR_SIZE) < 0) {
        printf("HV: T8152: AHCI NVMe: attach refused: cannot hook the ABAR at 0x%lx\n",
               HV_AHCI_ABAR);
        hv_ahci_nvme_close("ABAR hook refused");
        return HV_AHCI_ERROR;
    }
    attached = true;
    nvme_mode = true;
    printf("HV: T8152: AHCI: NVMe disk %s, %lu sectors (512e); guest RAM [0x%lx, 0x%lx); ABAR "
           "0x%lx hooked, SPI %u\n",
           ro ? "read-only" : "read-write", sectors, ram_lo, ram_hi, HV_AHCI_ABAR, HV_AHCI_INTID);
    return 0;
}

static u64 detach(void)
{
    if (hv_ahci_nvme_state() == HV_AHCI_NVME_RUNNING && !exited) {
        printf("HV: T8152: AHCI NVMe: detach refused: the guest session is still running (exit "
               "it; its exit closes the disk)\n");
        return HV_AHCI_ERROR;
    }
    enum hv_ahci_nvme_state st = hv_ahci_nvme_close("launcher DETACH");
    return st == HV_AHCI_NVME_FAILED ? HV_AHCI_ERROR : st;
}

static u64 query(u64 index)
{
    switch (index) {
        case HV_AHCI_Q_VERSION:
            return HV_AHCI_VERSION;
        case HV_AHCI_Q_ABAR:
            return HV_AHCI_ABAR;
        case HV_AHCI_Q_ABAR_SIZE:
            return HV_AHCI_ABAR_SIZE;
        case HV_AHCI_Q_INTID:
            return HV_AHCI_INTID;
        case HV_AHCI_Q_CAP:
            return AHCI_MODEL_CAP;
        case HV_AHCI_Q_MIN_GUEST:
            return HV_AHCI_MIN_GUEST;
        case HV_AHCI_Q_STORE_ALIGN:
            return HV_AHCI_STORE_ALIGN;
        case HV_AHCI_Q_DESC_SIZE:
            return HV_AHCI_DESC_SIZE;
        default:
            return HV_AHCI_ERROR;
    }
}

u64 hv_ahci_proxy(u64 op, u64 a1, u64 a2, u64 a3, u64 a4)
{
    switch (op) {
        case HV_AHCI_OP_QUERY:
            return query(a1);
        case HV_AHCI_OP_ATTACH:
            return attach(a1, a2, a3, a4);
        case HV_AHCI_OP_SEAL_UPLOAD:
            return seal_upload(a1);
        case HV_AHCI_OP_SEAL_EXIT:
            return seal_exit();
        case HV_AHCI_OP_STATUS:
            if (!attached || nvme_mode)
                return 0;
            return desc->state | (u64)desc->generation << 8 | (u64)(u8)desc->last_exit << 40;
        case HV_AHCI_OP_ATTACH_NVME:
            return attach_nvme(a1);
        case HV_AHCI_OP_DETACH:
            return detach();
        case HV_AHCI_OP_NVME_STATUS:
            return hv_ahci_nvme_status(a1);
        case HV_AHCI_OP_NVME_TEST:
            return hv_ahci_nvme_test(a1, a2, a3, a4);
        default:
            return HV_AHCI_ERROR;
    }
}

/* ---- guest entry and exit ---------------------------------------------------------------- */

struct hook_ctx {
    u64 covered;
    unsigned int bad;
};

static bool hook_leaf(void *opaque, u64 ipa, u64 size, u64 pte, int level)
{
    struct hook_ctx *ctx = opaque;
    UNUSED(level);
    if (ipa >= HV_AHCI_ABAR + HV_AHCI_ABAR_SIZE || ipa + size <= HV_AHCI_ABAR)
        return true;
    /* A software leaf (bit 0 clear) of type SPTE_HOOK (1) whose target is this file's hook. */
    if ((pte & BIT(0)) || FIELD_GET(GENMASK(52, 50), pte) != 1 ||
        (pte & GENMASK(49, 2)) != ((u64)hv_ahci_mmio & GENMASK(49, 2)))
        ctx->bad++;
    else
        ctx->covered += size;
    return true;
}

int hv_ahci_verify(void)
{
    if (hv_ahci_nvme_gate()) {
        printf("HV: T8152: AHCI NVMe: a gate (test) attach is open: DETACH it before a guest "
               "session; refusing guest entry\n");
        return -1;
    }
    if (!ahci_enabled)
        return 0;
    if (!attached) {
        printf("HV: T8152: AHCI: the configure flag is set but no store is attached; refusing "
               "guest entry\n");
        return -1;
    }
    if (nvme_mode ? hv_ahci_nvme_verify() < 0 : desc->state != HV_AHCI_READY) {
        if (!nvme_mode)
            printf("HV: T8152: AHCI: store state is %s, not READY (upload not sealed?); "
                   "refusing guest entry\n",
                   state_name(desc->state));
        return -1;
    }
    struct hook_ctx ctx = {0, 0};
    hv_pt_for_each_leaf(hook_leaf, &ctx);
    if (ctx.bad || ctx.covered < HV_AHCI_ABAR_SIZE) {
        printf("HV: T8152: AHCI: the ABAR 0x%lx is not wholly this carrier's hook (%u foreign "
               "leaves, 0x%lx bytes hooked); refusing guest entry\n",
               HV_AHCI_ABAR, ctx.bad, ctx.covered);
        return -1;
    }
    if (nvme_mode)
        printf("HV: T8152: AHCI: verified: ABAR 0x%lx hooked, NVMe backend attached, I/O fence "
               "set, bounce pool denied to stage 2, ANS denied by name\n",
               HV_AHCI_ABAR);
    else
        printf("HV: T8152: AHCI: verified: ABAR 0x%lx hooked, store [0x%lx, 0x%lx) READY and "
               "denied to stage 2\n",
               HV_AHCI_ABAR, store_base, window_top);
    return 0;
}

void hv_ahci_guest_entered(void)
{
    if (!ahci_enabled || !attached)
        return;
    entered = true;
    if (nvme_mode) {
        hv_ahci_nvme_guest_entered();
        return;
    }
    desc->last_exit = 0;
    desc_state(HV_AHCI_RUNNING);
}

void hv_ahci_guest_exited(int status)
{
    if (!attached || !entered)
        return;
    exited = true;
    exit_status = status;
    if (nvme_mode) {
        hv_ahci_report();
        return;
    }
    desc->last_exit = status;
    desc->alive_cntpct = mrs(CNTPCT_EL0);
    desc_seal();
    hv_ahci_report();
}

void hv_ahci_session_end(int status)
{
    if (hv_ahci_nvme_state() == HV_AHCI_NVME_DETACHED || hv_ahci_nvme_state() == HV_AHCI_NVME_CLEAN)
        return;
    hv_ahci_nvme_close(status == 1   ? "guest SYSTEM_OFF"
                       : status == 2 ? "guest SYSTEM_RESET"
                                     : "guest session failed, aborted or refused");
}

void hv_ahci_report(void)
{
    if (!attached)
        return;
    u64 now = mrs(CNTPCT_EL0);
    if (!exited && now < report_next)
        return;
    report_next = now + 10 * mrs(CNTFRQ_EL0);
    if (nvme_mode)
        hv_ahci_nvme_report(exited);
    if (entered && !exited && !nvme_mode) {
        /* The store's heartbeat (see alive_cntpct): this session still owns it. */
        desc->alive_cntpct = now;
        desc_seal();
    }
    struct ahci_model_stats s = model.stats;
    if (s.commands + s.errors + s.fatal == reported_commands && !exited)
        return;
    reported_commands = s.commands + s.errors + s.fatal;
    u64 us = longest_ticks * 1000000 / (mrs(CNTFRQ_EL0) ? mrs(CNTFRQ_EL0) : 1);
    printf("HV: T8152: AHCI: %lu commands, %lu errors, %lu fatal; read %lu MiB, wrote %lu MiB, "
           "%lu flushes; largest %u sectors; longest access %lu us\n",
           s.commands, s.errors, s.fatal, s.sectors_read >> 11, s.sectors_written >> 11, s.flushes,
           s.largest, us);
}

u64 hv_ahci_commands(void)
{
    return __atomic_load_n(&model.stats.commands, __ATOMIC_RELAXED);
}

/* ---- the fatal report (hv_fatal.h) ------------------------------------------------------- */

static u64 trace_age_us(u64 now, u64 then, u64 freq)
{
    return then && freq && then <= now ? (now - then) * 1000000 / freq : 0;
}

static const char *trace_state(u64 seq)
{
    return seq == ~0UL ? "changing" : (seq & 1) ? "IN PROGRESS" : "done";
}

void hv_ahci_fatal_report(u64 now, u64 freq)
{
    struct ahci_trace a, c;

    if (!attached)
        return;
    u64 sa = hv_fatal_read(&trace_access.seq, &trace_access, &a, sizeof(a));
    u64 sc = hv_fatal_read(&trace_command.seq, &trace_command, &c, sizeof(c));

    printf("HV: fatal: AHCI guest RAM [0x%lx, 0x%lx), store [0x%lx, 0x%lx), %lu commands\n", ram_lo,
           ram_hi, store_base, window_top, model.stats.commands);
    hv_ahci_nvme_fatal_report(now, freq);
    if (a.cntpct)
        printf("HV: fatal: AHCI last access: cpu %lu, %lu us ago, %s: %lu-byte %s ABAR+0x%lx "
               "value 0x%lx\n",
               a.cpu, trace_age_us(now, a.cntpct, freq), trace_state(sa), 1UL << (a.width & 7),
               a.write ? "write" : "read", a.off, a.val);
    if (!c.cntpct)
        return;
    u64 lba = (u64)c.fis[4] | (u64)c.fis[5] << 8 | (u64)c.fis[6] << 16 | (u64)c.fis[8] << 24 |
              (u64)c.fis[9] << 32 | (u64)c.fis[10] << 40;
    printf("HV: fatal: AHCI last command access: cpu %lu, %lu us ago, %s: ABAR+0x%lx = 0x%lx; "
           "%lu command(s), last slot %lu ATA 0x%02x LBA 0x%lx count %u\n",
           c.cpu, trace_age_us(now, c.cntpct, freq), trace_state(sc), c.off, c.val, c.commands,
           c.slot, c.fis[2], lba, (u32)c.fis[12] | (u32)c.fis[13] << 8);
    if (c.io_calls)
        printf("HV: fatal: AHCI   disk %s LBA 0x%lx, %lu sectors in %lu call(s)\n",
               c.io_write ? "write" : "read", c.io_lba, c.io_sectors, c.io_calls);
    u64 first = c.spans > TRACE_SPANS ? c.spans - TRACE_SPANS : 0;
    for (u64 i = first; i < c.spans; i++) {
        u64 k = i % TRACE_SPANS;
        printf("HV: fatal: AHCI   span %lu/%lu: IPA 0x%lx +0x%lx %s%s\n", i + 1, c.spans,
               c.span[k].ipa, c.span[k].len, c.span[k].write ? "to guest" : "from guest",
               c.span[k].ok ? "" : " REFUSED (outside guest RAM)");
    }
}
