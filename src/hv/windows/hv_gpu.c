/* SPDX-License-Identifier: MIT */

/*
 * G0 GPU passthrough, carrier side, logging only.  See hv_gpu.h and
 * windows-m6/G0-CARRIER-CONTRACT.md.  Every address comes from the admission (the live ADT);
 * the only fixed values are the J804 firmware receipt and the pool rules.
 */

#include "hv_gpu.h"
#include "adt.h"
#include "gpu_t8152_admit.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

extern void aura_sha256(const void *data, size_t size, u8 digest[32]);

/* The firmware the J804 profile pins (src/kboot_gpu_t8152.c publishes the same receipt). */
#define J804_GPU_RECEIPT "G18G-RTKit-3514.41.1.release"

/*
 * ADMIT ran this carrier session, whatever its result.  This is per carrier session, not per
 * boot: a carrier reload resets it.  G0 starts no firmware, so that is harmless today, but G1
 * (anything that powers the GPU or lets CPU_CONTROL RUN through) must first add a boot-wide
 * marker that survives a reload.  tests/python/test_windows_t8152_gpu.py refuses a hv_gpu.c that
 * sets GPU_HS_POWERED or GPU_HS_ASC_RUN without HV_GPU_BOOT_MARKER.
 */
static bool session_used;
static bool admitted, hooked;
static struct gpu_t8152_admission adm;
static struct gpu_range table[GPU_T8152_MAX_RANGES];
static u32 table_count;
static u8 *handoff;
static u64 handoff_pa, pool_base, pool_size;
static u32 state;
static u64 class_hits[GPU_C_COUNT], total_hits, gfx_hits;
static u32 logged;
/* fatal report: the last logging-hook access (address, 1 = write) */
static u64 last_hook_addr, last_hook_write;

#ifdef HV_GPU_HOST_TEST
#define gpu_read_text hv_gpu_host_read_text
#else
/* iBoot left the TEXT in RAM below boot_args RAM; aura maps all of DRAM identity. */
static bool gpu_read_text(u64 phys, void *buf, u64 size)
{
    memcpy(buf, (const void *)phys, size);
    return true;
}
#endif

/*
 * Build the handoff page and clean it to the point of coherency: Mu's PrePi reads it with the MMU
 * off (Normal non-cacheable), so what sits only in this cacheable EL2 mapping is invisible there.
 */
static bool handoff_publish(void)
{
    struct gpu_t8152_handoff_in in = {state, handoff_pa, pool_base, pool_size};
    if (!handoff || !gpu_t8152_handoff_build(handoff, GPU_HANDOFF_SIZE, &adm, table, table_count,
                                             &in))
        return false;
    dc_civac_range(handoff, GPU_HANDOFF_SIZE);
    return true;
}

static u64 err(int e)
{
    return (u64)(s64)e;
}

static bool state_ok(const char *op)
{
    u64 base, size;
    if (chip_id != T8152 || !hv_t8152_policy_armed() || !hv_t8152_guest_ram(&base, &size)) {
        printf("HV: GPU: %s refused: not T8152, the stage-2 policy is not armed, or guest RAM is "
               "not set\n",
               op);
        return false;
    }
    if (hv_windows_guest_entered()) {
        printf("HV: GPU: %s refused: the guest has already entered\n", op);
        return false;
    }
    return true;
}

static int pool_check(u64 base, u64 size)
{
    u64 rb, rs;
    hv_t8152_guest_ram(&rb, &rs);
    if (!size || (base | size) & (HV_GPU_POOL_ALIGN - 1) || base > UINT64_MAX - size ||
        base < rb || base + size > rb + rs || size > rs / 2) {
        printf("HV: GPU: pool [0x%lx, 0x%lx) refused: it must be 32 MiB aligned, at most half of "
               "guest RAM and inside guest RAM [0x%lx, 0x%lx)\n",
               base, base + size, rb, rb + rs);
        return -1;
    }
    return 0;
}

static u32 range_index(const struct gpu_range *r)
{
    return (u32)(r - table);
}

