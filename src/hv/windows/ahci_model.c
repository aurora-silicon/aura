/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Software AHCI 1.3.1 HBA, one port, one ATA disk.  See ahci_model.h for the scope and the
 * specifications this is written from (AHCI 1.3.1, ACS-3, SATA 3.x FIS layouts).
 */

#include "ahci_model.h"

#define PX(r) (AHCI_MODEL_PORT0 + (r))

/* PxIS bits software clears by writing 1 (AHCI §3.3.5).  PCS, PRCS and UFS are derived. */
#define PXIS_RWC                                                                                   \
    (AHCI_PX_IS_DHRS | AHCI_PX_IS_PSS | AHCI_PX_IS_DSS | AHCI_PX_IS_SDBS | AHCI_PX_IS_DPS |        \
     AHCI_PX_IS_DMPS | AHCI_PX_IS_IPMS | AHCI_PX_IS_OFS | AHCI_PX_IS_INFS | AHCI_PX_IS_IFS |       \
     AHCI_PX_IS_HBDS | AHCI_PX_IS_HBFS | AHCI_PX_IS_TFES | AHCI_PX_IS_CPDS)
/* PxIE: every defined enable bit (AHCI §3.3.6). */
#define PXIE_MASK 0xfdc000ffu
/* PxCMD bits stored as written; SUD/POD read 1, CR/FR/CCS are composed (AHCI §3.3.7). */
#define PXCMD_RW (AHCI_PX_CMD_ST | AHCI_PX_CMD_FRE | AHCI_PX_CMD_ATAPI | AHCI_PX_CMD_DLAE)

/* FIS types (SATA §10.5). */
#define FIS_REG_H2D   0x27
#define FIS_REG_D2H   0x34
#define FIS_PIO_SETUP 0x5f

/* PRD entry DBC field (AHCI §4.2.3.3): bits 21:0 byte count - 1, bit 31 interrupt. */
#define PRD_DBC_MASK 0x3fffffu
#define PRD_DBC_I    0x80000000u

/* Status after a successful command and after an error (ACS-3 §6.2). */
#define STS_OK  (ATA_STS_DRDY | ATA_STS_DSC)
#define STS_ERR (ATA_STS_DRDY | ATA_STS_DSC | ATA_STS_ERR)
/* A host bus fatal error leaves the device mid-command: DRQ, so software must CLO or reset. */
#define STS_FATAL (ATA_STS_DRDY | ATA_STS_DSC | ATA_STS_DRQ | ATA_STS_ERR)

enum xfer {
    XFER_NONE,
    XFER_DMA_IN,
    XFER_DMA_OUT,
    XFER_PIO_IN,
    XFER_PIO_OUT,
};

/* Outcome of one command slot. */
enum outcome {
    DONE,      /* completed: clear the slot */
    ATA_ERROR, /* TFES: slot stays set, port halted */
    FATAL,     /* HBFS or OFS: slot stays set, port halted */
};

struct ata_cmd {
    u8 fis[20];
    u8 op;
    u16 features;
    u64 lba48;
    u32 lba28;
    u32 count16; /* 0 means 65536 */
    u32 count8;  /* 0 means 256 */
};

/* Where data comes from or goes to while PRD entries are walked. */
struct stream {
    u64 lba;       /* disk transfers */
    const u8 *buf; /* model-generated data (IDENTIFY, log pages), or NULL */
    u64 pos;       /* bytes moved so far */
};

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u64 min64(u64 a, u64 b)
{
    return a < b ? a : b;
}

static void *span(struct ahci_model *m, u64 ipa, u64 len, bool write)
{
    if (!len || ipa > UINT64_MAX - (len - 1))
        return NULL;
    return m->env->guest_span(m->env->opaque, ipa, len, write);
}

static void model_log(struct ahci_model *m, const char *event, u64 a, u64 b)
{
    if (m->env->log)
        m->env->log(m->env->opaque, event, a, b);
}

static u32 port_is(const struct ahci_model *m)
{
    u32 is = m->pxis;
    /* PCS mirrors PxSERR.DIAG.X and PRCS PxSERR.DIAG.N (AHCI §3.3.5). */
    if (m->serr & AHCI_SERR_DIAG_X)
        is |= AHCI_PX_IS_PCS;
    if (m->serr & AHCI_SERR_DIAG_N)
        is |= AHCI_PX_IS_PRCS;
    return is;
}

bool ahci_model_line(const struct ahci_model *m)
{
    return (m->ghc & AHCI_GHC_IE) && (m->is & 1);
}

/*
 * AHCI §3.1.3 / §10.7.1.1: IS.IPS0 is set while (PxIS & PxIE) != 0, also after software has
 * cleared it; the line is GHC.IE && (IS & PI).
 */
static void update_irq(struct ahci_model *m)
{
    if (port_is(m) & m->pxie)
        m->is |= 1;
    bool level = ahci_model_line(m);
    if (level != m->line) {
        m->line = level;
        if (m->env->irq_level)
            m->env->irq_level(m->env->opaque, level);
    }
}

static void set_tfd(struct ahci_model *m, u8 status, u8 error)
{
    m->tfd = (u32)error << 8 | status;
}

/* Copy a received FIS into the FIS receive area, if FIS receive is enabled (AHCI §4.2.1). */
static bool post_fis(struct ahci_model *m, u32 off, const u8 *fis, u32 len)
{
    if (!(m->cmd & AHCI_PX_CMD_FRE))
        return true;
    u64 fb = (u64)m->fbu << 32 | m->fb;
    u8 *dst = span(m, fb + off, len, true);
    if (!dst) {
        model_log(m, "fis-area-outside-guest-ram", fb, off);
        return false;
    }
    __builtin_memcpy(dst, fis, len);
    return true;
}

