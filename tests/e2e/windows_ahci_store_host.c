/* SPDX-License-Identifier: MIT */
/*
 * Host replay of the carrier's AHCI store glue, src/hv/windows/hv_ahci.c, compiled unchanged
 * (it is #included below so each simulated carrier session can reset its file-scope state)
 * against stub headers that tests/python/test_windows_t8152_ahci.py writes: types.h, utils.h,
 * string.h, soc.h, hv.h and hv_t8152.h.  The stubs supply a settable CNTPCT_EL0, the guest RAM
 * window, the denylist and hook calls, and record what the glue asks for.
 *
 * The window is real host memory: a reservation of 4 GiB of guest RAM (never touched) plus a
 * small store, so the descriptor, the zero-fill and the disk CRC run on actual bytes.
 *
 * Covered (D1-AHCI-DESIGN 4.4/4.5 and the review of a70881d9):
 *  - fresh attach, upload seal, entry, PSCI exit, exit seal, keep with the CRC check;
 *  - a fresh attach over a READY/RUNNING/SEALED store is refused unless OVERWRITE;
 *  - keep-unsealed records the disk CRC, so a failed entry does not lock the store out;
 *  - keep-unsealed is refused when CNTPCT_EL0 is below the store's last-alive stamp;
 *  - the heartbeat while the guest runs; flag combinations; a changed disk is refused;
 *  - the fatal-report trace (D3): the last access and the last command with its CPU, FIS, disk
 *    range and guest spans, a span refused at the store, an access still in progress.
 * Usage: windows_ahci_store_host
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "hv_ahci.c"

/* ---- stubs ----------------------------------------------------------------------------- */

u64 stub_cntpct = 1000;
u64 stub_cntfrq = 24000000;
u32 chip_id = T8152;
int stub_cpu;

static u64 stub_ram_lo, stub_ram_size;
static int stub_reserved, stub_hooked;
static hv_hook_t *stub_hook;

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    *base = stub_ram_lo;
    *size = stub_ram_size;
    return stub_ram_size != 0;
}

int hv_t8152_reserve_store(u64 start, u64 end)
{
    (void)start;
    (void)end;
    stub_reserved++;
    return 0;
}

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    (void)from;
    (void)size;
    stub_hook = hook;
    stub_hooked++;
    return 0;
}

void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    /* One software leaf: type SPTE_HOOK (1) in bits 52:50, the hook's address in 49:2. */
    u64 pte = ((u64)1 << 50) | ((u64)stub_hook & GENMASK(49, 2));
    fn(ctx, HV_AHCI_ABAR, HV_AHCI_ABAR_SIZE, pte, 3);
}

void hv_t8152_gic_spi_level(u32 intid, bool level)
{
    (void)intid;
    (void)level;
}

void memcpy128(void *dst, void *src, size_t size)
{
    memcpy(dst, src, size);
}

void memset64(void *dst, u64 value, size_t size)
{
    u64 *p = dst;
    for (size_t i = 0; i < size / 8; i++)
        p[i] = value;
}

/* ---- harness --------------------------------------------------------------------------- */

static int failures;
static const char *current;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d [%s] CHECK(%s)\n", __FILE__, __LINE__, current, #cond);         \
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

#define DISK  (8UL << 20)
#define STORE (HV_AHCI_STORE_ALIGN) /* store_base .. top: the disk, slack, the descriptor */

static u64 base, top;

/* A new carrier session (an in-place carrier reload): the glue's file-scope state is fresh. */
static void session(void)
{
    ahci_enabled = true;
    attached = entered = exited = false;
    exit_status = 0;
    desc = NULL;
    logged = events_logged = 0;
    longest_ticks = report_next = reported_commands = 0;
    memset(&trace_access, 0, sizeof(trace_access));
    memset(&trace_command, 0, sizeof(trace_command));
    stub_ram_lo = base;
    stub_ram_size = top - STORE - base; /* guest RAM ends where the store begins */
}

static u64 do_attach(u64 flags)
{
    return hv_ahci_proxy(HV_AHCI_OP_ATTACH, top - STORE, top, DISK, flags);
}

static struct hv_ahci_store_desc *D(void)
{
    return (struct hv_ahci_store_desc *)(top - HV_AHCI_DESC_SIZE);
}

static u8 *disk(void)
{
    return (u8 *)(top - STORE);
}

static void fill(u8 *p, u64 n, u32 seed)
{
    for (u64 i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (u8)(seed >> 16);
    }
}

static void note(const char *text)
{
    memset(D()->host_note, 0, sizeof(D()->host_note));
    memcpy(D()->host_note, text, strlen(text));
}

