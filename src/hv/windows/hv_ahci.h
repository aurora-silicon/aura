/* SPDX-License-Identifier: MIT */

#ifndef HV_AHCI_H
#define HV_AHCI_H

#include "types.h"

/*
 * D1: the carrier-emulated AHCI disk on T8152 (J804).  The device model is ahci_model.c; this
 * is its carrier glue: the trapped ABAR page, the guest-RAM check for every DMA span, the
 * DRAM store at the top of the launcher's guest RAM window, the virtual SPI, and the proxy
 * operations the launcher drives (proxyclient/aura/windows.py, tools/run_guest.py).
 *
 * Contract (windows-m6/D1-AHCI-DESIGN.md; Mu J804Windows.h must agree):
 *   ABAR   IPA 0xF40000000, 16 KiB, hooked, never mapped;
 *   SPI    GSIV 990, level, active-high, no AIC line;
 *   store  [store_base, window_top): the disk [store_base, store_base + disk_bytes), unused
 *          slack, and a 64 KiB descriptor at window_top - 64 KiB.  Guest RAM is
 *          [window base, store_base); the store is added to the stage-2 denylist.
 */

#define HV_AHCI_ABAR        0xf40000000UL
#define HV_AHCI_ABAR_SIZE   0x4000UL
#define HV_AHCI_INTID       990u
#define HV_AHCI_DESC_SIZE   0x10000UL
#define HV_AHCI_STORE_ALIGN (32UL << 20) /* one level-2 block with the 16 KiB granule */
#define HV_AHCI_MIN_GUEST   (4UL << 30)  /* Windows 11 minimum RAM */
#define HV_AHCI_MIN_DISK    (1UL << 20)
#define HV_AHCI_VERSION     1

/* P_HV_WINDOWS_CAPS2: 'AWI2' in the top half, feature bits below. */
#define HV_WINDOWS_CAPS2_MAGIC 0x4157493200000000ULL
/*
 * CAPS2 bits 8-12: the T8152 carrier rules a launcher must find before it runs a J804 guest.
 * They were CAPS bits 11-15 before this branch was rebased on j813-windows, which uses CAPS bits
 * 11-14 for CPU frequency ownership, N1 acquire, HvXfer and the E-cluster boot state; CAPS bits
 * 16-31 are the ABI number, so CAPS has no free bit left for them.
 */
#define HV_WINDOWS_CAPS2_T8152_POLICY        BIT(8)  /* stage-2 policy, guest RAM op, Fast IPI */
#define HV_WINDOWS_CAPS2_T8152_LIVE_CPUS     BIT(9)  /* refuses a launch without 12 live CPUs */
#define HV_WINDOWS_CAPS2_T8152_WFE_PLACEMENT BIT(10) /* guest WFE trapped, code below CTXR */
#define HV_WINDOWS_CAPS2_T8152_ACTLR_ARCH    BIT(11) /* ACTLR_EL12 only, never the Apple alias */
#define HV_WINDOWS_CAPS2_T8152_GIC_TRAPALL   BIT(12) /* every ICC_* trapped, software cpuif */
#define HV_WINDOWS_CAPS2_AHCI  BIT(0) /* AHCI RAM store v1 (this file) */
/*
 * D5: the AHCI disk on an internal NVMe partition (hv_ahci_nvme.c), v1: ops 5-8, the backend
 * closes with a controller shutdown on every hv_start() return and in flush_and_reboot(), and
 * P_REBOOT goes through flush_and_reboot().  Bit 4 is G1 (GPU power-on), bit 6 G2 (hv_gpu.h).
 */
#define HV_WINDOWS_CAPS2_AHCI_NVME BIT(5)

/* P_HV_WINDOWS_AHCI args[0]. */
enum hv_ahci_op {
    HV_AHCI_OP_QUERY = 0,
    HV_AHCI_OP_ATTACH = 1,
    HV_AHCI_OP_SEAL_UPLOAD = 2,
    HV_AHCI_OP_SEAL_EXIT = 3,
    HV_AHCI_OP_STATUS = 4,
    /* D5 (hv_ahci_nvme.h): a1 = the 192-byte request; DETACH; a1 = index; gate test. */
    HV_AHCI_OP_ATTACH_NVME = 5,
    HV_AHCI_OP_DETACH = 6,
    HV_AHCI_OP_NVME_STATUS = 7,
    HV_AHCI_OP_NVME_TEST = 8,
};

