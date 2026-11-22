/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef GPU_T8152_PROBE_H
#define GPU_T8152_PROBE_H

#include "gpu_t8152_admit.h"
#include "types.h"

/*
 * G1 mediated forwarding (hv_gpu.c PROBE): which guest accesses inside the hooked sgx window and
 * post-office page the carrier performs on the hardware.  The hooks stay in place over the whole
 * window and page; every access is decided here, counted and logged by offset, so the G1 session
 * itself measures the passthrough map before anything is mapped directly.
 *
 * Forwarded (only once the GPU is powered), each at exactly the width and direction listed:
 *   - sgx: reads of the GPU ID words and the core mask that macOS reads at bring-up
 *     (gpu/FACT-LEDGER.md G-10 and B-09, our macOS-under-hypervisor trace O-125300, the same
 *     values our Linux run read): 32-bit reads only;
 *   - each ASC wrapper: CPU_STATUS (+0x48) reads, CPU_CONTROL (+0x44) reads; a CPU_CONTROL write
 *     is mediated: accepted (and not performed) only if it writes back exactly the value the
 *     register holds and RUN is set in it (macOS rewrites 0x10, ledger B-03/B-04); every other
 *     CPU_CONTROL write is refused, so the guest can never clear or toggle RUN;
 *   - each GFX window of the post-office page (ledger A-03, C-01, C-02): RX at +0x00 (one 128-bit
 *     read, or 64-bit reads of either half), TX at +0x10 (one 128-bit write, or 64-bit writes of
 *     either half), status at +0x20 (32-bit read).
 * Everything else (the rest of the window, its pmgr-child and error-handler holes, the iop-vbar
 * pages, the rest of each wrapper, the other coprocessors' post-office windows) reads as zero and
 * ignores writes, and never reaches the hardware.
 */

enum gpu_probe_action {
    GPU_P_DENY = 0,       /* read as zero, write ignored, hardware untouched */
    GPU_P_PASS = 1,       /* performed on the hardware at the same address and width */
    GPU_P_CPU_CONTROL = 2 /* a CPU_CONTROL write: the carrier mediates it */
};

/* What the access map records for each access (its "disposition"). */
enum gpu_access_disp {
    GPU_A_DENIED = 0,   /* RAZ/WI */
    GPU_A_PASSED = 1,   /* performed on the hardware */
    GPU_A_MEDIATED = 2, /* CPU_CONTROL rewrite of the held value, RUN set: accepted, not written */
    GPU_A_REFUSED = 3,  /* CPU_CONTROL write that would change it (clear or toggle RUN): ignored */
    GPU_A_FAULT = 4,    /* the forwarded access faulted; reads return zero */
    /* G2 (gpu_t8152_g2.h) */
    GPU_A_LOCKED = 5,   /* sgx access after init data went out (C4): RAZ/WI, not performed */
    GPU_A_BLOCKED = 6,  /* a firmware message a G2 guard refused (C5, UAT audit): not sent */
    GPU_A_COUNT = 7,
};

/* One forwarded sgx register (offset from the sgx window base). */
struct gpu_probe_reg {
    u32 offset;
    const char *what;
};

/* The sgx registers PROBE forwards (32-bit reads). */
const struct gpu_probe_reg *gpu_t8152_probe_sgx_reads(u32 *count);

/*
 * Decide one guest access at addr (width as the HV passes it: log2 bytes, 4 = 128-bit).  *cls is
 * the class of the range holding addr (GPU_C_NONE if none).  Pure: no MMIO, no state.
 */
u32 gpu_t8152_probe_decide(const struct gpu_t8152_admission *a, const struct gpu_range *table,
                           u32 count, u64 addr, int width, bool write, u32 *cls);

/*
 * G2: the same, except that the GFX windows take whole messages only (one 128-bit RX read, one
 * 128-bit TX write; the 64-bit halves are denied), so the carrier sees every message it forwards
 * whole and can decode, count and refuse it before the firmware sees any part of it.
 */
u32 gpu_t8152_probe_decide_g2(const struct gpu_t8152_admission *a, const struct gpu_range *table,
                              u32 count, u64 addr, int width, bool write, u32 *cls);

const char *gpu_access_disp_name(u32 disp);

/* The access map: one entry per distinct (address, width, direction, disposition). */
#define GPU_ACCESS_MAP_MAX 256

struct gpu_access {
    u64 addr;
    u64 first, last; /* value read or written (low 64 bits) */
    u32 count;
    u8 width, write, disp, cls;
};

struct gpu_access_map {
    struct gpu_access e[GPU_ACCESS_MAP_MAX];
    u32 n, dropped;
};

/*
 * Count one access.  Returns the entry index, or -1 when the map is full (counted in dropped);
 * *first is true the first time this key is seen.  Not locked: the caller serializes.
 */
int gpu_access_note(struct gpu_access_map *m, u64 addr, int width, bool write, u32 disp, u32 cls,
                    u64 value, bool *first);

#endif