/*
 * A fatal port error (AHCI §6.2.2, §6.2.2.1): set the bit, stop processing until ST is cleared.
 * Also used for OFS, which §6.2.2 lists as non-fatal but §6.1.5 calls a serious error that
 * software should recover from as from a fatal one; this HBA does not continue after it.
 */
static enum outcome fatal(struct ahci_model *m, u32 is_bit)
{
    m->pxis |= is_bit;
    m->halted = true;
    set_tfd(m, STS_FATAL, ATA_ERR_ABRT);
    m->stats.fatal++;
    return FATAL;
}

/*
 * The ATA device signature in a D2H Register FIS (ACS-3 §9.2, Device Signatures for Normal
 * Output: count 01h, LBA 000001h) with the diagnostic code 01h "passed" (ACS-3 §7.9.4) in
 * ERROR; PxSIG and PxTFD take it (AHCI §10.4).  `irq` sets the FIS's I bit: a reset's
 * signature is unsolicited (I = 0), EXECUTE DEVICE DIAGNOSTIC's ends a command (I = 1).
 */
static bool post_signature(struct ahci_model *m, bool irq)
{
    u8 fis[20] = {0};
    fis[0] = FIS_REG_D2H;
    fis[1] = irq ? 0x40 : 0;
    fis[2] = STS_OK;
    fis[3] = 0x01;  /* diagnostic code: passed */
    fis[4] = 0x01;  /* LBA 7:0 */
    fis[12] = 0x01; /* count */
    m->sig = AHCI_SIG_ATA;
    set_tfd(m, STS_OK, 0x01);
    return post_fis(m, AHCI_RFIS_RFIS, fis, sizeof(fis));
}

static void signature(struct ahci_model *m)
{
    if (!post_signature(m, false))
        fatal(m, AHCI_PX_IS_HBFS);
}

/* Power-on / hardware-reset defaults of the device's settings (no SSP, ACS-3 §4.21). */
static void device_defaults(struct ahci_model *m)
{
    m->write_cache = true;
    m->dma_mode = 0x46; /* Ultra DMA mode 6 */
    m->multiple = 16;   /* word 59 default: "the optimum value" (ACS-3 §7.12.7.21) */
}

/*
 * COMINIT received after a COMRESET (a hardware reset of the device), link at Gen3: PxSSTS,
 * PxSERR.DIAG.X, device defaults, signature (AHCI §10.4.2).
 */
static void link_up(struct ahci_model *m)
{
    m->ssts = AHCI_SSTS_UP;
    m->serr |= AHCI_SERR_DIAG_X;
    device_defaults(m);
    signature(m);
}

/*
 * GHC.HR (AHCI §10.4.3): GHC.IE, IS and every port field that is not HwInit return to their
 * reset values, except PxCLB/PxCLBU/PxFB/PxFBU, which "are not affected"; the port sends a
 * COMRESET (no staggered spin-up).  The disk contents are untouched.
 */
static void hba_reset(struct ahci_model *m)
{
    m->ghc = 0;
    m->is = 0;
    m->pxis = m->pxie = 0;
    m->cmd = 0;
    m->ccs = 0;
    m->sctl = 0;
    m->serr = 0;
    m->sact = 0;
    m->ci = 0;
    m->halted = m->comreset = m->srst = false;
    m->bounce_valid = false;
    link_up(m); /* FRE is 0: PxSIG and PxTFD only */
    update_irq(m);
}

void ahci_model_init(struct ahci_model *m, const struct ahci_env *env)
{
    __builtin_memset(m, 0, sizeof(*m)); /* power-on: PxCLB/PxFB are 0 */
    m->env = env;
    hba_reset(m);
}

/* ---- IDENTIFY DEVICE (ACS-3 §7.12.7) --------------------------------------------------- */

/* ATA strings: two characters per word, the first in bits 15:8, space padded (ACS-3 §3.3.10). */
static void ata_string(u16 *w, const char *s, unsigned words)
{
    bool ended = false;
    for (unsigned i = 0; i < words * 2; i++) {
        char c = ended ? 0 : s[i];
        if (!c) {
            ended = true;
            c = ' ';
        }
        if (i & 1)
            w[i / 2] |= (u8)c;
        else
            w[i / 2] = (u16)((u8)c << 8);
    }
}