/* Fresh attach, upload, seal, enter, PSCI exit `status`, exit seal (if 1/2). */
static void run_fresh(u64 flags, u32 seed)
{
    session();
    EQ(do_attach(flags), 0);
    EQ(D()->state, HV_AHCI_LOADING);
    fill(disk(), DISK, seed);
    note("image sha256 0000000000000000000000000000000000000000000000000000000000000000 "
         "bytes 8388608 uploaded 2026-11-17T00:00:00Z");
    u32 crc = ahci_crc32(0, disk(), DISK);
    EQ(hv_ahci_proxy(HV_AHCI_OP_SEAL_UPLOAD, crc ^ 1, 0, 0, 0), HV_AHCI_ERROR);
    EQ(hv_ahci_proxy(HV_AHCI_OP_SEAL_UPLOAD, crc, 0, 0, 0), crc);
    EQ(D()->state, HV_AHCI_READY);
    EQ(hv_ahci_verify(), 0);
}

static void enter_and_exit(int status)
{
    hv_ahci_guest_entered();
    EQ(D()->state, HV_AHCI_RUNNING);
    stub_cntpct += 5 * stub_cntfrq;
    hv_ahci_guest_exited(status);
}

static void begin(const char *name)
{
    current = name;
}

static void end(int before)
{
    printf("{\"case\": \"%s\", \"result\": \"%s\"}\n", current,
           failures == before ? "passed" : "failed");
}

/* ---- cases ----------------------------------------------------------------------------- */

static void case_lifecycle(void)
{
    int before = failures;
    begin("lifecycle");
    run_fresh(0, 1);
    u32 crc = D()->disk_crc32;
    enter_and_exit(2);
    u64 sealed = hv_ahci_proxy(HV_AHCI_OP_SEAL_EXIT, 0, 0, 0, 0);
    EQ(sealed, crc | 1ULL << 32);
    EQ(D()->state, HV_AHCI_SEALED);
    EQ(D()->generation, 1);
    EQ(D()->alive_cntpct, stub_cntpct);
    /* Keep: verified against the sealed CRC. */
    stub_cntpct += stub_cntfrq;
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), 0);
    EQ(D()->state, HV_AHCI_READY);
    EQ(hv_ahci_verify(), 0);
    /* A changed disk is refused on the next keep. */
    session();
    disk()[12345] ^= 0x40;
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), HV_AHCI_ERROR);
    disk()[12345] ^= 0x40;
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), 0);
    /* Not-a-PSCI exit: the seal is refused, the store stays RUNNING. */
    enter_and_exit(-1);
    EQ(hv_ahci_proxy(HV_AHCI_OP_SEAL_EXIT, 0, 0, 0, 0), HV_AHCI_ERROR);
    EQ(D()->state, HV_AHCI_RUNNING);
    CHECK(D()->last_exit == -1);
    end(before);
}

static void case_flags(void)
{
    int before = failures;
    begin("flags");
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_OVERWRITE), HV_AHCI_ERROR);
    session();
    EQ(do_attach(HV_AHCI_ATTACH_ACCEPT_UNSEALED), HV_AHCI_ERROR);
    session();
    EQ(do_attach(1u << 7), HV_AHCI_ERROR);
    session();
    ahci_enabled = false;
    EQ(do_attach(0), HV_AHCI_ERROR);
    end(before);
}

/* F10 / review: a fresh attach must not zero a store someone may still want. */
static void case_overwrite_guard(void)
{
    int before = failures;
    begin("overwrite_guard");
    run_fresh(HV_AHCI_ATTACH_OVERWRITE, 2);
    enter_and_exit(1);
    EQ(hv_ahci_proxy(HV_AHCI_OP_SEAL_EXIT, 0, 0, 0, 0) >> 32, 1);
    u32 crc = ahci_crc32(0, disk(), DISK);
    struct hv_ahci_store_desc saved = *D();
    /* SEALED: refused, nothing written. */
    session();
    EQ(do_attach(0), HV_AHCI_ERROR);
    EQ(ahci_crc32(0, disk(), DISK), crc);
    CHECK(!memcmp(D(), &saved, sizeof(saved)));
    /* READY and RUNNING too. */
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), 0);
    session();
    EQ(do_attach(0), HV_AHCI_ERROR);
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), 0);
    hv_ahci_guest_entered();
    session();
    EQ(D()->state, HV_AHCI_RUNNING);
    EQ(do_attach(0), HV_AHCI_ERROR);
    EQ(ahci_crc32(0, disk(), DISK), crc);
    /* With OVERWRITE: zeroed and LOADING. */
    session();
    EQ(do_attach(HV_AHCI_ATTACH_OVERWRITE), 0);
    EQ(D()->state, HV_AHCI_LOADING);
    EQ(D()->generation, 0);
    static u8 zeros[DISK];
    EQ(ahci_crc32(0, disk(), DISK), ahci_crc32(0, zeros, DISK));
    /* LOADING (an interrupted upload) and garbage are overwritten without the flag. */
    session();
    EQ(do_attach(0), 0);
    session();
    memset(D(), 0x5a, 0x80);
    EQ(do_attach(0), 0);
    end(before);
}

