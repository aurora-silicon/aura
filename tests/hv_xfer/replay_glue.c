/* SPDX-License-Identifier: MIT */
/*
 * Offline replay glue for HvXfer, compiled with the REAL Aura headers
 * (src/hv/windows/hv.h with AURA_WINDOWS, exception.h, uartproxy.h, ...).
 *
 * It links against the real src/hv/hv_vm.c (stage-2 tables, hv_handle_dabort,
 * the load/store decoder, hv_emulate_rw's access splitting and hook dispatch)
 * and the real src/hv/windows/hv_xfer.c, and supplies only what those two
 * files need from the rest of EL2: exception-register getters, the proxy
 * entry point (forwarded to the Python host engine), and no-op stubs for the
 * watchdog/SPRR/mmiotrace hooks the doorbell path never takes.
 *
 * A guest access is a REAL AArch64 load/store instruction placed in memory,
 * with ESR/FAR/ELR set the way the CPU would set them for a stage-2 data
 * abort, then handed to hv_handle_dabort() -- the same entry the Windows
 * hv_exc_sync() slow path calls after hv_exc_entry() takes the HV lock.
 */

/* Resolved through -I<tree>/src -I<tree>/src/hv, exactly like the firmware
 * build; run.sh points <tree> at a copy whose build/build_cfg.h says
 * AURA_WINDOWS, so hv.h selects src/hv/windows/hv.h. */
#include "arm_cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "hv/windows/hv_xfer.h"
#include "hv_sprr.h"
#include "iodev.h"
#include "memory.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "utils.h"

#include "AppleHvXferCore.h"

/* ---- globals the linked EL2 files expect --------------------------------- */

uint64_t ram_base;
u64 mem_size_actual;
char _base[64];
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
iodev_id_t uartproxy_iodev;

/* ---- exception state of the "current" trap ------------------------------- */

static struct exc_info replay_ctx;
static u32 replay_insn[4] __attribute__((aligned(16)));

u64 hv_get_esr(void)
{
    return replay_ctx.esr;
}

u64 hv_get_far(void)
{
    return replay_ctx.far;
}

u64 hv_get_spsr(void)
{
    return replay_ctx.spsr;
}

/* ---- proxy entry: forwarded to the Python host --------------------------- */

typedef void (*replay_proxy_cb_t)(u64 ctx, u32 reason, u32 type);
static replay_proxy_cb_t replay_proxy_cb;
static u32 replay_proxy_calls;
static u32 replay_proxy_last_reason, replay_proxy_last_type;

void replay_set_proxy_cb(replay_proxy_cb_t cb)
{
    replay_proxy_cb = cb;
}

u32 replay_proxy_count(void)
{
    return replay_proxy_calls;
}

u32 replay_proxy_last(u32 which)
{
    return which ? replay_proxy_last_type : replay_proxy_last_reason;
}

void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra)
{
    /* What _hv_exc_proxy() does before uartproxy_run(): publish `extra` and
     * hand the host the context address as the START message's info. */
    ctx->extra = extra;
    replay_proxy_calls++;
    replay_proxy_last_reason = reason;
    replay_proxy_last_type = type;
    if (replay_proxy_cb)
        replay_proxy_cb((u64)ctx, reason, type);
}

/* ---- cache maintenance is observed, not performed ------------------------- */

static u64 civac_calls, civac_last_addr, civac_last_len;

void dc_civac_range(void *addr, size_t length)
{
    civac_calls++;
    civac_last_addr = (u64)addr;
    civac_last_len = length;
}

u64 replay_civac(u32 which)
{
    return which == 0 ? civac_calls : which == 1 ? civac_last_addr : civac_last_len;
}

/* ---- stubs for paths the doorbell never takes ---------------------------- */

void hv_wdt_breadcrumb(char c)
{
    UNUSED(c);
}

void hv_wdt_suspend(void)
{
}

void hv_wdt_resume(void)
{
}

bool hv_sprr_is_pt_page(u64 ipa)
{
    UNUSED(ipa);
    return false;
}

bool hv_sprr_is_shadow_page(u64 ipa)
{
    UNUSED(ipa);
    return false;
}

void hv_sprr_pt_updated(struct exc_info *ctx, u64 ipa, u64 bytes)
{
    UNUSED(ctx);
    UNUSED(ipa);
    UNUSED(bytes);
}

void hv_sprr_pt_writer_check(struct exc_info *ctx, u64 ipa, u64 far)
{
    UNUSED(ctx);
    UNUSED(ipa);
    UNUSED(far);
}

static u32 replay_unexpected;

void uartproxy_send_event(u16 event_type, void *data, u16 length)
{
    UNUSED(event_type);
    UNUSED(data);
    UNUSED(length);
    replay_unexpected++;
}

void iodev_flush(iodev_id_t id)
{
    UNUSED(id);
}

void get_simd_state(void *state)
{
    UNUSED(state);
    replay_unexpected++;
}

void put_simd_state(void *state)
{
    UNUSED(state);
    replay_unexpected++;
}

void hexdump(const void *d, size_t len)
{
    UNUSED(d);
    printf("hexdump(%zu bytes)\n", len);
}

