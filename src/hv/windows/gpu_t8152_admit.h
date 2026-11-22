/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef GPU_T8152_ADMIT_H
#define GPU_T8152_ADMIT_H

#include "types.h"

/*
 * G0 (windows-m6/WINDOWS-GPU-PLAN.md, H2/H3): the G18G GPU admission port for the Windows
 * carrier, and the G0 range table the carrier and Mu's GPU0 node share.
 *
 * gpu_t8152_admit() runs the checks src/kboot_gpu_t8152.c makes before Linux may touch the GPU,
 * against the live ADT only (the carrier has no FDT): chip and board, the sgx node, the eight
 * firmware regions, the gfx-asc/gfx1-asc shape, both firmware TEXT identities (profile UUID,
 * TLVs against this boot's ADT, SHA-256), plus what the carrier needs on top: the ASC wrappers
 * and iop-vbar words inside the sgx window, one post-office bank for both roles outside it, the
 * UAT regions above boot_args RAM, the firmware segments below it, the ASC interrupt lines and
 * the perf-state table.  Every value is lifted from the ADT at run time; nothing per machine is
 * compiled in.  It never starts, powers or touches the GPU: the only memory it reads besides the
 * ADT is the TEXT that iBoot left in RAM (through env->read_text).
 *
 * gpu_t8152_build_ranges() turns an admission into the 16 KiB-granular class table (enum
 * gpu_class).  Inside the sgx window every page that any other /arm-io node also claims
 * (pmgr-child, error-handler, ...) is a hole, rounded outward; the wrapper page that holds
 * CPU_CONTROL/STATUS and the pages that hold the iop-vbar words get classes of their own.
 */

#define GPU_T8152_ROLES        2
#define GPU_T8152_REGIONS      8
#define GPU_T8152_PAGE         0x4000UL
#define GPU_T8152_WRAPPER_SIZE 0x74000UL  /* ADT gfx-asc reg 0 */
#define GPU_T8152_VBAR_BELOW   0x5b0000UL /* ADT gfx-asc reg 1 = reg 0 - this, 8 bytes */
#define GPU_T8152_VBAR_SIZE    8
#define GPU_T8152_MAILBOX_SIZE 0x10000UL /* ADT gfx-asc reg 2, shared by both roles */
#define GPU_T8152_PERF_MAX     32
/* ASC CPU_CONTROL / CPU_STATUS, as src/asc.c names them: in the wrapper's first page. */
#define GPU_T8152_ASC_CPU_CONTROL 0x44
#define GPU_T8152_ASC_CPU_STATUS  0x48
/* CPU_CONTROL RUN (m1n1 hw/asc.py; our traces: power-on leaves 0x10, ledger B-03/B-04). */
#define GPU_ASC_CPU_RUN BIT(4)
/*
 * The two GFX windows of the shared post-office page: [num * 0x100, num * 0x100 + 0x80) for the
 * ADT's msgbox-mailbox-num 1 (gfx-asc) and 2 (gfx1-asc).  Source: the ADT value, and our own
 * macOS-under-hypervisor trace of 2026-11-10 (run 125300), where both roles' RTKit traffic used
 * exactly bank + 0x100 and bank + 0x200 (RX +0x00, TX +0x10, status +0x20).  Used only to label
 * logged accesses in G0; nothing is forwarded before S2 checks it under our own code.
 */
#define GPU_T8152_MBOX_STRIDE 0x100UL
#define GPU_T8152_MBOX_WINDOW 0x80UL

/* One firmware identity (src/kboot_gpu_t8152_profile.h). */
struct gpu_profile {
    const char *role, *uuid;
    u32 text_size, patchbay;
    u8 digest[32];
};

/* One patchbay TLV: tag (reversed, as stored), payload size, zeroed before the digest. */
struct gpu_tlv {
    char tag[5];
    u32 size;
    bool exclude;
};

/*
 * The J804 identities and the patchbay TLV sequence, from src/kboot_gpu_t8152_profile.h (the
 * J804 entries are copied from J804_PRIVATE_DATA at build time; nothing per machine is in git).
 */
const struct gpu_profile *gpu_t8152_j804_profiles(void);
const struct gpu_tlv *gpu_t8152_tlvs(u32 *count);