void ahci_model_identify(const struct ahci_model *m, u16 w[256])
{
    u64 sectors = m->env->sectors;
    u32 lba28 = sectors > 0x0fffffff ? 0x0fffffff : (u32)sectors;

    __builtin_memset(w, 0, 512);
    w[0] = 0x0040; /* ATA device, fixed, response complete */
    w[1] = 16383;  /* obsolete CHS geometry, as ACS recommends for > 8 GB */
    w[2] = 0xc837; /* no SET FEATURES spin-up, IDENTIFY complete */
    w[3] = 16;
    w[6] = 63;
    const struct ahci_env *e = m->env;
    ata_string(&w[10], e->serial ? e->serial : "AURA-AHCI-0000000001", 10);
    ata_string(&w[23], e->firmware ? e->firmware : "D1.0", 4);
    ata_string(&w[27], e->model_name ? e->model_name : "Aura carrier RAM disk", 20);
    w[47] = 0x8010; /* at most 16 sectors per DRQ block (SATA: 16 or less, ACS-3 §7.12.7.15) */
    w[49] = 0x0300; /* LBA, DMA */
    w[50] = 0x4000;
    w[53] = 0x0006;                      /* words 64-70 and 88 valid */
    w[59] = (u16)(0x0100 | m->multiple); /* the current DRQ block setting is valid */
    w[60] = (u16)lba28;
    w[61] = (u16)(lba28 >> 16);
    /* Multiword DMA 0-2 and Ultra DMA 0-6 supported; the selected mode's enabled bit is set
       in word 63 or 88, never both (ACS-3 §7.12.7.24, §7.12.7.42, §7.45.8). */
    w[63] = 0x0007;
    if ((m->dma_mode & 0xf8) == 0x20)
        w[63] |= (u16)(0x0100 << (m->dma_mode & 7));
    w[64] = 0x0003; /* PIO 3 and 4 */
    w[65] = w[66] = w[67] = w[68] = 120;
    w[76] = 0x000e; /* SATA Gen1-3; bit 8 (NCQ) clear: CAP.SNCQ = 0 */
    w[80] = 0x07f0; /* ATA/ATAPI-4 .. ACS-3 */
    w[82] = 0x4020; /* NOP, volatile write cache; no SMART, security, HPA */
    w[83] = 0x7400; /* FLUSH CACHE EXT, FLUSH CACHE, 48-bit LBA */
    w[84] = 0x4000;
    w[85] = (u16)(0x4000 | (m->write_cache ? 0x0020 : 0));
    w[86] = 0x3400;
    w[87] = 0x4000;
    w[88] = 0x007f;
    if ((m->dma_mode & 0xf8) == 0x40)
        w[88] |= (u16)(0x0100 << (m->dma_mode & 7));
    w[100] = (u16)sectors;
    w[101] = (u16)(sectors >> 16);
    w[102] = (u16)(sectors >> 32);
    w[103] = (u16)(sectors >> 48);
    if (e->phys_log2) {
        /* 512e: valid, multiple logical sectors per physical, 2^n of them (ACS-3 §7.12.7.40);
           logical sector 0 at offset 0 of its physical sector, alignment valid (§7.12.7.67). */
        w[106] = (u16)(0x6000 | (e->phys_log2 & 0xf));
        w[209] = 0x4000;
    } else {
        w[106] = 0x4000; /* one 512-byte logical sector per 512-byte physical sector */
    }
    w[217] = 0x0001; /* non-rotating medium */
    /* Integrity word: signature 0xA5, checksum making the 512-byte sum 0 (ACS-3 §7.12.7.91). */
    u8 sum = 0xa5;
    for (unsigned i = 0; i < 255; i++)
        sum += (u8)w[i] + (u8)(w[i] >> 8);
    w[255] = (u16)(0xa5 | (u8)(0x100 - sum) << 8);
}

/* ---- data movement ----------------------------------------------------------------------- */

/* Device to guest: `len` bytes of the stream into dst. */
static bool stream_in(struct ahci_model *m, struct stream *s, u8 *dst, u64 len)
{
    if (s->buf) {
        __builtin_memcpy(dst, s->buf + s->pos, len);
        s->pos += len;
        return true;
    }
    while (len) {
        u64 off = s->pos % AHCI_MODEL_SECTOR;
        u64 lba = s->lba + s->pos / AHCI_MODEL_SECTOR;
        if (!off && len >= AHCI_MODEL_SECTOR) {
            u64 n = len / AHCI_MODEL_SECTOR;
            if (!m->env->read(m->env->opaque, lba, (u32)n, dst))
                return false;
            dst += n * AHCI_MODEL_SECTOR;
            s->pos += n * AHCI_MODEL_SECTOR;
            len -= n * AHCI_MODEL_SECTOR;
            continue;
        }
        /* A PRD boundary inside a sector: stage the sector once. */
        if (!m->bounce_valid || m->bounce_lba != lba) {
            if (!m->env->read(m->env->opaque, lba, 1, m->bounce))
                return false;
            m->bounce_lba = lba;
            m->bounce_valid = true;
        }
        u64 take = min64(len, AHCI_MODEL_SECTOR - off);
        __builtin_memcpy(dst, m->bounce + off, take);
        dst += take;
        s->pos += take;
        len -= take;
    }
    return true;
}

/* Guest to device: `len` bytes from src into the stream. */
static bool stream_out(struct ahci_model *m, struct stream *s, const u8 *src, u64 len)
{
    while (len) {
        u64 off = s->pos % AHCI_MODEL_SECTOR;
        u64 lba = s->lba + s->pos / AHCI_MODEL_SECTOR;
        if (!off && len >= AHCI_MODEL_SECTOR) {
            u64 n = len / AHCI_MODEL_SECTOR;
            if (!m->env->write(m->env->opaque, lba, (u32)n, src))
                return false;
            src += n * AHCI_MODEL_SECTOR;
            s->pos += n * AHCI_MODEL_SECTOR;
            len -= n * AHCI_MODEL_SECTOR;
            continue;
        }
        /* Collect a sector split across PRD entries; write it once it is whole. */
        u64 take = min64(len, AHCI_MODEL_SECTOR - off);
        __builtin_memcpy(m->bounce + off, src, take);
        src += take;
        s->pos += take;
        len -= take;
        if (s->pos % AHCI_MODEL_SECTOR == 0 && !m->env->write(m->env->opaque, lba, 1, m->bounce))
            return false;
    }
    return true;
}

enum walk {
    WALK_OK,
    WALK_HBFS,   /* a table or buffer span outside guest RAM */
    WALK_SHORT,  /* the PRDT describes less than the transfer */
    WALK_FORMAT, /* DBA bit 0 set or an odd byte count */
    WALK_MEDIUM, /* the backend failed */
};

/*
 * Walk the PRDT for `bytes` of data (AHCI §4.2.3.3, §5.4.2).  Only the entries the transfer
 * consumes are read, at most PRDTL of them.  With `move` false nothing is transferred: this
 * checks every span first, so a bad entry anywhere stops the command before any data moves.
 * With `move` true each entry is fetched and checked again before it is used (the guest may
 * have changed it), so no span outside guest RAM is ever touched.
 */
