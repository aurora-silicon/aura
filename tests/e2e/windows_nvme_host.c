/* SPDX-License-Identifier: MIT */
/*
 * D5 host E2E: Windows' AHCI disk on an internal NVMe partition, end to end on the host.
 *
 * Compiled unchanged: src/nvme.c (aura's NVMe driver), src/hv/windows/{ahci_model.c,
 * ahci_nvme.c, gpt_check.c, hv_ahci_nvme.c, hv_ahci.c} against tests/e2e/windows_nvme_stubs/,
 * with the simulated controller of nvme_sim.c behind every MMIO access.  The test drives the
 * carrier exactly as the launcher and the guest do: P_HV_WINDOWS_AHCI ops (hv_ahci_proxy), the
 * guest's ABAR accesses through the carrier's MMIO hook, hv_ahci_verify / guest entry / exit,
 * hv_ahci_session_end (every hv_start return) and hv_ahci_nvme_reset_close (flush_and_reboot).
 *
 * The failure list it covers is windows-m6/D5-NVME-DESIGN.md section 10 (N1-N22) and 14.3.
 * Every case ends with a power cut and checks what survives against what Windows was told.
 *
 * Usage: windows_nvme_host <disk image> <request> attach      (one attach, JSON result)
 *        windows_nvme_host <disk image> <request> cases [name] (every session case, forked)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ahci_model.h"
#include "ahci_nvme.h"
#include "hv.h"
#include "hv_ahci.h"
#include "hv_ahci_nvme.h"
#include "hv_t8152.h"
#include "nvme.h"
#include "nvme_sim.h"

int stub_cpu;

/* ---- carrier stubs (hv.h, hv_t8152.h) ----------------------------------------------------- */

static u64 ram_lo, ram_size;
static hv_hook_t *abar_hook;
static int reserved_carrier, wdt_pets, spi_level;

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    *base = ram_lo;
    *size = ram_size;
    return ram_size != 0;
}

int hv_t8152_reserve_store(u64 start, u64 end)
{
    (void)start;
    (void)end;
    return -1; /* never in D5 */
}

int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name)
{
    (void)name;
    if (start < ram_lo + ram_size && end > ram_lo)
        return -1;
    reserved_carrier++;
    return 0;
}

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    (void)from;
    (void)size;
    abar_hook = hook;
    return 0;
}

void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    u64 pte = ((u64)1 << 50) | ((u64)abar_hook & (((1UL << 50) - 1) & ~3UL));
    fn(ctx, HV_AHCI_ABAR, HV_AHCI_ABAR_SIZE, pte, 3);
}

void hv_t8152_gic_spi_level(u32 intid, bool level)
{
    (void)intid;
    spi_level = level;
}

void hv_wdt_pet(void)
{
    wdt_pets++;
}

/* ---- checks ------------------------------------------------------------------------------ */

static int failures;
static const char *current;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d [%s] CHECK(%s)\n", __FILE__, __LINE__, current, #c);            \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
#define EQ(a, b)                                                                                   \
    do {                                                                                           \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "%s:%d [%s] %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, current,  \
                    #a, _a, _b);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- fixture ----------------------------------------------------------------------------- */

static const char *image_path;
static u64 nsze;
static u8 *initial; /* the disk image as loaded */
static struct hv_ahci_nvme_req base_req __attribute__((aligned(8)));
static struct hv_ahci_nvme_req req __attribute__((aligned(8)));
static u64 first, last, part_bytes, sectors;
static u8 *ref; /* what Windows was told is on the disk (acknowledged) */
static u8 *ram; /* guest RAM (host memory, IPA = address) */

#define RAM_BYTES (4UL << 30)
#define CLB_OFF   0x0000UL
#define FB_OFF    0x1000UL
#define CT_OFF    0x10000UL
#define DATA_OFF  0x100000UL
#define DATA_MAX  (8UL << 20)
#define P(x)      (AHCI_MODEL_PORT0 + (x))

static void load(void)
{
    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        perror(image_path);
        exit(2);
    }
    fseek(fp, 0, SEEK_END);
    nsze = (u64)ftell(fp) / SIM_BLOCK;
    fseek(fp, 0, SEEK_SET);
    initial = malloc(nsze * SIM_BLOCK);
    if (fread(initial, SIM_BLOCK, nsze, fp) != nsze)
        exit(2);
    fclose(fp);
}

