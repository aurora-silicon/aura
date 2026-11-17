/* SPDX-License-Identifier: MIT */
/*
 * Host replay of the carrier's AHCI disk model (src/hv/windows/ahci_model.c) and of the
 * carrier-emulated SPI routing (src/hv/windows/hv_vdev_spi.c), both compiled unchanged.
 *
 * Expected values come from the public specifications, not from the model:
 * Intel AHCI 1.3.1 (register reset values, PxIS/PxCMD/PxTFD semantics, the command list,
 * command table, PRDT and FIS receive layouts, §6.2.2 error handling, §10.4 resets) and
 * T13 ACS-3 (command opcodes, status/error encodings, the §9.2 signature, §7.45.8 transfer
 * modes, §7.46 SET MULTIPLE MODE).  The IDENTIFY DEVICE bytes are printed and compared
 * against an independent Python encoder in tests/python.
 *
 * The fake guest RAM sits at a synthetic IPA above 1 TiB (0x10100000000, like the J804 guest
 * RAM window but not read from any machine) with canary bytes on both sides, as does the disk
 * store; every case checks them.  One JSON line per case.
 * Usage: windows_ahci_host <output dir> [fuzz iterations]
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ahci_model.h"
#include "hv_vdev_spi.h"

#define RAM_IPA   0x10100000000ULL /* synthetic */
#define RAM_SIZE  0x2800000ULL /* 40 MiB */
#define GUARD     0x10000ULL
#define CANARY    0xa5
#define SECTORS   81920ULL /* 40 MiB disk */
#define DISK_SIZE (SECTORS * 512)

/* Fake guest RAM layout (offsets from RAM_IPA). */
#define CLB_OFF   0x0000ULL
#define FB_OFF    0x1000ULL
#define CT_OFF(s) (0x10000ULL + (u64)(s) * 0x10000ULL)
#define BIGCT_OFF 0x400000ULL /* room for 65535 PRD entries */
#define DATA_OFF  0x800000ULL /* 32 MiB of data buffers up to RAM_SIZE */
#define P(x)      (AHCI_MODEL_PORT0 + (x))

struct fake {
    u8 *ram_alloc, *ram;
    u8 *disk_alloc, *disk;
    int level_changes;
    bool level;
    u64 reads, writes, flushes;
    bool fail_backend;
    bool out_of_range;
    int logs;
    char last_log[64];
    u64 last_log_a;
    /* The diagnostics observer (env->command): slots run and the last FIS it was shown. */
    int commands_seen;
    u32 last_slot;
    u8 last_fis[20];
};

static struct fake F;
static struct ahci_env E;
static struct ahci_model M;
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

/* ---- environment ---------------------------------------------------------------------- */

static void *env_span(void *opaque, u64 ipa, u64 len, bool write)
{
    (void)opaque;
    (void)write;
    if (!len || ipa < RAM_IPA)
        return NULL;
    u64 off = ipa - RAM_IPA;
    if (off > RAM_SIZE || len > RAM_SIZE - off)
        return NULL;
    return F.ram + off;
}

static bool env_read(void *opaque, u64 lba, u32 count, void *dst)
{
    (void)opaque;
    if (!count || lba > SECTORS || count > SECTORS - lba) {
        F.out_of_range = true;
        return false;
    }
    if (F.fail_backend)
        return false;
    memcpy(dst, F.disk + lba * 512, (size_t)count * 512);
    F.reads += count;
    return true;
}

static bool env_write(void *opaque, u64 lba, u32 count, const void *src)
{
    (void)opaque;
    if (!count || lba > SECTORS || count > SECTORS - lba) {
        F.out_of_range = true;
        return false;
    }
    if (F.fail_backend)
        return false;
    memcpy(F.disk + lba * 512, src, (size_t)count * 512);
    F.writes += count;
    return true;
}

static bool env_flush(void *opaque)
{
    (void)opaque;
    F.flushes++;
    return !F.fail_backend;
}

static void env_irq(void *opaque, bool level)
{
    (void)opaque;
    F.level_changes++;
    F.level = level;
}

static void env_log(void *opaque, const char *event, u64 a, u64 b)
{
    (void)opaque;
    (void)b;
    F.logs++;
    snprintf(F.last_log, sizeof(F.last_log), "%s", event);
    F.last_log_a = a;
}

static void env_command(void *opaque, u32 slot, const u8 *fis)
{
    (void)opaque;
    F.commands_seen++;
    F.last_slot = slot;
    memcpy(F.last_fis, fis, sizeof(F.last_fis));
}

static void canaries_check(void)
{
    for (u64 i = 0; i < GUARD; i++) {
        if (F.ram_alloc[i] != CANARY || F.ram_alloc[GUARD + RAM_SIZE + i] != CANARY ||
            F.disk_alloc[i] != CANARY || F.disk_alloc[GUARD + DISK_SIZE + i] != CANARY) {
            fprintf(stderr, "[%s] canary overwritten at guard byte %llu\n", current,
                    (unsigned long long)i);
            failures++;
            return;
        }
    }
    CHECK(!F.out_of_range);
}

static void fresh(void)
{
    memset(F.ram_alloc, CANARY, RAM_SIZE + 2 * GUARD);
    memset(F.disk_alloc, CANARY, DISK_SIZE + 2 * GUARD);
    memset(F.ram, 0, RAM_SIZE);
    memset(F.disk, 0, DISK_SIZE);
    F.level_changes = 0;
    F.level = false;
    F.reads = F.writes = F.flushes = 0;
    F.fail_backend = F.out_of_range = false;
    F.logs = 0;
    F.last_log[0] = 0;
    F.commands_seen = 0;
    E = (struct ahci_env){env_span, env_read, env_write, env_flush, env_irq,
                          env_log,  &F,       SECTORS,   env_command};
    ahci_model_init(&M, &E);
}

/* ---- a minimal driver, from AHCI 1.3.1 §10 ------------------------------------------------- */

static u32 rd(u32 off)
{
    return ahci_model_read(&M, off);
}

static void wr(u32 off, u32 v)
{
    ahci_model_write(&M, off, v, 0xffffffff);
}

static u8 *g(u64 off)
{
    return F.ram + off;
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* GHC.AE, port addresses, FRE, then ST (AHCI §10.1.2, §10.3.1). */
static void port_start(void)
{
    wr(AHCI_GHC, AHCI_GHC_AE);
    wr(P(AHCI_PX_CLB), (u32)(RAM_IPA + CLB_OFF));
    wr(P(AHCI_PX_CLBU), (u32)((RAM_IPA + CLB_OFF) >> 32));
    wr(P(AHCI_PX_FB), (u32)(RAM_IPA + FB_OFF));
    wr(P(AHCI_PX_FBU), (u32)((RAM_IPA + FB_OFF) >> 32));
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);
}

static void port_stop(void)
{
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) & ~AHCI_PX_CMD_ST);
}

struct prd {
    u64 addr; /* IPA */
    u32 bytes;
    bool irq;
    u32 raw_dbc; /* if nonzero, written verbatim as the DBC dword */
};

struct cmd {
    u8 fis[20];
    u32 cfl;
    bool atapi, write, reset, clear_busy;
    const struct prd *prds;
    u32 nprd;
    u64 ctba;  /* IPA; 0 = CT_OFF(slot) */
    u32 prdtl; /* 0 = nprd */
};

static void h2d(struct cmd *c, u8 op, u64 lba, u32 count, u16 features)
{
    memset(c, 0, sizeof(*c));
    c->cfl = 5;
    c->fis[0] = 0x27;
    c->fis[1] = 0x80; /* C: command register update */
    c->fis[2] = op;
    c->fis[3] = features;
    c->fis[4] = lba;
    c->fis[5] = lba >> 8;
    c->fis[6] = lba >> 16;
    c->fis[7] = 0x40; /* LBA mode */
    c->fis[8] = lba >> 24;
    c->fis[9] = lba >> 32;
    c->fis[10] = lba >> 40;
    c->fis[11] = features >> 8;
    c->fis[12] = count;
    c->fis[13] = count >> 8;
}

/* LBA28 commands carry LBA 27:24 in the DEVICE field (ACS-3 §7.21.3). */
static void h2d28(struct cmd *c, u8 op, u32 lba, u8 count)
{
    h2d(c, op, lba & 0xffffff, count, 0);
    c->fis[7] = 0x40 | ((lba >> 24) & 0xf);
}

static void build(u32 slot, const struct cmd *c)
{
    u64 ctba = c->ctba ? c->ctba : RAM_IPA + CT_OFF(slot);
    u8 *hdr = g(CLB_OFF + slot * 32);
    u32 prdtl = c->prdtl ? c->prdtl : c->nprd;
    u32 dw0 = (c->cfl & 0x1f) | (c->atapi ? BIT(5) : 0) | (c->write ? BIT(6) : 0) |
              (c->reset ? BIT(8) : 0) | (c->clear_busy ? BIT(10) : 0) | (prdtl << 16);
    put32(hdr, dw0);
    put32(hdr + 4, 0xdeadbeef); /* PRDBC: the HBA must overwrite it */
    put32(hdr + 8, (u32)ctba);
    put32(hdr + 12, (u32)(ctba >> 32));
    if (ctba < RAM_IPA || ctba - RAM_IPA >= RAM_SIZE - 0x80)
        return;
    u8 *ct = g(ctba - RAM_IPA);
    memset(ct, 0, 0x80);
    memcpy(ct, c->fis, sizeof(c->fis));
    for (u32 i = 0; i < c->nprd; i++) {
        u8 *e = ct + 0x80 + i * 16;
        put32(e, (u32)c->prds[i].addr);
        put32(e + 4, (u32)(c->prds[i].addr >> 32));
        put32(e + 8, 0);
        u32 dbc = c->prds[i].raw_dbc ? c->prds[i].raw_dbc : (c->prds[i].bytes - 1);
        put32(e + 12, dbc | (c->prds[i].irq ? BIT(31) : 0));
    }
}

