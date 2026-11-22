/* SPDX-License-Identifier: MIT */
/*
 * D5 host tests: the simulated T8152 ANS NVMe controller and the platform stubs aura's
 * src/nvme.c needs on the host (MMIO, time, ADT, RTKit, SART, PMGR, heap).  See nvme_sim.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "malloc.h"
#include "nvme_sim.h"
#include "pmgr.h"
#include "rtkit.h"
#include "sart.h"
#include "types.h"
#include "utils.h"

/* Register offsets (NVMe base spec for the architected ones; T8152 secure layout). */
#define R_CC          0x14
#define R_CSTS        0x1c
#define R_AQA         0x4024
#define R_ASQ         0x4028
#define R_ACQ         0x4030
#define R_DB_ACQ      0x1004
#define R_DB_IOCQ     0x100c
#define R_BOOT        0x1300
#define R_IOQ_CMDS    0x5200
#define R_IOQ_CQES    0x5208
#define R_LSQ_ASQ     0x2490c
#define R_LSQ_IOSQ    0x24910
#define R_NVMMU_ASQ   0x28108
#define R_NVMMU_IOSQ  0x28110
#define R_TCB_INVAL   0x28118
#define R_TCB_STAT    0x28120
#define R_SIZE        0x30000
#define BOOT_OK       0xde71ce55u
#define QUEUE_ENTRIES 64

u64 sim_now;
u32 chip_id = 0x8152;
void *adt = (void *)1;

struct pending {
    bool live;
    u64 due;
    bool admin;
    u8 tag;
    u16 status;
};

static struct {
    u64 nsze;
    u8 *durable, *vol, *snap;
    u8 *dirty;
    u32 regs[R_SIZE / 4];
    bool enabled, shn_requested, shst_done;
    u32 shst_countdown;
    u64 iosq, iocq; /* from Create SQ/CQ (and the T8152 register copies) */
    u32 acq_tail, iocq_tail;
    u8 acq_phase, iocq_phase;
    u64 guest_lo, guest_hi;
    bool track, reset;
    u64 hook_n;
    void (*hook)(void);
    struct sim_faults f;
    struct sim_stats s;
    struct pending pend;
    char events[4096];
} S;

static u64 reg64(u32 off)
{
    return (u64)S.regs[off / 4] | (u64)S.regs[off / 4 + 1] << 32;
}

void sim_event(char c)
{
    size_t n = strlen(S.events);
    if (n + 1 < sizeof(S.events))
        S.events[n] = c;
}

const char *sim_events(void)
{
    return S.events;
}

void sim_init(u64 nsze, const char *image)
{
    free(S.durable);
    free(S.vol);
    free(S.snap);
    free(S.dirty);
    memset(&S, 0, sizeof(S));
    S.nsze = nsze;
    S.durable = calloc(nsze, SIM_BLOCK);
    S.vol = calloc(nsze, SIM_BLOCK);
    S.dirty = calloc(nsze, 1);
    if (!S.durable || !S.vol || !S.dirty) {
        fprintf(stderr, "sim: out of memory\n");
        exit(2);
    }
    if (image) {
        FILE *fp = fopen(image, "rb");
        if (!fp) {
            perror(image);
            exit(2);
        }
        size_t got = fread(S.durable, 1, nsze * SIM_BLOCK, fp);
        fclose(fp);
        (void)got;
    }
    S.f.shst_polls = 3;
    S.f.lbads = 12;
    S.s.io_min_block = ~0ULL;
    sim_now = 1000;
}

void sim_set_guest(u64 lo, u64 hi)
{
    S.guest_lo = lo;
    S.guest_hi = hi;
}

void sim_track_io(bool on)
{
    S.track = on;
}

struct sim_faults *sim_faults(void)
{
    return &S.f;
}

const struct sim_stats *sim_stats(void)
{
    return &S.s;
}

void sim_set_hook(u64 n, void (*hook)(void))
{
    S.hook_n = n;
    S.hook = hook;
}

bool sim_enabled(void)
{
    return S.enabled;
}