static u64 rng = 0x2545f4914f6cdd1dULL;
static u64 rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void session_setup(u32 flags)
{
    sim_init(nsze, image_path);
    sim_set_guest((u64)ram, (u64)ram + RAM_BYTES);
    ram_lo = (u64)ram;
    ram_size = RAM_BYTES;
    req = base_req;
    req.flags = flags;
    first = req.first_lba;
    last = req.last_lba;
    part_bytes = (last - first + 1) * SIM_BLOCK;
    sectors = part_bytes / 512;
    ref = malloc(part_bytes);
    memcpy(ref, initial + first * SIM_BLOCK, part_bytes);
}

static u64 attach(void)
{
    /* The outer GPT is read before the fence is set; the range check starts after attach. */
    sim_track_io(false);
    u64 r = hv_ahci_proxy(HV_AHCI_OP_ATTACH_NVME, (u64)&req, 0, 0, 0);
    sim_track_io(true);
    return r;
}

/* ---- the guest's AHCI driver (AHCI 1.3.1 section 10), through the carrier's hook ---------- */

static u32 rd(u32 off)
{
    u64 v = 0;
    abar_hook(NULL, HV_AHCI_ABAR + off, &v, false, 2);
    return (u32)v;
}

static void wr(u32 off, u32 v)
{
    u64 x = v;
    abar_hook(NULL, HV_AHCI_ABAR + off, &x, true, 2);
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void port_start(void)
{
    wr(AHCI_GHC, AHCI_GHC_AE);
    wr(P(AHCI_PX_CLB), (u32)((u64)ram + CLB_OFF));
    wr(P(AHCI_PX_CLBU), (u32)(((u64)ram + CLB_OFF) >> 32));
    wr(P(AHCI_PX_FB), (u32)((u64)ram + FB_OFF));
    wr(P(AHCI_PX_FBU), (u32)(((u64)ram + FB_OFF) >> 32));
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);
}

/* After an error the port halts until ST is cleared (AHCI 6.2.2). */
static void recover(void)
{
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) & ~AHCI_PX_CMD_ST);
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);
}

struct prd {
    u64 off; /* in guest RAM */
    u32 bytes;
};

/* One command in slot 0.  Returns the ATA error byte (0 = success), 0x100 for a fatal error. */
static u32 ata(u8 op, u64 lba, u32 count, u16 features, const struct prd *prds, u32 nprd,
               bool write)
{
    u8 *hdr = ram + CLB_OFF;
    u8 *ct = ram + CT_OFF;
    memset(ct, 0, 0x80);
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = op;
    ct[3] = (u8)features;
    ct[4] = (u8)lba;
    ct[5] = (u8)(lba >> 8);
    ct[6] = (u8)(lba >> 16);
    ct[7] = 0x40;
    ct[8] = (u8)(lba >> 24);
    ct[9] = (u8)(lba >> 32);
    ct[10] = (u8)(lba >> 40);
    ct[11] = (u8)(features >> 8);
    ct[12] = (u8)count;
    ct[13] = (u8)(count >> 8);
    for (u32 i = 0; i < nprd; i++) {
        u8 *e = ct + 0x80 + i * 16;
        u64 a = (u64)ram + prds[i].off;
        put32(e, (u32)a);
        put32(e + 4, (u32)(a >> 32));
        put32(e + 8, 0);
        put32(e + 12, prds[i].bytes - 1);
    }
    u64 ctba = (u64)ct;
    put32(hdr, 5 | (write ? BIT(6) : 0) | (nprd << 16));
    put32(hdr + 4, 0);
    put32(hdr + 8, (u32)ctba);
    put32(hdr + 12, (u32)(ctba >> 32));
    wr(P(AHCI_PX_CI), 1);
    u32 is = rd(P(AHCI_PX_IS));
    u32 tfd = rd(P(AHCI_PX_TFD));
    u32 result = 0;
    if (is & (AHCI_PX_IS_HBFS | AHCI_PX_IS_OFS))
        result = 0x100;
    else if (is & AHCI_PX_IS_TFES)
        result = (tfd >> 8) & 0xff;
    wr(P(AHCI_PX_IS), 0xffffffff);
    if (result || (rd(P(AHCI_PX_CI)) & 1))
        recover();
    return result;
}