static u32 prdbc(u32 slot)
{
    return le32(g(CLB_OFF + slot * 32 + 4));
}

/* Issue one slot and return PxCI's bit for it afterwards (0 = completed). */
static u32 issue(u32 slot, const struct cmd *c)
{
    build(slot, c);
    wr(P(AHCI_PX_CI), BIT(slot));
    return rd(P(AHCI_PX_CI)) & BIT(slot);
}

static void fill(u8 *p, u64 n, u32 seed)
{
    for (u64 i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = seed >> 16;
    }
}

static bool zero(const u8 *p, u64 n)
{
    for (u64 i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

static void begin(const char *name)
{
    current = name;
    fresh();
}

static int end(int before)
{
    canaries_check();
    printf("{\"case\": \"%s\", \"result\": \"%s\"}\n", current,
           failures == before ? "passed" : "failed");
    return failures == before;
}

/* A successful command's checks: completion, IS bits, TFD, D2H FIS at FB+0x40. */
static void expect_d2h_ok(u32 slot, u32 bytes)
{
    EQ(rd(P(AHCI_PX_CI)) & BIT(slot), 0);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DHRS);
    EQ(rd(P(AHCI_PX_IS)) & (AHCI_PX_IS_TFES | AHCI_PX_IS_HBFS | AHCI_PX_IS_OFS), 0);
    EQ(rd(P(AHCI_PX_TFD)), 0x50);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[0], 0x34);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[1] & 0x40, 0x40); /* I */
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[2], 0x50);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[3], 0);
    EQ(prdbc(slot), bytes);
}

/* An ATA error (AHCI §6.2.2): TFES, TFD ERR with `err`, the slot left in PxCI. */
static void expect_ata_error(u32 slot, u8 err)
{
    CHECK(rd(P(AHCI_PX_CI)) & BIT(slot));
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES);
    EQ(rd(P(AHCI_PX_TFD)), 0x51 | (u32)err << 8);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[0], 0x34);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[2], 0x51);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[3], err);
}

/* The edk2 / AHCI §6.2.2 recovery: clear ST, wait CR = 0, CLO if BSY/DRQ, restart. */
static void recover(void)
{
    port_stop();
    EQ(rd(P(AHCI_PX_CMD)) & (AHCI_PX_CMD_ST | AHCI_PX_CMD_CR), 0);
    EQ(rd(P(AHCI_PX_CI)), 0);
    if (rd(P(AHCI_PX_TFD)) & (ATA_STS_BSY | ATA_STS_DRQ)) {
        wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_CLO);
        EQ(rd(P(AHCI_PX_CMD)) & AHCI_PX_CMD_CLO, 0);
        EQ(rd(P(AHCI_PX_TFD)) & (ATA_STS_BSY | ATA_STS_DRQ), 0);
    }
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);
}

/* Prove the port works again with a small read. */
static void expect_usable(void)
{
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF, 512, false, 0};
    h2d(&c, 0x25, 1, 1, 0);
    c.prds = &p;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(3, &c), 0);
    expect_d2h_ok(3, 512);
}

/* ---- cases ----------------------------------------------------------------------------- */

static void case_reset_values(void)
{
    int before = failures;
    begin("reset_values");
    /* AHCI §3.1 */
    EQ(rd(AHCI_CAP), 0x81341f00);
    EQ(rd(AHCI_GHC), AHCI_GHC_AE); /* CAP.SAM = 1: AE reads 1 */
    EQ(rd(AHCI_IS), 0);
    EQ(rd(AHCI_PI), 1);
    EQ(rd(AHCI_VS), 0x00010301);
    EQ(rd(AHCI_CAP2), 0);
    EQ(rd(AHCI_CCC_CTL), 0);
    EQ(rd(AHCI_BOHC), 0);
    /* AHCI §3.3: device present, link up, signature received */
    EQ(rd(P(AHCI_PX_SSTS)), 0x133);
    EQ(rd(P(AHCI_PX_SIG)), 0x101);
    EQ(rd(P(AHCI_PX_TFD)) & 0xff, 0x50);
    EQ(rd(P(AHCI_PX_CMD)), AHCI_PX_CMD_SUD | AHCI_PX_CMD_POD);
    EQ(rd(P(AHCI_PX_CI)), 0);
    EQ(rd(P(AHCI_PX_SACT)), 0);
    EQ(rd(P(AHCI_PX_IE)), 0);
    /* PxIS.PCS mirrors PxSERR.DIAG.X (AHCI §3.3.5) and clears only with it. */
    CHECK(rd(P(AHCI_PX_SERR)) & AHCI_SERR_DIAG_X);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PCS);
    wr(P(AHCI_PX_IS), AHCI_PX_IS_PCS);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PCS);
    wr(P(AHCI_PX_SERR), AHCI_SERR_DIAG_X);
    EQ(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PCS, 0);
    EQ(rd(P(AHCI_PX_SERR)), 0);
    /* Reserved / vendor space and port 1+ read 0 and ignore writes. */
    EQ(rd(0xa0), 0);
    wr(0xa0, 0x12345678);
    EQ(rd(0xa0), 0);
    EQ(rd(0x180), 0);
    wr(0x180 + AHCI_PX_CI, 1);
    EQ(rd(0x180 + AHCI_PX_CI), 0);
    EQ(rd(0x3ffc), 0);
    /* Read-only registers. */
    wr(AHCI_CAP, 0);
    EQ(rd(AHCI_CAP), 0x81341f00);
    wr(AHCI_PI, 0);
    EQ(rd(AHCI_PI), 1);
    wr(P(AHCI_PX_SIG), 0);
    EQ(rd(P(AHCI_PX_SIG)), 0x101);
    /* GHC.AE cannot be cleared (SAM = 1); HR completes inside the write. */
    wr(AHCI_GHC, 0);
    EQ(rd(AHCI_GHC), AHCI_GHC_AE);
    wr(AHCI_GHC, AHCI_GHC_HR);
    EQ(rd(AHCI_GHC), AHCI_GHC_AE);
    /* CLB is 1 KiB aligned, FB 256 bytes (AHCI §3.3.1, §3.3.3). */
    wr(P(AHCI_PX_CLB), 0x123457ff);
    EQ(rd(P(AHCI_PX_CLB)), 0x12345400);
    wr(P(AHCI_PX_FB), 0x123457ff);
    EQ(rd(P(AHCI_PX_FB)), 0x12345700);
    CHECK(!ahci_model_line(&M));
    end(before);
}

static void case_identify(void)
{
    int before = failures;
    begin("identify");
    port_start();
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF + 0x200, 512, true, 0};
    h2d(&c, 0xec, 0, 0, 0);
    c.prds = &p;
    c.nprd = 1;
    EQ(issue(0, &c), 0);
    /* PIO data-in ends with a PIO Setup FIS (D = 1, I = 1, E_Status) at FB+0x20. */
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PSS);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DPS); /* the PRD had I = 1 */
    EQ(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES, 0);
    const u8 *ps = g(FB_OFF + AHCI_RFIS_PSFIS);
    EQ(ps[0], 0x5f);
    EQ(ps[1] & 0x60, 0x60);
    EQ(ps[15], 0x50);
    EQ(ps[16] | ps[17] << 8, 512);
    EQ(rd(P(AHCI_PX_TFD)), 0x50);
    EQ(prdbc(0), 512);
    const u8 *d = g(DATA_OFF + 0x200);
    u8 sum = 0;
    for (int i = 0; i < 512; i++)
        sum += d[i];
    EQ(sum, 0);
    EQ(d[510], 0xa5);
    u16 words[256];
    ahci_model_identify(&M, words);
    for (int i = 0; i < 256; i++)
        EQ(d[2 * i] | d[2 * i + 1] << 8, words[i]);
    /* Capacity words 100..103 and 60..61. */
    EQ(words[100] | (u64)words[101] << 16 | (u64)words[102] << 32 | (u64)words[103] << 48, SECTORS);
    EQ(words[60] | (u32)words[61] << 16, SECTORS);
    printf("{\"identify\": \"");
    for (int i = 0; i < 512; i++)
        printf("%02x", d[i]);
    printf("\"}\n");
    /* With the write cache off (SET FEATURES 0x82) word 85 bit 5 clears. */
    h2d(&c, 0xef, 0, 0, 0x82);
    c.nprd = 0;
    EQ(issue(1, &c), 0);
    ahci_model_identify(&M, words);
    EQ(words[85] & 0x20, 0);
    h2d(&c, 0xef, 0, 0, 0x02);
    EQ(issue(1, &c), 0);
    ahci_model_identify(&M, words);
    EQ(words[85] & 0x20, 0x20);
    end(before);
}

