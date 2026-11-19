/* SPDX-License-Identifier: MIT */

#ifndef HV_MTP_T8152_H
#define HV_MTP_T8152_H

#include "types.h"

/*
 * I1/I2: the J804 (T8152) MTP owned by the Windows carrier for the internal keyboard and
 * trackpad.  Hardware
 * glue for mtp_bringup.c (the protocol, pure and host-tested); hv_input.c calls it from its
 * ATTACH, STATUS and PROBE ops and from the tick and the idle path.
 *
 * Rules kept here (windows-m6/INPUT-I1I2-BUILD.md):
 *  - identity from the live ADT (mtp ascwrap-v8 at the measured addresses, dart-sbr, its
 *    mapper-mtp stream 4 and vm-base, the keyboard nub) before any MMIO;
 *  - the reserved area is checked and denied to stage 2 (hv_t8152_reserve_input_area());
 *  - MMIO only to an allow-list: read ASC_CPU_CONTROL, dart-sbr TCR[4]/TTBR[4] and the 13 PMGR
 *    MTP states; write only those PMGR states; every access under the exception guard; the
 *    RTKit mailbox through asc.c; RUN is written by rtkit's boot, only after RUN read clear;
 *  - DMA only through stream-4 entries this side writes (mtp_dart_t8152.c) into the area;
 *    the device-written rings are Normal-NC in the carrier's own page tables;
 *  - SMC: only with the trackpad, only before entry, only the key pcIO and only the two values
 *    of the ADT's multi-touch function-afe-reset pKW8('pcIO', 0x10000, 0x1c0000) (level 1
 *    asserts the AFE reset, 0 releases it), each command bounded; no other key, no audio or
 *    speaker key, ever.  No NVRAM;
 *  - the trackpad firmware is the launcher's copy in the area (the owner's own lifted file,
 *    never in a repo): CRC-checked against the parameter page before it is mapped;
 *  - the MTP is serviced by polling only (lines 1500/1501 stay masked), at most every 1 ms,
 *    by one CPU at a time, never waiting; a latched failure releases the keyboard and stops.
 *
 * After any session that brought the MTP up it keeps DMAing into the area: the machine must be
 * left by a reboot, never by loading something else in place.
 */

/*
 * Bring the MTP up to its keyboard (and, with `trackpad`, its trackpad) in the reserved area
 * [area, area + size).  `fn_fkeys` selects the Fn default.  True when the keyboard reached
 * READY; the reasons are in STATUS.  Before guest entry only, once per carrier session.
 */
bool hv_mtp_t8152_attach(u64 area, u64 size, bool fn_fkeys, bool trackpad);

/* The keyboard is up and nothing has latched. */
bool hv_mtp_t8152_live(void);
/* The trackpad came up at ATTACH (FEATURES bit 18). */
bool hv_mtp_t8152_trackpad_up(void);

/* Service RTKit and the rings (rate-limited to 1 ms, one servicer, never waits). */
void hv_mtp_t8152_poll(void);

/* STATUS indices 23..31 (hv_input.h); HV_INPUT_ERROR-style ~0 for others. */
u64 hv_mtp_t8152_status(u32 index);

/*
 * Before guest entry: service for `ms` (at most 10000) with every frame and every keyboard
 * translation logged (rate-limited).  Frames handled, or -1 when the keyboard is not live.
 */
s64 hv_mtp_t8152_probe(u32 ms);

/* The 1 Hz report line, and the EL2 fatal report's line (reads counters without locks). */
void hv_mtp_t8152_report(void);
void hv_mtp_t8152_fatal_report(void);

#endif
