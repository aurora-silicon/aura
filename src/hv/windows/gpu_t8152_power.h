/* SPDX-License-Identifier: MIT */

#ifndef GPU_T8152_POWER_H
#define GPU_T8152_POWER_H

#include "types.h"

/*
 * G1 (windows-m6/WINDOWS-GPU-PLAN.md, G0-CARRIER-CONTRACT.md section "G1"): GPU power-on for
 * the Windows carrier, and the boot-wide marker that keeps it to once per boot.
 *
 * Power-on is a PMGR PSD request per GPU service, in a fixed order, each from cold idle: write
 * the request, poll the acknowledge register, then read the state once (reading the state
 * consumes the acknowledge) and compare it with the service's active state.  Nothing here
 * powers anything off: a failure stops the sequence where it is.  Once the last service is on,
 * the GPU firmware is running (the ASCs come up with CPU_CONTROL RUN already set), so a second
 * power-on in the same boot would be a second firmware session, which the firmware does not
 * survive.
 *
 * The recipe (block address rule, register offsets, service ids, request and active values,
 * order) is not in this repository: gpu_t8152_power_recipe.c takes it from the builder's private
 * data at build time (J804_PRIVATE_DATA/aura/g18g-j804-gpu-power.h), and without that file the
 * carrier is built with power-on disabled.  Every block is checked against a pmgr* register range
 * of the live ADT before it is touched.
 */

#define GPU_PSD_MAX_STEPS 4

struct gpu_psd_step {
    u32 service; /* PSD service id */
    u32 request; /* value written to the request register */
    u32 target;  /* the state that means "on" */
    const char *name;
};

struct gpu_psd_recipe {
    bool available; /* false: this carrier was built without the private recipe */
    const char *source;
    u64 base;  /* block of service n = base + n * stride */
    u32 stride, block_size;
    u32 state_off, state_mask, request_off, ack_off, ack_bit;
    u32 cold_idle; /* every service reads this before power-on (cold boot) */
    u32 poll_us, polls;
    u32 nsteps;
    struct gpu_psd_step step[GPU_PSD_MAX_STEPS];
};

/* gpu_t8152_power_recipe.c (the build-time private header); host tests supply their own. */
const struct gpu_psd_recipe *gpu_t8152_psd_recipe(void);

struct gpu_psd_ops {
    u32 (*read32)(u64 addr);
    void (*write32)(u64 addr, u32 value);
    void (*delay_us)(u32 us);
};

struct gpu_psd_sample {
    u32 service, state, ack;
};

struct gpu_psd_result {
    struct gpu_psd_sample before[GPU_PSD_MAX_STEPS], after[GPU_PSD_MAX_STEPS];
    u32 polls[GPU_PSD_MAX_STEPS]; /* acknowledge polls each step took */
    int failed_step;              /* -1 when every step reached its state */
};

/*
 * The recipe is complete and sane, and every service's block lies inside a register range of
 * an /arm-io node whose name starts with "pmgr" in the live ADT (the global adt).  0, or -1 with
 * the reason printed.  Reads nothing but the ADT.
 */
int gpu_t8152_psd_check(const struct gpu_psd_recipe *r);

/* Read every service of the recipe: acknowledge first, then state. */
void gpu_t8152_psd_sample(const struct gpu_psd_recipe *r, const struct gpu_psd_ops *ops,
                          struct gpu_psd_sample *out);

/* Every service reads cold idle with no acknowledge pending. */
bool gpu_t8152_psd_all_cold(const struct gpu_psd_recipe *r, const struct gpu_psd_sample *s);

/*
 * The power-on, in recipe order.  Each step re-reads its service and refuses to write unless it
 * is still cold idle and every earlier step's service still reads its active state; then it
 * writes the request, polls the acknowledge (at most polls x poll_us) and reads the state once.
 * Stops at the first failure without touching anything else (no power-off, no retry).  0 when
 * every service reached its active state, else -1; *res says how far it got either way.
 */
int gpu_t8152_psd_power_on(const struct gpu_psd_recipe *r, const struct gpu_psd_ops *ops,
                           struct gpu_psd_result *res);

/* state | ack << 8 | service << 16, as the marker and the STATUS op report a sample. */
static inline u32 gpu_psd_pack(const struct gpu_psd_sample *s)
{
    return (s->state & 0xff) | (s->ack & 0xff) << 8 | (s->service & 0xffff) << 16;
}

/*
 * The boot-wide marker (hv_gpu.c).  One 16 KiB carrier page holds this record; the live ADT's
 * /chosen/memory-map slot HV_GPU_BOOT_MARKER points at it.  iBoot builds a fresh ADT on every
 * boot and every in-place carrier reload keeps the live one (chainload edits it in place), so
 * the slot is exactly as long-lived as the GPU firmware session: one boot.  The record says how
 * far that session got; the slot alone already means "this boot's GPU session is used".
 */
#define GPU_BOOT_MARKER_MAGIC   0x314b4d4255504741ULL /* "AGPUBMK1" */
#define GPU_BOOT_MARKER_VERSION 1
#define GPU_BOOT_MARKER_SIZE    0x4000UL

#define GPU_BM_CLAIMED      BIT(0) /* written before the first PSD request of this boot */
#define GPU_BM_POWERED      BIT(1) /* every service reached its active state */
#define GPU_BM_POWER_FAILED BIT(2) /* a step failed; the GPU is in an unknown state */
#define GPU_BM_FOREIGN      BIT(3) /* a service was already on before this carrier asked */
#define GPU_BM_PROBE        BIT(4) /* mediated forwarding went live for a guest */
#define GPU_BM_ENDED        BIT(5) /* that session ended; the firmware was left running */

struct gpu_boot_marker {
    u64 magic;
    u32 version, state;
    u64 pool_base, pool_size; /* GPU-reachable guest memory, reserved for the rest of the boot */
    u32 psd_before[GPU_PSD_MAX_STEPS], psd_after[GPU_PSD_MAX_STEPS]; /* gpu_psd_pack() */
    u32 forwarded, denied, run_refused, faults;
    u32 reserved[3];
    u32 crc; /* CRC-32 (gpu_t8152_crc32) of everything before it */
};

_Static_assert(sizeof(struct gpu_boot_marker) == 96, "boot marker record layout");

void gpu_t8152_marker_seal(struct gpu_boot_marker *m);
bool gpu_t8152_marker_valid(const struct gpu_boot_marker *m);

#endif