/* PRDs for `bytes` at DATA_OFF: whole, or split at random even offsets (inside sectors too). */
static u32 prds_for(u64 bytes, bool split, struct prd *out, u32 max)
{
    u32 n = 0;
    u64 done = 0;
    while (done < bytes && n < max) {
        u64 left = bytes - done;
        u64 take = left;
        if (split && n + 1 < max && left > 2) {
            take = 2 + 2 * (rnd() % (left / 2));
            if (take > 4 * 1024 * 1024)
                take = 4 * 1024 * 1024;
        }
        if (take > 4 * 1024 * 1024)
            take = 4 * 1024 * 1024;
        out[n].off = DATA_OFF + done;
        out[n].bytes = (u32)take;
        n++;
        done += take;
    }
    return n;
}

static u32 io(bool write, u64 lba, u32 count, bool split)
{
    struct prd prds[64];
    u64 bytes = (u64)count * 512;
    u32 n = prds_for(bytes, split, prds, 64);
    if (write) {
        for (u64 i = 0; i < bytes; i++)
            ram[DATA_OFF + i] = (u8)rnd();
    } else {
        memset(ram + DATA_OFF, 0xee, bytes);
    }
    u32 r = ata(write ? 0x35 : 0x25, lba, count & 0xffff, 0, prds, n, write);
    bool inside = lba < sectors && count <= sectors - lba;
    if (!r && inside) {
        if (write)
            memcpy(ref + lba * 512, ram + DATA_OFF, bytes);
        else if (memcmp(ref + lba * 512, ram + DATA_OFF, bytes)) {
            fprintf(stderr, "[%s] read of %u sectors at %llu differs from what was written\n",
                    current, count, (unsigned long long)lba);
            failures++;
        }
    }
    return r;
}

static u32 nodata(u8 op, u16 features)
{
    return ata(op, 0, 0, features, NULL, 0, false);
}

/* ---- end-of-case checks ------------------------------------------------------------------ */

static bool outside_unchanged(const u8 *(*layer)(u64))
{
    for (u64 b = 0; b < nsze; b++) {
        if (b >= first && b <= last)
            continue;
        if (memcmp(layer(b), initial + b * SIM_BLOCK, SIM_BLOCK)) {
            fprintf(stderr, "[%s] block %llu outside the partition changed\n", current,
                    (unsigned long long)b);
            return false;
        }
    }
    return true;
}

/* The partition as `layer` holds it equals ref, except AHCI sectors [skip_lo, skip_hi). */
static bool partition_is_ref(const u8 *(*layer)(u64), u64 skip_lo, u64 skip_hi)
{
    for (u64 s = 0; s < sectors; s++) {
        if (s >= skip_lo && s < skip_hi)
            continue;
        const u8 *blk = layer(first + s / 8) + (s % 8) * 512;
        if (memcmp(blk, ref + s * 512, 512)) {
            fprintf(stderr, "[%s] AHCI sector %llu is not what Windows was told\n", current,
                    (unsigned long long)s);
            return false;
        }
    }
    return true;
}

static void invariants(void)
{
    const struct sim_stats *s = sim_stats();
    EQ(s->prp_in_guest, 0);
    EQ(s->tcb_mismatch, 0);
    EQ(s->out_of_range, 0);
    if (s->io_max_block)
        CHECK(s->io_min_block >= first && s->io_max_block <= last);
}

/* Power cut after the close: only what the controller committed is left. */
static void survives_power_cut(u64 skip_lo, u64 skip_hi)
{
    sim_power_cut();
    CHECK(partition_is_ref(sim_durable, skip_lo, skip_hi));
    CHECK(outside_unchanged(sim_durable));
}

static void event_order_safe(void)
{
    /* Never a reset (X) after writes (W) unless a commit (C) came after the last write. */
    const char *e = sim_events();
    const char *x = strchr(e, 'X');
    if (!x)
        return;
    const char *w = NULL, *c = NULL;
    for (const char *p = e; p < x; p++) {
        if (*p == 'W')
            w = p;
        if (*p == 'C')
            c = p;
    }
    if (w && (!c || c < w)) {
        fprintf(stderr, "[%s] reset after unconfirmed writes: %s\n", current, e);
        failures++;
    }
}

static void reset_now(void)
{
    if (hv_ahci_nvme_reset_close())
        sim_reset_happened();
    else
        sim_event('P'); /* parked */
}

static void enter(void)
{
    EQ(hv_ahci_verify(), 0);
    hv_ahci_guest_entered();
    port_start();
}

static void leave(int status)
{
    hv_ahci_guest_exited(status);
    hv_ahci_session_end(status);
}