/* Review finding: accept-unsealed, then a failed entry, must not lock the store out. */
static void case_unsealed_then_failed_entry(void)
{
    int before = failures;
    begin("unsealed_failed_entry");
    run_fresh(HV_AHCI_ATTACH_OVERWRITE, 3);
    hv_ahci_guest_entered(); /* the guest runs, writes, and the carrier dies: no exit */
    fill(disk() + 4096, 4096, 33);
    u32 now_crc = ahci_crc32(0, disk(), DISK);
    stub_cntpct += 30 * stub_cntfrq;
    /* Keep without the unsealed flag: refused. */
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), HV_AHCI_ERROR);
    /* Keep-unsealed: READY with the CRC of what the disk holds now. */
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_ACCEPT_UNSEALED), 0);
    EQ(D()->state, HV_AHCI_READY);
    EQ(D()->disk_crc32, now_crc);
    /* Entry fails (hv_t8152_verify refuses, Ctrl-C, ...): hv_ahci_guest_entered never runs. */
    /* The next session keeps it with either flag set. */
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP), 0);
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_ACCEPT_UNSEALED), 0);
    EQ(D()->state, HV_AHCI_READY);
    end(before);
}

/* Review finding: keep-unsealed after a SoC reset (the counter went backwards) is refused. */
static void case_unsealed_after_reset(void)
{
    int before = failures;
    begin("unsealed_after_reset");
    stub_cntpct = 600 * stub_cntfrq;
    run_fresh(HV_AHCI_ATTACH_OVERWRITE, 4);
    hv_ahci_guest_entered();
    /* The heartbeat: at most every 10 s while the guest runs. */
    u64 entered_at = D()->alive_cntpct;
    EQ(entered_at, stub_cntpct);
    stub_cntpct += 11 * stub_cntfrq;
    hv_ahci_report();
    EQ(D()->alive_cntpct, stub_cntpct);
    u64 alive = stub_cntpct;
    stub_cntpct += 2 * stub_cntfrq;
    hv_ahci_report(); /* rate limited: unchanged */
    EQ(D()->alive_cntpct, alive);
    /* The carrier hangs; a hard reset restarts the counter; aura is back after 40 s. */
    stub_cntpct = 40 * stub_cntfrq;
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_ACCEPT_UNSEALED), HV_AHCI_ERROR);
    EQ(D()->state, HV_AHCI_RUNNING);
    /* An in-place reload (the counter kept running) is accepted. */
    stub_cntpct = alive + 60 * stub_cntfrq;
    session();
    EQ(do_attach(HV_AHCI_ATTACH_KEEP | HV_AHCI_ATTACH_ACCEPT_UNSEALED), 0);
    EQ(D()->state, HV_AHCI_READY);
    end(before);
}

/* ---- the fatal-report trace (hv_fatal.h, D3) --------------------------------------------- */

#define P(x) (AHCI_MODEL_PORT0 + (x))

static u32 mmio_rd(u32 off)
{
    u64 v[2] = {0, 0};
    hv_ahci_mmio(NULL, HV_AHCI_ABAR + off, v, false, 2);
    return (u32)v[0];
}

static void mmio_wr(u32 off, u32 value)
{
    u64 v[2] = {value, 0};
    hv_ahci_mmio(NULL, HV_AHCI_ABAR + off, v, true, 2);
}

static void put32(u8 *p, u32 v)
{
    memcpy(p, &v, 4);
}

/* One READ DMA EXT in slot `slot` with two PRD entries; guest RAM is identity mapped. */
static void read_ext(u32 slot, u64 lba, u32 count, u64 buf0, u64 buf1)
{
    u64 clb = base + 0x1000, ctba = base + 0x3000 + slot * 0x100;
    u8 *hdr = (u8 *)clb + slot * 32;
    put32(hdr, 5 | 2u << 16);
    put32(hdr + 4, 0);
    put32(hdr + 8, (u32)ctba);
    put32(hdr + 12, (u32)(ctba >> 32));
    u8 *ct = (u8 *)ctba;
    memset(ct, 0, 0xa0);
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = 0x25;
    ct[4] = lba, ct[5] = lba >> 8, ct[6] = lba >> 16, ct[7] = 0x40;
    ct[8] = lba >> 24, ct[9] = lba >> 32, ct[10] = lba >> 40;
    ct[12] = count, ct[13] = count >> 8;
    u32 half = count * AHCI_MODEL_SECTOR / 2;
    put32(ct + 0x80, (u32)buf0);
    put32(ct + 0x84, (u32)(buf0 >> 32));
    put32(ct + 0x8c, half - 1);
    put32(ct + 0x90, (u32)buf1);
    put32(ct + 0x94, (u32)(buf1 >> 32));
    put32(ct + 0x9c, half - 1);
    mmio_wr(P(AHCI_PX_CI), 1u << slot);
}

