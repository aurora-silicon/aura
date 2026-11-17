/* SPDX-License-Identifier: MIT */

#ifndef AHCI_MODEL_H
#define AHCI_MODEL_H

#include "types.h"

/*
 * A software AHCI 1.3.1 host bus adapter with one SATA port and one ATA disk behind it.
 *
 * Clean room.  Written only from the public specifications:
 *  - Intel, "Serial ATA Advanced Host Controller Interface (AHCI) 1.3.1" (cited as AHCI §n);
 *  - T13, "ATA/ATAPI Command Set - 3" (INCITS 522-2014, also ISO/IEC 17760-103), cited as
 *    ACS-3 §n with ACS-3's own clause numbers (the T13/2161-D revision 5 draft numbers them
 *    the same way); nothing here is cited from ACS-4;
 *  - Serial ATA Revision 3.x for the FIS layouts (SATA §10.5).
 * No code from QEMU hw/ide, Linux libahci/libata or any vendor driver was used.
 *
 * Pure logic, like hv_gic_cpuif.c: no system registers, no locks, no printing.  The carrier
 * (hv_ahci.c) or a host test drives it through struct ahci_env and the two register entry
 * points below.  The caller serializes every call on one model.
 *
 * Model choices (all within AHCI 1.3.1 and ACS-3):
 *  - CAP 0x81341F00: 64-bit addressing, command list override, Gen3, AHCI-only, 32 command
 *    slots, one port; no NCQ, no staggered spin-up, no ALPM, no port multiplier, no FIS-based
 *    switching, no CCC, no enclosure management, no hot plug, no cold presence detect;
 *  - commands complete synchronously inside the PxCI write that issues them, in ascending
 *    slot order;
 *  - 512-byte logical and physical sectors, LBA48, a non-rotating device, no SMART, no
 *    security, no TRIM, volatile write cache (FLUSH CACHE is the backend's flush);
 *  - the ACS-3 §4.2 General feature set: its mandatory commands (EXECUTE DEVICE DIAGNOSTIC,
 *    IDENTIFY DEVICE, SET FEATURES) and, of its optional ones, FLUSH CACHE, READ/WRITE DMA,
 *    READ/WRITE MULTIPLE, READ/WRITE SECTOR(S), READ VERIFY SECTOR(S) and SET MULTIPLE MODE
 *    (16 sectors per DRQ block at most).  NOP, the BUFFER commands, DOWNLOAD MICROCODE, DATA
 *    SET MANAGEMENT, SET DATE & TIME and WRITE UNCORRECTABLE EXT (all optional) are aborted;
 *  - SET FEATURES "set transfer mode" (ACS-3 §7.45.8) is recorded and IDENTIFY words 63 and
 *    88 report the selected DMA mode; power-on and hardware resets (COMRESET, GHC.HR) restore
 *    the defaults (UDMA 6, write cache on, 16 sectors per DRQ block): no Software Settings
 *    Preservation (ACS-3 §4.21) is offered;
 *  - every guest memory span (command list, FIS area, command table, PRDT, data buffers) is
 *    passed to env->guest_span before it is touched; a NULL answer is a host bus fatal error
 *    (PxIS.HBFS) and nothing outside the returned span is read or written.
 */

#define AHCI_MODEL_ABAR_SIZE 0x4000u
#define AHCI_MODEL_CAP       0x81341f00u
#define AHCI_MODEL_VS        0x00010301u
#define AHCI_MODEL_SECTOR    512u
#define AHCI_MODEL_SLOTS     32u
#define AHCI_MODEL_PORT0     0x100u
#define AHCI_MODEL_PORT_END  0x180u
/* ACS-3 §7.22/§7.59: READ/WRITE DMA EXT move at most 65536 sectors (count 0). */
#define AHCI_MODEL_MAX_SECTORS 65536u

/* Generic host control (AHCI §3.1). */
#define AHCI_CAP     0x00
#define AHCI_GHC     0x04
#define AHCI_IS      0x08
#define AHCI_PI      0x0c
#define AHCI_VS      0x10
#define AHCI_CCC_CTL 0x14
#define AHCI_CCC_PTS 0x18
#define AHCI_EM_LOC  0x1c
#define AHCI_EM_CTL  0x20
#define AHCI_CAP2    0x24
#define AHCI_BOHC    0x28