static enum walk walk_prdt(struct ahci_model *m, u64 ctba, u32 prdtl, u64 bytes, bool to_guest,
                           bool move, struct stream *s, bool *irq)
{
    u64 covered = 0;
    for (u32 i = 0; i < prdtl && covered < bytes; i++) {
        u64 off = 0x80 + (u64)i * 16;
        if (ctba > UINT64_MAX - off)
            return WALK_HBFS;
        const u8 *e = span(m, ctba + off, 16, false);
        if (!e)
            return WALK_HBFS;
        u64 dba = (u64)le32(e + 4) << 32 | le32(e);
        u32 dbc = le32(e + 12);
        u64 n = (u64)(dbc & PRD_DBC_MASK) + 1;
        if ((dba & 1) || (n & 1))
            return WALK_FORMAT;
        u64 take = min64(n, bytes - covered);
        u8 *p = span(m, dba, take, to_guest);
        if (!p)
            return WALK_HBFS;
        if (move) {
            bool ok = to_guest ? stream_in(m, s, p, take) : stream_out(m, s, p, take);
            if (!ok)
                return WALK_MEDIUM;
            if (dbc & PRD_DBC_I)
                *irq = true;
        }
        covered += take;
    }
    return covered < bytes ? WALK_SHORT : WALK_OK;
}

/* ---- command completion ----------------------------------------------------------------- */

static void write_prdbc(u8 *hdr, u64 bytes)
{
    put32(hdr + 4, (u32)bytes);
}

/* D2H Register FIS (SATA §10.5.6) with I = 1. */
static bool post_d2h(struct ahci_model *m, const struct ata_cmd *c, u8 status, u8 error, u8 count)
{
    u8 fis[20] = {0};
    fis[0] = FIS_REG_D2H;
    fis[1] = 0x40;
    fis[2] = status;
    fis[3] = error;
    fis[4] = c->fis[4];
    fis[5] = c->fis[5];
    fis[6] = c->fis[6];
    fis[7] = c->fis[7];
    fis[8] = c->fis[8];
    fis[9] = c->fis[9];
    fis[10] = c->fis[10];
    fis[12] = count;
    return post_fis(m, AHCI_RFIS_RFIS, fis, sizeof(fis));
}

static enum outcome ata_error(struct ahci_model *m, const struct ata_cmd *c, u8 error)
{
    set_tfd(m, STS_ERR, error);
    m->pxis |= AHCI_PX_IS_TFES | AHCI_PX_IS_DHRS;
    m->halted = true;
    m->stats.errors++;
    if (!post_d2h(m, c, STS_ERR, error, 0))
        return fatal(m, AHCI_PX_IS_HBFS);
    return ATA_ERROR;
}

static bool is_multiple(u8 op)
{
    return op == 0xc4 || op == 0xc5 || op == 0x29 || op == 0x39;
}

static enum outcome complete(struct ahci_model *m, const struct ata_cmd *c, enum xfer x, u64 bytes,
                             u8 count)
{
    set_tfd(m, STS_OK, 0);
    m->stats.commands++;
    if (x == XFER_PIO_IN) {
        /* PIO Setup FIS (SATA §10.5.11): D = device to host, I, E_Status final status.  The
           transfer count is one DRQ data block: a sector, or the MULTIPLE setting. */
        u8 fis[20] = {0};
        u64 drq = is_multiple(c->op) ? (u64)m->multiple * AHCI_MODEL_SECTOR : AHCI_MODEL_SECTOR;
        u32 block = (u32)min64(bytes, drq);
        fis[0] = FIS_PIO_SETUP;
        fis[1] = 0x60;
        fis[2] = STS_OK | ATA_STS_DRQ;
        fis[4] = c->fis[4];
        fis[5] = c->fis[5];
        fis[6] = c->fis[6];
        fis[7] = c->fis[7];
        fis[12] = c->fis[12];
        fis[13] = c->fis[13];
        fis[15] = STS_OK;
        fis[16] = (u8)block;
        fis[17] = (u8)(block >> 8);
        m->pxis |= AHCI_PX_IS_PSS;
        if (!post_fis(m, AHCI_RFIS_PSFIS, fis, sizeof(fis)))
            return fatal(m, AHCI_PX_IS_HBFS);
        return DONE;
    }
    m->pxis |= AHCI_PX_IS_DHRS;
    if (!post_d2h(m, c, STS_OK, 0, count))
        return fatal(m, AHCI_PX_IS_HBFS);
    return DONE;
}

/* Move `bytes` between the PRDT and the stream; on success complete the command. */
static enum outcome data_command(struct ahci_model *m, const struct ata_cmd *c, u8 *hdr, u64 ctba,
                                 u32 prdtl, enum xfer x, u64 bytes, struct stream *s)
{
    bool to_guest = x == XFER_DMA_IN || x == XFER_PIO_IN;
    bool irq = false;

    m->bounce_valid = false;
    enum walk w = walk_prdt(m, ctba, prdtl, bytes, to_guest, false, s, &irq);
    /* D5: the backend sees the transfer only once the whole PRDT has been checked. */
    bool backend = !s->buf && m->env->begin;
    if (w == WALK_OK && backend &&
        !m->env->begin(m->env->opaque, s->lba, (u32)(bytes / AHCI_MODEL_SECTOR), !to_guest)) {
        write_prdbc(hdr, 0);
        return ata_error(m, c, ATA_ERR_UNC);
    }
    if (w == WALK_OK)
        w = walk_prdt(m, ctba, prdtl, bytes, to_guest, true, s, &irq);
    else
        backend = false;
    m->bounce_valid = false;
    /* D5: nothing is reported complete before the backend has the data where it belongs. */
    if (backend && !m->env->end(m->env->opaque, w == WALK_OK, !m->write_cache) && w == WALK_OK)
        w = WALK_MEDIUM;
    write_prdbc(hdr, s->pos);

    switch (w) {
        case WALK_OK:
            break;
        case WALK_HBFS:
            model_log(m, "dma-outside-guest-ram", ctba, bytes);
            return fatal(m, AHCI_PX_IS_HBFS);
        case WALK_SHORT:
            /* Data-in overflow sets PxIS.OFS (AHCI §3.3.5, §6.1.5: "Command List Overflow",
               handled as a fatal error here); data-out overflow may skip OFS (§6.1.5) and
               ends in an error: aborted. */
            if (to_guest) {
                model_log(m, "prd-overflow", ctba, bytes);
                return fatal(m, AHCI_PX_IS_OFS);
            }
            return ata_error(m, c, ATA_ERR_ABRT);
        case WALK_FORMAT:
            return ata_error(m, c, ATA_ERR_ABRT);
        case WALK_MEDIUM:
            return ata_error(m, c, ATA_ERR_UNC);
    }

    if (irq)
        m->pxis |= AHCI_PX_IS_DPS;
    if (!s->buf) {
        u64 n = bytes / AHCI_MODEL_SECTOR;
        if (to_guest)
            m->stats.sectors_read += n;
        else
            m->stats.sectors_written += n;
        if (n > m->stats.largest)
            m->stats.largest = (u32)n;
    }
    return complete(m, c, x, bytes, 0);
}

