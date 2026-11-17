/* SPDX-License-Identifier: MIT */

#ifndef HV_GPU_H
#define HV_GPU_H

#include "types.h"

/*
 * G0 + G1 (windows-m6/WINDOWS-GPU-PLAN.md, windows-m6/G0-CARRIER-CONTRACT.md): the carrier side
 * of GPU passthrough for the J804 Windows guest.  Off unless the launcher asks
 * (--windows-gpu admit|log|probe); with it off nothing here runs and the guest sees no change.
 *
 *   ADMIT  once per carrier session, after guest RAM is set and before guest entry: the
 *          admission port (gpu_t8152_admit.c) against the live ADT and both TEXT identities;
 *          the range table; the firmware __TEXT segments onto the stage-2 denylist; the four
 *          UAT regions (RW) and four DATA regions (RO) registered as identity exceptions (not
 *          mapped); the GPU pool recorded (inside guest RAM, which stays normal identity RAM);
 *          the read-only handoff page built in carrier memory and mapped at IPA = PA.
 *   HOOK   (G0, "log") logging hooks (read-as-zero, write-ignored, counted per class, the first
 *          accesses printed) over the whole sgx register window and the post-office page.  No
 *          access reaches the hardware.
 *   POWER  (G1, "probe") once per boot: the boot-wide marker (HV_GPU_BOOT_MARKER) must be free
 *          and every GPU PSD service cold; the marker is claimed, then the PSD services are
 *          requested in the private recipe's order (gpu_t8152_power.h) and verified; the ID
 *          words and both ASCs' CPU_CONTROL/CPU_STATUS are read into the handoff page.  The ASC
 *          firmware is running from then on (RUN comes up set); nothing ever stops it.
 *   PROBE  (G1) mediated forwarding hooks over the same window and page (gpu_t8152_probe.h:
 *          the GPU ID words, CPU_CONTROL/CPU_STATUS with RUN guarded, the two GFX post-office
 *          windows; everything else RAZ/WI), every access counted in the access map by address,
 *          width, direction and outcome; the UAT regions mapped RW and the DATA regions RO.
 *   STOP   teardown (also run by hv_start() on every exit): forwarding off, firmware regions
 *          unmapped, the access map printed.  The firmware is left running and the GPU powered
 *          for the rest of the boot; the pool stays reserved in the live ADT (HV_GPU_POOL_SLOT)
 *          because the GPU may still reach it.
 *
 * Not here yet: the GPU IRQ aliases (G1 polls, as Linux does), direct stage-2 passthrough of the
 * measured registers (after S2 freezes the map), iop-vbar pinning by value (the page is RAZ/WI).
 */

/* P_HV_WINDOWS_CAPS2 bit 2: G0 GPU admission and logging hooks (this file, P_HV_WINDOWS_GPU). */
#define HV_WINDOWS_CAPS2_GPU BIT(2)

#define HV_GPU_ERROR ((u64) - 1)
/* Pool: 32 MiB aligned (one level-2 block with the 16 KiB granule), at most half guest RAM. */
#define HV_GPU_POOL_ALIGN (32UL << 20)
#define HV_GPU_LOG_LIMIT  32

/* P_HV_WINDOWS_CAPS2 bit 4: G1 GPU power-on, boot marker and mediated forwarding. */
#define HV_WINDOWS_CAPS2_GPU_PROBE BIT(4)

/*
 * The boot-wide marker: a 16-byte /chosen/memory-map slot of the live ADT (all zero as iBoot
 * hands it over) that POWER sets to (marker page, 16 KiB) before its first PSD request, and the
 * slot that keeps the GPU-reachable pool reserved for the rest of the boot.  A carrier reloaded
 * in place finds them (chainload keeps the live ADT); a reboot clears them (iBoot builds a new
 * ADT).  The guest RAM checks (hv_t8152.c, windows.py) already refuse any memory-map range.
 */
#define HV_GPU_BOOT_MARKER "MemoryMapReserved-15"
#define HV_GPU_POOL_SLOT   "MemoryMapReserved-14"

/* P_HV_WINDOWS_GPU args[0]. */
enum hv_gpu_op {
    HV_GPU_OP_ADMIT = 1,  /* args[1] = pool base, args[2] = pool size */
    HV_GPU_OP_HOOK = 2,   /* G0 logging hooks on the sgx window and the post-office page */
    HV_GPU_OP_STATUS = 3, /* args[1] = hv_gpu_status index */
    HV_GPU_OP_POWER = 4,  /* G1: PSD power-on, once per boot (boot marker) */
    HV_GPU_OP_PROBE = 5,  /* G1: mediated forwarding hooks + firmware regions mapped */
    HV_GPU_OP_STOP = 6,   /* teardown (idempotent; hv_start() runs it on every exit) */
};

