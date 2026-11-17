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
 *  - the heartbeat while the guest runs; flag combinations; a changed disk is refused.
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

    munmap(map, len);
    return failures ? 1 : 0;
}