u64 sim_nsze(void)
{
    return S.nsze;
}

const u8 *sim_view(u64 b)
{
    return S.dirty[b] ? S.vol + b * SIM_BLOCK : S.durable + b * SIM_BLOCK;
}

const u8 *sim_durable(u64 b)
{
    return S.durable + b * SIM_BLOCK;
}

const u8 *sim_snapshot(u64 b)
{
    return S.snap ? S.snap + b * SIM_BLOCK : NULL;
}

static void commit(void)
{
    for (u64 b = 0; b < S.nsze; b++)
        if (S.dirty[b]) {
            memcpy(S.durable + b * SIM_BLOCK, S.vol + b * SIM_BLOCK, SIM_BLOCK);
            S.dirty[b] = 0;
        }
}

void sim_power_cut(void)
{
    memset(S.dirty, 0, S.nsze);
    S.enabled = S.shn_requested = S.shst_done = false;
    S.pend.live = false;
    sim_event('X');
}

void sim_fill(u64 block, u64 count, u8 v)
{
    for (u64 b = block; b < block + count && b < S.nsze; b++) {
        memset(S.durable + b * SIM_BLOCK, v, SIM_BLOCK);
        memset(S.vol + b * SIM_BLOCK, v, SIM_BLOCK);
        S.dirty[b] = 0;
    }
}

void sim_reset_happened(void)
{
    if (!S.snap)
        S.snap = malloc(S.nsze * SIM_BLOCK);
    memcpy(S.snap, S.durable, S.nsze * SIM_BLOCK);
    S.reset = true;
    sim_power_cut();
}

int sim_dump_durable(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    size_t n = fwrite(S.durable, SIM_BLOCK, S.nsze, fp);
    fclose(fp);
    return n == S.nsze ? 0 : -1;
}

/* ---- completions ------------------------------------------------------------------------ */

static void post(bool admin, u8 tag, u16 status)
{
    u64 base = admin ? reg64(R_ACQ) : S.iocq;
    u32 *tail = admin ? &S.acq_tail : &S.iocq_tail;
    u8 *phase = admin ? &S.acq_phase : &S.iocq_phase;
    if (!base)
        return;
    u8 *cqe = (u8 *)base + (u64)*tail * 16;
    memset(cqe, 0, 12);
    cqe[12] = tag;
    cqe[13] = 0;
    u16 st = (u16)(status << 1 | *phase);
    cqe[14] = (u8)st;
    cqe[15] = (u8)(st >> 8);
    if (++*tail == QUEUE_ENTRIES) {
        *tail = 0;
        *phase ^= 1;
    }
}

static void tick(void)
{
    if (S.pend.live && sim_now >= S.pend.due) {
        S.pend.live = false;
        post(S.pend.admin, S.pend.tag, S.pend.status);
    }
}

/* ---- commands ------------------------------------------------------------------------- */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | (u64)rd32(p + 4) << 32;
}

/* The data page `i` of a command's PRPs (NVMe base spec, PRP entries and lists). */
static u8 *prp_page(const u8 *cmd, u32 n, u32 i)
{
    u64 prp1 = rd64(cmd + 24), prp2 = rd64(cmd + 32);
    u32 entry = S.f.prp_list_broken ? 0 : i - 1;
    u64 page = i == 0 ? prp1 : n == 2 ? prp2 : ((const u64 *)prp2)[entry];
    if (page & (SIM_BLOCK - 1))
        return NULL;
    if (page < S.guest_hi && page + SIM_BLOCK > S.guest_lo) {
        S.s.prp_in_guest++;
        return NULL;
    }
    return (u8 *)page;
}

static void check_tcb(const u8 *cmd, u64 tcbs, u8 tag)
{
    const u8 *t = (const u8 *)tcbs + (u64)tag * 128;
    u8 opcode = cmd[0];
    u64 prp1 = rd64(cmd + 24);
    u8 flags = !prp1 ? 0 : (opcode & 1) ? 2 : 1;
    if (!tcbs || t[1] != flags || t[2] != tag || rd32(t + 4) != rd32(cmd + 48) ||
        rd64(t + 24) != prp1 || rd64(t + 32) != rd64(cmd + 32))
        S.s.tcb_mismatch++;
}