static void case_fatal_trace(void)
{
    int before = failures;
    begin("fatal_trace");
    run_fresh(HV_AHCI_ATTACH_OVERWRITE, 5);
    hv_ahci_guest_entered();
    stub_cpu = 5;
    u64 clb = base + 0x1000, fb = base + 0x2000, buf0 = base + 0x10000, buf1 = base + 0x20000;
    mmio_wr(AHCI_GHC, AHCI_GHC_AE);
    mmio_wr(P(AHCI_PX_CLB), (u32)clb);
    mmio_wr(P(AHCI_PX_CLBU), (u32)(clb >> 32));
    mmio_wr(P(AHCI_PX_FB), (u32)fb);
    mmio_wr(P(AHCI_PX_FBU), (u32)(fb >> 32));
    mmio_wr(P(AHCI_PX_SERR), 0xffffffff);
    mmio_wr(P(AHCI_PX_IS), 0xffffffff);
    mmio_wr(P(AHCI_PX_CMD), mmio_rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_FRE);
    mmio_wr(P(AHCI_PX_CMD), mmio_rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);

    stub_cntpct += stub_cntfrq;
    read_ext(0, 16, 8, buf0, buf1);
    EQ(mmio_rd(P(AHCI_PX_CI)), 0);
    CHECK(!memcmp((void *)buf0, disk() + 16 * 512, 2048));
    CHECK(!memcmp((void *)buf1, disk() + 16 * 512 + 2048, 2048));
    /* The command record: CPU, FIS, disk range, spans; stable. */
    struct ahci_trace *c = &trace_command;
    EQ(c->seq & 1, 0);
    EQ(c->cpu, 5);
    EQ(c->off, P(AHCI_PX_CI));
    EQ(c->val, 1);
    EQ(c->commands, 1);
    EQ(c->slot, 0);
    EQ(c->fis[2], 0x25);
    EQ(c->io_lba, 16);
    EQ(c->io_sectors, 8);
    EQ(c->io_write, 0);
    CHECK(c->spans >= 6);
    int saw_cfis = 0, saw_buf1 = 0;
    for (u64 i = 0; i < TRACE_SPANS && i < c->spans; i++) {
        saw_cfis |= c->span[i].ipa == base + 0x3000 && c->span[i].len == 64 && c->span[i].ok;
        saw_buf1 |= c->span[i].ipa == buf1 && c->span[i].len == 2048 && c->span[i].write &&
                    c->span[i].ok;
    }
    CHECK(saw_cfis || c->spans > TRACE_SPANS);
    CHECK(saw_buf1);
    /* A register read afterwards is the last access, not the last command. */
    mmio_rd(P(AHCI_PX_IS));
    EQ(trace_access.off, P(AHCI_PX_IS));
    EQ(trace_access.write, 0);
    EQ(trace_access.seq & 1, 0);
    EQ(trace_command.fis[2], 0x25);

    /* A data buffer in the store (the first byte past guest RAM): refused, recorded. */
    stub_cpu = 2;
    read_ext(1, 24, 2, buf0, ram_hi);
    EQ(trace_command.cpu, 2);
    EQ(trace_command.slot, 1);
    int refused = 0;
    for (u64 i = 0; i < TRACE_SPANS && i < trace_command.spans; i++)
        refused |= trace_command.span[i].ipa == ram_hi && !trace_command.span[i].ok;
    CHECK(refused);
    EQ(model.stats.fatal, 1);

    /* An access still under way when another CPU reports. */
    trace_access.seq |= 1;
    printf("ahci-report-begin\n");
    hv_ahci_fatal_report(stub_cntpct + stub_cntfrq / 1000, stub_cntfrq);
    printf("ahci-report-end\n");
    trace_access.seq++;
    end(before);
}

int main(void)
{
    /* 4 GiB of guest RAM (reserved, untouched) + the store, the store 32 MiB aligned. */
    u64 len = HV_AHCI_MIN_GUEST + 3 * STORE;
    u8 *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED)
        return 2;
    base = ((u64)map + HV_AHCI_DESC_SIZE - 1) & ~(u64)(HV_AHCI_DESC_SIZE - 1);
    u64 store = (base + HV_AHCI_MIN_GUEST + STORE - 1) & ~(u64)(STORE - 1);
    top = store + STORE;
    if (top > (u64)map + len)
        return 2;

    case_lifecycle();
    case_flags();
    case_overwrite_guard();
    case_unsealed_then_failed_entry();
    case_unsealed_after_reset();
    case_fatal_trace();

    munmap(map, len);
    return failures ? 1 : 0;
}