static void case_dma_ext(void)
{
    int before = failures;
    begin("dma_ext");
    port_start();
    u8 *src = g(DATA_OFF);
    fill(src, 8192, 7);
    /* WRITE DMA EXT LBA 100, 16 sectors, three PRDs that split sectors. */
    struct prd wp[3] = {{RAM_IPA + DATA_OFF, 1536, false, 0},
                        {RAM_IPA + DATA_OFF + 1536, 4094, false, 0},
                        {RAM_IPA + DATA_OFF + 1536 + 4094, 2562, true, 0}};
    struct cmd c;
    h2d(&c, 0x35, 100, 16, 0);
    c.write = true;
    c.prds = wp;
    c.nprd = 3;
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 8192);
    /* The observer saw exactly this slot's FIS, once (D3 fatal report). */
    EQ(F.commands_seen, 1);
    EQ(F.last_slot, 0);
    CHECK(!memcmp(F.last_fis, c.fis, sizeof(c.fis)));
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DPS);
    CHECK(!memcmp(F.disk + 100 * 512, src, 8192));
    CHECK(zero(F.disk + 99 * 512, 512) && zero(F.disk + 116 * 512, 512));
    /* READ DMA EXT back into odd-sized (even byte) pieces at word-aligned addresses. */
    u8 *dst = g(DATA_OFF + 0x100000);
    struct prd rp[4] = {{RAM_IPA + DATA_OFF + 0x100000, 2, false, 0},
                        {RAM_IPA + DATA_OFF + 0x100002, 1000, false, 0},
                        {RAM_IPA + DATA_OFF + 0x100002 + 1000, 6000, false, 0},
                        {RAM_IPA + DATA_OFF + 0x100002 + 7000, 1190, false, 0}};
    wr(P(AHCI_PX_IS), 0xffffffff);
    h2d(&c, 0x25, 100, 16, 0);
    c.prds = rp;
    c.nprd = 4;
    EQ(issue(5, &c), 0);
    expect_d2h_ok(5, 8192);
    EQ(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DPS, 0);
    CHECK(!memcmp(dst, src, 8192));
    CHECK(zero(dst + 8192, 512));
    EQ((rd(P(AHCI_PX_CMD)) >> 8) & 0x1f, 5); /* CCS: the slot last processed */
    /* More PRD space than needed is fine; PRDBC is what moved. */
    struct prd big = {RAM_IPA + DATA_OFF + 0x200000, 0x400000, false, 0};
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &big;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(2, &c), 0);
    expect_d2h_ok(2, 512);
    /* LBA28 READ DMA / WRITE DMA, the device register carries LBA 27:24. */
    fill(F.disk + 0x1234 * 512, 1024, 9);
    h2d28(&c, 0xc8, 0x1234, 2);
    struct prd p2 = {RAM_IPA + DATA_OFF + 0x300000, 1024, false, 0};
    c.prds = &p2;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(1, &c), 0);
    expect_d2h_ok(1, 1024);
    CHECK(!memcmp(g(DATA_OFF + 0x300000), F.disk + 0x1234 * 512, 1024));
    fill(g(DATA_OFF + 0x300000), 1024, 11);
    h2d28(&c, 0xca, 0x10000 + 7, 2);
    c.write = true;
    c.prds = &p2;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(1, &c), 0);
    expect_d2h_ok(1, 1024);
    CHECK(!memcmp(g(DATA_OFF + 0x300000), F.disk + (0x10000 + 7) * 512, 1024));
    /* A medium error from the backend: UNC (ACS-3 §6.3.9). */
    F.fail_backend = true;
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &p2;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(4, &c);
    expect_ata_error(4, ATA_ERR_UNC);
    F.fail_backend = false;
    recover();
    expect_usable();
    EQ(M.stats.sectors_written, 18);
    end(before);
}

static void case_pio(void)
{
    int before = failures;
    begin("pio");
    port_start();
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF, 2048, false, 0};
    fill(g(DATA_OFF), 2048, 3);
    /* WRITE SECTORS EXT: PIO data-out ends with a D2H Register FIS. */
    h2d(&c, 0x34, 40, 4, 0);
    c.write = true;
    c.prds = &p;
    c.nprd = 1;
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 2048);
    CHECK(!memcmp(F.disk + 40 * 512, g(DATA_OFF), 2048));
    /* READ SECTORS (LBA28): PIO data-in ends with a PIO Setup FIS. */
    struct prd q = {RAM_IPA + DATA_OFF + 0x10000, 2048, false, 0};
    h2d28(&c, 0x20, 40, 4);
    c.prds = &q;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PSS);
    EQ(prdbc(0), 2048);
    CHECK(!memcmp(g(DATA_OFF + 0x10000), F.disk + 40 * 512, 2048));
    /* READ SECTOR(S) count 0 = 256 sectors (ACS-3 §7.28). */
    struct prd r = {RAM_IPA + DATA_OFF + 0x100000, 256 * 512, false, 0};
    h2d28(&c, 0x20, 0, 0);
    c.prds = &r;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    EQ(prdbc(0), 256 * 512);
    end(before);
}

static void case_count_zero(void)
{
    int before = failures;
    begin("count_zero");
    port_start();
    fill(F.disk, 32ULL << 20, 21);
    /* READ DMA EXT count 0 = 65536 sectors = 32 MiB, eight 4 MiB PRDs (AHCI §4.2.3.3 max). */
    struct prd p[8];
    for (int i = 0; i < 8; i++)
        p[i] = (struct prd){RAM_IPA + DATA_OFF + (u64)i * 0x400000, 0x400000, i == 7, 0};
    struct cmd c;
    h2d(&c, 0x25, 0, 0, 0);
    c.prds = p;
    c.nprd = 8;
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 32u << 20);
    CHECK(!memcmp(g(DATA_OFF), F.disk, 32ULL << 20));
    EQ(M.stats.largest, 65536);
    /* READ DMA (LBA28) count 0 = 256 sectors. */
    h2d28(&c, 0xc8, 5, 0);
    c.prds = p;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 256 * 512);
    /* A DBC above 4 MiB - 1 is not representable: bit 22+ are reserved and ignored. */
    end(before);
}

static void case_range_errors(void)
{
    int before = failures;
    begin("range_errors");
    port_start();
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF, 0x400000, false, 0};
    memset(g(DATA_OFF), 0x77, 0x2000);
    /* The last sector reads; one past the end is IDNF, and no data moves. */
    h2d(&c, 0x25, SECTORS - 1, 1, 0);
    c.prds = &p;
    c.nprd = 1;
    EQ(issue(0, &c), 0);
    wr(P(AHCI_PX_IS), 0xffffffff);
    h2d(&c, 0x25, SECTORS - 1, 2, 0);
    memset(g(DATA_OFF), 0x77, 0x2000);
    issue(1, &c);
    expect_ata_error(1, ATA_ERR_IDNF);
    EQ(g(DATA_OFF)[0], 0x77);
    /* TFES halts the port (AHCI §6.2.2): later CI bits wait until ST is cleared. */
    h2d(&c, 0x25, 0, 1, 0);
    build(2, &c);
    wr(P(AHCI_PX_CI), BIT(2));
    EQ(rd(P(AHCI_PX_CI)), BIT(1) | BIT(2));
    EQ(g(DATA_OFF)[0], 0x77);
    recover();
    expect_usable();
    /* LBA48 near 2^48, and lba + count wrapping: IDNF, the backend is never asked. */
    u64 reads = F.reads;
    h2d(&c, 0x35, 0xffffffffffffULL, 0, 0);
    c.write = true;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_IDNF);
    recover();
    h2d(&c, 0x42, SECTORS - 4, 5, 0); /* READ VERIFY EXT past the end */
    c.nprd = 0;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_IDNF);
    recover();
    h2d(&c, 0x42, SECTORS - 4, 4, 0);
    c.nprd = 0;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 0);
    h2d28(&c, 0x40, 1000, 8); /* READ VERIFY (LBA28) */
    c.nprd = 0;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    EQ(F.reads, reads); /* READ VERIFY checks the range only */
    EQ(F.writes, 0);
    end(before);
}

static void case_prd_space(void)
{
    int before = failures;
    begin("prd_space");
    port_start();
    struct cmd c;
    fill(F.disk, 16 * 512, 5);
    /* Read, PRD space 4 KiB for 8 KiB: OFS is fatal (AHCI §6.1.x), nothing written. */
    struct prd small = {RAM_IPA + DATA_OFF, 4096, false, 0};
    memset(g(DATA_OFF), 0x33, 8192);
    h2d(&c, 0x25, 0, 16, 0);
    c.prds = &small;
    c.nprd = 1;
    issue(0, &c);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_OFS);
    CHECK(rd(P(AHCI_PX_CI)) & 1);
    EQ(g(DATA_OFF)[0], 0x33);
    EQ(g(DATA_OFF)[4095], 0x33);
    EQ(prdbc(0), 0);
    CHECK(ahci_model_line(&M) == false); /* GHC.IE is 0 */
    recover();
    expect_usable();
    /* Write with too little PRD space: ABRT, the store untouched, PRDBC exact (0). */
    h2d(&c, 0x35, 50, 16, 0);
    c.write = true;
    memset(g(DATA_OFF), 0x44, 4096);
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    EQ(prdbc(0), 0);
    CHECK(zero(F.disk + 50 * 512, 16 * 512));
    recover();
    /* PRDTL 0 on a data-in command: no space at all, OFS. */
    h2d(&c, 0x25, 0, 1, 0);
    c.nprd = 0;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_OFS);
    recover();
    expect_usable();
    end(before);
}