static u16 admin_cmd(const u8 *c)
{
    S.s.admin_commands++;
    switch (c[0]) {
        case 0x00: /* delete SQ */
        case 0x04: /* delete CQ */
            return 0;
        case 0x01: /* create SQ */
            S.iosq = rd64(c + 24);
            return 0;
        case 0x05: /* create CQ */
            S.iocq = rd64(c + 24);
            S.iocq_tail = 0;
            S.iocq_phase = 1;
            return 0;
        case 0x06: { /* Identify, CNS 0: namespace 1 */
            S.s.identifies++;
            sim_event('I');
            if (S.f.identify_fails || rd32(c + 40) != 0 || rd32(c + 4) != 1)
                return 0x0b;
            u8 *d = prp_page(c, 1, 0);
            if (!d)
                return 0x04;
            memset(d, 0, SIM_BLOCK);
            for (int i = 0; i < 8; i++)
                d[i] = d[8 + i] = d[16 + i] = (u8)(S.nsze >> (8 * i));
            d[26] = 0; /* FLBAS: format 0 */
            d[128 + 2] = (u8)S.f.lbads;
            return 0;
        }
        default:
            return 0x01; /* invalid opcode */
    }
}

static u16 io_cmd(const u8 *c)
{
    u8 op = c[0];
    u64 lba = rd64(c + 40);
    u32 n = (rd32(c + 48) & 0xffff) + 1;
    if (rd32(c + 4) != 1)
        return 0x0b;
    if (op == 0x00) {
        S.s.flushes++;
        sim_event('F');
        if (S.f.flush_durable)
            commit();
        return 0;
    }
    if (op != 0x01 && op != 0x02)
        return 0x01;
    if (lba >= S.nsze || n > S.nsze - lba) {
        S.s.out_of_range++;
        return 0x80;
    }
    if (S.track) {
        if (lba < S.s.io_min_block)
            S.s.io_min_block = lba;
        if (lba + n - 1 > S.s.io_max_block)
            S.s.io_max_block = lba + n - 1;
    }
    for (u32 i = 0; i < n; i++)
        if (!prp_page(c, n, i))
            return 0x04;
    for (u32 i = 0; i < n; i++) {
        u8 *page = prp_page(c, n, i);
        u64 b = lba + i;
        if (op == 0x02) {
            memcpy(page, sim_view(b), SIM_BLOCK);
        } else {
            memcpy(S.vol + b * SIM_BLOCK, page, SIM_BLOCK);
            S.dirty[b] = 1;
        }
    }
    if (op == 0x02) {
        S.s.reads++;
        sim_event('R');
    } else {
        S.s.writes++;
        S.s.blocks_written += n;
        if (n > 1)
            S.s.multi_writes++;
        sim_event('W');
    }
    return 0;
}

static void doorbell(bool admin, u32 tag)
{
    if (S.reset) {
        S.s.after_reset++;
        return;
    }
    u64 sq = admin ? reg64(R_ASQ) : (S.iosq ? S.iosq : reg64(R_IOQ_CMDS));
    u64 tcbs = admin ? reg64(R_NVMMU_ASQ) : reg64(R_NVMMU_IOSQ);
    if (!sq || tag >= QUEUE_ENTRIES)
        return;
    const u8 *c = (const u8 *)sq + (u64)tag * 64;
    check_tcb(c, tcbs, (u8)tag);
    if (!admin) {
        S.s.io_commands++;
        if (S.hook && S.hook_n == S.s.io_commands) {
            void (*h)(void) = S.hook;
            S.hook = NULL;
            h();
        }
        if (S.reset) {
            S.s.after_reset++;
            return;
        }
    }
    /* A controller that is shutting down or disabled takes no new commands. */
    if (!S.enabled || S.shn_requested)
        return;
    u16 status = admin ? admin_cmd(c) : io_cmd(c);
    if (!admin) {
        u64 n = S.s.io_commands;
        if (n == S.f.drop_completion_at)
            return;
        if (n == S.f.error_at)
            status = 0x06;
        if (n == S.f.delay_at) {
            S.pend = (struct pending){true, sim_now + S.f.delay_us, false, (u8)tag, status};
            return;
        }
    }
    post(admin, (u8)tag, status);
}