enum hv_gpu_status {
    HV_GPU_S_STATE = 0,         /* handoff state bits (GPU_HS_*) */
    HV_GPU_S_HANDOFF_IPA = 1,   /* 0 before ADMIT succeeded */
    HV_GPU_S_HANDOFF_SIZE = 2,
    HV_GPU_S_RANGES = 3,        /* entries in the range table */
    HV_GPU_S_ACCESSES = 4,      /* guest accesses the hooks took */
    HV_GPU_S_MAILBOX_GFX = 5,   /* of those, inside a GFX post-office window */
    HV_GPU_S_BOOT = 6,          /* the boot marker now: 0 free, GPU_BM_* of a valid record, or
                                   HV_GPU_BOOT_FOREIGN / HV_GPU_BOOT_NO_SLOT */
    HV_GPU_S_FORWARDED = 7,     /* G1 accesses performed on the hardware */
    HV_GPU_S_DENIED = 8,        /* G1 accesses RAZ/WI */
    HV_GPU_S_RUN_REFUSED = 9,   /* CPU_CONTROL writes refused (would clear or toggle RUN) */
    HV_GPU_S_FAULTS = 10,       /* forwarded accesses that faulted */
    HV_GPU_S_MAP_COUNT = 11,    /* entries in the access map */
    HV_GPU_S_MAP_DROPPED = 12,  /* accesses not recorded (map full) */
    HV_GPU_S_POWER_RECIPE = 13, /* 1 if this carrier has the private PSD recipe */
    HV_GPU_S_PSD_BEFORE = 0x20, /* + step: gpu_psd_pack() before POWER */
    HV_GPU_S_PSD_AFTER = 0x28,  /* + step: after POWER */
    HV_GPU_S_GPU_ID = 0x30,     /* + i < 7: the ID words read after POWER */
    HV_GPU_S_CPU = 0x40,        /* + {0, 1} CPU_CONTROL, {2, 3} CPU_STATUS after POWER */
    HV_GPU_S_CLASS = 0x100,     /* + enum gpu_class: accesses per class */
    HV_GPU_S_RANGE = 0x200,     /* + 4 * i + {0 base, 1 size, 2 class | tag << 16, 3 flags} */
    HV_GPU_S_MAP = 0x1000,      /* + 4 * i + {0 address, 1 count, 2 width | write << 8 |
                                   disposition << 16 | class << 24, 3 first value} */
};

#define HV_GPU_BOOT_FOREIGN BIT(31) /* the marker slot is set but holds no valid record */
#define HV_GPU_BOOT_NO_SLOT BIT(30) /* this ADT has no free-able marker slot */

/* ADMIT / HOOK results (negative; 0 is success). */
enum hv_gpu_err {
    HV_GPU_E_STATE = -1,   /* not T8152, policy not armed, no guest RAM, guest entered, ADMIT
                              already ran this session (passed or refused), HOOK before ADMIT
                              or twice */
    HV_GPU_E_POOL = -2,    /* the pool is not 32 MiB aligned inside guest RAM (nothing spent) */
    HV_GPU_E_ADMIT = -3,   /* the admission refused (the console says why) */
    HV_GPU_E_RANGES = -4,  /* the range table does not fit or breaks a rule */
    HV_GPU_E_POLICY = -5,  /* the stage-2 policy refused a class, or a map/hook failed */
    HV_GPU_E_HANDOFF = -6, /* no memory for the handoff page, or it does not fit */
    HV_GPU_E_RECIPE = -7,  /* built without the private PSD recipe, or a PSD block is not inside
                              an ADT pmgr range (nothing touched; nothing spent) */
    HV_GPU_E_BOOT = -8,    /* the boot marker says this boot's GPU session is used (or it cannot
                              be claimed, or a service was already on): reboot the M6 */
    HV_GPU_E_POWER = -9,   /* a PSD step failed: the GPU is in an unknown state; reboot */
};

/* P_HV_WINDOWS_GPU. */
u64 hv_gpu_proxy(u64 op, u64 a1, u64 a2);

/*
 * The EL2 fatal report's GPU part (guest SError attribution): the session state, the handoff
 * page and pool, how many admitted firmware ranges there are (registered identity exceptions,
 * never mapped in G0), and the logging hooks' hit count with the last access.  Recorded values
 * only, no locks, no MMIO.
 */
void hv_gpu_fatal_report(void);
/* Every exit path of a guest session (hv_start()); idempotent; never touches PSD or the ASCs. */
void hv_gpu_teardown(const char *why);
/* flush_and_reboot(): lock-free, forwarding off before the reset. */
void hv_gpu_emergency_stop(void);

#ifdef HV_GPU_HOST_TEST
void hv_gpu_host_new_session(void);
bool hv_gpu_host_read_text(u64 phys, void *buf, u64 size);
bool hv_gpu_host_hook(u64 addr, u64 *val, bool write, int width);
bool hv_gpu_host_probe_hook(u64 addr, u64 *val, bool write, int width);
const void *hv_gpu_host_handoff(void);
const void *hv_gpu_host_marker(void);
/* Supplied by the host harness: the hardware behind the carrier. */
bool hv_gpu_host_mmio(u64 addr, u64 *val, bool write, int width);
u32 hv_gpu_host_read32(u64 addr);
void hv_gpu_host_write32(u64 addr, u32 value);
void hv_gpu_host_delay(u32 us);
bool hv_gpu_host_marker_read(u64 pa, void *out, u64 size);
#endif

#endif