static void case_prd_format(void)
{
    int before = failures;
    begin("prd_format");
    port_start();
    struct cmd c;
    /* DBA bit 0 set (AHCI §4.2.3.3: DBA[0] reserved, word aligned): ABRT, no data. */
    struct prd odd_addr = {RAM_IPA + DATA_OFF + 1, 512, false, 0};
    memset(g(DATA_OFF), 0x55, 1024);
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &odd_addr;
    c.nprd = 1;
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    EQ(g(DATA_OFF)[1], 0x55);
    recover();
    /* An odd byte count (DBC bit 0 must be 1). */
    struct prd odd_len = {RAM_IPA + DATA_OFF, 0, false, 510}; /* 511 bytes */
    c.prds = &odd_len;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    /* CFL below 5 dwords, or above 16. */
    struct prd ok = {RAM_IPA + DATA_OFF, 512, false, 0};
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    c.cfl = 4;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    c.cfl = 17;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    /*
     * PRDTL 65535 with the table running past guest RAM: only the entries the transfer
     * consumes are read (one here), so the command succeeds.
     */
    u64 ct = RAM_IPA + RAM_SIZE - 0x1000;
    h2d(&c, 0x25, 0, 1, 0);
    c.cfl = 5;
    c.prds = &ok;
    c.nprd = 1;
    c.prdtl = 65535;
    c.ctba = ct;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 512);
    /* PRDTL 65535 of 2-byte entries inside RAM: bounded by the table, not the guest. */
    u64 big = RAM_IPA + BIGCT_OFF;
    u8 *e = g(BIGCT_OFF) + 0x80;
    for (u32 i = 0; i < 65535; i++) {
        put32(e + i * 16, (u32)(RAM_IPA + DATA_OFF + i * 2));
        put32(e + i * 16 + 4, (u32)((RAM_IPA + DATA_OFF) >> 32));
        put32(e + i * 16 + 8, 0);
        put32(e + i * 16 + 12, 1); /* 2 bytes */
    }
    memcpy(g(BIGCT_OFF), c.fis, 20);
    h2d(&c, 0x25, 0, 256, 0); /* 131072 bytes > 65535 * 2: OFS */
    memcpy(g(BIGCT_OFF), c.fis, 20);
    put32(g(CLB_OFF), 5 | 65535u << 16);
    put32(g(CLB_OFF + 8), (u32)big);
    put32(g(CLB_OFF + 12), (u32)(big >> 32));
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CI), 1);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_OFS);
    recover();
    h2d(&c, 0x25, 0, 128, 0); /* 65536 bytes in 32768 two-byte pieces */
    memcpy(g(BIGCT_OFF), c.fis, 20);
    fill(F.disk, 65536, 13);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CI), 1);
    EQ(rd(P(AHCI_PX_CI)), 0);
    EQ(prdbc(0), 65536);
    CHECK(!memcmp(g(DATA_OFF), F.disk, 65536));
    end(before);
}

/* Every guest span the HBA touches outside guest RAM is HBFS; nothing moves (F1). */
static void case_out_of_window(void)
{
    int before = failures;
    begin("out_of_window");
    struct cmd c;
    struct prd ok = {RAM_IPA + DATA_OFF, 512, false, 0};
    fill(F.disk, 4096, 1);

    struct {
        const char *what;
        u64 dba;
        u32 bytes;
    } bad[] = {
        {"below", RAM_IPA - 0x1000, 512},
        {"straddle-end", RAM_IPA + RAM_SIZE - 256, 512},
        {"above", RAM_IPA + RAM_SIZE, 512},
        {"wrap", 0xfffffffffffffe00ULL, 1024},
        {"zero", 0, 512},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fresh();
        port_start();
        struct prd p[2] = {{RAM_IPA + DATA_OFF, 512, false, 0}, {bad[i].dba, bad[i].bytes, 0, 0}};
        memset(g(DATA_OFF), 0x66, 1024);
        h2d(&c, 0x25, 0, (512 + bad[i].bytes) / 512, 0);
        c.prds = p;
        c.nprd = 2;
        issue(0, &c);
        CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_HBFS);
        CHECK(rd(P(AHCI_PX_CI)) & 1);
        EQ(g(DATA_OFF)[0], 0x66); /* the first, valid PRD was not filled either */
        EQ(F.reads, 0);
        CHECK(rd(P(AHCI_PX_TFD)) & (ATA_STS_DRQ | ATA_STS_BSY));
        /* writes too: the store stays untouched */
        h2d(&c, 0x35, 8, (512 + bad[i].bytes) / 512, 0);
        c.write = true;
        c.prds = p;
        c.nprd = 2;
        recover();
        issue(0, &c);
        CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_HBFS);
        CHECK(zero(F.disk + 8 * 512, 1024));
        recover();
        expect_usable();
        canaries_check();
    }

    /* CLB outside guest RAM. */
    fresh();
    wr(AHCI_GHC, AHCI_GHC_AE);
    wr(P(AHCI_PX_CLB), (u32)(RAM_IPA + RAM_SIZE));
    wr(P(AHCI_PX_CLBU), (u32)((RAM_IPA + RAM_SIZE) >> 32));
    wr(P(AHCI_PX_FB), (u32)(RAM_IPA + FB_OFF));
    wr(P(AHCI_PX_FBU), (u32)((RAM_IPA + FB_OFF) >> 32));
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_FRE | AHCI_PX_CMD_ST);
    wr(P(AHCI_PX_CI), 1);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_HBFS);
    /* FB outside guest RAM: the command runs, the FIS cannot be posted: HBFS. */
    fresh();
    wr(AHCI_GHC, AHCI_GHC_AE);
    wr(P(AHCI_PX_CLB), (u32)(RAM_IPA + CLB_OFF));
    wr(P(AHCI_PX_CLBU), (u32)((RAM_IPA + CLB_OFF) >> 32));
    wr(P(AHCI_PX_FB), 0xffffff00);
    wr(P(AHCI_PX_FBU), 0xffffffff);
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_FRE | AHCI_PX_CMD_ST);
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    issue(0, &c);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_HBFS);
    /* CTBA outside guest RAM. */
    fresh();
    port_start();
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    c.ctba = RAM_IPA - 0x80;
    issue(0, &c);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_HBFS);
    EQ(F.reads, 0);
    end(before);
}

static void case_port_state(void)
{
    int before = failures;
    begin("port_state");
    struct cmd c;
    struct prd ok = {RAM_IPA + DATA_OFF, 512, false, 0};
    /* CI written with ST = 0 is ignored (AHCI §3.3.14). */
    wr(AHCI_GHC, AHCI_GHC_AE);
    wr(P(AHCI_PX_CLB), (u32)(RAM_IPA + CLB_OFF));
    wr(P(AHCI_PX_CLBU), (u32)((RAM_IPA + CLB_OFF) >> 32));
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    build(0, &c);
    wr(P(AHCI_PX_CI), 1);
    EQ(rd(P(AHCI_PX_CI)), 0);
    EQ(F.reads, 0);
    /* ST with FRE = 0: the command completes, PxTFD and PxIS update, no FIS is posted. */
    memset(g(FB_OFF), 0xee, 256);
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_ST);
    EQ(rd(P(AHCI_PX_CMD)) & (AHCI_PX_CMD_CR | AHCI_PX_CMD_FR), AHCI_PX_CMD_CR);
    EQ(issue(0, &c), 0);
    EQ(rd(P(AHCI_PX_TFD)), 0x50);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DHRS);
    EQ(g(FB_OFF + 0x40)[0], 0xee);
    /* CLB/CLBU are read-only while ST = 1; FB/FBU while FRE = 1. */
    wr(P(AHCI_PX_CLB), 0x4000);
    EQ(rd(P(AHCI_PX_CLB)), (u32)(RAM_IPA + CLB_OFF));
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_ST | AHCI_PX_CMD_FRE);
    EQ(rd(P(AHCI_PX_CMD)) & AHCI_PX_CMD_FR, AHCI_PX_CMD_FR);
    wr(P(AHCI_PX_FB), (u32)(RAM_IPA + FB_OFF));
    wr(P(AHCI_PX_FBU), (u32)((RAM_IPA + FB_OFF) >> 32));
    EQ(rd(P(AHCI_PX_FB)), 0);
    /* SACT is stored while ST = 1; clearing ST clears CI, SACT, CR, CCS (AHCI §3.3.7). */
    wr(P(AHCI_PX_SACT), 0xf0);
    EQ(rd(P(AHCI_PX_SACT)), 0xf0);
    port_stop();
    EQ(rd(P(AHCI_PX_SACT)), 0);
    EQ(rd(P(AHCI_PX_CMD)) & (AHCI_PX_CMD_CR | AHCI_PX_CMD_ST | (0x1f << 8)), 0);
    wr(P(AHCI_PX_CMD), AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_FB), (u32)(RAM_IPA + FB_OFF)); /* FRE = 1: ignored */
    EQ(rd(P(AHCI_PX_FB)), 0);
    wr(P(AHCI_PX_CMD), 0);
    EQ(rd(P(AHCI_PX_CMD)) & AHCI_PX_CMD_FR, 0);
    wr(P(AHCI_PX_FB), (u32)(RAM_IPA + FB_OFF));
    EQ(rd(P(AHCI_PX_FB)), (u32)(RAM_IPA + FB_OFF));
    /* Several slots at once: ascending order, all complete, CCS = the last one. */
    port_start();
    for (u32 s = 0; s < 32; s += 7) {
        struct prd p = {RAM_IPA + DATA_OFF + s * 512, 512, false, 0};
        h2d(&c, 0x25, s, 1, 0);
        c.prds = &p;
        c.nprd = 1;
        build(s, &c);
    }
    wr(P(AHCI_PX_CI), BIT(0) | BIT(7) | BIT(14) | BIT(21) | BIT(28));
    EQ(rd(P(AHCI_PX_CI)), 0);
    EQ((rd(P(AHCI_PX_CMD)) >> 8) & 0x1f, 28);
    /* An error in the middle slot: earlier slots done, it and later ones stay in PxCI. */
    wr(P(AHCI_PX_IS), 0xffffffff);
    h2d(&c, 0x25, SECTORS, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    build(14, &c);
    wr(P(AHCI_PX_CI), BIT(0) | BIT(14) | BIT(28));
    EQ(rd(P(AHCI_PX_CI)), BIT(14) | BIT(28));
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES);
    recover();
    EQ(rd(P(AHCI_PX_CI)), 0);
    /* A CI write of 0, or of bits already set, does nothing. */
    wr(P(AHCI_PX_CI), 0);
    EQ(rd(P(AHCI_PX_CI)), 0);
    end(before);
}