static bool in_range(const struct ahci_model *m, u64 lba, u64 count)
{
    return lba <= m->env->sectors && count <= m->env->sectors - lba;
}

static enum outcome disk_command(struct ahci_model *m, const struct ata_cmd *c, u8 *hdr, u64 ctba,
                                 u32 prdtl, enum xfer x, u64 lba, u64 count)
{
    if (!in_range(m, lba, count))
        return ata_error(m, c, ATA_ERR_IDNF);
    if (m->env->read_only && (x == XFER_DMA_OUT || x == XFER_PIO_OUT)) {
        /* D5 read-only sessions: no write reaches the backend. */
        write_prdbc(hdr, 0);
        return ata_error(m, c, ATA_ERR_ABRT);
    }
    struct stream s = {lba, NULL, 0};
    return data_command(m, c, hdr, ctba, prdtl, x, count * AHCI_MODEL_SECTOR, &s);
}

static void log_opcode(struct ahci_model *m, u8 op)
{
    u32 bit = 1u << (op % 32);
    if (m->logged_opcodes[op / 32] & bit)
        return;
    m->logged_opcodes[op / 32] |= bit;
    model_log(m, "ata-opcode-aborted", op, 0);
}

/* READ LOG (DMA) EXT (ACS-3 §7.24, §7.25): only the GPL directory (log 0, ACS-3 §A.2). */
static enum outcome read_log(struct ahci_model *m, const struct ata_cmd *c, u8 *hdr, u64 ctba,
                             u32 prdtl, enum xfer x)
{
    u8 log = (u8)c->lba48;
    u32 page = (u32)((c->lba48 >> 8) & 0xff) | (u32)((c->lba48 >> 32) & 0xffff) << 8;
    u32 pages = c->fis[12] | c->fis[13] << 8;
    if (log != 0 || page != 0 || pages != 1)
        return ata_error(m, c, ATA_ERR_ABRT);
    __builtin_memset(m->bounce, 0, sizeof(m->bounce));
    m->bounce[0] = 0x01; /* GPL version 1; every log address reports 0 pages */
    struct stream s = {0, m->bounce, 0};
    return data_command(m, c, hdr, ctba, prdtl, x, AHCI_MODEL_SECTOR, &s);
}

/*
 * Set transfer mode (ACS-3 §7.45.8, Table 105): the COUNT field's bits 7:3 are the type and
 * bits 2:0 the mode.  PIO default (00h/01h) and PIO flow control modes 0-4 are accepted
 * (word 64: PIO 3 and 4); a Multiword DMA (0-2) or Ultra DMA (0-6) mode becomes the selected
 * DMA mode, which disables any other.  Anything else is not supported: false.
 */
static bool set_transfer_mode(struct ahci_model *m, u8 value)
{
    u8 type = value >> 3, mode = value & 7;
    switch (type) {
        case 0x00: /* PIO default mode (, disable IORDY) */
            return mode <= 1;
        case 0x01: /* PIO flow control transfer mode */
            return mode <= 4;
        case 0x04: /* Multiword DMA mode */
            if (mode > 2)
                return false;
            m->dma_mode = value;
            return true;
        case 0x08: /* Ultra DMA mode */
            if (mode > 6)
                return false;
            m->dma_mode = value;
            return true;
        default:
            return false;
    }
}

/* SET FEATURES (ACS-3 §7.45). */
static enum outcome set_features(struct ahci_model *m, const struct ata_cmd *c)
{
    switch (c->features & 0xff) {
        case 0x02: /* enable volatile write cache (ACS-3 §7.45.7) */
            m->write_cache = true;
            break;
        case 0x82: /* disable volatile write cache */
            m->write_cache = false;
            break;
        case 0x03: /* set transfer mode (ACS-3 §7.45.8) */
            if (!set_transfer_mode(m, c->fis[12]))
                return ata_error(m, c, ATA_ERR_ABRT);
            break;
        case 0x55: /* disable read look-ahead (ACS-3 §7.45.14) */
        case 0xaa: /* enable read look-ahead */
        case 0x66: /* disable reverting to power-on defaults (ACS-3 §7.45.15) */
        case 0xcc: /* enable reverting to power-on defaults */
            break;
        default:
            return ata_error(m, c, ATA_ERR_ABRT);
    }
    return complete(m, c, XFER_NONE, 0, 0);
}