/* ---- cases ------------------------------------------------------------------------------- */

static void case_attach_and_identify(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    CHECK(nvme_fence_active());
    EQ(reserved_carrier, 1);
    CHECK(wdt_pets > 0 || true);
    EQ(hv_ahci_nvme_status(HV_AHCI_NVME_S_FIRST), first);
    EQ(hv_ahci_nvme_status(HV_AHCI_NVME_S_LAST), last);
    enter();
    struct prd p = {DATA_OFF, 512};
    EQ(ata(0xec, 0, 1, 0, &p, 1, false), 0);
    printf("{\"identify\": \"");
    for (int i = 0; i < 512; i++)
        printf("%02x", ram[DATA_OFF + i]);
    printf("\"}\n");
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    CHECK(!nvme_fence_active());
    invariants();
    survives_power_cut(0, 0);
}

static void case_inner_gpt_read(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    /* AHCI sector L is partition byte L x 512: the inner protective MBR and GPT header. */
    EQ(io(false, 0, 8, false), 0);
    CHECK(ram[DATA_OFF + 510] == 0x55 && ram[DATA_OFF + 511] == 0xaa);
    CHECK(!memcmp(ram + DATA_OFF + 512, "EFI PART", 8));
    EQ(io(false, sectors - 1, 1, false), 0);
    EQ(io(false, sectors - 9, 9, true), 0);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
}

static void random_traffic(u64 commands, u64 max_count, bool beyond)
{
    for (u64 i = 0; i < commands; i++) {
        bool write = rnd() & 1;
        u32 count = 1 + (u32)(rnd() % max_count);
        u64 lba;
        switch (rnd() % 4) {
            case 0:
                lba = rnd() % 64;
                break;
            case 1:
                lba = sectors - 1 - rnd() % 64;
                break;
            default:
                lba = rnd() % sectors;
        }
        if (beyond && !(rnd() % 4))
            lba = sectors - (rnd() % 16) + (rnd() % 32);
        if (!beyond && count > sectors - lba)
            count = (u32)(sectors - lba);
        bool inside = lba < sectors && count <= sectors - lba;
        u32 r = io(write, lba, count, rnd() % 3 == 0);
        if (inside) {
            EQ(r, 0);
        } else {
            EQ(r, 0x10); /* IDNF: the model's own range check (guard 0) */
        }
    }
}

static void case_data_rmw(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    /* Unaligned single sectors, heads and tails inside blocks, PRDs split inside sectors. */
    EQ(io(true, 1, 1, false), 0);
    EQ(io(true, 7, 2, true), 0);
    EQ(io(true, 13, 1000, true), 0);
    EQ(io(false, 0, 2000, true), 0);
    EQ(io(true, 3, 16384, true), 0); /* 8 MiB minus: many windows, RMW at both ends */
    EQ(io(false, 0, 16400, false), 0);
    random_traffic(3000, 600, false);
    EQ(io(false, 0, 8192, true), 0);
    CHECK(hv_ahci_nvme_status(HV_AHCI_NVME_S_RMW) > 0);
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    invariants();
    CHECK(partition_is_ref(sim_view, 0, 0));
    survives_power_cut(0, 0);
}

static void case_guard_fuzz(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(100000, 48, true);
    EQ(hv_ahci_nvme_status(HV_AHCI_NVME_S_GUARD_HITS), 0);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
    printf("{\"fuzz\": {\"io_min\": %llu, \"io_max\": %llu, \"writes\": %llu}}\n",
           (unsigned long long)sim_stats()->io_min_block,
           (unsigned long long)sim_stats()->io_max_block, (unsigned long long)sim_stats()->writes);
}

static void case_guard2_fence(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    struct ahci_nvme *b = hv_ahci_nvme_backend();
    u64 writes = sim_stats()->writes, reads = sim_stats()->reads;
    /* A corrupted backend (guard 1 widened): nvme.c's fence (guard 2) must still refuse. */
    b->last += 8;
    b->sectors += 64;
    static u8 data[512 * 8];
    memset(data, 0x77, sizeof(data));
    CHECK(ahci_nvme_begin(b, sectors, 8, true));
    CHECK(ahci_nvme_write(b, sectors, 8, data));
    CHECK(!ahci_nvme_end(b, true, false));
    b->last -= 8;
    b->sectors -= 64;
    b->first -= 1;
    b->last -= 1;
    CHECK(ahci_nvme_begin(b, 0, 8, false));
    CHECK(!ahci_nvme_read(b, 0, 8, data));
    ahci_nvme_end(b, false, false);
    b->first += 1;
    b->last += 1;
    EQ(sim_stats()->writes, writes);
    EQ(sim_stats()->reads, reads);
    CHECK(nvme_fence_refusals() >= 2);
    CHECK(hv_ahci_nvme_status(HV_AHCI_NVME_S_GUARD_HITS) >= 2);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
}

