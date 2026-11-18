/* SPDX-License-Identifier: MIT */

#ifndef GPU_T8152_G2_H
#define GPU_T8152_G2_H

#include "gpu_t8152_admit.h"
#include "types.h"

/*
 * G2 (windows-m6/G2-DESIGN.md section 7): what the carrier adds so that a UEFI probe can publish
 * init data, open the queues and replay one compute job.  G2 forwards no new register: every
 * doorbell is a message on the GFX post-office windows G1 already forwards.  What changes:
 *
 *   C1  one checked sgx boot-control step at POWER (read, refuse unless the value is one the
 *       recipe accepts, set the recipe's bits, read back the expected value), only when the
 *       launcher grants it (owner decision G2-D2: the register lies in a pmgr-child hole);
 *   C2  handoff state bit GPU_HS_SGX_BOOT and the value read back at GPU_HO_BOOT_CONTROL;
 *   C3  the firmware's register windows declared as class GPU_C_FW_MMIO handoff entries, each
 *       anchored at ADMIT to the ADT node registers the recipe names, only when the launcher
 *       grants it (owner decision G2-D1: the GPU firmware's page tables may then map them);
 *   C4  no guest sgx access once an init-data message went out (the firmware gates sgx);
 *   C5  one init-data message per firmware role, and the recipe's refused message types never
 *       reach the firmware;
 *   C6  every forwarded message classified and counted per role, direction and kind;
 *   C7  P_HV_WINDOWS_CAPS2 bit 5.
 * Plus the UAT audit (gpu_t8152_uat.h) that every gated message and the teardown run.
 *
 * The recipe values (boot-control offset and values, window anchors, refused types, the extra
 * UAT root) come from the upstream driver (owner decision D1(a): private bring-up only) and are
 * not in this repository: gpu_t8152_g2_recipe.c takes them from the builder's private data at
 * build time, and without that file --windows-gpu g2 is refused.
 */

#define GPU_G2_MAX_WINDOWS 24
#define GPU_G2_MAX_REFUSED 4
#define GPU_G2_MAX_ROOTS   4

/* One firmware register window: inside the hull of regs [reg_first, reg_last] of an /arm-io node. */
struct gpu_g2_window {
    u32 slot;       /* the firmware's register-map slot (published as the range tag) */
    const char *node;
    u32 reg_first, reg_last;
    u64 offset;     /* from the base of reg reg_first */
    u64 size;
    bool writable;  /* the firmware may write it (else read-only) */
};

/* An extra UAT root to audit: a page at region[region] + offset (region: enum gpu_t8152_region). */
struct gpu_g2_root {
    u32 region;
    u64 offset;
};

/* A host-to-firmware message type the carrier never forwards. */
struct gpu_g2_refused {
    u32 ep;
    u32 type; /* message bits 63:48 */
};

struct gpu_g2_recipe {
    bool available; /* false: this carrier was built without the private G2 recipe */
    const char *source;
    /* C1, from the sgx window base */
    u32 boot_offset;
    u32 boot_accept[2]; /* values accepted before the step */
    u32 boot_set;       /* bits the step sets */
    u32 boot_expect;    /* the value that must read back */
    u32 nwindows;
    struct gpu_g2_window window[GPU_G2_MAX_WINDOWS];
    u32 nrefused;
    struct gpu_g2_refused refused[GPU_G2_MAX_REFUSED];
    u32 nroots;
    struct gpu_g2_root root[GPU_G2_MAX_ROOTS];
};

/* gpu_t8152_g2_recipe.c (the build-time private header); host tests supply their own. */
const struct gpu_g2_recipe *gpu_t8152_g2_recipe(void);

/* The launcher's grants (HV_GPU_OP_G2 args[1]): the owner decisions G2-D1 and G2-D2. */
#define GPU_G2_GRANT_FW_MMIO      BIT(0)
#define GPU_G2_GRANT_BOOT_CONTROL BIT(1)
#define GPU_G2_GRANTS             (GPU_G2_GRANT_FW_MMIO | GPU_G2_GRANT_BOOT_CONTROL)

/* The recipe is complete and sane.  0, or -1 with the reason printed.  No ADT, no MMIO. */
int gpu_t8152_g2_check(const struct gpu_g2_recipe *r);

/*
 * C3: anchor every window of the recipe in the live ADT (the global adt) and turn it into a
 * GPU_C_FW_MMIO range (tag = slot, flags GPU_RF_DECLARED, plus GPU_RF_READ_ONLY unless
 * writable).  Refused if a window leaves the hull of its node's registers, if its 16 KiB pages
 * touch DRAM or guest RAM, or touch a class-table range of any class other than sgx and sgx-hole
 * (ASC wrappers, CPU_CONTROL, iop-vbar, the post-office page, firmware memory, pool, handoff).
 * Count, or -1 with the reason printed.
 */