#define AHCI_GHC_HR BIT(0)
#define AHCI_GHC_IE BIT(1)
#define AHCI_GHC_AE BIT(31)

/* Port registers, offsets from AHCI_MODEL_PORT0 (AHCI §3.3). */
#define AHCI_PX_CLB    0x00
#define AHCI_PX_CLBU   0x04
#define AHCI_PX_FB     0x08
#define AHCI_PX_FBU    0x0c
#define AHCI_PX_IS     0x10
#define AHCI_PX_IE     0x14
#define AHCI_PX_CMD    0x18
#define AHCI_PX_TFD    0x20
#define AHCI_PX_SIG    0x24
#define AHCI_PX_SSTS   0x28
#define AHCI_PX_SCTL   0x2c
#define AHCI_PX_SERR   0x30
#define AHCI_PX_SACT   0x34
#define AHCI_PX_CI     0x38
#define AHCI_PX_SNTF   0x3c
#define AHCI_PX_FBS    0x40
#define AHCI_PX_DEVSLP 0x44

/* PxIS / PxIE (AHCI §3.3.5, §3.3.6). */
#define AHCI_PX_IS_DHRS BIT(0)
#define AHCI_PX_IS_PSS  BIT(1)
#define AHCI_PX_IS_DSS  BIT(2)
#define AHCI_PX_IS_SDBS BIT(3)
#define AHCI_PX_IS_UFS  BIT(4)
#define AHCI_PX_IS_DPS  BIT(5)
#define AHCI_PX_IS_PCS  BIT(6)
#define AHCI_PX_IS_DMPS BIT(7)
#define AHCI_PX_IS_PRCS BIT(22)
#define AHCI_PX_IS_IPMS BIT(23)
#define AHCI_PX_IS_OFS  BIT(24)
#define AHCI_PX_IS_INFS BIT(26)
#define AHCI_PX_IS_IFS  BIT(27)
#define AHCI_PX_IS_HBDS BIT(28)
#define AHCI_PX_IS_HBFS BIT(29)
#define AHCI_PX_IS_TFES BIT(30)
#define AHCI_PX_IS_CPDS BIT(31)

/* PxCMD (AHCI §3.3.7). */
#define AHCI_PX_CMD_ST     BIT(0)
#define AHCI_PX_CMD_SUD    BIT(1)
#define AHCI_PX_CMD_POD    BIT(2)
#define AHCI_PX_CMD_CLO    BIT(3)
#define AHCI_PX_CMD_FRE    BIT(4)
#define AHCI_PX_CMD_CCS(n) ((u32)(n) << 8)
#define AHCI_PX_CMD_FR     BIT(14)
#define AHCI_PX_CMD_CR     BIT(15)
#define AHCI_PX_CMD_ATAPI  BIT(24)
#define AHCI_PX_CMD_DLAE   BIT(25)

/* PxTFD status bits (ACS-3 §6.2, AHCI §3.3.8). */
#define ATA_STS_ERR  BIT(0)
#define ATA_STS_DRQ  BIT(3)
#define ATA_STS_DSC  BIT(4)
#define ATA_STS_DRDY BIT(6)
#define ATA_STS_BSY  BIT(7)
/* ATA ERROR field bits (ACS-3 §6.3). */
#define ATA_ERR_ABRT 0x04
#define ATA_ERR_IDNF 0x10
#define ATA_ERR_UNC  0x40

/* PxSERR.DIAG (AHCI §3.3.12): N (PhyRdy change) and X (exchanged). */
#define AHCI_SERR_DIAG_N BIT(16)
#define AHCI_SERR_DIAG_X BIT(26)

#define AHCI_SIG_ATA 0x00000101u
#define AHCI_SSTS_UP 0x133u /* IPM active, SPD Gen3, DET present + phy communication */

/* FIS receive area offsets (AHCI §4.2.1). */
#define AHCI_RFIS_PSFIS 0x20
#define AHCI_RFIS_RFIS  0x40

