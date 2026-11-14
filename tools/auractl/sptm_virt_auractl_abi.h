/* Copied verbatim from aura src/sptm_virt_auractl_abi.h -- keep in sync. */
/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_AURACTL_ABI_H
#define SPTM_VIRT_AURACTL_ABI_H

/*
 * auractl: the ABI of aura's synthetic in-guest tracing-control device.
 *
 * aura exposes one 16 KiB MMIO page to the macOS guest that the hypervisor traps (hv_map_hook /
 * SPTE_HOOK). A tool running inside the guest ("auractl") reads and writes the register file below
 * to arm MMIO trace ranges, start/stop recording and dump the trace -- with no debug cable and
 * without taking a USB port from macOS. The page is NOT backed by RAM or any Apple block; the HV
 * implements every register from its own state.
 *
 * This header is the single source of truth for the ABI. aura includes it; the macOS kext and the
 * auractl tool carry a verbatim copy. Clean-room: all names, values and layout are ours.
 */

#define AURACTL_PAGE_SIZE 0x4000UL

/* ABI handshake. */
#define AURACTL_MAGIC   0x4154434cu /* 'ACTL' little-endian */
#define AURACTL_VERSION 1u

/*
 * MVP fixed guest-physical base. A reserved hole in the guest's device-MMIO space, mapped by the
 * kext directly (SIP off). The discoverable path (an aura,auractl ADT node) uses the same register
 * file at whatever reg the node carries. Chosen high in the MMIO aperture, 16 KiB aligned, away
 * from any real Apple block; validated against the live guest ADT before the hardware proof.
 */
#define AURACTL_MMIO_BASE 0x230000000UL

/* Register offsets from the page base. 32-bit unless marked u64. All little-endian. */
#define AURACTL_REG_MAGIC      0x000 /* R   u32  AURACTL_MAGIC */
#define AURACTL_REG_VERSION    0x004 /* R   u32  AURACTL_VERSION */
#define AURACTL_REG_CAPS       0x008 /* R   u32  see AURACTL_CAPS_* */
#define AURACTL_REG_ARG0       0x010 /* RW  u64  command arg 0: range base / ring bytes */
#define AURACTL_REG_ARG1       0x018 /* RW  u64  command arg 1: range size / flags */
#define AURACTL_REG_CMD        0x020 /* W   u32  doorbell: write an AURACTL_CMD_* opcode */
#define AURACTL_REG_STATUS     0x024 /* R   u32  AURACTL_STATE in [3:0], AURACTL_ERR in [15:8] */
#define AURACTL_REG_NREC       0x028 /* R   u64  records captured since RESET (saturating) */
#define AURACTL_REG_DROPPED    0x030 /* R   u64  records overwritten by ring wrap before a DUMP */
#define AURACTL_REG_FIFO_DATA  0x040 /* R   u64  pop one u64 of the dump stream (4 per record) */
#define AURACTL_REG_FIFO_AVAIL 0x048 /* R   u64  u64 words still waiting in the dump FIFO */
#define AURACTL_REG_RANGE_IDX  0x050 /* RW  u32  select an armed range for CMD_READBACK */
#define AURACTL_REG_NRANGES    0x054 /* R   u32  number of currently armed ranges */
#define AURACTL_REG_MAX        0x058

/* CAPS bitmap. */
#define AURACTL_CAPS_RANGES_MASK 0x000000ffu /* max armable ranges */
#define AURACTL_CAPS_FIFO        0x00000100u /* dump FIFO present */
#define AURACTL_CAPS_RECBYTES_SH 16          /* bytes per record in [23:16] (== 32) */

/* Command opcodes (write to AURACTL_REG_CMD). */
#define AURACTL_CMD_RESET    0u /* clear ranges, ring and counters; -> IDLE */
#define AURACTL_CMD_ARM      1u /* add trace range [ARG0, ARG0+ARG1); -> ARMED */
#define AURACTL_CMD_START    2u /* begin recording; ARMED -> RUNNING */
#define AURACTL_CMD_STOP     3u /* stop recording; RUNNING -> STOPPED */
#define AURACTL_CMD_DUMP     4u /* latch the ring for FIFO readout (oldest-first) */
#define AURACTL_CMD_READBACK 5u /* load range RANGE_IDX into ARG0 (base) / ARG1 (size) */
#define AURACTL_CMD_DISARM   6u /* drop all ranges (unhook); -> IDLE */
#define AURACTL_CMD_MAX      7u

/* STATUS state field [3:0]. */
#define AURACTL_STATE_IDLE    0u
#define AURACTL_STATE_ARMED   1u
#define AURACTL_STATE_RUNNING 2u
#define AURACTL_STATE_STOPPED 3u
#define AURACTL_STATE_MASK    0x0fu

/* STATUS error field [15:8] (last command's result; 0 == OK). */
#define AURACTL_ERR_NONE     0u
#define AURACTL_ERR_BADCMD   1u /* unknown opcode */
#define AURACTL_ERR_BADSTATE 2u /* opcode not legal in the current state */
#define AURACTL_ERR_BADRANGE 3u /* range not in guest device-MMIO space / misaligned / zero */
#define AURACTL_ERR_FULL     4u /* RANGES_MAX reached, or total traced size over the cap */
#define AURACTL_ERR_OVERLAP  5u /* range overlaps one already armed */
#define AURACTL_ERR_NOMEM    6u /* ring not allocated */
#define AURACTL_ERR_SH       8
#define AURACTL_ERR_MASK     0xff00u

/*
 * A trace record. Layout matches sptm_virt_gpu.c's gpu_rec so ring-dump.py decodes both:
 *   t    CNTPCT_EL0 at the access
 *   meta pa[47:0] | width[50:48] | write[51] | cpu[55:52]
 *   val  value written, or read
 *   pc   guest PC (ELR) at the access
 */
#define AURACTL_REC_BYTES 32

/* meta field helpers (shared by the decoder test and ring-dump). */
#define AURACTL_META_PA(m)    ((m) & ((1UL << 48) - 1))
#define AURACTL_META_WIDTH(m) (((m) >> 48) & 7)
#define AURACTL_META_WRITE(m) (((m) >> 51) & 1)
#define AURACTL_META_CPU(m)   (((m) >> 52) & 15)
#define AURACTL_META(pa, width, write, cpu)                                                        \
    (((pa) & ((1UL << 48) - 1)) | (((u64)(width) & 7) << 48) | (((u64) !!(write)) << 51) |         \
     (((u64)(cpu) & 15) << 52))

#endif