static void case_irq_matrix(void)
{
    int before = failures;
    begin("irq_matrix");
    port_start();
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(AHCI_IS, 0xffffffff);
    CHECK(!F.level && !ahci_model_line(&M));
    struct cmd c;
    h2d(&c, 0xe7, 0, 0, 0); /* FLUSH CACHE: non-data, D2H with I = 1 */
    for (int ie = 0; ie < 2; ie++) {
        for (int pie = 0; pie < 2; pie++) {
            for (int run = 0; run < 2; run++) {
                wr(P(AHCI_PX_IS), 0xffffffff);
                wr(AHCI_IS, 1);
                wr(AHCI_GHC, AHCI_GHC_AE | (ie ? AHCI_GHC_IE : 0));
                wr(P(AHCI_PX_IE), pie ? AHCI_PX_IS_DHRS : AHCI_PX_IS_PSS);
                CHECK(!ahci_model_line(&M) && !F.level);
                if (run)
                    EQ(issue(0, &c), 0);
                bool want = ie && pie && run;
                EQ(ahci_model_line(&M), want);
                EQ(F.level, want);
                /* IS.IPS0 follows (PxIS & PxIE) regardless of GHC.IE (AHCI §3.1.3). */
                EQ(rd(AHCI_IS), (pie && run) ? 1 : 0);
                if (!want)
                    continue;
                /* IS.IPS is re-set while the port condition persists. */
                wr(AHCI_IS, 1);
                EQ(rd(AHCI_IS), 1);
                CHECK(F.level);
                /* Clearing PxIS then IS drops the line (the storahci ISR order). */
                wr(P(AHCI_PX_IS), AHCI_PX_IS_DHRS);
                CHECK(F.level);
                wr(AHCI_IS, 1);
                CHECK(!F.level && !ahci_model_line(&M));
                /* IE masking a still-set PxIS bit drops it as well. */
                EQ(issue(0, &c), 0);
                CHECK(F.level);
                wr(AHCI_GHC, AHCI_GHC_AE);
                CHECK(!F.level);
                wr(AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_IE);
                CHECK(F.level);
                /* GHC.HR drops the line and clears every interrupt state. */
                wr(AHCI_GHC, AHCI_GHC_HR);
                CHECK(!F.level);
                EQ(rd(AHCI_GHC), AHCI_GHC_AE);
                EQ(rd(AHCI_IS), 0);
                EQ(rd(P(AHCI_PX_IE)), 0);
                port_start();
            }
        }
    }
    /* Error bits interrupt too: TFES with TFEE (bit 30 of PxIE). */
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(AHCI_IS, 1);
    wr(AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_IE);
    wr(P(AHCI_PX_IE), AHCI_PX_IS_TFES);
    h2d(&c, 0x00, 0, 0, 0); /* NOP aborts */
    issue(0, &c);
    CHECK(F.level);
    /* The callback fires only on a change of level. */
    int changes = F.level_changes;
    wr(P(AHCI_PX_IE), AHCI_PX_IS_TFES | AHCI_PX_IS_DHRS);
    EQ(F.level_changes, changes);
    /* Byte write to PxIS (mask): only the written byte's 1s clear. */
    ahci_model_write(&M, P(AHCI_PX_IS), 0xffffffff, 0x000000ff);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES);
    ahci_model_write(&M, P(AHCI_PX_IS), 0xffffffff, 0xff000000);
    EQ(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES, 0);
    wr(AHCI_IS, 1);
    CHECK(!F.level);
    end(before);
}

static void post_sig_checks(void)
{
    const u8 *f = g(FB_OFF + AHCI_RFIS_RFIS);
    EQ(f[0], 0x34);
    EQ(f[2], 0x50);
    EQ(f[12], 1); /* count 01h, LBA 000001h: the ATA signature (ACS-3 §9.2, Table 206) */
    EQ(f[4], 1);
    EQ(f[5], 0);
    EQ(f[6], 0);
    EQ(rd(P(AHCI_PX_SIG)), 0x101);
    EQ(rd(P(AHCI_PX_TFD)) & 0xff, 0x50);
}

static void case_resets(void)
{
    int before = failures;
    begin("resets");
    port_start();
    /* COMRESET (AHCI §10.4.2): DET = 1 holds the link down, DET = 0 brings it up. */
    memset(g(FB_OFF), 0, 256);
    wr(P(AHCI_PX_SERR), 0xffffffff);
    port_stop();
    wr(P(AHCI_PX_SCTL), 0x301);
    EQ(rd(P(AHCI_PX_SSTS)) & 0xf, 0);
    CHECK(rd(P(AHCI_PX_TFD)) & ATA_STS_BSY);
    EQ(rd(P(AHCI_PX_SCTL)), 0x301);
    wr(P(AHCI_PX_SCTL), 0x300);
    EQ(rd(P(AHCI_PX_SSTS)), 0x133);
    CHECK(rd(P(AHCI_PX_SERR)) & AHCI_SERR_DIAG_X);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PCS);
    post_sig_checks();
    /* Software reset (AHCI §10.4.1): two device-control FISes, the first with R and C. */
    port_start();
    struct cmd c;
    memset(&c, 0, sizeof(c));
    c.cfl = 5;
    c.fis[0] = 0x27;
    c.fis[1] = 0x00;  /* C = 0: device control */
    c.fis[15] = 0x04; /* SRST */
    c.reset = true;
    c.clear_busy = true;
    memset(g(FB_OFF), 0, 256);
    EQ(issue(0, &c), 0);
    EQ(rd(P(AHCI_PX_TFD)) & ATA_STS_BSY, 0); /* C: BSY cleared on R_OK */
    c.fis[15] = 0x00;
    c.reset = false;
    c.clear_busy = false;
    EQ(issue(1, &c), 0);
    post_sig_checks();
    /*
     * GHC.HR mid-flight (AHCI §10.4.3): GHC.IE, IS and the port's non-HwInit fields back to
     * reset values, but PxCLB/PxCLBU/PxFB/PxFBU "are not affected"; the store untouched.
     */
    fill(F.disk, 512, 4);
    u8 keep[512];
    memcpy(keep, F.disk, 512);
    wr(P(AHCI_PX_IE), 0xffffffff);
    wr(AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_IE | AHCI_GHC_HR);
    EQ(rd(AHCI_GHC), AHCI_GHC_AE);
    EQ(rd(AHCI_IS), 0);
    EQ(rd(P(AHCI_PX_CMD)), AHCI_PX_CMD_SUD | AHCI_PX_CMD_POD);
    EQ(rd(P(AHCI_PX_IE)), 0);
    EQ(rd(P(AHCI_PX_CI)), 0);
    EQ(rd(P(AHCI_PX_SACT)), 0);
    EQ(rd(P(AHCI_PX_SCTL)), 0);
    EQ(rd(P(AHCI_PX_SSTS)), 0x133);
    EQ(rd(P(AHCI_PX_SIG)), 0x101);
    EQ(rd(P(AHCI_PX_CLB)), (u32)(RAM_IPA + CLB_OFF));
    EQ(rd(P(AHCI_PX_CLBU)), (u32)((RAM_IPA + CLB_OFF) >> 32));
    EQ(rd(P(AHCI_PX_FB)), (u32)(RAM_IPA + FB_OFF));
    EQ(rd(P(AHCI_PX_FBU)), (u32)((RAM_IPA + FB_OFF) >> 32));
    CHECK(!F.level);
    CHECK(!memcmp(F.disk, keep, 512));
    /* A driver that only restarts the port (FRE, then ST) after HR keeps working. */
    wr(P(AHCI_PX_SERR), 0xffffffff);
    wr(P(AHCI_PX_IS), 0xffffffff);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_FRE);
    wr(P(AHCI_PX_CMD), rd(P(AHCI_PX_CMD)) | AHCI_PX_CMD_ST);
    expect_usable();
    EQ(M.stats.fatal, 0);
    port_start();
    expect_usable();
    /* COMRESET while ST = 1 stops command processing until the link is back. */
    wr(P(AHCI_PX_SCTL), 1);
    struct prd ok = {RAM_IPA + DATA_OFF, 512, false, 0};
    h2d(&c, 0x25, 0, 1, 0);
    c.prds = &ok;
    c.nprd = 1;
    build(0, &c);
    wr(P(AHCI_PX_CI), 1);
    EQ(F.reads, 2); /* only the two expect_usable reads so far */
    wr(P(AHCI_PX_SCTL), 0);
    recover();
    expect_usable();
    end(before);
}