/* ---- MMIO ------------------------------------------------------------------------------- */

static u32 off_of(u64 addr)
{
    if (addr < SIM_BASE || addr >= SIM_BASE + R_SIZE || (addr & 3)) {
        fprintf(stderr, "sim: MMIO outside the controller: 0x%llx\n", (unsigned long long)addr);
        abort();
    }
    return (u32)(addr - SIM_BASE);
}

u32 read32(u64 addr)
{
    u32 off = off_of(addr);
    if (off == R_CSTS) {
        if (S.shn_requested && !S.shst_done && !S.f.shst_stuck && S.shst_countdown &&
            !--S.shst_countdown) {
            commit();
            S.shst_done = true;
            S.s.shutdowns_done++;
            sim_event('C');
        }
        u32 shst = S.shst_done ? 2 : S.shn_requested ? 1 : 0;
        return (S.enabled ? 1 : 0) | shst << 2;
    }
    if (off == R_TCB_STAT)
        return 0;
    return S.regs[off / 4];
}

void write32(u64 addr, u32 value)
{
    u32 off = off_of(addr);
    u32 old = S.regs[off / 4];
    S.regs[off / 4] = value;
    switch (off) {
        case R_CC: {
            bool en = value & 1;
            u32 shn = (value >> 14) & 3;
            if (en && !(old & 1)) {
                S.enabled = true;
                S.shn_requested = S.shst_done = false;
                S.acq_tail = 0;
                S.acq_phase = 1;
                sim_event('E');
            } else if (!en && (old & 1)) {
                S.enabled = false;
                S.shn_requested = S.shst_done = false;
                sim_event('D');
            }
            if (S.enabled && shn == 1 && ((old >> 14) & 3) != 1) {
                S.shn_requested = true;
                S.shst_countdown = S.f.shst_polls ? S.f.shst_polls : 1;
                sim_event('S');
            }
            break;
        }
        case R_LSQ_ASQ:
            doorbell(true, value);
            break;
        case R_LSQ_IOSQ:
            doorbell(false, value);
            break;
        case R_IOQ_CQES + 4:
            if (reg64(R_IOQ_CQES))
                S.iocq = reg64(R_IOQ_CQES);
            break;
        default:
            break;
    }
}

/* ---- time ------------------------------------------------------------------------------- */

u64 get_ticks(void)
{
    return sim_now;
}

u64 ticks_to_msecs(u64 ticks)
{
    return ticks / 1000;
}

u64 timeout_calculate(u32 usec)
{
    return sim_now + usec;
}

bool timeout_expired(u64 timeout)
{
    sim_now += 50;
    tick();
    return sim_now > timeout;
}

/* ---- platform stubs ------------------------------------------------------------------- */

void *memalign(size_t align, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, align, size))
        return NULL;
    memset(p, 0, size);
    return p;
}

void memcpy128(void *dst, void *src, size_t size)
{
    memcpy(dst, src, size);
}

void memset64(void *dst, u64 value, size_t size)
{
    u64 *d = dst;
    for (size_t i = 0; i < size / 8; i++)
        d[i] = value;
}

int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    (void)path;
    return 1;
}

int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    (void)a;
    (void)path;
    trace[0] = 1;
    return 1;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    (void)node;
    (void)name;
    *len = 0;
    return NULL; /* no nvme-secure-bar: T8152 has the secure register layout instead */
}

const void *adt_get_property(const void *a, int node, const char *name)
{
    (void)a;
    (void)node;
    return strcmp(name, "nvme-secure-reg-layout") ? NULL : (const void *)1;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a;
    (void)node;
    (void)compat;
    return false;
}

int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a;
    (void)path;
    (void)prop;
    if (idx != 3)
        return -1;
    *addr = SIM_BASE;
    *size = R_SIZE;
    return 0;
}