/* The firmware classes: __TEXT onto the denylist, UAT (RW) and DATA (RO) as exceptions. */
static int register_classes(void)
{
    u64 dram_hi = adm.dram_base + adm.dram_size;
    for (u32 i = 0; i < table_count; i++) {
        struct gpu_range *r = &table[i];
        if (r->cls == GPU_C_FW_TEXT) {
            if (hv_t8152_gpu_deny(r->base, r->base + r->size, r->name))
                return -1;
            r->flags |= GPU_RF_DENIED;
        } else if (r->cls == GPU_C_FW_RW || r->cls == GPU_C_FW_RO) {
            bool ro = r->cls == GPU_C_FW_RO;
            if (hv_t8152_gpu_fw_allow(r->base, r->size, ro, adm.dram_base, dram_hi, r->name))
                return -1;
            r->flags |= GPU_RF_ALLOWED | (ro ? GPU_RF_READ_ONLY : 0);
        }
    }
    return 0;
}

static int admit(u64 pbase, u64 psize)
{
    if (!state_ok("ADMIT"))
        return HV_GPU_E_STATE;
    if (session_used) {
        printf("HV: GPU: ADMIT refused: ADMIT runs once per carrier session (one GPU firmware "
               "session per boot); reload the carrier\n");
        return HV_GPU_E_STATE;
    }
    if (pool_check(pbase, psize))
        return HV_GPU_E_POOL; /* a malformed request spends nothing */
    session_used = true;

    u32 ntlv = 0;
    const struct gpu_tlv *tlvs = gpu_t8152_tlvs(&ntlv);
    struct gpu_t8152_env env = {
        .chip_id = chip_id,
        .board_id = board_id,
        .ram_base = cur_boot_args.phys_base,
        .ram_top = cur_boot_args.phys_base + cur_boot_args.mem_size,
        .profiles = gpu_t8152_j804_profiles(),
        .tlvs = tlvs,
        .tlv_count = ntlv,
        .receipt = J804_GPU_RECEIPT,
        .read_text = gpu_read_text,
        .sha256 = aura_sha256,
    };
    if (gpu_t8152_admit(&env, &adm))
        return HV_GPU_E_ADMIT;

    struct gpu_range holes[GPU_T8152_MAX_HOLES];
    int nholes = gpu_t8152_collect_holes(&adm, holes, ARRAY_SIZE(holes));
    int n = nholes < 0 ? -1 :
                         gpu_t8152_build_ranges(&adm, holes, (u32)nholes, table, ARRAY_SIZE(table));
    if (n < 0)
        return HV_GPU_E_RANGES;
    table_count = (u32)n;

    handoff = memalign(GPU_HANDOFF_SIZE, GPU_HANDOFF_SIZE);
    if (!handoff) {
        printf("HV: GPU: no memory for the handoff page\n");
        return HV_GPU_E_HANDOFF;
    }
    memset(handoff, 0, GPU_HANDOFF_SIZE);
    handoff_pa = (u64)handoff;
    if (gpu_t8152_add_range(table, &table_count, ARRAY_SIZE(table), pbase, psize, GPU_C_POOL,
                            "GPU pool") ||
        gpu_t8152_add_range(table, &table_count, ARRAY_SIZE(table), handoff_pa,
                            GPU_HANDOFF_SIZE, GPU_C_HANDOFF, "GPU handoff page"))
        return HV_GPU_E_RANGES;
    pool_base = pbase;
    pool_size = psize;
    state = GPU_HS_ADMITTED;
    if (!handoff_publish()) {
        printf("HV: GPU: the handoff page does not fit in 16 KiB\n");
        return HV_GPU_E_HANDOFF;
    }

    /*
     * Everything from here on either completes or is rolled back: a failed ADMIT must not leave
     * firmware exceptions in the stage-2 allow table (pa_rule() would still accept a later map of
     * them, RW for the UAT regions).  The TEXT denylist entries stay: they only forbid more.
     */
    unsigned int mark = hv_t8152_allow_mark();
    if (register_classes()) {
        hv_t8152_allow_rollback(mark, "GPU admission refused");
        return HV_GPU_E_POLICY;
    }
    struct gpu_range *h = &table[table_count - 1];
    if (hv_t8152_allow(handoff_pa, handoff_pa, GPU_HANDOFF_SIZE, true, "G0 GPU handoff page")) {
        printf("HV: GPU: the handoff page could not be allowed read-only\n");
        hv_t8152_allow_rollback(mark, "GPU admission refused");
        return HV_GPU_E_POLICY;
    }
    if (hv_map_hw_ro(handoff_pa, handoff_pa, GPU_HANDOFF_SIZE) < 0) {
        printf("HV: GPU: the handoff page could not be mapped read-only\n");
        hv_unmap(handoff_pa, GPU_HANDOFF_SIZE);
        hv_t8152_allow_rollback(mark, "GPU admission refused");
        return HV_GPU_E_POLICY;
    }
    h->flags |= GPU_RF_MAPPED | GPU_RF_READ_ONLY;
    admitted = true;
    handoff_publish();

    u32 nhole = 0;
    for (u32 i = 0; i < table_count; i++)
        nhole += table[i].cls == GPU_C_SGX_HOLE;
    printf("HV: GPU: admitted J804 %s: sgx [0x%lx, 0x%lx), %u ranges (%u foreign holes in the "
           "window), post-office page 0x%lx, ASC lines %u/%u and %u/%u (not published), "
           "%u perf states\n",
           adm.receipt, adm.sgx.base, adm.sgx.base + adm.sgx.size, table_count, nhole,
           adm.mailbox.base, adm.irq[0][0], adm.irq[0][1], adm.irq[1][0], adm.irq[1][1],
           adm.perf_count);
    printf("HV: GPU: pool [0x%lx, 0x%lx) in guest RAM, handoff page IPA 0x%lx (read-only); "
           "nothing passed through, not powered, no IRQ published\n",
           pool_base, pool_base + pool_size, handoff_pa);
    return 0;
}