/* QUERY indices. */
enum hv_ahci_query {
    HV_AHCI_Q_VERSION = 0,
    HV_AHCI_Q_ABAR = 1,
    HV_AHCI_Q_ABAR_SIZE = 2,
    HV_AHCI_Q_INTID = 3,
    HV_AHCI_Q_CAP = 4,
    HV_AHCI_Q_MIN_GUEST = 5,
    HV_AHCI_Q_STORE_ALIGN = 6,
    HV_AHCI_Q_DESC_SIZE = 7,
};

/*
 * ATTACH flags.  KEEP reuses the store at window_top - 64 KiB; ACCEPT_UNSEALED also accepts a
 * RUNNING one (its disk CRC is then recorded, so a failed entry does not lock it out), but
 * never after the system counter went backwards since it was last alive (a SoC reset).
 * OVERWRITE lets a fresh attach zero a store whose descriptor is READY, RUNNING or SEALED.
 */
#define HV_AHCI_ATTACH_KEEP            BIT(0)
#define HV_AHCI_ATTACH_ACCEPT_UNSEALED BIT(1)
#define HV_AHCI_ATTACH_OVERWRITE       BIT(2)

#define HV_AHCI_ERROR ((u64) - 1)

enum hv_ahci_state {
    HV_AHCI_EMPTY = 0,
    HV_AHCI_LOADING = 1,
    HV_AHCI_READY = 2,
    HV_AHCI_RUNNING = 3,
    HV_AHCI_SEALED = 4,
};

/*
 * The store descriptor, little-endian, at window_top - HV_AHCI_DESC_SIZE.  Carrier-owned;
 * the launcher reads it (keep) and writes only host_note.  desc_crc32 is the CRC-32 (zlib)
 * of bytes [0, offsetof(desc_crc32)).
 */
#define HV_AHCI_DESC_MAGIC "AWAHCI01"
struct hv_ahci_store_desc {
    char magic[8];
    u32 version;
    u32 header_size;
    u64 window_top;
    u64 store_base;
    u64 disk_bytes;
    u32 state;
    u32 generation;
    u32 disk_crc32;
    s32 last_exit; /* 0 none, 1 SYSTEM_OFF, 2 SYSTEM_RESET, negative: aborted */
    /*
     * CNTPCT_EL0 when the carrier session owning the store last knew it consistent: at
     * attach, upload seal, guest entry, every 10 s while the guest runs (the HV report), guest
     * exit and exit seal.  The counter restarts at a SoC reset, so a value above the current
     * counter means the machine was reset since (hard reset, power cycle, panic reboot).
     */
    u64 alive_cntpct;
    u32 desc_crc32;
    u8 pad[0x200 - 0x44];
    u8 host_note[256]; /* launcher-written, informational: image SHA-256 and size */
};

_Static_assert(__builtin_offsetof(struct hv_ahci_store_desc, desc_crc32) == 0x40,
               "descriptor layout is part of the launcher ABI");
_Static_assert(__builtin_offsetof(struct hv_ahci_store_desc, host_note) == 0x200,
               "descriptor layout is part of the launcher ABI");

/* hv_windows_configure(HV_WINDOWS_T8152_AHCI). */
void hv_ahci_enable(void);
bool hv_ahci_enabled(void);

/* P_HV_WINDOWS_AHCI. Returns the op's value, or HV_AHCI_ERROR with the reason printed. */
u64 hv_ahci_proxy(u64 op, u64 a1, u64 a2, u64 a3, u64 a4);

/*
 * hv_start(), after hv_t8152_verify(): with the configure flag set, refuse unless a store is
 * attached and READY and the ABAR page is this file's hook.  0, or -1 with the reason.
 */
int hv_ahci_verify(void);
/* hv_start(), right before guest entry: the store becomes RUNNING. */
void hv_ahci_guest_entered(void);
/* hv_start() returned: the exit status the launcher sees (1 OFF, 2 RESET, else failure). */
void hv_ahci_guest_exited(int status);
/*
 * Every hv_start() return (exits, refusals, aborts): an NVMe backend closes with the controller
 * shutdown here (D5).  Nothing for the RAM store.
 */
void hv_ahci_session_end(int status);

/*
 * The periodic HV report: commands, errors and the longest command under the lock; while the
 * guest runs it also refreshes the descriptor's alive_cntpct (at most every 10 s).
 */
void hv_ahci_report(void);
/* G5b status block: commands so far this session (a relaxed read, no lock). */
u64 hv_ahci_commands(void);

/*
 * The fatal report's AHCI part (hv_fatal.c): the access being handled now or last, and the
 * last command with its guest spans.  Takes no lock; prints nothing before attach.
 */
void hv_ahci_fatal_report(u64 now, u64 freq);

#endif