int gpu_t8152_g2_declare(const struct gpu_g2_recipe *r, const struct gpu_t8152_admission *a,
                         const struct gpu_range *table, u32 count, u64 guest_lo, u64 guest_hi,
                         struct gpu_range *out, u32 cap);

/*
 * C1 location check: sgx base + boot_offset is a naturally aligned 32-bit register inside the
 * sgx window, inside a GPU_C_SGX_HOLE range of the table, and inside a pmgr* register range of
 * the live ADT (it is the one PMGR-hole register the plan's rule lets the carrier write, by owner
 * decision).  0 with *addr set, or -1 with the reason printed.
 */
int gpu_t8152_g2_boot_check(const struct gpu_g2_recipe *r, const struct gpu_t8152_admission *a,
                            const struct gpu_range *table, u32 count, u64 *addr);

/* Is the 32-bit value one the recipe accepts before the step? */
bool gpu_t8152_g2_boot_accepts(const struct gpu_g2_recipe *r, u32 value);

/* ---- C5 / C6: firmware messages ------------------------------------------------------------- */

/*
 * The two application endpoints and the message types the carrier classifies.  Sources: the
 * public m1n1 AGX code and our own macOS traces (gpu/FACT-LEDGER.md F-01, F-02, F-04, F-05,
 * F-06, F-08); the type is message bits 63:48.
 */
#define GPU_MSG_EP_MGMT     0x0
#define GPU_MSG_EP_CRASHLOG 0x1
#define GPU_MSG_EP_GFX      0x20 /* host <-> firmware messages */
#define GPU_MSG_EP_DOORBELL 0x21 /* host -> firmware notes and doorbells */
#define GPU_MSG_INIT        0x81 /* EP 0x20, host -> firmware: init-data root (F-01) */
#define GPU_MSG_INIT_ACK    0x9  /* EP 0x20, firmware -> host: init data accepted (F-02) */
#define GPU_MSG_EVENT       0x42 /* EP 0x20, firmware -> host: records posted (F-08) */
#define GPU_MSG_POWER_END   0x89 /* EP 0x21: post-init note (F-04) */
#define GPU_MSG_DOORBELL    0x83 /* EP 0x21: doorbell (F-06) */
#define GPU_MSG_FWCTL       0x84 /* EP 0x21: firmware-control doorbell (F-05) */

enum gpu_msg_kind {
    GPU_MK_MGMT = 0,      /* EP 0 (RTKit management) */
    GPU_MK_CRASHLOG = 1,  /* EP 1 */
    GPU_MK_INIT = 2,      /* EP 0x20 type 0x81 */
    GPU_MK_INIT_ACK = 3,  /* EP 0x20 type 0x0009 */
    GPU_MK_EVENT = 4,     /* EP 0x20 type 0x42 */
    GPU_MK_GFX = 5,       /* any other EP 0x20 message */
    GPU_MK_POWER_END = 6, /* EP 0x21 type 0x89 */
    GPU_MK_DOORBELL = 7,  /* EP 0x21 type 0x83 */
    GPU_MK_FWCTL = 8,     /* EP 0x21 type 0x84 */
    GPU_MK_DOORBELL_OTHER = 9, /* any other EP 0x21 message */
    GPU_MK_OTHER = 10,    /* any other endpoint */
    GPU_MK_HALF = 11,     /* a 64-bit half (not decoded) */
    GPU_MK_COUNT = 12,
};

/* ep = the high 64 bits of a mailbox message (endpoint in bits 7:0), msg = the low 64 bits. */
u32 gpu_t8152_msg_kind(u64 ep, u64 msg);
const char *gpu_msg_kind_name(u32 kind);

/* Is (ep, type of msg) on the recipe's refused list? */
bool gpu_t8152_g2_refused(const struct gpu_g2_recipe *r, u64 ep, u64 msg);

/*
 * RTKit management (public m1n1 rtkit.py): the type is bits 59:52; type 6 sets the IOP power
 * state, 0x20 = on.  Any other IOP power state from the host would put the firmware to sleep or
 * off, and waking it again is a second firmware start: G2 never forwards one.
 */
#define GPU_MGMT_TYPE(msg)    (((msg) >> 52) & 0xff)
#define GPU_MGMT_IOP_PWR      6
#define GPU_MGMT_PWR_ON       0x20
bool gpu_t8152_g2_iop_power_down(u64 ep, u64 msg);

#endif