static void case_abort_paths(void)
{
    int before = failures;
    begin("abort_paths");
    port_start();
    struct cmd c;
    /* ACS-3: NOP, SMART, IDENTIFY PACKET, SECURITY FREEZE LOCK, FPDMA, DSM: all ABRT here. */
    /* Also WRITE MULTIPLE FUA EXT (CEh: FUA is not offered) and READ BUFFER (E4h). */
    const u8 ops[] = {0x00, 0xb0, 0xa1, 0xf5, 0x60, 0x61, 0x06, 0xce, 0xf8, 0x92, 0xe4};
    for (unsigned i = 0; i < sizeof(ops); i++) {
        int logs = F.logs;
        h2d(&c, ops[i], 0, 1, 0);
        wr(P(AHCI_PX_IS), 0xffffffff);
        issue(0, &c);
        expect_ata_error(0, ATA_ERR_ABRT);
        EQ(F.logs, logs + 1); /* one log line per new opcode */
        recover();
        issue(0, &c);
        EQ(F.logs, logs + 1); /* and not again */
        recover();
    }
    EQ(F.reads + F.writes, 0);
    /* ATAPI (A = 1). */
    h2d(&c, 0xa0, 0, 0, 0);
    c.atapi = true;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    /* A FIS that is not a Register H2D FIS. */
    h2d(&c, 0x25, 0, 1, 0);
    c.fis[0] = 0x34;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    /* SET FEATURES subcommands outside the model: APM (0x05), SATA features (0x10/0x90). */
    const u8 sf_bad[] = {0x05, 0x10, 0x90, 0x85, 0x42};
    for (unsigned i = 0; i < sizeof(sf_bad); i++) {
        h2d(&c, 0xef, 0, 0, sf_bad[i]);
        wr(P(AHCI_PX_IS), 0xffffffff);
        issue(0, &c);
        expect_ata_error(0, ATA_ERR_ABRT);
        recover();
    }
    const u8 sf_ok[] = {0x02, 0x82, 0x03, 0xaa, 0x55, 0x66, 0xcc};
    for (unsigned i = 0; i < sizeof(sf_ok); i++) {
        h2d(&c, 0xef, 0, 0x45, sf_ok[i]);
        wr(P(AHCI_PX_IS), 0xffffffff);
        EQ(issue(0, &c), 0);
        expect_d2h_ok(0, 0);
    }
    /* READ LOG EXT: only the log directory (log 0, page 0, one page). */
    struct prd lp = {RAM_IPA + DATA_OFF, 512, false, 0};
    memset(g(DATA_OFF), 0xcc, 512);
    h2d(&c, 0x2f, 0x00, 1, 0);
    c.prds = &lp;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PSS);
    EQ(g(DATA_OFF)[0], 1); /* GPL version 1 */
    EQ(g(DATA_OFF)[1], 0);
    CHECK(zero(g(DATA_OFF) + 2, 510));
    h2d(&c, 0x47, 0x00, 1, 0); /* READ LOG DMA EXT */
    c.prds = &lp;
    c.nprd = 1;
    memset(g(DATA_OFF), 0xcc, 512);
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 512);
    EQ(g(DATA_OFF)[0], 1);
    const u64 bad_logs[] = {0x30, 0x13, 0x10, 0x0100 /* log 0 page 1 */};
    for (unsigned i = 0; i < 4; i++) {
        h2d(&c, 0x2f, bad_logs[i], 1, 0);
        c.prds = &lp;
        c.nprd = 1;
        wr(P(AHCI_PX_IS), 0xffffffff);
        issue(0, &c);
        expect_ata_error(0, ATA_ERR_ABRT);
        recover();
    }
    h2d(&c, 0x2f, 0x00, 2, 0); /* two pages of a one-page log */
    c.prds = &lp;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    recover();
    expect_usable();
    end(before);
}

static void case_non_data(void)
{
    int before = failures;
    begin("non_data");
    port_start();
    struct cmd c;
    /* FLUSH CACHE (0xE7) and FLUSH CACHE EXT (0xEA) reach the backend. */
    h2d(&c, 0xe7, 0, 0, 0);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 0);
    h2d(&c, 0xea, 0, 0, 0);
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    EQ(F.flushes, 2);
    F.fail_backend = true;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    F.fail_backend = false;
    recover();
    /* Power management: success; CHECK POWER MODE reports active (count 0xFF, ACS-3 §7.3). */
    const u8 pm[] = {0xe0, 0xe1, 0xe2, 0xe3, 0x10};
    for (unsigned i = 0; i < sizeof(pm); i++) {
        h2d(&c, pm[i], 0, 0, 0);
        wr(P(AHCI_PX_IS), 0xffffffff);
        EQ(issue(0, &c), 0);
        expect_d2h_ok(0, 0);
    }
    h2d(&c, 0xe5, 0, 0, 0);
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[12], 0xff);
    EQ(F.reads + F.writes, 0);
    end(before);
}

/* IDENTIFY DEVICE into the data buffer, as 256 words. */
static void identify_words(u16 w[256])
{
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF + 0x10000, 512, false, 0};
    h2d(&c, 0xec, 0, 0, 0);
    c.prds = &p;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    const u8 *d = g(DATA_OFF + 0x10000);
    for (unsigned i = 0; i < 256; i++)
        w[i] = (u16)(d[2 * i] | d[2 * i + 1] << 8);
    u8 sum = 0;
    for (unsigned i = 0; i < 512; i++)
        sum += d[i];
    EQ(sum, 0);
}

static void set_features(u8 sub, u8 count, bool ok)
{
    struct cmd c;
    h2d(&c, 0xef, 0, count, sub);
    wr(P(AHCI_PX_IS), 0xffffffff);
    if (ok) {
        EQ(issue(0, &c), 0);
        expect_d2h_ok(0, 0);
    } else {
        issue(0, &c);
        expect_ata_error(0, ATA_ERR_ABRT);
        recover();
    }
}

static void set_multiple(u8 count, bool ok)
{
    struct cmd c;
    h2d28(&c, 0xc6, 0, count);
    wr(P(AHCI_PX_IS), 0xffffffff);
    if (ok) {
        EQ(issue(0, &c), 0);
        expect_d2h_ok(0, 0);
    } else {
        issue(0, &c);
        expect_ata_error(0, ATA_ERR_ABRT);
        recover();
    }
}

/*
 * ACS-3 §7.45.8 (set transfer mode, words 63/88), §7.46 and §7.26/7.27/7.64/7.65 (SET MULTIPLE
 * MODE and the MULTIPLE commands, words 47/59), §7.9 (EXECUTE DEVICE DIAGNOSTIC), and the
 * defaults a hardware reset (COMRESET, GHC.HR) restores.
 */
static void case_device_settings(void)
{
    int before = failures;
    begin("device_settings");
    port_start();
    u16 w[256];
    identify_words(w);
    EQ(w[47], 0x8010);
    EQ(w[59], 0x0110);
    EQ(w[63], 0x0007);
    EQ(w[88], 0x407f);
    /* Ultra DMA 5, then Multiword DMA 2 (which disables UDMA), then a PIO mode (no change). */
    set_features(0x03, 0x45, true);
    identify_words(w);
    EQ(w[88], 0x207f);
    EQ(w[63], 0x0007);
    set_features(0x03, 0x22, true);
    identify_words(w);
    EQ(w[63], 0x0407);
    EQ(w[88], 0x007f);
    set_features(0x03, 0x0c, true); /* PIO flow control mode 4 */
    set_features(0x03, 0x00, true); /* PIO default */
    set_features(0x03, 0x01, true); /* PIO default, IORDY off */
    identify_words(w);
    EQ(w[63], 0x0407);
    EQ(w[88], 0x007f);
    /* Unsupported modes abort and change nothing. */
    const u8 bad_modes[] = {0x47, 0x23, 0x0d, 0x10, 0x80, 0x02};
    for (unsigned i = 0; i < sizeof(bad_modes); i++)
        set_features(0x03, bad_modes[i], false);
    identify_words(w);
    EQ(w[63], 0x0407);
    EQ(w[88], 0x007f);
    set_features(0x03, 0x40, true); /* UDMA 0 */
    identify_words(w);
    EQ(w[88], 0x017f);
    EQ(w[63], 0x0007);
    set_features(0x82, 0, true); /* write cache off */

    /* SET MULTIPLE MODE: powers of two up to word 47's 16; zero and others abort. */
    set_multiple(4, true);
    identify_words(w);
    EQ(w[59], 0x0104);
    const u8 bad_counts[] = {0, 3, 32, 17, 6};
    for (unsigned i = 0; i < sizeof(bad_counts); i++)
        set_multiple(bad_counts[i], false);
    identify_words(w);
    EQ(w[59], 0x0104);

    /* READ MULTIPLE (C4h, LBA28) and READ MULTIPLE EXT (29h): PIO Setup with one DRQ block. */
    fill(F.disk + 100 * 512, 9 * 512, 77);
    struct cmd c;
    struct prd p = {RAM_IPA + DATA_OFF, 9 * 512, false, 0};
    memset(g(DATA_OFF), 0, 9 * 512);
    h2d28(&c, 0xc4, 100, 9);
    c.prds = &p;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_PSS);
    EQ(g(FB_OFF + AHCI_RFIS_PSFIS)[16] | g(FB_OFF + AHCI_RFIS_PSFIS)[17] << 8, 4 * 512);
    EQ(prdbc(0), 9 * 512);
    CHECK(!memcmp(g(DATA_OFF), F.disk + 100 * 512, 9 * 512));
    memset(g(DATA_OFF), 0, 9 * 512);
    h2d(&c, 0x29, 100, 9, 0);
    c.prds = &p;
    c.nprd = 1;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    CHECK(!memcmp(g(DATA_OFF), F.disk + 100 * 512, 9 * 512));
    /* WRITE MULTIPLE (C5h) and WRITE MULTIPLE EXT (39h). */
    fill(g(DATA_OFF), 9 * 512, 78);
    h2d28(&c, 0xc5, 200, 9);
    c.prds = &p;
    c.nprd = 1;
    c.write = true;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 9 * 512);
    CHECK(!memcmp(g(DATA_OFF), F.disk + 200 * 512, 9 * 512));
    fill(g(DATA_OFF), 9 * 512, 79);
    h2d(&c, 0x39, 300, 9, 0);
    c.prds = &p;
    c.nprd = 1;
    c.write = true;
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    expect_d2h_ok(0, 9 * 512);
    CHECK(!memcmp(g(DATA_OFF), F.disk + 300 * 512, 9 * 512));
    /* Out of range: IDNF, nothing written. */
    h2d(&c, 0x39, SECTORS - 1, 2, 0);
    c.prds = &p;
    c.nprd = 1;
    u64 writes = F.writes;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_IDNF);
    EQ(F.writes, writes);
    recover();

    /* EXECUTE DEVICE DIAGNOSTIC: passed (01h), the ATA signature, an interrupt (I = 1). */
    memset(g(FB_OFF), 0, 256);
    h2d(&c, 0x90, 0, 0, 0);
    wr(P(AHCI_PX_IS), 0xffffffff);
    EQ(issue(0, &c), 0);
    CHECK(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_DHRS);
    EQ(rd(P(AHCI_PX_IS)) & AHCI_PX_IS_TFES, 0);
    EQ(rd(P(AHCI_PX_TFD)), 0x0150);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[1] & 0x40, 0x40);
    EQ(g(FB_OFF + AHCI_RFIS_RFIS)[3], 0x01);
    post_sig_checks();
    EQ(prdbc(0), 0);
    /* The settings are unchanged by the diagnostic ... */
    identify_words(w);
    EQ(w[59], 0x0104);
    EQ(w[85] & 0x20, 0);
    /* ... and a hardware reset (COMRESET) restores every default (no SSP). */
    port_stop();
    wr(P(AHCI_PX_SCTL), 0x301);
    wr(P(AHCI_PX_SCTL), 0x300);
    port_start();
    identify_words(w);
    EQ(w[59], 0x0110);
    EQ(w[63], 0x0007);
    EQ(w[88], 0x407f);
    EQ(w[85] & 0x20, 0x20);
    /* So does GHC.HR. */
    set_multiple(2, true);
    set_features(0x03, 0x21, true);
    wr(AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_HR);
    port_start();
    identify_words(w);
    EQ(w[59], 0x0110);
    EQ(w[63], 0x0007);
    EQ(w[88], 0x407f);
    /* SET MULTIPLE MODE never disables multiple mode here (zero aborts); force the state to
       check that the MULTIPLE commands then abort (ACS-3 §7.26.2) without moving data. */
    M.multiple = 0;
    h2d28(&c, 0xc4, 0, 1);
    c.prds = &p;
    c.nprd = 1;
    u64 reads = F.reads;
    wr(P(AHCI_PX_IS), 0xffffffff);
    issue(0, &c);
    expect_ata_error(0, ATA_ERR_ABRT);
    EQ(F.reads, reads);
    recover();
    end(before);
}