static enum outcome ata_command(struct ahci_model *m, const struct ata_cmd *c, u8 *hdr, u64 ctba,
                                u32 prdtl)
{
    u32 n16 = c->count16 ? c->count16 : 65536;
    u32 n8 = c->count8 ? c->count8 : 256;

    switch (c->op) {
        case 0xec: { /* IDENTIFY DEVICE (ACS-3 §7.12) */
            /* Staged in the (otherwise idle) bounce buffer, as little-endian bytes. */
            u16 *w = (u16 *)(void *)m->bounce;
            ahci_model_identify(m, w);
            for (unsigned i = 0; i < 256; i++) {
                u16 v = w[i];
                m->bounce[2 * i] = (u8)v;
                m->bounce[2 * i + 1] = (u8)(v >> 8);
            }
            struct stream s = {0, m->bounce, 0};
            return data_command(m, c, hdr, ctba, prdtl, XFER_PIO_IN, AHCI_MODEL_SECTOR, &s);
        }
        case 0x25: /* READ DMA EXT (ACS-3 §7.22) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_DMA_IN, c->lba48, n16);
        case 0x35: /* WRITE DMA EXT (ACS-3 §7.59) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_DMA_OUT, c->lba48, n16);
        case 0xc8: /* READ DMA (ACS-3 §7.21) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_DMA_IN, c->lba28, n8);
        case 0xca: /* WRITE DMA (ACS-3 §7.58) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_DMA_OUT, c->lba28, n8);
        case 0x24: /* READ SECTOR(S) EXT (ACS-3 §7.29) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_PIO_IN, c->lba48, n16);
        case 0x20: /* READ SECTOR(S) (ACS-3 §7.28) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_PIO_IN, c->lba28, n8);
        case 0x34: /* WRITE SECTOR(S) EXT (ACS-3 §7.68) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_PIO_OUT, c->lba48, n16);
        case 0x30: /* WRITE SECTOR(S) (ACS-3 §7.67) */
            return disk_command(m, c, hdr, ctba, prdtl, XFER_PIO_OUT, c->lba28, n8);
        /*
         * READ MULTIPLE (ACS-3 §7.26), READ MULTIPLE EXT (§7.27), WRITE MULTIPLE (§7.64) and
         * WRITE MULTIPLE EXT (§7.65): the PIO path; the DRQ block size only changes the PIO
         * Setup FIS.  Aborted while multiple mode is disabled (word 59 bits 7:0 zero).
         */
        case 0xc4:
        case 0x29:
        case 0xc5:
        case 0x39: {
            bool ext = c->op == 0x29 || c->op == 0x39;
            bool in = c->op == 0xc4 || c->op == 0x29;
            if (!m->multiple) {
                write_prdbc(hdr, 0);
                return ata_error(m, c, ATA_ERR_ABRT);
            }
            return disk_command(m, c, hdr, ctba, prdtl, in ? XFER_PIO_IN : XFER_PIO_OUT,
                                ext ? c->lba48 : c->lba28, ext ? n16 : n8);
        }
        case 0xc6: { /* SET MULTIPLE MODE (ACS-3 §7.46) */
            /* 1, 2, 4, 8 or 16 (word 47); zero is aborted (one of §7.46.2's choices). */
            u8 n = c->fis[12];
            write_prdbc(hdr, 0);
            if (!n || n > 16 || (n & (n - 1)))
                return ata_error(m, c, ATA_ERR_ABRT);
            m->multiple = n;
            return complete(m, c, XFER_NONE, 0, 0);
        }
        case 0x90: /* EXECUTE DEVICE DIAGNOSTIC (ACS-3 §7.9): passed, the signature (§9.2) */
            write_prdbc(hdr, 0);
            m->stats.commands++;
            m->pxis |= AHCI_PX_IS_DHRS;
            if (!post_signature(m, true))
                return fatal(m, AHCI_PX_IS_HBFS);
            return DONE;
        case 0x42:   /* READ VERIFY SECTOR(S) EXT (ACS-3 §7.33): range check only */
        case 0x40: { /* READ VERIFY SECTOR(S) (ACS-3 §7.32) */
            u64 lba = c->op == 0x42 ? c->lba48 : c->lba28;
            u64 n = c->op == 0x42 ? n16 : n8;
            write_prdbc(hdr, 0);
            if (!in_range(m, lba, n))
                return ata_error(m, c, ATA_ERR_IDNF);
            return complete(m, c, XFER_NONE, 0, 0);
        }
        case 0xe7: /* FLUSH CACHE (ACS-3 §7.10) */
        case 0xea: /* FLUSH CACHE EXT (ACS-3 §7.11) */
            write_prdbc(hdr, 0);
            m->stats.flushes++;
            if (m->env->flush && !m->env->flush(m->env->opaque))
                return ata_error(m, c, ATA_ERR_ABRT);
            return complete(m, c, XFER_NONE, 0, 0);
        case 0xef:
            write_prdbc(hdr, 0);
            return set_features(m, c);
        case 0x2f: /* READ LOG EXT (ACS-3 §7.24) */
            return read_log(m, c, hdr, ctba, prdtl, XFER_PIO_IN);
        case 0x47: /* READ LOG DMA EXT (ACS-3 §7.25) */
            return read_log(m, c, hdr, ctba, prdtl, XFER_DMA_IN);
        case 0xe0: /* STANDBY IMMEDIATE (ACS-3 §7.50) */
        case 0xe1: /* IDLE IMMEDIATE (ACS-3 §7.15) */
        case 0xe2: /* STANDBY (ACS-3 §7.49) */
        case 0xe3: /* IDLE (ACS-3 §7.14) */
        case 0x10: /* RECALIBRATE (obsolete in ACS-3; success) */
            write_prdbc(hdr, 0);
            return complete(m, c, XFER_NONE, 0, 0);
        case 0xe5: /* CHECK POWER MODE (ACS-3 §7.3): 0xFF = active or idle */
            write_prdbc(hdr, 0);
            return complete(m, c, XFER_NONE, 0, 0xff);
        default:
            /* NOP, SMART, NCQ, DSM/TRIM, security, the BUFFER and FUA commands and everything
               else (no data moved). */
            write_prdbc(hdr, 0);
            log_opcode(m, c->op);
            return ata_error(m, c, ATA_ERR_ABRT);
    }
}