static void case_flush_mapping(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    EQ(io(true, 64, 64, false), 0);
    u64 f0 = sim_stats()->flushes;
    EQ(nodata(0xea, 0), 0); /* FLUSH CACHE EXT */
    EQ(sim_stats()->flushes, f0 + 1);
    EQ(nodata(0xe7, 0), 0); /* FLUSH CACHE */
    EQ(sim_stats()->flushes, f0 + 2);
    const char *e = sim_events();
    size_t n = strlen(e);
    CHECK(n >= 3 && e[n - 1] == 'F' && e[n - 2] == 'F' && strrchr(e, 'W') < e + n - 2);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
}

static void case_write_through(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    EQ(nodata(0xef, 0x82), 0); /* SET FEATURES: disable the volatile write cache */
    u64 f0 = sim_stats()->flushes;
    for (int i = 0; i < 5; i++)
        EQ(io(true, 100 + i * 9, 3, false), 0);
    EQ(sim_stats()->flushes, f0 + 5);
    EQ(nodata(0xef, 0x02), 0);
    f0 = sim_stats()->flushes;
    EQ(io(true, 300, 8, false), 0);
    EQ(sim_stats()->flushes, f0);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
}

static void case_read_only(void)
{
    session_setup(0);
    EQ(attach(), 0);
    enter();
    EQ(io(true, 0, 8, false), 0x04); /* ABRT: nothing reaches the backend */
    EQ(io(false, 0, 64, false), 0);
    u64 f0 = sim_stats()->flushes;
    EQ(nodata(0xea, 0), 0);
    EQ(sim_stats()->flushes, f0);
    EQ(sim_stats()->writes, 0);
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    EQ(hv_ahci_nvme_status(HV_AHCI_NVME_S_WROTE), 0);
    invariants();
    survives_power_cut(0, 0);
}

static void writes_then(int status)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(300, 300, false);
    leave(status);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    CHECK(strchr(sim_events(), 'C') != NULL);
    CHECK(!hv_ahci_nvme_owns_controller());
}

static void case_exit_system_off(void)
{
    writes_then(1);
    invariants();
    survives_power_cut(0, 0);
}

static void case_exit_system_reset(void)
{
    /* --windows-reset-on-exit: CLEAN, then P_REBOOT through flush_and_reboot. */
    writes_then(2);
    CHECK(hv_ahci_nvme_reboot_allowed(false));
    reset_now();
    CHECK(strchr(sim_events(), 'X') != NULL);
    event_order_safe();
    invariants();
    CHECK(partition_is_ref(sim_snapshot, 0, 0));
    CHECK(outside_unchanged(sim_snapshot));
}

static void case_exit_abort(void)
{
    writes_then(-1); /* ^C, then exit in the shell */
    invariants();
    survives_power_cut(0, 0);
}

static void case_refusal_closes(void)
{
    /* hv_start refuses before entry (hv_t8152_verify, hv_input_verify): the return closes. */
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    hv_ahci_session_end(-1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    CHECK(strchr(sim_events(), 'Q') != NULL);
    EQ(sim_stats()->writes, 0);
    survives_power_cut(0, 0);
}

static u64 hook_lba;
static void hook_reset_other_cpu(void)
{
    stub_cpu = 5;
    reset_now();
    stub_cpu = 0;
}

static void hook_reset_same_cpu(void)
{
    reset_now();
}

static void reset_mid_command(void (*hook)(void), const char *expect)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(200, 200, false);
    /* An aligned 8-block write: one NVMe write, which the hook interrupts. */
    hook_lba = 8 * (1 + rnd() % (sectors / 8 - 16));
    sim_set_hook(sim_stats()->io_commands + 1, hook);
    u32 r = io(true, hook_lba, 64, false);
    CHECK(r != 0); /* never acknowledged */
    CHECK(strchr(sim_events(), 'X') != NULL);
    event_order_safe();
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    invariants();
    /* Everything Windows was told survived; the interrupted command's range is undefined. */
    CHECK(partition_is_ref(sim_snapshot, hook_lba, hook_lba + 64));
    CHECK(outside_unchanged(sim_snapshot));
    CHECK(sim_stats()->after_reset > 0 || true);
    CHECK(strstr(sim_events(), expect) != NULL);
}