/* F16: widths and alignment through the MMIO entry point. */
static void case_access_widths(void)
{
    int before = failures;
    begin("access_widths");
    u64 v;
    /* 8/16-bit reads inside a dword. */
    v = 0;
    CHECK(ahci_model_mmio(&M, AHCI_VS, 0, &v, false));
    EQ(v, 0x01);
    CHECK(ahci_model_mmio(&M, AHCI_VS + 2, 1, &v, false));
    EQ(v, 0x0001);
    CHECK(ahci_model_mmio(&M, AHCI_VS + 1, 0, &v, false));
    EQ(v, 0x03);
    /* 64-bit: two dwords, low first. */
    CHECK(ahci_model_mmio(&M, AHCI_CAP, 3, &v, false));
    EQ(v, 0x81341f00ULL | (u64)AHCI_GHC_AE << 32);
    v = (u64)(RAM_IPA + CLB_OFF);
    CHECK(ahci_model_mmio(&M, P(AHCI_PX_CLB), 3, &v, true));
    EQ(rd(P(AHCI_PX_CLB)), (u32)(RAM_IPA + CLB_OFF));
    EQ(rd(P(AHCI_PX_CLBU)), (u32)((RAM_IPA + CLB_OFF) >> 32));
    /* A 16-bit write to PxIE keeps the other half. */
    wr(P(AHCI_PX_IE), 0x40000001);
    v = 0x0020;
    CHECK(ahci_model_mmio(&M, P(AHCI_PX_IE), 1, &v, true));
    EQ(rd(P(AHCI_PX_IE)), 0x40000020);
    v = 0x4000;
    CHECK(ahci_model_mmio(&M, P(AHCI_PX_IE) + 2, 1, &v, true));
    EQ(rd(P(AHCI_PX_IE)), 0x40000020);
    /* Unaligned and cross-dword accesses: RAZ/WI, refused. */
    v = 0xffff;
    CHECK(!ahci_model_mmio(&M, AHCI_VS + 1, 1, &v, false));
    EQ(v, 0);
    CHECK(!ahci_model_mmio(&M, AHCI_VS + 2, 2, &v, false));
    CHECK(!ahci_model_mmio(&M, AHCI_CAP + 4, 3, &v, false));
    v = 0xffffffff;
    CHECK(!ahci_model_mmio(&M, P(AHCI_PX_IE) + 1, 2, &v, true));
    EQ(rd(P(AHCI_PX_IE)), 0x40000020);
    /* Out of the ABAR, and widths above 64 bits. */
    CHECK(!ahci_model_mmio(&M, AHCI_MODEL_ABAR_SIZE, 2, &v, false));
    CHECK(!ahci_model_mmio(&M, 0xfffffffffffffffcULL, 2, &v, false));
    CHECK(!ahci_model_mmio(&M, 0, 4, &v, false));
    end(before);
}

/* The IROUTER decision and the level rule for carrier-emulated SPIs (IHI 0069). */
static void case_spi_route(void)
{
    int before = failures;
    begin("spi_route");
    u64 mpidr[12];
    for (int i = 0; i < 12; i++)
        mpidr[i] = 0x80000000ULL | (u64)(i < 6 ? 0 : i < 8 ? 1 : 2) << 8 | (u64)(i % 6);
    u64 all = (1ULL << 12) - 1;
    /* IRM = 0: exact affinity match. */
    EQ(vdev_spi_route(mpidr[7] & 0xffffff, 3, 6, mpidr, all, 12), 7);
    EQ(vdev_spi_route(0x000203, 3, 6, mpidr, all, 12), 9);
    /* Unknown affinity, or a target that is not a candidate: the fallback. */
    EQ(vdev_spi_route(0x000307, 3, 6, mpidr, all, 12), 6);
    EQ(vdev_spi_route(mpidr[7] & 0xffffff, 3, 6, mpidr, all & ~(1ULL << 7), 12), 6);
    /* Aff3 is compared too. */
    EQ(vdev_spi_route((1ULL << 32) | (mpidr[7] & 0xffffff), 3, 6, mpidr, all, 12), 6);
    /* IRM = 1: the CPU that changed the level, when it is a candidate. */
    EQ(vdev_spi_route(VDEV_SPI_IROUTER_IRM, 3, 6, mpidr, all, 12), 3);
    EQ(vdev_spi_route(VDEV_SPI_IROUTER_IRM, 3, 6, mpidr, all & ~8ULL, 12), 6);
    EQ(vdev_spi_route(VDEV_SPI_IROUTER_IRM, 40, 6, mpidr, all, 12), 6);
    /* No candidate at all. */
    EQ(vdev_spi_route(0x000307, 3, 6, mpidr, 0, 12), (u64)-1);
    EQ(vdev_spi_route(0, 0, 6, mpidr, all, 0), (u64)-1);
    /* Level rule. */
    CHECK(vdev_spi_should_pend(true, true));
    CHECK(!vdev_spi_should_pend(true, false));
    CHECK(!vdev_spi_should_pend(false, true));
    CHECK(vdev_spi_is_virtual(990));
    CHECK(!vdev_spi_is_virtual(995) && !vdev_spi_is_virtual(996) && !vdev_spi_is_virtual(997));
    CHECK(!vdev_spi_is_virtual(30));
    end(before);
}

/*
 * One GICv3 state machine for a carrier-emulated SPI (IHI 0069): pending on at most one CPU
 * interface, on none while it is active anywhere.  Simulates hv_t8152_vspi_apply's use of
 * vdev_spi_plan with IRM = 1 re-applies from different CPUs (GICD writes, refreshes).
 */
static u64 rng_state;
static u64 rnd(void);

struct spi_sim {
    u64 pending, active;
};

static void sim_apply(struct spi_sim *s, bool level, bool enabled, int self, const u64 *mpidr,
                      u64 irouter)
{
    u64 all = (1ULL << 12) - 1;
    bool pend = vdev_spi_should_pend(level, enabled);
    int target = pend && !s->active ? vdev_spi_route(irouter, self, 0, mpidr, all, 12) : -1;
    struct vdev_spi_plan plan = vdev_spi_plan(pend, target, s->pending, s->active);
    CHECK(plan.raise < 0 || !(plan.clear & (1ULL << plan.raise)));
    s->pending &= ~plan.clear;
    if (plan.raise >= 0)
        s->pending |= 1ULL << plan.raise;
}