/* A device-control Register H2D FIS (C = 0): the software reset sequence (AHCI §10.4.1). */
static enum outcome device_control(struct ahci_model *m, const struct ata_cmd *c, u32 dw0)
{
    if (c->fis[15] & 0x04) {
        m->srst = true;
        set_tfd(m, ATA_STS_BSY, 0);
        if (dw0 & BIT(10)) /* C: clear BSY upon R_OK */
            set_tfd(m, 0, 0);
        return DONE;
    }
    if (m->srst) {
        m->srst = false;
        signature(m);
        if (m->halted)
            return FATAL;
    }
    return DONE;
}

/* One command slot (AHCI §5.3, §4.2.2). */
static enum outcome run_slot(struct ahci_model *m, u32 slot)
{
    u64 clb = (u64)m->clbu << 32 | m->clb;
    u8 *hdr = span(m, clb + slot * 32, 32, true);
    if (!hdr) {
        model_log(m, "command-list-outside-guest-ram", clb, slot);
        return fatal(m, AHCI_PX_IS_HBFS);
    }
    u32 dw0 = le32(hdr);
    u64 ctba = ((u64)le32(hdr + 12) << 32 | le32(hdr + 8)) & ~0x7fULL;
    u32 cfl = dw0 & 0x1f;
    u32 prdtl = dw0 >> 16;

    const u8 *cfis = span(m, ctba, 64, false);
    if (!cfis) {
        model_log(m, "command-table-outside-guest-ram", ctba, slot);
        return fatal(m, AHCI_PX_IS_HBFS);
    }

    struct ata_cmd c;
    __builtin_memcpy(c.fis, cfis, sizeof(c.fis));
    c.op = c.fis[2];
    c.features = (u16)(c.fis[3] | c.fis[11] << 8);
    c.lba48 = (u64)c.fis[4] | (u64)c.fis[5] << 8 | (u64)c.fis[6] << 16 | (u64)c.fis[8] << 24 |
              (u64)c.fis[9] << 32 | (u64)c.fis[10] << 40;
    c.lba28 =
        (u32)c.fis[4] | (u32)c.fis[5] << 8 | (u32)c.fis[6] << 16 | (u32)(c.fis[7] & 0xf) << 24;
    c.count16 = (u32)c.fis[12] | (u32)c.fis[13] << 8;
    c.count8 = c.fis[12];
    /* Diagnostics only (the carrier's fatal report): the command FIS as fetched. */
    if (m->env->command)
        m->env->command(m->env->opaque, slot, c.fis);

    if (cfl < 5 || cfl > 16 || c.fis[0] != FIS_REG_H2D) {
        write_prdbc(hdr, 0);
        return ata_error(m, &c, ATA_ERR_ABRT);
    }
    if (!(c.fis[1] & 0x80)) {
        write_prdbc(hdr, 0);
        return device_control(m, &c, dw0);
    }
    if (dw0 & BIT(5)) { /* A: ATAPI, no packet device here */
        write_prdbc(hdr, 0);
        return ata_error(m, &c, ATA_ERR_ABRT);
    }
    return ata_command(m, &c, hdr, ctba, prdtl);
}

/* Issue every slot set in PxCI, lowest first, until one fails (AHCI §5.3.2, §6.2.2). */
static void process(struct ahci_model *m)
{
    if (!(m->cmd & AHCI_PX_CMD_ST) || m->halted || m->comreset)
        return;
    for (u32 slot = 0; slot < AHCI_MODEL_SLOTS && m->ci; slot++) {
        if (!(m->ci & BIT(slot)))
            continue;
        m->ccs = slot;
        if (run_slot(m, slot) != DONE)
            break;
        m->ci &= ~(u32)BIT(slot);
    }
}

/* ---- registers --------------------------------------------------------------------------- */

static u32 read_cmd(const struct ahci_model *m)
{
    u32 v = m->cmd | AHCI_PX_CMD_SUD | AHCI_PX_CMD_POD;
    if (m->cmd & AHCI_PX_CMD_ST)
        v |= AHCI_PX_CMD_CR | AHCI_PX_CMD_CCS(m->ccs);
    if (m->cmd & AHCI_PX_CMD_FRE)
        v |= AHCI_PX_CMD_FR;
    return v;
}

u32 ahci_model_read(struct ahci_model *m, u32 off)
{
    if (off >= AHCI_MODEL_ABAR_SIZE || (off & 3))
        return 0;
    switch (off) {
        case AHCI_CAP:
            return AHCI_MODEL_CAP;
        case AHCI_GHC:
            return AHCI_GHC_AE | (m->ghc & AHCI_GHC_IE);
        case AHCI_IS:
            return m->is;
        case AHCI_PI:
            return 1;
        case AHCI_VS:
            return AHCI_MODEL_VS;
        case PX(AHCI_PX_CLB):
            return m->clb;
        case PX(AHCI_PX_CLBU):
            return m->clbu;
        case PX(AHCI_PX_FB):
            return m->fb;
        case PX(AHCI_PX_FBU):
            return m->fbu;
        case PX(AHCI_PX_IS):
            return port_is(m);
        case PX(AHCI_PX_IE):
            return m->pxie;
        case PX(AHCI_PX_CMD):
            return read_cmd(m);
        case PX(AHCI_PX_TFD):
            return m->tfd;
        case PX(AHCI_PX_SIG):
            return m->sig;
        case PX(AHCI_PX_SSTS):
            return m->ssts;
        case PX(AHCI_PX_SCTL):
            return m->sctl;
        case PX(AHCI_PX_SERR):
            return m->serr;
        case PX(AHCI_PX_SACT):
            return m->sact;
        case PX(AHCI_PX_CI):
            return m->ci;
        default:
            /* CCC, EM, CAP2, BOHC, vendor space, port 0 reserved/VS, ports 1-31: 0. */
            return 0;
    }
}