static bool gpu_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    (void)ctx;
    const struct gpu_range *r = gpu_t8152_find(table, table_count, addr);
    u32 cls = r ? r->cls : GPU_C_NONE, role = 0;
    bool gfx = cls == GPU_C_MAILBOX && gpu_t8152_mailbox_window(&adm, addr, &role);

    __atomic_add_fetch(&class_hits[cls < GPU_C_COUNT ? cls : 0], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&total_hits, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&last_hook_addr, addr, __ATOMIC_RELAXED);
    __atomic_store_n(&last_hook_write, write ? 1 : 0, __ATOMIC_RELAXED);
    if (gfx)
        __atomic_add_fetch(&gfx_hits, 1, __ATOMIC_RELAXED);
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < HV_GPU_LOG_LIMIT)
        printf("HV: GPU: guest %s %s 0x%lx (%s +0x%lx, %d bytes%s) %s%lx: G0 logs only\n",
               write ? "write" : "read", gpu_class_name(cls), addr, r ? r->name : "?",
               r ? addr - r->base : 0, 1 << width,
               gfx ? (role ? ", GFX1 mailbox window" : ", GFX mailbox window") : "",
               write ? "value 0x" : "returns 0x", write ? val[0] : 0UL);
    if (!write) {
        val[0] = 0;
        if (width > 3)
            val[1] = 0;
    }
    return true;
}

void hv_gpu_fatal_report(void)
{
    if (!session_used)
        return;
    u32 hooked_ranges = 0;
    for (u32 i = 0; i < table_count; i++)
        if (table[i].flags & GPU_RF_HOOKED)
            hooked_ranges++;
    printf("HV: fatal: GPU: state 0x%x, admitted %d, hooks %d; %u ranges, %u behind the logging "
           "hooks, none passed through (firmware regions are registered identity exceptions "
           "without a stage-2 leaf in G0); handoff page IPA 0x%lx read-only; pool [0x%lx, "
           "0x%lx) in guest RAM\n",
           state, (int)admitted, (int)hooked, table_count, hooked_ranges, handoff_pa, pool_base,
           pool_base + pool_size);
    u64 hits = __atomic_load_n(&total_hits, __ATOMIC_RELAXED);
    if (hooked || hits)
        printf("HV: fatal: GPU: logging hooks hit %lu times (GFX mailbox %lu); last %s 0x%lx\n",
               hits, __atomic_load_n(&gfx_hits, __ATOMIC_RELAXED),
               __atomic_load_n(&last_hook_write, __ATOMIC_RELAXED) ? "write" : "read",
               __atomic_load_n(&last_hook_addr, __ATOMIC_RELAXED));
}