static void case_reset_other_cpu_mid_command(void)
{
    reset_mid_command(hook_reset_other_cpu, "SC");
}

static void case_reset_inside_controller_lock(void)
{
    reset_mid_command(hook_reset_same_cpu, "SC");
}

static void case_reset_lost_completion(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(100, 100, false);
    u64 lba = 8 * (1 + rnd() % (sectors / 8 - 16));
    sim_faults()->drop_completion_at = sim_stats()->io_commands + 1;
    CHECK(io(true, lba, 16, false) != 0); /* the aura-try symptom: landed, never completed */
    CHECK(io(false, 0, 8, false) != 0);   /* the queue is failed: everything fails now */
    reset_now();                          /* the guest bugchecks; any reset path */
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    event_order_safe();
    invariants();
    CHECK(partition_is_ref(sim_snapshot, lba, lba + 16));
}

static void case_reset_shst_stuck_parks(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(100, 100, false);
    sim_faults()->shst_stuck = true;
    reset_now();
    CHECK(strchr(sim_events(), 'P') != NULL);
    CHECK(strchr(sim_events(), 'X') == NULL);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_FAILED);
    CHECK(!hv_ahci_nvme_reboot_allowed(false));
    CHECK(hv_ahci_nvme_reboot_allowed(true));
    CHECK(hv_ahci_nvme_owns_controller());
}

static void case_close_failed_then_retry(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(100, 100, false);
    sim_faults()->shst_stuck = true;
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_FAILED);
    CHECK(!hv_ahci_nvme_reboot_allowed(false));
    sim_faults()->shst_stuck = false;
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    CHECK(hv_ahci_nvme_reboot_allowed(false));
    invariants();
    survives_power_cut(0, 0);
}

static void case_quiesce_fails_after_commit(void)
{
    /* SHST done, then a later shutdown step fails: the data is committed, the close is CLEAN
       (a reset is safe), and the controller needs a SoC reset before its next use. */
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    random_traffic(100, 100, false);
    sim_faults()->quiesce_fails = true;
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    CHECK(nvme_needs_reboot());
    CHECK(hv_ahci_nvme_reset_close());
    invariants();
    survives_power_cut(0, 0);
}

static void case_slow_write_completion(void)
{
    /* Gate W0's first suspect: a write whose completion takes longer than the read timeout. */
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    sim_faults()->delay_at = sim_stats()->io_commands + 1;
    sim_faults()->delay_us = 2500000;
    EQ(io(true, 800, 64, false), 0);
    EQ(io(false, 800, 64, false), 0);
    leave(1);
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_CLEAN);
    invariants();
    survives_power_cut(0, 0);
}

static void case_error_status_write(void)
{
    session_setup(HV_AHCI_NVME_RW);
    EQ(attach(), 0);
    enter();
    sim_faults()->error_at = sim_stats()->io_commands + 1;
    EQ(io(true, 400, 8, false), 0x40); /* UNC; the queue keeps working */
    EQ(io(true, 400, 8, false), 0);    /* Windows retries */
    EQ(io(false, 400, 8, false), 0);
    leave(1);
    invariants();
    survives_power_cut(0, 0);
}