static u32 merge(u32 old, u32 value, u32 mask)
{
    return (old & ~mask) | (value & mask);
}

static void write_cmd(struct ahci_model *m, u32 value, u32 mask)
{
    u32 v = merge(read_cmd(m), value, mask);
    bool was_st = m->cmd & AHCI_PX_CMD_ST;

    /* CLO: clear PxTFD.STS.BSY and DRQ, then read back 0 (AHCI §3.3.7, CAP.SCLO = 1). */
    if (value & mask & AHCI_PX_CMD_CLO)
        m->tfd &= ~(u32)(ATA_STS_BSY | ATA_STS_DRQ);
    m->cmd = v & PXCMD_RW;
    if (was_st && !(m->cmd & AHCI_PX_CMD_ST)) {
        /* ST 1 -> 0: PxCI, PxSACT and CCS clear; the engine is idle at once (CR = 0). */
        m->ci = 0;
        m->sact = 0;
        m->ccs = 0;
        m->halted = false;
    } else if (!was_st && (m->cmd & AHCI_PX_CMD_ST)) {
        m->ccs = 0;
    }
}

static void write_sctl(struct ahci_model *m, u32 value)
{
    u32 det = value & 0xf;
    m->sctl = value & 0xfffff;
    if (det == 1 || det == 4) {
        /* COMRESET held (1) or the interface off-line (4) (AHCI §3.3.11). */
        m->comreset = true;
        m->ssts = det == 4 ? 4 : 0;
        set_tfd(m, ATA_STS_BSY, 0);
    } else if (det == 0 && m->comreset) {
        m->comreset = false;
        m->srst = false;
        link_up(m);
    }
}

void ahci_model_write(struct ahci_model *m, u32 off, u32 value, u32 mask)
{
    if (off >= AHCI_MODEL_ABAR_SIZE || (off & 3) || !mask)
        return;
    u32 w1 = value & mask; /* for write-1-to-clear / write-1-to-set registers */
    switch (off) {
        case AHCI_GHC:
            if (w1 & AHCI_GHC_HR) {
                /* HBA reset completes inside the write; the disk contents are untouched. */
                hba_reset(m);
                return;
            }
            m->ghc = merge(m->ghc, value, mask) & AHCI_GHC_IE;
            break;
        case AHCI_IS:
            m->is &= ~w1;
            break;
        case PX(AHCI_PX_CLB):
            if (!(m->cmd & AHCI_PX_CMD_ST))
                m->clb = merge(m->clb, value, mask) & ~0x3ffu;
            break;
        case PX(AHCI_PX_CLBU):
            if (!(m->cmd & AHCI_PX_CMD_ST))
                m->clbu = merge(m->clbu, value, mask);
            break;
        case PX(AHCI_PX_FB):
            if (!(m->cmd & AHCI_PX_CMD_FRE))
                m->fb = merge(m->fb, value, mask) & ~0xffu;
            break;
        case PX(AHCI_PX_FBU):
            if (!(m->cmd & AHCI_PX_CMD_FRE))
                m->fbu = merge(m->fbu, value, mask);
            break;
        case PX(AHCI_PX_IS):
            m->pxis &= ~(w1 & PXIS_RWC);
            break;
        case PX(AHCI_PX_IE):
            m->pxie = merge(m->pxie, value, mask) & PXIE_MASK;
            break;
        case PX(AHCI_PX_CMD):
            write_cmd(m, value, mask);
            break;
        case PX(AHCI_PX_SCTL):
            write_sctl(m, merge(m->sctl, value, mask));
            break;
        case PX(AHCI_PX_SERR):
            m->serr &= ~w1;
            break;
        case PX(AHCI_PX_SACT):
            if (m->cmd & AHCI_PX_CMD_ST)
                m->sact |= w1;
            break;
        case PX(AHCI_PX_CI):
            if (m->cmd & AHCI_PX_CMD_ST) {
                m->ci |= w1;
                process(m);
            }
            break;
        default:
            /* Read-only (CAP, PI, VS, TFD, SIG, SSTS) and unimplemented: ignored. */
            break;
    }
    update_irq(m);
}

bool ahci_model_mmio(struct ahci_model *m, u64 off, u32 width, u64 *val, bool write)
{
    if (width > 3 || off >= AHCI_MODEL_ABAR_SIZE || (off & ((1u << width) - 1))) {
        if (!write)
            *val = 0;
        return false;
    }
    u32 base = (u32)off & ~3u;
    if (width == 3) {
        if (write) {
            ahci_model_write(m, base, (u32)*val, 0xffffffffu);
            ahci_model_write(m, base + 4, (u32)(*val >> 32), 0xffffffffu);
        } else {
            *val = ahci_model_read(m, base) | (u64)ahci_model_read(m, base + 4) << 32;
        }
        return true;
    }
    u32 shift = ((u32)off & 3) * 8;
    u32 mask = (width == 2 ? 0xffffffffu : ((1u << (8u << width)) - 1)) << shift;
    if (write)
        ahci_model_write(m, base, (u32)(*val << shift), mask);
    else
        *val = (ahci_model_read(m, base) & mask) >> shift;
    return true;
}

/* ---- CRC-32 ------------------------------------------------------------------------------ */

#if defined(__aarch64__)
__attribute__((target("crc"))) u32 ahci_crc32(u32 crc, const void *buf, u64 len)
{
    const u8 *p = buf;
    crc = ~crc;
    while (len && ((uintptr_t)p & 7)) {
        crc = __builtin_arm_crc32b(crc, *p++);
        len--;
    }
    for (; len >= 8; len -= 8, p += 8)
        crc = __builtin_arm_crc32d(crc, *(const u64 *)p);
    while (len--)
        crc = __builtin_arm_crc32b(crc, *p++);
    return ~crc;
}
#else
u32 ahci_crc32(u32 crc, const void *buf, u64 len)
{
    const u8 *p = buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
#endif