static int hook(void)
{
    if (!state_ok("HOOK"))
        return HV_GPU_E_STATE;
    if (!admitted || hooked) {
        printf("HV: GPU: HOOK refused: %s\n", hooked ? "the hooks are already in place" :
                                                       "the GPU is not admitted");
        return HV_GPU_E_STATE;
    }
    hooked = true;
    const struct gpu_range *mbox = NULL;
    for (u32 i = 0; i < table_count; i++)
        if (table[i].cls == GPU_C_MAILBOX)
            mbox = &table[i];
    if (!mbox || hv_map_hook(adm.sgx.base, gpu_hook, adm.sgx.size) < 0 ||
        hv_map_hook(mbox->base, gpu_hook, mbox->size) < 0) {
        printf("HV: GPU: the logging hooks could not be installed\n");
        return HV_GPU_E_POLICY;
    }
    u32 n = 0;
    for (u32 i = 0; i < table_count; i++) {
        struct gpu_range *r = &table[i];
        if ((r->base >= adm.sgx.base && r->base - adm.sgx.base < adm.sgx.size) || r == mbox) {
            r->flags |= GPU_RF_HOOKED;
            n++;
        }
    }
    state |= GPU_HS_HOOKS_LOG;
    handoff_publish();
    printf("HV: GPU: logging hooks (read-as-zero, write-ignored) on sgx [0x%lx, 0x%lx) and the "
           "post-office page 0x%lx: %u ranges, range %u is the GFX page\n",
           adm.sgx.base, adm.sgx.base + adm.sgx.size, mbox->base, n, range_index(mbox));
    return 0;
}

static u64 status(u64 index)
{
    if (index == HV_GPU_S_STATE)
        return state;
    if (index == HV_GPU_S_HANDOFF_IPA)
        return admitted ? handoff_pa : 0;
    if (index == HV_GPU_S_HANDOFF_SIZE)
        return admitted ? GPU_HANDOFF_SIZE : 0;
    if (index == HV_GPU_S_RANGES)
        return admitted ? table_count : 0;
    if (index == HV_GPU_S_ACCESSES)
        return __atomic_load_n(&total_hits, __ATOMIC_RELAXED);
    if (index == HV_GPU_S_MAILBOX_GFX)
        return __atomic_load_n(&gfx_hits, __ATOMIC_RELAXED);
    if (index >= HV_GPU_S_CLASS && index < HV_GPU_S_CLASS + GPU_C_COUNT)
        return __atomic_load_n(&class_hits[index - HV_GPU_S_CLASS], __ATOMIC_RELAXED);
    if (admitted && index >= HV_GPU_S_RANGE && index < HV_GPU_S_RANGE + 4 * (u64)table_count) {
        const struct gpu_range *r = &table[(index - HV_GPU_S_RANGE) / 4];
        switch ((index - HV_GPU_S_RANGE) % 4) {
            case 0:
                return r->base;
            case 1:
                return r->size;
            case 2:
                return r->cls | (u64)r->tag << 16;
            default:
                return r->flags;
        }
    }
    return HV_GPU_ERROR;
}

u64 hv_gpu_proxy(u64 op, u64 a1, u64 a2)
{
    switch (op) {
        case HV_GPU_OP_ADMIT:
            return err(admit(a1, a2));
        case HV_GPU_OP_HOOK:
            return err(hook());
        case HV_GPU_OP_STATUS:
            return status(a1);
        default:
            return HV_GPU_ERROR;
    }
}

#ifdef HV_GPU_HOST_TEST
void hv_gpu_host_new_session(void)
{
    session_used = admitted = hooked = false;
    memset(&adm, 0, sizeof(adm));
    table_count = 0;
    handoff = NULL;
    handoff_pa = pool_base = pool_size = 0;
    state = 0;
    memset(class_hits, 0, sizeof(class_hits));
    total_hits = gfx_hits = 0;
    last_hook_addr = last_hook_write = 0;
    logged = 0;
}

bool hv_gpu_host_hook(u64 addr, u64 *val, bool write, int width)
{
    return gpu_hook(NULL, addr, val, write, width);
}

const void *hv_gpu_host_handoff(void)
{
    return handoff;
}
#endif