/* What the admission needs from its caller; host tests pass synthetic values. */
struct gpu_t8152_env {
    u32 chip_id, board_id;
    u64 ram_base, ram_top; /* boot_args RAM [phys_base, phys_base + mem_size) */
    const struct gpu_profile *profiles; /* [2]: gfx-asc, gfx1-asc */
    const struct gpu_tlv *tlvs;
    u32 tlv_count;
    const char *receipt; /* the firmware this profile pins, e.g. "G18G-RTKit-<ver>.release" */
    /* Copy the iBoot-loaded TEXT [phys, phys + size) into buf; false if it cannot. */
    bool (*read_text)(u64 phys, void *buf, u64 size);
    void (*sha256)(const void *data, size_t size, u8 digest[32]);
};

struct gpu_t8152_span {
    u64 base, size;
};

/* Order of gpu_t8152_admission.region[]: the kboot order (ADT <name>-base / -size of sgx). */
enum gpu_t8152_region {
    GPU_R_TTBS = 0,      /* gpu-region */
    GPU_R_PAGETABLES,    /* gfx-shared-region */
    GPU_R_SHARED_L2,     /* gfx-shared-l2-region */
    GPU_R_HANDOFF,       /* gfx-handoff */
    GPU_R_DATA,          /* gfx-data (gfx-asc __DATA) */
    GPU_R_DATA1,         /* gfx1-data */
    GPU_R_DATA_RO,       /* gfx-data-shared-ro (gfx-asc __DATA_SHARED_RO) */
    GPU_R_DATA1_RO,      /* gfx1-data-shared-ro */
};

struct gpu_t8152_admission {
    u32 chip_id, board_id;
    u64 dram_base, dram_size; /* /chosen dram-base, dram-size */
    u64 vm_base;              /* sgx rtkit-private-vm-region-base */
    struct gpu_t8152_span region[GPU_T8152_REGIONS];
    struct gpu_t8152_span text[GPU_T8152_ROLES];    /* __TEXT, never mapped */
    struct gpu_t8152_span sgx;                      /* sgx reg 0: the register window */
    struct gpu_t8152_span sgx1;                     /* sgx reg 1, inside reg 0 */
    struct gpu_t8152_span wrapper[GPU_T8152_ROLES]; /* reg 0 of gfx-asc, gfx1-asc */
    struct gpu_t8152_span vbar[GPU_T8152_ROLES];    /* reg 1: the iop-vbar word */
    struct gpu_t8152_span mailbox;                  /* reg 2: the post-office bank */
    u32 mailbox_num[GPU_T8152_ROLES];
    u32 irq[GPU_T8152_ROLES][2]; /* each ASC's ADT interrupts, in ADT order */
    u32 perf_count;
    const u8 *perf_states, *perf_sram; /* ADT perf-states / perf-states-sram, 8 bytes each */
    const char *receipt;
    const char *uuid[GPU_T8152_ROLES];
};

/*
 * Read the live ADT (the global adt) and admit the GPU firmware, or refuse.  0, or -1 with the
 * reason printed; on refusal *out is zeroed.  Pure: no MMIO, no state, callable more than once
 * (the carrier itself allows one admission per session, hv_gpu.c).
 */
int gpu_t8152_admit(const struct gpu_t8152_env *env, struct gpu_t8152_admission *out);

/* Stage-2 classes of the G0 range table (and of the handoff page, which Mu reads). */
enum gpu_class {
    GPU_C_NONE = 0,
    GPU_C_SGX = 1,      /* sgx register window: G0 logging hook; passthrough after S2 */
    GPU_C_SGX_HOLE = 2, /* another node's registers inside the window: never passed through */
    GPU_C_ASC_CTRL = 3, /* wrapper page with CPU_CONTROL/STATUS: carrier-owned (RUN never cleared) */
    GPU_C_ASC = 4,      /* the rest of an ASC wrapper */
    GPU_C_IOP_VBAR = 5, /* page holding an iop-vbar word: carrier-pinned, guest read-only */
    GPU_C_MAILBOX = 6,  /* post-office page with both GFX windows (shared with other IOPs) */
    GPU_C_FW_RW = 7,    /* UAT regions: identity, RW (admitted firmware carve-out) */
    GPU_C_FW_RO = 8,    /* __DATA, __DATA_SHARED_RO: identity, RO */
    GPU_C_FW_TEXT = 9,  /* __TEXT: denied, never mapped */
    GPU_C_POOL = 10,    /* GPU-visible pool inside guest RAM (Mu reserves it) */
    GPU_C_HANDOFF = 11, /* the carrier's read-only handoff page */
    /*
     * G2: a register window the GPU firmware's own page tables may map (declared, never mapped
     * to the guest CPU).  Published only in the handoff page, after the table ranges, and only
     * when the launcher granted the declaration (gpu_t8152_g2.h); it may overlap sgx ranges and
     * other declarations, so it is never part of the class table gpu_t8152_find() searches.
     */
    GPU_C_FW_MMIO = 12,
    GPU_C_COUNT = 13,
};

