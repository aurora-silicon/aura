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
 *   G2     (G2, "g2") after ADMIT, before POWER: arms the G2 guards on top of PROBE
 *          (gpu_t8152_g2.h) with the launcher's grants: G2-D1 declares the firmware register
 *          windows in the handoff page (class GPU_C_FW_MMIO), G2-D2 lets POWER perform the one
 *          checked sgx boot-control step.  PROBE then takes whole messages only, counts them by
 *          kind, refuses a second init-data message per role and the recipe's refused types,
 *          locks sgx once init data went out, and audits the GPU page tables (gpu_t8152_uat.h)
 *          before every message on the two application endpoints.  No new register is
 *          forwarded.  Every PROBE (G1 too) records a UAT baseline and audits again at STOP.
 *
 * Not here yet: the GPU IRQ aliases (G1 and G2 poll, as Linux does), direct stage-2 passthrough
 * of the measured registers, iop-vbar pinning by value (the page is RAZ/WI).
 */

/* P_HV_WINDOWS_CAPS2 bit 2: G0 GPU admission and logging hooks (this file, P_HV_WINDOWS_GPU). */
#define HV_WINDOWS_CAPS2_GPU BIT(2)

#define HV_GPU_ERROR ((u64) - 1)
/* Pool: 32 MiB aligned (one level-2 block with the 16 KiB granule), at most half guest RAM. */
#define HV_GPU_POOL_ALIGN (32UL << 20)
#define HV_GPU_LOG_LIMIT  32

/* P_HV_WINDOWS_CAPS2 bit 4: G1 GPU power-on, boot marker and mediated forwarding. */
#define HV_WINDOWS_CAPS2_GPU_PROBE BIT(4)
/* P_HV_WINDOWS_CAPS2 bit 5: G2 guards, grants and the UAT audit (the recipe may be absent). */
#define HV_WINDOWS_CAPS2_GPU_G2 BIT(5)

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
    HV_GPU_OP_G2 = 7,     /* G2: args[1] = GPU_G2_GRANT_* (after ADMIT, before POWER, once) */
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
    HV_GPU_S_G2_RECIPE = 14,    /* 1 if this carrier has the private G2 recipe (and it is sane) */
    HV_GPU_S_G2 = 15,           /* HV_GPU_G2_* flags of this session */
    HV_GPU_S_DECLARED = 16,     /* fw-mmio windows declared in the handoff page */
    HV_GPU_S_BOOT_CONTROL = 0x48, /* + {0 before, 1 after}: the sgx boot-control step */
    HV_GPU_S_MSG = 0x400,       /* + 0x10 * (2 * role + tx) + enum gpu_msg_kind: messages seen */
    HV_GPU_S_GUARD = 0x460,     /* + enum hv_gpu_guard: G2 refusals */
    HV_GPU_S_AUDIT = 0x480,     /* + enum hv_gpu_audit_field: the last UAT audit */
    HV_GPU_S_AUDIT_FRESH = 0x4c0, /* + 4 * i + {0 key, 1 descriptor, 2 class | level << 8 |
                                     in_pool << 16}: its first fresh outliers */
    HV_GPU_S_PSD_BEFORE = 0x20, /* + step: gpu_psd_pack() before POWER */
    HV_GPU_S_PSD_AFTER = 0x28,  /* + step: after POWER */
    HV_GPU_S_GPU_ID = 0x30,     /* + i < 7: the ID words read after POWER */
    HV_GPU_S_CPU = 0x40,        /* + {0, 1} CPU_CONTROL, {2, 3} CPU_STATUS after POWER */
    HV_GPU_S_CLASS = 0x100,     /* + enum gpu_class: accesses per class */
    HV_GPU_S_RANGE = 0x200,     /* + 4 * i + {0 base, 1 size, 2 class | tag << 16, 3 flags} */
    HV_GPU_S_MAP = 0x1000,      /* + 4 * i + {0 address, 1 count, 2 width | write << 8 |
                                   disposition << 16 | class << 24, 3 first value} */
};

/* HV_GPU_S_G2 */
#define HV_GPU_G2_ARMED        BIT(0)
#define HV_GPU_G2_FW_MMIO      BIT(1) /* G2-D1 granted: windows declared */
#define HV_GPU_G2_BOOT_GRANTED BIT(2) /* G2-D2 granted */
#define HV_GPU_G2_BOOT_DONE    BIT(3) /* the boot-control step read back the expected value */
#define HV_GPU_G2_SGX_LOCKED   BIT(4) /* init data went out: sgx is locked */
#define HV_GPU_G2_INIT_GFX     BIT(8) /* init data forwarded to gfx-asc */
#define HV_GPU_G2_INIT_GFX1    BIT(9) /* ... to gfx1-asc */

enum hv_gpu_guard {
    HV_GPU_G_INIT_AGAIN = 0, /* a second init-data message to one role */
    HV_GPU_G_REFUSED = 1,    /* a message type the recipe refuses */
    HV_GPU_G_AUDIT = 2,      /* a message refused by the UAT audit */
    HV_GPU_G_LOCKED = 3,     /* sgx accesses after init data */
    HV_GPU_G_HALF = 4,       /* 64-bit mailbox halves (G2 takes whole messages only) */
    HV_GPU_G_COUNT = 5,
};

enum hv_gpu_audit_field {
    HV_GPU_A_AUDITS = 0,  /* audits run this session (baseline included) */
    HV_GPU_A_ROOTS = 1,
    HV_GPU_A_TABLES = 2,
    HV_GPU_A_LEAVES = 3,  /* + enum gpu_uat_class (9 of them, up to 11) */
    HV_GPU_A_OUTLIERS = 12,
    HV_GPU_A_KNOWN = 13,
    HV_GPU_A_FRESH = 14,
    HV_GPU_A_FRESH_POOL = 15,
    HV_GPU_A_CAPPED = 16,
    HV_GPU_A_BASELINE = 17,   /* outliers in the PROBE baseline */
    HV_GPU_A_BASELINE_OVF = 18,
    HV_GPU_A_FRESH_MAX = 19,  /* the most fresh outliers any audit of this session found */
    HV_GPU_A_COUNT = 20,
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
    HV_GPU_E_BOOT_CONTROL = -10, /* G2: the boot-control step refused or read back the wrong
                                    value (the GPU is powered; reboot) */
    HV_GPU_E_G2 = -11,     /* G2: no private recipe, malformed grants, a window or the
                              boot-control register fails its check (nothing touched) */
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
const u64 *hv_gpu_host_uat_page(u64 pa);
#endif

#endif