static int popcount64(u64 v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static void case_spi_plan(void)
{
    int before = failures;
    begin("spi_plan");
    u64 mpidr[12];
    for (int i = 0; i < 12; i++)
        mpidr[i] = 0x80000000ULL | (u64)(i < 6 ? 0 : i < 8 ? 1 : 2) << 8 | (u64)(i % 6);
    struct spi_sim s = {0, 0};
    /* Level high, IRM = 1: the command completes on CPU 3. */
    sim_apply(&s, true, true, 3, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 1ULL << 3);
    /* CPU 5 writes some GICD register: the refresh moves it, it is never on both. */
    sim_apply(&s, true, true, 5, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 1ULL << 5);
    /* CPU 5 acknowledges it: active on 5, no longer pending. */
    s.pending &= ~(1ULL << 5);
    s.active |= 1ULL << 5;
    /* Refreshes from CPUs 3 and 7 while it is active on 5: pending nowhere (one ISR). */
    sim_apply(&s, true, true, 3, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 0);
    sim_apply(&s, true, true, 7, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 0);
    /* A stale copy elsewhere while active is cleared; an active-and-pending copy stays. */
    s.pending = (1ULL << 2) | (1ULL << 5);
    sim_apply(&s, true, true, 7, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 1ULL << 5);
    s.pending = 0;
    /* Deactivation on 5 with the line still high: re-applied, delivered once (to 5, IRM 1). */
    s.active = 0;
    sim_apply(&s, true, true, 5, mpidr, VDEV_SPI_IROUTER_IRM);
    EQ(s.pending, 1ULL << 5);
    /* IRM = 0 and the guest moves IROUTER to CPU 7 while the line is high. */
    sim_apply(&s, true, true, 2, mpidr, mpidr[7] & 0xffffff);
    EQ(s.pending, 1ULL << 7);
    /* Disabled at the GICD, then the line drops: cleared everywhere. */
    sim_apply(&s, true, false, 2, mpidr, 0);
    EQ(s.pending, 0);
    sim_apply(&s, true, true, 2, mpidr, 0);
    EQ(s.pending, 1ULL << 0);
    sim_apply(&s, false, true, 9, mpidr, 0);
    EQ(s.pending, 0);
    /* Seeded random sequence: the invariants always hold. */
    rng_state = 0x243f6a8885a308d3ULL;
    s = (struct spi_sim){0, 0};
    for (int i = 0; i < 100000; i++) {
        u64 r = rnd();
        int cpu = (int)(rnd() % 12);
        switch (r % 6) {
            case 0: /* acknowledge on a CPU where it is pending */
                if (s.pending & (1ULL << cpu) && !s.active) {
                    s.pending &= ~(1ULL << cpu);
                    s.active |= 1ULL << cpu;
                }
                break;
            case 1: /* deactivate (the EOI path clears pending there, then re-applies) */
                if (s.active & (1ULL << cpu)) {
                    s.active &= ~(1ULL << cpu);
                    s.pending &= ~(1ULL << cpu);
                    sim_apply(&s, rnd() & 1, true, cpu, mpidr, VDEV_SPI_IROUTER_IRM);
                }
                break;
            default:
                sim_apply(&s, rnd() % 4 != 0, rnd() % 8 != 0, cpu, mpidr,
                          rnd() & 1 ? VDEV_SPI_IROUTER_IRM : mpidr[rnd() % 12] & 0xffffff);
                break;
        }
        CHECK(popcount64(s.pending) <= 1);
        CHECK(popcount64(s.active) <= 1);
        CHECK(!s.active || !(s.pending & ~s.active));
        if (failures > before + 10)
            break;
    }
    end(before);
}

/* CRC-32 vectors; the random-buffer comparison with zlib is done by the Python driver. */
static void case_crc(const char *outdir)
{
    int before = failures;
    begin("crc");
    EQ(ahci_crc32(0, "123456789", 9), 0xcbf43926);
    EQ(ahci_crc32(0, "", 0), 0);
    static u8 buf[(1 << 20) + 13];
    fill(buf, sizeof(buf), 99);
    char path[1024];
    snprintf(path, sizeof(path), "%s/crc_input.bin", outdir);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
    if (f)
        fclose(f);
    const u64 sizes[] = {0, 1, 7, 8, 9, 15, 16, 17, 63, 64, 65, 4095, 4096, 65537, sizeof(buf)};
    printf("{\"crc\": [");
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        printf("%s[%llu, %u]", i ? ", " : "", (unsigned long long)sizes[i],
               ahci_crc32(0, buf, sizes[i]));
    printf("]}\n");
    /* Chaining at odd split points equals one pass; unaligned starts too. */
    u32 whole = ahci_crc32(0, buf, sizeof(buf));
    u32 part = ahci_crc32(0, buf, 3);
    part = ahci_crc32(part, buf + 3, 1000);
    part = ahci_crc32(part, buf + 1003, sizeof(buf) - 1003);
    EQ(part, whole);
    end(before);
}

/* ---- randomized commands (F1-F5, F16 invariants) ----------------------------------------- */

static u64 rng_state;

static u64 rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static u64 rnd_ipa(void)
{
    switch (rnd() % 8) {
        case 0:
            return rnd();
        case 1:
            return RAM_IPA - (rnd() % 0x2000);
        case 2:
            return RAM_IPA + RAM_SIZE - (rnd() % 0x2000);
        default:
            return RAM_IPA + DATA_OFF + (rnd() % (RAM_SIZE - DATA_OFF));
    }
}

static void case_fuzz(u64 iterations)
{
    int before = failures;
    begin("fuzz");
    static const u8 ops[] = {0x25, 0x35, 0xc8, 0xca, 0x20, 0x24, 0x30, 0x34, 0xec, 0xe7,
                             0xea, 0xef, 0x2f, 0x47, 0x42, 0x40, 0xe5, 0x00, 0xb0, 0x60,
                             0xc4, 0xc5, 0x29, 0x39, 0xc6, 0x90};
    rng_state = 0x9e3779b97f4a7c15ULL;
    u64 commands = 0, done = 0, errors = 0, fatals = 0, moved = 0;
    for (u64 it = 0; it < iterations; it++) {
        if (it % 50000 == 0) {
            canaries_check();
            done += M.stats.commands;
            errors += M.stats.errors;
            fatals += M.stats.fatal;
            moved += M.stats.sectors_read + M.stats.sectors_written;
            fresh();
            port_start();
        }
        u64 r = rnd();
        if (r % 16 == 0) {
            /* A random register write, with a random byte mask. */
            u32 off = (u32)(rnd() % 0x200) & ~3u;
            u32 mask = (u32)rnd() | 0xff;
            /* Keep CLB/FB inside the window most of the time so commands run. */
            if (off == P(AHCI_PX_CLB) || off == P(AHCI_PX_FB) || off == P(AHCI_PX_CLBU) ||
                off == P(AHCI_PX_FBU)) {
                if (rnd() % 4)
                    continue;
            }
            ahci_model_write(&M, off, (u32)rnd(), mask);
        } else if (r % 16 == 1) {
            u64 v = 0;
            ahci_model_mmio(&M, rnd() % 0x4100, rnd() % 5, &v, rnd() & 1);
        } else {
            u32 slot = rnd() % 32;
            struct cmd c;
            u64 lba = rnd() % 8 ? rnd() % SECTORS : rnd();
            u32 count = rnd() % 8 ? rnd() % 64 : (u32)rnd();
            h2d(&c, ops[rnd() % sizeof(ops)], lba, count, (u16)rnd());
            if (rnd() % 32 == 0)
                c.fis[1] = 0; /* device control */
            if (rnd() % 64 == 0)
                c.cfl = rnd() % 32;
            c.atapi = rnd() % 64 == 0;
            struct prd p[8];
            c.nprd = rnd() % 16 ? 1 + rnd() % 8 : 0;
            for (u32 i = 0; i < c.nprd; i++)
                p[i] = (struct prd){rnd() % 32 ? (RAM_IPA + DATA_OFF + (rnd() % 0x1000000)) & ~1ULL
                                               : rnd_ipa(),
                                    (u32)((rnd() % 0x10000) & ~1u) + 2, rnd() & 1,
                                    rnd() % 64 == 0 ? (u32)rnd() & 0x3fffff : 0};
            c.prds = p;
            if (rnd() % 32 == 0)
                c.ctba = rnd_ipa() & ~0x7fULL;
            build(slot, &c);
            if (rd(P(AHCI_PX_CLB)) != (u32)(RAM_IPA + CLB_OFF) ||
                rd(P(AHCI_PX_FB)) != (u32)(RAM_IPA + FB_OFF) || rd(P(AHCI_PX_CLBU)) ||
                rd(P(AHCI_PX_FBU)) != (u32)((RAM_IPA + FB_OFF) >> 32)) {
                /* A random HBA reset or address write: start the port again. */
                wr(P(AHCI_PX_CMD), 0);
                wr(P(AHCI_PX_SCTL), 0);
                port_start();
            }
            if (!(rd(P(AHCI_PX_CMD)) & AHCI_PX_CMD_ST) || (rd(P(AHCI_PX_IS)) & 0x7d000000))
                recover();
            wr(P(AHCI_PX_CI), BIT(slot));
            commands++;
        }
        /* Invariants. */
        u32 cmd = rd(P(AHCI_PX_CMD));
        if (!(cmd & AHCI_PX_CMD_ST)) {
            EQ(rd(P(AHCI_PX_CI)), 0);
            EQ(cmd & AHCI_PX_CMD_CR, 0);
        }
        bool want = (rd(AHCI_GHC) & AHCI_GHC_IE) && (rd(AHCI_IS) & 1);
        EQ(ahci_model_line(&M), want);
        EQ(F.level, want);
        if (failures > before + 20)
            break;
    }
    canaries_check();
    done += M.stats.commands;
    errors += M.stats.errors;
    fatals += M.stats.fatal;
    moved += M.stats.sectors_read + M.stats.sectors_written;
    printf("{\"fuzz\": {\"issued\": %llu, \"completed\": %llu, \"ata_errors\": %llu, "
           "\"fatal\": %llu, \"sectors\": %llu}}\n",
           (unsigned long long)commands, (unsigned long long)done, (unsigned long long)errors,
           (unsigned long long)fatals, (unsigned long long)moved);
    end(before);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    u64 iterations = argc > 2 ? strtoull(argv[2], NULL, 0) : 1000000;
    F.ram_alloc = malloc(RAM_SIZE + 2 * GUARD);
    F.disk_alloc = malloc(DISK_SIZE + 2 * GUARD);
    if (!F.ram_alloc || !F.disk_alloc)
        return 2;
    F.ram = F.ram_alloc + GUARD;
    F.disk = F.disk_alloc + GUARD;

    case_reset_values();
    case_identify();
    case_dma_ext();
    case_pio();
    case_count_zero();
    case_range_errors();
    case_prd_space();
    case_prd_format();
    case_out_of_window();
    case_port_state();
    case_irq_matrix();
    case_resets();
    case_abort_paths();
    case_non_data();
    case_device_settings();
    case_access_widths();
    case_spi_route();
    case_spi_plan();
    case_crc(outdir);
    case_fuzz(iterations);

    free(F.ram_alloc);
    free(F.disk_alloc);
    return failures ? 1 : 0;
}