static int dummy;
struct asc_dev *asc_init(const char *path)
{
    (void)path;
    return (struct asc_dev *)&dummy;
}
void asc_free(struct asc_dev *asc)
{
    (void)asc;
}
void asc_cpu_stop(struct asc_dev *asc)
{
    (void)asc;
}
bool asc_cpu_running(struct asc_dev *asc)
{
    (void)asc;
    return true;
}
struct sart_dev *sart_init(const char *p)
{
    (void)p;
    return (struct sart_dev *)&dummy;
}
void sart_free(struct sart_dev *s)
{
    (void)s;
}
struct rtkit_dev *rtkit_init(const char *name, struct asc_dev *asc, void *dart, void *ops,
                             struct sart_dev *sart, bool sram)
{
    (void)name;
    (void)asc;
    (void)dart;
    (void)ops;
    (void)sart;
    (void)sram;
    return (struct rtkit_dev *)&dummy;
}
bool rtkit_boot(struct rtkit_dev *r)
{
    (void)r;
    if (S.f.boot_fails)
        return false;
    S.regs[R_BOOT / 4] = BOOT_OK;
    S.s.boots++;
    sim_event('B');
    return true;
}
bool rtkit_quiesce(struct rtkit_dev *r)
{
    (void)r;
    if (S.f.quiesce_fails)
        return false;
    S.regs[R_BOOT / 4] = 0;
    S.s.quiesces++;
    sim_event('Q');
    return true;
}
bool rtkit_quiesce_ap(struct rtkit_dev *r)
{
    (void)r;
    return true;
}
bool rtkit_sleep(struct rtkit_dev *r)
{
    (void)r;
    return true;
}
void rtkit_stop(struct rtkit_dev *r)
{
    (void)r;
}
void rtkit_free(struct rtkit_dev *r)
{
    (void)r;
}
int rtkit_recv(struct rtkit_dev *r, struct rtkit_message *msg)
{
    (void)r;
    (void)msg;
    tick();
    return 0;
}
int pmgr_reset(int die, const char *name)
{
    (void)die;
    (void)name;
    return 0;
}
/* Upstream nvme.c's post-M4 and cold-handoff paths: never taken by the simulated T8152. */
u64 ticks_to_usecs(u64 ticks)
{
    return ticks;
}
bool asc_cpu_idle(struct asc_dev *asc)
{
    (void)asc;
    return false;
}
void asc_i2a_enable(struct asc_dev *asc, bool on)
{
    (void)asc;
    (void)on;
}
u32 asc_cpu_status(struct asc_dev *asc)
{
    (void)asc;
    return 0;
}
bool asc_recv(struct asc_dev *asc, struct asc_message *msg)
{
    (void)asc;
    (void)msg;
    return false;
}
bool rtkit_idle(struct rtkit_dev *r)
{
    (void)r;
    return true;
}
int rtkit_probe_hello(struct asc_dev *asc)
{
    (void)asc;
    return 0;
}
bool rtkit_boot_warm(struct rtkit_dev *r, u32 last_mask)
{
    (void)last_mask;
    return rtkit_boot(r);
}
bool sart_get_allowed(struct sart_dev *s, int index, u64 *paddr, size_t *sz)
{
    (void)s;
    (void)index;
    (void)paddr;
    (void)sz;
    return false;
}
bool sart_clear_entry(struct sart_dev *s, int index)
{
    (void)s;
    (void)index;
    return true;
}
bool sart_set_allowed(struct sart_dev *s, int index, u64 paddr, size_t sz)
{
    (void)s;
    (void)index;
    (void)paddr;
    (void)sz;
    return true;
}
void sart_release(struct sart_dev *s)
{
    (void)s;
}
int sart_get_entries(struct sart_dev *s, struct sart_entry *entries, int max)
{
    (void)s;
    (void)entries;
    (void)max;
    return 0;
}
bool sart_set_entries(struct sart_dev *s, const struct sart_entry *entries, int count)
{
    (void)s;
    (void)entries;
    (void)count;
    return true;
}