struct gpu_range {
    u64 base, size;
    u32 cls;  /* enum gpu_class */
    u32 tag;  /* role for ASC classes, region index for firmware, 0 otherwise */
    u32 flags; /* GPU_RF_* (set by the carrier as it acts on the range) */
    const char *name;
};

#define GPU_RF_HOOKED    BIT(0) /* covered by the carrier's logging hook */
#define GPU_RF_MAPPED    BIT(1) /* the guest has a hardware stage-2 entry */
#define GPU_RF_READ_ONLY BIT(2)
#define GPU_RF_DENIED    BIT(3) /* on the carrier's stage-2 denylist */
#define GPU_RF_ALLOWED   BIT(4) /* registered with the stage-2 policy, not mapped */
#define GPU_RF_DECLARED  BIT(5) /* G2 fw-mmio: declared to the GPU firmware, not to the guest CPU */

#define GPU_T8152_MAX_HOLES  64
#define GPU_T8152_MAX_RANGES 96

const char *gpu_class_name(u32 cls);

/*
 * Every register range of every other /arm-io node (all but sgx, gfx-asc, gfx1-asc) that
 * intersects the sgx window, clipped to it.  Count, or -1 if there are more than cap.
 */
int gpu_t8152_collect_holes(const struct gpu_t8152_admission *a, struct gpu_range *holes,
                            u32 cap);

/*
 * The class table: the sgx window page by page (hole > iop-vbar > CPU_CONTROL page > wrapper >
 * sgx), coalesced; then the post-office page, the four UAT regions (RW), the four DATA regions
 * (RO) and both TEXT segments.  Every range is 16 KiB aligned; ranges never overlap.  Count, or
 * -1 (with the reason printed) if the table does not fit or the layout breaks a rule.
 */
int gpu_t8152_build_ranges(const struct gpu_t8152_admission *a, const struct gpu_range *holes,
                           u32 nholes, struct gpu_range *out, u32 cap);

/* Append one range (pool, handoff page): aligned, non-empty, overlapping nothing.  0 or -1. */
int gpu_t8152_add_range(struct gpu_range *table, u32 *count, u32 cap, u64 base, u64 size,
                        u32 cls, const char *name);

/* The range holding addr, or NULL. */
const struct gpu_range *gpu_t8152_find(const struct gpu_range *table, u32 count, u64 addr);

/* Is addr inside a GFX post-office window?  *role is 0 (gfx-asc) or 1 (gfx1-asc). */
bool gpu_t8152_mailbox_window(const struct gpu_t8152_admission *a, u64 addr, u32 *role);

/*
 * The handoff page Mu and the guest driver read (read-only to the guest).  Little endian; all
 * offsets are from the start of the page.  Mu fails closed unless magic, version, header size,
 * total size and the CRC-32 (zlib, over [0, total) with the crc field zero) all match.
 */
#define GPU_HANDOFF_MAGIC   0x31484741u /* "AGH1" */
#define GPU_HANDOFF_VERSION 1
#define GPU_HANDOFF_SIZE    0x4000UL
#define GPU_HANDOFF_HEADER  0x100
#define GPU_HANDOFF_RANGE   32 /* u64 base, u64 size, u32 class, u32 tag, u32 flags, u32 0 */
#define GPU_HANDOFF_PERF    16 /* u32 freq (Hz), u32 core mV, u32 SRAM mV, u32 0 */
/* G1: the ID words read after power-on (the six GPU ID words and the core mask). */
#define GPU_HANDOFF_ID_WORDS 7

/* Handoff state bits (offset 0x10): what the carrier has done, not what it may do later. */
#define GPU_HS_ADMITTED     BIT(0)
#define GPU_HS_HOOKS_LOG    BIT(1) /* logging hooks (RAZ/WI) on the sgx window and mailbox */
#define GPU_HS_POWERED      BIT(2) /* G1: every GPU PSD service acked and on (hv_gpu.c POWER) */
#define GPU_HS_IRQ_LIVE     BIT(3) /* ASC lines published (not yet: G1 polls) */
/*
 * CPU_CONTROL RUN state guarded (G1 PROBE).  Our J804 Linux logs and the macOS traces show RUN
 * already set once PSD power-on completes, before any host write; macOS only rewrites the same
 * value.  So the carrier refuses any write that clears or toggles RUN rather than granting one
 * RUN, pending S2 confirmation under the carrier.
 */