void memset64(void *dst, u64 value, size_t size)
{
    u64 *p = dst;
    for (size_t i = 0; i < size / 8; i++)
        p[i] = value;
}

u32 replay_unexpected_count(void)
{
    return replay_unexpected;
}

/* ---- guest accesses through the real dabort path ------------------------- */

/*
 * Encode one guest access as the instruction a compiler would emit:
 *   log2 0..3: STR/LDR (immediate, unsigned offset 0) of 1/2/4/8 bytes
 *   log2 4:    STP/LDP of two X registers (16 bytes, the hook sees width 4)
 * Base register x1 holds the address; data in x2 (and x3 for pairs).
 */
static u32 replay_encode(u32 size_log2, bool write)
{
    if (size_log2 == 4)
        return (write ? 0xa9000000u : 0xa9400000u) | (3u << 10) | (1u << 5) | 2u;
    return (write ? 0x39000000u : 0x39400000u) | (size_log2 << 30) | (1u << 5) | 2u;
}

/*
 * Returns 1 if hv_handle_dabort() handled the access (the guest resumes), 0 if
 * it failed (on Aura Windows: EXC_SYNC to the host, which aborts the guest).
 */
int replay_access(u64 ipa, u32 size_log2, int write, u64 value, u64 value2, u64 *out,
                  u64 *out2)
{
    memset(&replay_ctx, 0, sizeof(replay_ctx));
    replay_insn[0] = replay_encode(size_log2, write);
    replay_ctx.regs[1] = ipa;
    replay_ctx.regs[2] = value;
    replay_ctx.regs[3] = value2;
    replay_ctx.spsr = 0x3c5; /* EL1h, DAIF masked */
    replay_ctx.elr = (u64)&replay_insn[0];
    replay_ctx.far = ipa;
    replay_ctx.esr = FIELD_PREP(ESR_EC, ESR_EC_DABORT_LOWER) | ESR_IL |
                     (write ? ESR_ISS_DABORT_WnR : 0);

    bool handled = hv_handle_dabort(&replay_ctx);

    if (out)
        *out = replay_ctx.regs[2];
    if (out2)
        *out2 = replay_ctx.regs[3];
    return handled ? 1 : 0;
}

/* Call the stage-2 hook registered at `addr` directly, with an arbitrary
 * width -- for defensive branches the decoder can never reach. */
int replay_call_hook(u64 addr, u64 *val, int write, int width)
{
    u64 pte = hv_pt_walk(addr);

    if (!pte || (pte & 1) || ((pte >> 50) & 7) != 1)
        return -1;
    hv_hook_t *hook = (hv_hook_t *)(pte & GENMASK(49, 2));
    return hook(&replay_ctx, addr, val, write, width) ? 1 : 0;
}

/* A stand-in for any other emulated device (vUART, a C-only AIC/vGIC hook). */
static u32 replay_dummy_hits;

static bool replay_dummy_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    UNUSED(addr);
    UNUSED(width);
    replay_dummy_hits++;
    if (!write)
        *val = 0x0badf00d;
    return true;
}

int replay_map_dummy_hook(u64 base, u64 size)
{
    return hv_map_hook(base, replay_dummy_hook, size);
}

u32 replay_dummy_count(void)
{
    return replay_dummy_hits;
}

/* ---- the real guest driver core, wired to real traps --------------------- */

static u32 guest_accesses, guest_faults, guest_non32;

static uint32_t guest_read32(void *ctx, uint32_t off)
{
    u64 v = 0;
    guest_accesses++;
    if (off & 3)
        guest_non32++;
    if (!replay_access(*(u64 *)ctx + off, 2, 0, 0, 0, &v, NULL))
        guest_faults++;
    return (uint32_t)v;
}

static void guest_write32(void *ctx, uint32_t off, uint32_t value)
{
    guest_accesses++;
    if (off & 3)
        guest_non32++;
    if (!replay_access(*(u64 *)ctx + off, 2, 1, value, 0, NULL, NULL))
        guest_faults++;
}

const apple_hvxfer_ops_t replay_guest_ops = {
    .read32 = guest_read32,
    .write32 = guest_write32,
    .barrier = NULL,
};

u32 replay_guest_stats(u32 which)
{
    return which == 0 ? guest_accesses : which == 1 ? guest_faults : guest_non32;
}

/* ---- ABI facts from the real headers ------------------------------------- */

u64 replay_abi(u32 which)
{
    switch (which) {
        case 0:
            return sizeof(struct hv_xfer_exc_info);
        case 1:
            return offsetof(struct hv_xfer_exc_info, cmd);
        case 2:
            return offsetof(struct hv_xfer_exc_info, status);
        case 3:
            return HV_XFER;
        case 4:
            return START_HV;
        case 5:
            return sizeof(struct exc_info);
        case 6:
            return offsetof(struct exc_info, extra);
        case 7:
            return HV_TPM;
        case 8:
            return HV_SYSREG_ASSIST_READY;
        case 9:
            return HV_PANIC;
        case 10:
            return offsetof(struct hv_xfer_exc_info, name_phys);
        case 11:
            return offsetof(struct hv_xfer_exc_info, result);
        default:
            return ~0UL;
    }
}

void replay_set_dram(u64 base, u64 size)
{
    ram_base = base;
    mem_size_actual = size;
}