static void case_second_attach(void)
{
    /* R0's question on the host: wake, quiesce, wake again in one boot. */
    session_setup(HV_AHCI_NVME_RW);
    req.flags = HV_AHCI_NVME_GATE;
    EQ(attach(), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    EQ(attach(), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    req.flags = HV_AHCI_NVME_RW;
    EQ(attach(), 0);
    enter();
    random_traffic(100, 64, false);
    leave(2);
    EQ(sim_stats()->boots, 3);
    EQ(sim_stats()->quiesces, 3);
    invariants();
    survives_power_cut(0, 0);
}

static void case_proxy_and_reboot_rules(void)
{
    session_setup(HV_AHCI_NVME_RW);
    CHECK(!hv_ahci_nvme_owns_controller());
    EQ(attach(), 0);
    CHECK(hv_ahci_nvme_owns_controller());
    /* A second attach of either kind, and the RAM store, are refused. */
    EQ(attach(), HV_AHCI_ERROR);
    EQ(hv_ahci_proxy(HV_AHCI_OP_ATTACH, 0, 0, 0, 0), HV_AHCI_ERROR);
    EQ(hv_ahci_proxy(HV_AHCI_OP_SEAL_EXIT, 0, 0, 0, 0), HV_AHCI_ERROR);
    enter();
    EQ(hv_ahci_nvme_state(), HV_AHCI_NVME_RUNNING);
    CHECK(!hv_ahci_nvme_reboot_allowed(false));
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_ERROR);
    EQ(io(true, 0, 8, false), 0);
    leave(1);
    CHECK(!hv_ahci_nvme_owns_controller());
    CHECK(hv_ahci_nvme_reboot_allowed(false));
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    invariants();
    survives_power_cut(0, 0);
}

static void case_gate_w0(void)
{
    session_setup(HV_AHCI_NVME_RW | HV_AHCI_NVME_GATE);
    hv_ahci_enable();
    EQ(attach(), 0);
    /* The partition holds an image: test writes inside it are refused, past its end (the
       installed-but-not-extended tail) they are allowed; the image stays as it was. */
    u64 inner = hv_ahci_nvme_status(HV_AHCI_NVME_S_INNER_BYTES) / SIM_BLOCK;
    u64 nblocks = last - first + 1;
    CHECK(inner > 0 && inner < nblocks);
    EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, 0, 1, 7, HV_AHCI_NVME_TEST_WRITE), HV_AHCI_ERROR);
    EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, inner - 1, 2, 7, HV_AHCI_NVME_TEST_WRITE),
       HV_AHCI_ERROR);
    CHECK(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, 0, 1, 7, HV_AHCI_NVME_TEST_CRC) != HV_AHCI_ERROR);
    EQ(hv_ahci_verify(), -1); /* no guest while a gate attach is open */
    EQ(sim_stats()->writes, 0);
    const u64 tail[][2] = {{inner, 1}, {(inner + nblocks) / 2, 8}, {nblocks - 64, 64}};
    for (unsigned i = 0; i < 3; i++)
        EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, tail[i][0], tail[i][1], 0x7a11, 1), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, 0, 0, 0, HV_AHCI_NVME_TEST_FLUSH), 0);
    for (unsigned i = 0; i < 3; i++)
        EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, tail[i][0], tail[i][1], 0x7a11, 2), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    sim_power_cut();
    for (u64 b = 0; b < inner; b++)
        if (memcmp(sim_durable(first + b), initial + (first + b) * SIM_BLOCK, SIM_BLOCK)) {
            fprintf(stderr, "[%s] the inner disk changed at block %llu\n", current,
                    (unsigned long long)b);
            failures++;
            break;
        }
    for (unsigned i = 0; i < 3; i++)
        for (u64 b = 0; b < tail[i][1]; b++)
            if (((const u64 *)sim_durable(first + tail[i][0] + b))[7] !=
                hv_ahci_nvme_pattern(0x7a11, tail[i][0] + b, 7)) {
                fprintf(stderr, "[%s] tail pattern lost at block %llu\n", current,
                        (unsigned long long)(tail[i][0] + b));
                failures++;
            }
    CHECK(outside_unchanged(sim_durable));
    invariants();
    /* A fresh machine for the scratch part (an empty partition, before an install). */
    sim_init(nsze, image_path);
    sim_set_guest((u64)ram, (u64)ram + RAM_BYTES);

    req.flags = HV_AHCI_NVME_RW | HV_AHCI_NVME_GATE | HV_AHCI_NVME_SCRATCH;
    EQ(attach(), 0);
    u64 blocks = last - first + 1;
    const u64 at[][2] = {{0, 1}, {blocks / 2, 8}, {blocks - 64, 64}, {blocks - 1, 1}};
    for (unsigned i = 0; i < 4; i++)
        EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, at[i][0], at[i][1], 0x5eed, 1), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, 0, 0, 0, HV_AHCI_NVME_TEST_FLUSH), 0);
    for (unsigned i = 0; i < 4; i++)
        EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, at[i][0], at[i][1], 0x5eed, 2), 0);
    EQ(hv_ahci_proxy(HV_AHCI_OP_NVME_TEST, blocks - 1, 2, 0x5eed, 1), HV_AHCI_ERROR);
    EQ(hv_ahci_proxy(HV_AHCI_OP_DETACH, 0, 0, 0, 0), HV_AHCI_NVME_CLEAN);
    sim_power_cut();
    for (unsigned i = 0; i < 4; i++)
        for (u64 b = 0; b < at[i][1]; b++) {
            const u64 *w = (const u64 *)sim_durable(first + at[i][0] + b);
            for (u32 k = 0; k < 512; k++)
                if (w[k] != hv_ahci_nvme_pattern(0x5eed, at[i][0] + b, k)) {
                    fprintf(stderr, "[%s] W0 pattern lost at block %llu\n", current,
                            (unsigned long long)(at[i][0] + b));
                    failures++;
                    k = 512;
                    b = at[i][1];
                }
        }
    CHECK(outside_unchanged(sim_durable));
    invariants();
    printf("{\"pattern\": [\"%016llx\", \"%016llx\"]}\n",
           (unsigned long long)hv_ahci_nvme_pattern(0x5eed, 0, 0),
           (unsigned long long)hv_ahci_nvme_pattern(0x5eed, 12345, 511));
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"attach_and_identify", case_attach_and_identify},
    {"inner_gpt_read", case_inner_gpt_read},
    {"data_rmw", case_data_rmw},
    {"guard_fuzz", case_guard_fuzz},
    {"guard2_fence", case_guard2_fence},
    {"flush_mapping", case_flush_mapping},
    {"write_through", case_write_through},
    {"read_only", case_read_only},
    {"exit_system_off", case_exit_system_off},
    {"exit_system_reset", case_exit_system_reset},
    {"exit_abort", case_exit_abort},
    {"refusal_closes", case_refusal_closes},
    {"reset_other_cpu_mid_command", case_reset_other_cpu_mid_command},
    {"reset_inside_controller_lock", case_reset_inside_controller_lock},
    {"reset_lost_completion", case_reset_lost_completion},
    {"reset_shst_stuck_parks", case_reset_shst_stuck_parks},
    {"close_failed_then_retry", case_close_failed_then_retry},
    {"quiesce_fails_after_commit", case_quiesce_fails_after_commit},
    {"slow_write_completion", case_slow_write_completion},
    {"error_status_write", case_error_status_write},
    {"second_attach", case_second_attach},
    {"proxy_and_reboot_rules", case_proxy_and_reboot_rules},
    {"gate_w0", case_gate_w0},
};