struct ahci_env {
    /*
     * A host pointer to guest IPA span [ipa, ipa + len) if the whole span lies inside guest
     * RAM, else NULL.  len is never 0.  `write` says the model will store into it.
     */
    void *(*guest_span)(void *opaque, u64 ipa, u64 len, bool write);
    /* The disk: 512-byte sectors, lba + count <= sectors guaranteed.  false = medium error. */
    bool (*read)(void *opaque, u64 lba, u32 count, void *dst);
    bool (*write)(void *opaque, u64 lba, u32 count, const void *src);
    bool (*flush)(void *opaque);
    /* The interrupt line level changed (AHCI §10.7.1). */
    void (*irq_level)(void *opaque, bool level);
    /* Rare events for the carrier's log (rate limiting is the caller's business). */
    void (*log)(void *opaque, const char *event, u64 a, u64 b);
    void *opaque;
    u64 sectors;
    /*
     * Optional, may be NULL: each command slot about to run, with its 20-byte H2D register FIS
     * as fetched (before any check).  An observer only; it must not touch the model.
     */
    void (*command)(void *opaque, u32 slot, const u8 *fis);
};

struct ahci_model_stats {
    u64 commands; /* completed without error */
    u64 errors;   /* ATA errors (TFES) */
    u64 fatal;    /* HBFS / OFS */
    u64 sectors_read;
    u64 sectors_written;
    u64 flushes;
    u32 largest; /* largest single transfer, sectors */
};

struct ahci_model {
    const struct ahci_env *env;
    u32 ghc;
    u32 is;
    /* Port 0. */
    u32 clb, clbu, fb, fbu;
    u32 pxis, pxie;
    u32 cmd; /* stored RW bits only: ST, FRE, ATAPI, DLAE; the rest is composed on read */
    u32 ccs;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    bool halted;   /* TFES or a fatal error: nothing is processed until ST is cleared */
    bool comreset; /* SCTL.DET = 1 is held */
    bool srst;     /* a device-control FIS with SRST set was seen */
    bool line;     /* the level last given to env->irq_level */
    /* Device settings that SET FEATURES / SET MULTIPLE MODE change (ACS-3 §7.45, §7.46). */
    bool write_cache;
    u8 dma_mode; /* the selected DMA mode, a set-transfer-mode count: 0x20|n MWDMA, 0x40|n UDMA */
    u8 multiple; /* logical sectors per DRQ block for the MULTIPLE commands; 0 = disabled */
    /* Partial-sector staging for PRD entries that do not end on a sector boundary. */
    u64 bounce_lba;
    bool bounce_valid;
    /* Also stages IDENTIFY and log data; aligned for the u16 IDENTIFY words. */
    u8 bounce[AHCI_MODEL_SECTOR] __attribute__((aligned(8)));
    u32 logged_opcodes[8];
    struct ahci_model_stats stats;
};

/* Power-on: the HBA reset state with the device present and its signature received. */
void ahci_model_init(struct ahci_model *m, const struct ahci_env *env);

/*
 * One aligned 32-bit register access at `off` (0 .. AHCI_MODEL_ABAR_SIZE - 4, multiple of 4).
 * For a write, `mask` selects the bytes the guest wrote (0xffffffff for a 32-bit store);
 * unwritten bytes of a read/write register keep their value, and unwritten bytes of a
 * write-1-to-clear or write-1-to-set register are written as 0.  Unimplemented offsets
 * read 0 and ignore writes.
 */
u32 ahci_model_read(struct ahci_model *m, u32 off);
void ahci_model_write(struct ahci_model *m, u32 off, u32 value, u32 mask);

/*
 * A trapped guest access of 1 << width bytes at ABAR offset `off` (width 0..3).  8/16-bit
 * accesses inside one dword are merged into that dword (the other bytes keep their value, or
 * are written as 0 for write-1 registers); an aligned 64-bit access is two dword accesses,
 * low first.  Unaligned, dword-crossing, wider or out-of-range accesses read 0, write
 * nothing and return false so the caller can log them once.
 */
bool ahci_model_mmio(struct ahci_model *m, u64 off, u32 width, u64 *val, bool write);

/* The interrupt line: GHC.IE && (IS & PI) (AHCI §10.7.1.1). */
bool ahci_model_line(const struct ahci_model *m);

/* IDENTIFY DEVICE data (ACS-3 §7.12.7) for this model's disk, as 256 words. */
void ahci_model_identify(const struct ahci_model *m, u16 words[256]);

/*
 * CRC-32 (ISO-HDLC: reflected polynomial 0xEDB88320, initial and final XOR 0xFFFFFFFF), the
 * CRC zlib's crc32() computes.  Chain by passing the previous result.  Uses the Armv8 CRC32
 * instructions when built for AArch64.
 */
u32 ahci_crc32(u32 crc, const void *buf, u64 len);

#endif