#define GPU_HS_ASC_RUN      BIT(4)
/*
 * G1 PROBE: the measured registers are forwarded by the carrier's hooks (gpu_t8152_probe.h);
 * nothing in the sgx window or the post-office page is mapped to the guest directly yet.
 */
#define GPU_HS_MMIO_PASS    BIT(5)
#define GPU_HS_FW_MAPPED    BIT(6) /* G1 PROBE: UAT regions mapped RW, DATA regions RO (IPA = PA) */
/*
 * G2: the carrier performed the private recipe's one checked sgx boot-control step at POWER
 * (gpu_t8152_g2.h); header word GPU_HO_BOOT_CONTROL holds the value it read back.
 */
#define GPU_HS_SGX_BOOT     BIT(7)

struct gpu_t8152_handoff_in {
    u32 state;
    u64 handoff_ipa;
    u64 pool_base, pool_size;
    /* G1, valid once GPU_HS_POWERED is set (zero before): what the carrier read after power-on. */
    u32 gpu_id[GPU_HANDOFF_ID_WORDS]; /* forwarded ID words, gpu_t8152_probe_sgx_reads() order */
    u32 cpu_control[GPU_T8152_ROLES], cpu_status[GPU_T8152_ROLES];
    /* G2: the boot-control value read back (only with GPU_HS_SGX_BOOT, zero otherwise). */
    u32 boot_control;
    /* G2: class GPU_C_FW_MMIO declarations, published after the table ranges (may be none). */
    const struct gpu_range *declared;
    u32 declared_count;
};

/* Header field offsets (also checked by the host test against a model written from this). */
enum gpu_handoff_field {
    GPU_HO_MAGIC = 0x00,
    GPU_HO_VERSION = 0x04,     /* u16 */
    GPU_HO_HEADER = 0x06,      /* u16 */
    GPU_HO_TOTAL = 0x08,
    GPU_HO_CRC = 0x0c,
    GPU_HO_STATE = 0x10,
    GPU_HO_CHIP = 0x14,
    GPU_HO_BOARD = 0x18,
    GPU_HO_RANGE_COUNT = 0x1c,
    GPU_HO_RANGE_OFFSET = 0x20,
    GPU_HO_RANGE_STRIDE = 0x24,
    GPU_HO_PERF_COUNT = 0x28,
    GPU_HO_PERF_OFFSET = 0x2c,
    GPU_HO_PERF_STRIDE = 0x30,
    GPU_HO_GSIV = 0x34,        /* u32[4]: gfx-asc irq 0, 1; gfx1-asc irq 0, 1 */
    GPU_HO_MAILBOX_NUM = 0x44, /* u32[2] */
    GPU_HO_POOL_BASE = 0x50,   /* u64 */
    GPU_HO_POOL_SIZE = 0x58,
    GPU_HO_HANDOFF_IPA = 0x60,
    GPU_HO_VM_BASE = 0x68,
    GPU_HO_DRAM_BASE = 0x70,
    GPU_HO_DRAM_SIZE = 0x78,
    GPU_HO_RECEIPT = 0x80, /* char[64], NUL padded */
    /*
     * G1 fields in the header's spare tail (zero until GPU_HS_POWERED; version stays 1, Mu's
     * GPU0 reads none of them): the raw ID words the carrier read after power-on, so the probe
     * and the driver compare against the admitted values instead of decoding them (ledger G-10),
     * and each ASC's CPU_CONTROL / CPU_STATUS as power-on left them (RUN expected set, B-03).
     */
    GPU_HO_GPU_ID = 0xc0,      /* u32[7] */
    GPU_HO_CPU_CONTROL = 0xdc, /* u32[2]: gfx-asc, gfx1-asc */
    GPU_HO_CPU_STATUS = 0xe4,  /* u32[2] */
    /* G2: the sgx boot-control value read back after the carrier's step (GPU_HS_SGX_BOOT). */
    GPU_HO_BOOT_CONTROL = 0xec,
};

/* Build the page; bytes used, or 0 if it does not fit. */
u32 gpu_t8152_handoff_build(void *page, u64 page_size, const struct gpu_t8152_admission *a,
                            const struct gpu_range *table, u32 count,
                            const struct gpu_t8152_handoff_in *in);

/* CRC-32 (zlib, reflected 0xedb88320) as Mu's CalculateCrc32 computes it. */
u32 gpu_t8152_crc32(const void *data, u64 len);

#endif