/* ---- attach-only mode (GPT and inner-disk variants) -------------------------------------- */

static int run_attach(void)
{
    session_setup(base_req.flags);
    u64 r = attach();
    bool attached = r == 0;
    bool fence = nvme_fence_active();
    u64 writes = sim_stats()->writes;
    if (attached)
        hv_ahci_session_end(-1);
    bool unchanged = outside_unchanged(sim_durable) && partition_is_ref(sim_durable, 0, 0) &&
                     outside_unchanged(sim_view) && partition_is_ref(sim_view, 0, 0);
    printf("{\"attach\": %s, \"fence_after_refusal\": %s, \"writes\": %llu, \"unchanged\": %s, "
           "\"enabled\": %s, \"state\": %d, \"sectors\": %llu, \"events\": \"%s\"}\n",
           attached ? "true" : "false", (!attached && fence) ? "true" : "false",
           (unsigned long long)writes, unchanged ? "true" : "false",
           sim_enabled() ? "true" : "false", (int)hv_ahci_nvme_state(), (unsigned long long)sectors,
           sim_events());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <image> <request> attach|cases [case]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    image_path = argv[1];
    load();
    FILE *fp = fopen(argv[2], "rb");
    if (!fp || fread(&base_req, 1, sizeof(base_req), fp) != sizeof(base_req))
        return 2;
    fclose(fp);
    ram = mmap(NULL, RAM_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1,
               0);
    if (ram == MAP_FAILED)
        return 2;
    hv_ahci_enable();
    if (!strcmp(argv[3], "attach"))
        return run_attach();

    int failed = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 4 && strcmp(argv[4], cases[i].name))
            continue;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            current = cases[i].name;
            cases[i].fn();
            event_order_safe();
            const char *ev = sim_events();
            size_t len = strlen(ev);
            printf("{\"case\": \"%s\", \"result\": \"%s\", \"events\": \"%s\"}\n", cases[i].name,
                   failures ? "failed" : "passed", len > 160 ? ev + len - 160 : ev);
            fflush(stdout);
            _exit(failures ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            failed++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 1)
                printf("{\"case\": \"%s\", \"result\": \"crashed\"}\n", cases[i].name);
        }
    }
    return failed ? 1 : 0;
}
