/* SPDX-License-Identifier: MIT */

/*
 * G0 + G1 + G2 GPU passthrough, carrier side.  See hv_gpu.h and windows-m6/G0-CARRIER-CONTRACT.md.
 * Every address comes from the admission (the live ADT); the only fixed values are the J804
 * firmware receipt, the pool rules and the forwarded register offsets of gpu_t8152_probe.c (our
 * own observation).  The PSD power recipe and the G2 recipe come from private data at build time.
 */

#include "hv_gpu.h"
#include "adt.h"
#include "gpu_t8152_admit.h"
#include "gpu_t8152_g2.h"
#include "gpu_t8152_power.h"
#include "gpu_t8152_probe.h"
#include "gpu_t8152_uat.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"
#ifndef HV_GPU_HOST_TEST
#include "mcc.h"
#endif

extern void aura_sha256(const void *data, size_t size, u8 digest[32]);

/* The firmware the J804 profile pins (src/kboot_gpu_t8152.c publishes the same receipt). */
#define J804_GPU_RECEIPT "G18G-RTKit-3514.41.1.release"

/* Lines the G1 hooks print: one per new access-map entry, and the first mailbox messages. */
#define HV_GPU_MSG_LOG_LIMIT 128

/*
 * ADMIT ran this carrier session, whatever its result.  This is per carrier session: a carrier
 * reload resets it.  Anything that can start the GPU firmware (POWER) is additionally once per
 * boot, through the boot marker HV_GPU_BOOT_MARKER in the live ADT, which a reload keeps.
 */
static bool session_used;
static bool admitted, hooked, power_tried, probing, torn_down;
static bool forwarding; /* G1 hooks perform the measured accesses (atomic) */
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
/* G1 */
static struct gpu_boot_marker *marker; /* this session's claimed record (a 16 KiB page) */
static struct gpu_psd_result psd;
static u32 gpu_id[GPU_HANDOFF_ID_WORDS], cpu_control[GPU_T8152_ROLES], cpu_status[GPU_T8152_ROLES];
static struct gpu_access_map amap;
static u32 amap_lock;
static u64 forwarded, denied, run_refused, faults;
static u32 msgs_logged;
/* G2 */
static bool g2;       /* HV_GPU_OP_G2 armed this session */
static u32 g2_grants; /* GPU_G2_GRANT_* */
static struct gpu_range declared[GPU_G2_MAX_WINDOWS];
static u32 declared_count;
static u64 boot_addr;
static u32 boot_before, boot_after;
static bool boot_done, sgx_locked;
static u32 init_sent; /* BIT(role): init data forwarded */
static u64 msg_count[GPU_T8152_ROLES][2][GPU_MK_COUNT];
static u64 guard[HV_GPU_G_COUNT];
/*
 * The last forwarded doorbell (EP 0x21 type 0x83) for the fatal report and GPU SUM: the message,
 * BIT(63) | role in last_db_role (0 = none yet), and the forwarded counter just after it, which
 * places it in the session's access sequence.  Written by the forwarding CPU, read lock-free.
 */
static u64 last_db_msg, last_db_role, last_db_seq;
/*
 * Serialises a G2 message decision with its forward (one init-data message per role even if two
 * CPUs write at once) and every UAT audit (one walk buffer, one report).
 */
static u32 g2_lock;
/* The UAT audit (every PROBE) */
#define UAT_VISITED_CAP  1024
#define UAT_PRINT_LIMIT  64
static struct gpu_uat_scope uat_scope;
static struct gpu_uat_baseline uat_base;
static struct gpu_uat_report uat_last;
static u64 *uat_visited;
static u32 uat_audits, uat_printed, uat_fresh_max;
static bool uat_ready;

#ifdef HV_GPU_HOST_TEST
#define gpu_read_text hv_gpu_host_read_text
#define hw_rw(ctx, addr, val, write, width) hv_gpu_host_mmio(addr, val, write, width)
#define psd_read32  hv_gpu_host_read32
#define psd_write32 hv_gpu_host_write32
#define psd_delay   hv_gpu_host_delay
#define marker_read hv_gpu_host_marker_read
static const u64 *uat_page(u64 pa, void *arg)
{
    (void)arg;
    return hv_gpu_host_uat_page(pa);
}
#else
/*
 * A forwarded access, with aura's guarded MMIO (a fault returns false instead of taking the
 * carrier down).  Outside a hook (POWER's verification reads) there is no guest context.
 */
static bool hw_rw(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    static struct exc_info scratch;
    return hv_pa_rw(ctx ? ctx : &scratch, addr, val, write, width);
}

/* The PSD blocks: plain MMIO, as every PSD user in aura does (dcp.c, usb_t8152.c). */
static u32 psd_read32(u64 addr)
{
    return read32(addr);
}

static void psd_write32(u64 addr, u32 value)
{
    write32(addr, value);
}

static void psd_delay(u32 us)
{
    udelay(us);
}

/* A marker page named by the live ADT: only a 16 KiB-aligned page of boot_args RAM is read. */
static bool marker_read(u64 pa, void *out, u64 size)
{
    u64 lo = cur_boot_args.phys_base, hi = lo + cur_boot_args.mem_size;
    if (pa & (GPU_BOOT_MARKER_SIZE - 1) || pa < lo || hi < GPU_BOOT_MARKER_SIZE ||
        pa > hi - GPU_BOOT_MARKER_SIZE || size > GPU_BOOT_MARKER_SIZE)
        return false;
    memcpy(out, (const void *)pa, size);
    return true;
}

/* iBoot left the TEXT in RAM below boot_args RAM; aura maps all of DRAM identity. */
static bool gpu_read_text(u64 phys, void *buf, u64 size)
{
    memcpy(buf, (const void *)phys, size);
    return true;
}

/*
 * One 16 KiB page of GPU page tables, coherent with what the guest (write-combined pool, or
 * write-back plus clean) and the firmware wrote.  Only RAM aura maps, outside every unmapped
 * carve-out, and only after a guarded read proved the page is mapped at EL2 (a cache maintenance
 * operation on an unmapped page would fault unguarded).  The callers only ask for pages of the
 * pool and the two page-table regions.
 */
static const u64 *uat_page(u64 pa, void *arg)
{
    static struct exc_info scratch;
    (void)arg;
    if (pa & (GPU_T8152_PAGE - 1) || pa < ram_base || pa - ram_base >= mem_size_actual ||
        mem_size_actual - (pa - ram_base) < GPU_T8152_PAGE)
        return NULL;
    for (size_t i = 0; i < mcc_carveout_count; i++)
        if (pa < mcc_carveouts[i].base + mcc_carveouts[i].size &&
            mcc_carveouts[i].base < pa + GPU_T8152_PAGE)
            return NULL;
    u64 v[2];
    if (!hv_pa_read(&scratch, pa, v, 3))
        return NULL;
    sysop("dsb sy");
    dc_civac_range((void *)pa, GPU_T8152_PAGE);
    sysop("dsb sy");
    return (const u64 *)pa;
}
#endif

/*
 * Build the handoff page and clean it to the point of coherency: Mu's PrePi reads it with the MMU
 * off (Normal non-cacheable), so what sits only in this cacheable EL2 mapping is invisible there.
 */
static bool handoff_publish(void)
{
    struct gpu_t8152_handoff_in in = {state, handoff_pa, pool_base, pool_size, {0}, {0}, {0},
                                      0, NULL, 0};
    if (state & GPU_HS_POWERED) {
        memcpy(in.gpu_id, gpu_id, sizeof(gpu_id));
        memcpy(in.cpu_control, cpu_control, sizeof(cpu_control));
        memcpy(in.cpu_status, cpu_status, sizeof(cpu_status));
    }
    if (state & GPU_HS_SGX_BOOT)
        in.boot_control = boot_after;
    if (g2_grants & GPU_G2_GRANT_FW_MMIO) {
        in.declared = declared;
        in.declared_count = declared_count;
    }
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

static u64 ld(const u64 *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/*
 * The EL2 fatal report's GPU part.  G5b: it describes the G1/G2 session (gpu3's state), not G0's
 * logging hooks only.  Recorded values only: relaxed loads of the statics and of this session's
 * boot-marker record (RAM); no g2_lock or amap_lock (the report may run on a CPU that holds
 * either, and a torn read only makes one number stale), no MMIO, no allocation.
 */
void hv_gpu_fatal_report(void)
{
    if (!session_used)
        return;
    u32 hooked_ranges = 0;
    for (u32 i = 0; i < table_count; i++)
        if (table[i].flags & GPU_RF_HOOKED)
            hooked_ranges++;
    bool fwd_on = __atomic_load_n(&forwarding, __ATOMIC_RELAXED);
    printf("HV: fatal: GPU: state 0x%x, admitted %d, hooks %d, power tried %d, probe %d, "
           "forwarding %s, torn down %d; %u ranges (%u hooked); handoff page IPA "
           "0x%lx read-only; pool [0x%lx, 0x%lx)\n",
           state, (int)admitted, (int)hooked, (int)power_tried, (int)probing,
           fwd_on ? "ON" : "off", (int)torn_down, table_count, hooked_ranges, handoff_pa,
           pool_base, pool_base + pool_size);
    u64 hits = ld(&total_hits);
    if (hooked)
        printf("HV: fatal: GPU: G0 logging hooks hit %lu times (GFX mailbox %lu); last %s 0x%lx\n",
               hits, ld(&gfx_hits), ld(&last_hook_write) ? "write" : "read", ld(&last_hook_addr));
    if (!power_tried && !probing)
        return;
    /* G1 */
    u32 mstate = marker ? marker->state : 0;
    printf("HV: fatal: GPU: G1 %lu forwarded, %lu denied, %lu CPU_CONTROL writes refused, %lu "
           "faults; %lu guest GPU accesses (GFX windows %lu); boot marker %s0x%x%s%s\n",
           ld(&forwarded), ld(&denied), ld(&run_refused), ld(&faults), hits, ld(&gfx_hits),
           marker ? "" : "(none) ", mstate, (mstate & GPU_BM_GUARD) ? " GUARD" : "",
           (mstate & GPU_BM_ENDED) ? " ENDED" : "");
    if (!g2)
        return;
    /* G2 */
    u64 gs[HV_GPU_G_COUNT];
    for (u32 i = 0; i < HV_GPU_G_COUNT; i++)
        gs[i] = ld(&guard[i]);
    printf("HV: fatal: GPU: G2 grants 0x%x, %u declared windows, boot-control %s (0x%x -> 0x%x), "
           "sgx %s, init data sent to%s%s%s; guards: %lu second init-data, %lu refused types, "
           "%lu refused by the UAT audit, %lu sgx locked, %lu mailbox halves\n",
           g2_grants, declared_count, boot_done ? "done" : "not done", boot_before, boot_after,
           __atomic_load_n(&sgx_locked, __ATOMIC_RELAXED) ? "LOCKED" : "the guest's",
           (init_sent & BIT(0)) ? " GFX" : "", (init_sent & BIT(1)) ? " GFX1" : "",
           init_sent ? "" : " no role", gs[HV_GPU_G_INIT_AGAIN], gs[HV_GPU_G_REFUSED],
           gs[HV_GPU_G_AUDIT], gs[HV_GPU_G_LOCKED], gs[HV_GPU_G_HALF]);
    printf("HV: fatal: GPU: UAT audits %u (fresh at most %u in any walk); the last: %u roots, %u "
           "tables, %u outliers (%u known), fresh %u (%u in guest-writable tables)%s\n",
           __atomic_load_n(&uat_audits, __ATOMIC_RELAXED),
           __atomic_load_n(&uat_fresh_max, __ATOMIC_RELAXED), uat_last.roots, uat_last.tables,
           uat_last.outliers, uat_last.known, uat_last.fresh, uat_last.fresh_in_pool,
           uat_last.capped ? ", CAPPED" : "");
    for (u32 role = 0; role < GPU_T8152_ROLES && role < 2; role++) {
        for (u32 tx = 0; tx < 2; tx++) {
            bool any = false;
            for (u32 k = 0; k < GPU_MK_COUNT; k++) {
                u64 n = ld(&msg_count[role][tx][k]);
                if (!n)
                    continue;
                if (!any)
                    printf("HV: fatal: GPU: messages %s %s:", role ? "GFX1" : "GFX", tx ? "TX" : "RX");
                any = true;
                printf(" %s %lu", gpu_msg_kind_name(k), n);
            }
            if (any)
                printf("\n");
        }
    }
    u64 dbr = __atomic_load_n(&last_db_role, __ATOMIC_ACQUIRE);
    if (dbr)
        printf("HV: fatal: GPU: last doorbell %s 0x%016lx after forwarded access %lu (%lu "
               "forwarded since)\n",
               (dbr & 1) ? "GFX1" : "GFX", ld(&last_db_msg), ld(&last_db_seq),
               ld(&forwarded) - ld(&last_db_seq));
    else
        printf("HV: fatal: GPU: no doorbell forwarded this session\n");
}

static int hook(void)
{
    if (!state_ok("HOOK"))
        return HV_GPU_E_STATE;
    if (!admitted || hooked || probing) {
        printf("HV: GPU: HOOK refused: %s\n", hooked || probing ? "the hooks are already in place" :
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


/* ---- G1: the boot-wide marker ------------------------------------------------------------- */

enum marker_found { MARK_NONE, MARK_OURS, MARK_FOREIGN, MARK_NO_SLOT };

static void *slot_value(const char *name)
{
    int mm = adt_path_offset(adt, "/chosen/memory-map");
    u32 len = 0;
    const void *p = mm < 0 ? NULL : adt_getprop(adt, mm, name, &len);
    return p && len == 16 ? (void *)p : NULL;
}

static bool slot_read(const char *name, u64 v[2])
{
    const void *p = slot_value(name);
    if (!p)
        return false;
    memcpy(v, p, 16);
    return true;
}

/* In place (same size), then cleaned to the PoC: the next carrier reads the ADT MMU-off. */
static int slot_write(const char *name, u64 a, u64 b)
{
    int mm = adt_path_offset(adt, "/chosen/memory-map");
    u64 v[2] = {a, b};
    if (mm < 0 || adt_setprop(adt, mm, name, v, sizeof(v)) != (int)sizeof(v))
        return -1;
    void *p = slot_value(name);
    if (!p || memcmp(p, v, sizeof(v)))
        return -1;
    dc_civac_range(p, sizeof(v));
    return 0;
}

static int marker_find(struct gpu_boot_marker *out)
{
    u64 v[2];
    memset(out, 0, sizeof(*out));
    if (!slot_read(HV_GPU_BOOT_MARKER, v))
        return MARK_NO_SLOT;
    if (!v[0] && !v[1])
        return MARK_NONE;
    if (v[1] != GPU_BOOT_MARKER_SIZE || !marker_read(v[0], out, sizeof(*out)) ||
        !gpu_t8152_marker_valid(out))
        return MARK_FOREIGN;
    return MARK_OURS;
}

static u64 boot_status(void)
{
    struct gpu_boot_marker m;
    switch (marker_find(&m)) {
        case MARK_NONE:
            return 0;
        case MARK_OURS:
            return m.state;
        case MARK_FOREIGN:
            return HV_GPU_BOOT_FOREIGN;
        default:
            return HV_GPU_BOOT_NO_SLOT;
    }
}

static void marker_update(u32 set)
{
    if (!marker)
        return;
    marker->state |= set;
    marker->forwarded = (u32)__atomic_load_n(&forwarded, __ATOMIC_RELAXED);
    marker->denied = (u32)__atomic_load_n(&denied, __ATOMIC_RELAXED);
    marker->run_refused = (u32)__atomic_load_n(&run_refused, __ATOMIC_RELAXED);
    marker->faults = (u32)__atomic_load_n(&faults, __ATOMIC_RELAXED);
    marker->boot_control_before = boot_before;
    marker->boot_control_after = boot_after;
    marker->g2 = g2 ? HV_GPU_G2_ARMED | g2_grants << 1 : 0;
    gpu_t8152_marker_seal(marker);
    dc_civac_range(marker, sizeof(*marker));
}

static const char *marker_says(u32 st)
{
    if (st & GPU_BM_POWER_FAILED)
        return "a power-on failed";
    if (st & GPU_BM_FOREIGN)
        return "the GPU was found already on";
    if (st & GPU_BM_ENDED)
        return "a session ran and ended with the firmware left running";
    if (st & GPU_BM_PROBE)
        return "a session is (or was) forwarding to it";
    if (st & GPU_BM_POWERED)
        return "it was powered on";
    return "a power-on was claimed";
}

/*
 * Claim this boot's GPU session: the record first (so the slot never names an empty page), then
 * the marker slot, then the pool slot.  Any failure after the marker slot is set leaves the boot
 * claimed: that only forbids, so it is the safe direction.
 */
static int marker_claim(const struct gpu_psd_sample *before, u32 nsteps, bool cold)
{
    u64 pv[2];
    if (!slot_read(HV_GPU_POOL_SLOT, pv) || pv[0] || pv[1]) {
        printf("HV: GPU: POWER refused: /chosen/memory-map/%s is %s; the pool cannot be "
               "reserved for the rest of the boot\n",
               HV_GPU_POOL_SLOT, slot_value(HV_GPU_POOL_SLOT) ? "in use" : "missing");
        return -1;
    }
    marker = memalign(GPU_BOOT_MARKER_SIZE, GPU_BOOT_MARKER_SIZE);
    if (!marker) {
        printf("HV: GPU: POWER refused: no memory for the boot marker page\n");
        return -1;
    }
    memset(marker, 0, GPU_BOOT_MARKER_SIZE);
    u64 pa = (u64)marker;
    if (hv_t8152_reserve_carrier(pa, pa + GPU_BOOT_MARKER_SIZE, "GPU boot marker (G1)")) {
        free(marker);
        marker = NULL;
        return -1;
    }
    marker->state = GPU_BM_CLAIMED | (cold ? 0 : GPU_BM_FOREIGN) | (g2 ? GPU_BM_G2 : 0);
    marker->pool_base = pool_base;
    marker->pool_size = pool_size;
    for (u32 i = 0; i < nsteps && i < GPU_PSD_MAX_STEPS; i++)
        marker->psd_before[i] = gpu_psd_pack(&before[i]);
    marker_update(0);
    if (slot_write(HV_GPU_BOOT_MARKER, pa, GPU_BOOT_MARKER_SIZE)) {
        printf("HV: GPU: POWER refused: /chosen/memory-map/%s could not be written\n",
               HV_GPU_BOOT_MARKER);
        marker = NULL; /* the page stays reserved (denied); nothing was claimed */
        return -1;
    }
    if (slot_write(HV_GPU_POOL_SLOT, pool_base, pool_size)) {
        printf("HV: GPU: POWER refused: the pool slot could not be written; this boot's GPU "
               "session is claimed anyway (reboot)\n");
        return -1;
    }
    printf("HV: GPU: boot marker claimed: /chosen/memory-map/%s -> [0x%lx, +0x%lx), %s -> pool "
           "[0x%lx, 0x%lx) (reserved for the rest of the boot)\n",
           HV_GPU_BOOT_MARKER, pa, GPU_BOOT_MARKER_SIZE, HV_GPU_POOL_SLOT, pool_base,
           pool_base + pool_size);
    return 0;
}

/* ---- G1: POWER ------------------------------------------------------------------------------ */

static u32 hw_read32(u64 addr)
{
    u64 v[2] = {0, 0};
    if (!hw_rw(NULL, addr, v, false, 2)) {
        __atomic_add_fetch(&faults, 1, __ATOMIC_RELAXED);
        printf("HV: GPU: read of 0x%lx faulted\n", addr);
        return 0;
    }
    return (u32)v[0];
}

/* After power-on: the ID words the probe may read, and how power-on left each ASC. */
static void read_identity(void)
{
    u32 n;
    const struct gpu_probe_reg *regs = gpu_t8152_probe_sgx_reads(&n);
    for (u32 i = 0; i < n && i < GPU_HANDOFF_ID_WORDS; i++)
        gpu_id[i] = hw_read32(adm.sgx.base + regs[i].offset);
    printf("HV: GPU: GPU ID 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x, core mask 0x%x\n", gpu_id[0], gpu_id[1],
           gpu_id[2], gpu_id[3], gpu_id[4], gpu_id[5], gpu_id[6]);
    for (u32 r = 0; r < GPU_T8152_ROLES; r++) {
        cpu_control[r] = hw_read32(adm.wrapper[r].base + GPU_T8152_ASC_CPU_CONTROL);
        cpu_status[r] = hw_read32(adm.wrapper[r].base + GPU_T8152_ASC_CPU_STATUS);
        bool run = cpu_control[r] & GPU_ASC_CPU_RUN;
        printf("HV: GPU: %s CPU_CONTROL 0x%x (RUN %s) CPU_STATUS 0x%x%s\n",
               r ? "gfx1-asc" : "gfx-asc", cpu_control[r], run ? "set" : "CLEAR", cpu_status[r],
               run ? ": the firmware is running; the guest adopts it" :
                     ": the carrier never sets RUN, so the probe cannot adopt this ASC");
    }
}

/*
 * G2 C1 (owner decision G2-D2): the one checked sgx boot-control step, right after power-on and
 * before the guest exists.  Read; refuse unless the recipe accepts the value; write it with the
 * recipe's bits set; read back; it must be the recipe's expected value.  Never again this boot
 * (POWER runs once per boot).  The GPU stays powered whatever happens.
 */
static int boot_control_step(void)
{
    const struct gpu_g2_recipe *r = gpu_t8152_g2_recipe();
    boot_before = hw_read32(boot_addr);
    if (!gpu_t8152_g2_boot_accepts(r, boot_before)) {
        marker_update(GPU_BM_GUARD);
        printf("HV: GPU: boot-control sgx+0x%lx reads 0x%x, not a value the recipe accepts: not "
               "written; the GPU stays powered: reboot the M6\n",
               boot_addr - adm.sgx.base, boot_before);
        return HV_GPU_E_BOOT_CONTROL;
    }
    u64 v[2] = {boot_before | r->boot_set, 0};
    if (!hw_rw(NULL, boot_addr, v, true, 2)) {
        __atomic_add_fetch(&faults, 1, __ATOMIC_RELAXED);
        printf("HV: GPU: boot-control write faulted\n");
    }
    boot_after = hw_read32(boot_addr);
    marker_update(GPU_BM_BOOT_CONTROL);
    printf("HV: GPU: boot-control sgx+0x%lx 0x%x -> 0x%x (G2-D2, once this boot)%s\n",
           boot_addr - adm.sgx.base, boot_before, boot_after,
           boot_after == r->boot_expect ? "" : ": NOT the expected value; reboot the M6");
    if (boot_after != r->boot_expect)
        return HV_GPU_E_BOOT_CONTROL;
    boot_done = true;
    state |= GPU_HS_SGX_BOOT;
    handoff_publish();
    return 0;
}

static int power(void)
{
    if (!state_ok("POWER"))
        return HV_GPU_E_STATE;
    if (!admitted || power_tried || hooked) {
        printf("HV: GPU: POWER refused: %s\n",
               !admitted ? "the GPU is not admitted" :
               hooked    ? "this session has the G0 logging hooks (use --windows-gpu probe)" :
                           "POWER runs once per carrier session");
        return HV_GPU_E_STATE;
    }
    const struct gpu_psd_recipe *r = gpu_t8152_psd_recipe();
    if (gpu_t8152_psd_check(r))
        return HV_GPU_E_RECIPE; /* nothing touched, nothing spent */
    power_tried = true;

    struct gpu_boot_marker found;
    int m = marker_find(&found);
    if (m != MARK_NONE) {
        if (m == MARK_OURS)
            printf("HV: GPU: POWER refused: the boot marker says %s earlier this boot (state 0x%x, "
                   "pool [0x%lx, 0x%lx)); one GPU firmware session per boot: reboot the M6\n",
                   marker_says(found.state), found.state, found.pool_base,
                   found.pool_base + found.pool_size);
        else
            printf("HV: GPU: POWER refused: /chosen/memory-map/%s is %s; not powering the GPU "
                   "(reboot the M6)\n",
                   HV_GPU_BOOT_MARKER,
                   m == MARK_NO_SLOT ? "missing" : "set but holds no valid GPU marker");
        return HV_GPU_E_BOOT;
    }

    static const struct gpu_psd_ops ops = {psd_read32, psd_write32, psd_delay};
    struct gpu_psd_sample before[GPU_PSD_MAX_STEPS];
    gpu_t8152_psd_sample(r, &ops, before);
    bool cold = gpu_t8152_psd_all_cold(r, before);
    for (u32 i = 0; i < r->nsteps; i++)
        printf("HV: GPU: PSD %s (service %u) before: state %u ack 0x%x\n", r->step[i].name,
               before[i].service, before[i].state, before[i].ack);
    if (marker_claim(before, r->nsteps, cold))
        return HV_GPU_E_BOOT;
    if (!cold) {
        printf("HV: GPU: POWER refused: a GPU PSD service is not in the recipe's precondition state (%u) although no "
               "marker was set: something else powered it this boot; marked used, reboot\n",
               r->cold_idle);
        return HV_GPU_E_BOOT;
    }

    printf("HV: GPU: powering on (%s): %u PSD services in order\n", r->source, r->nsteps);
    int rc = gpu_t8152_psd_power_on(r, &ops, &psd);
    for (u32 i = 0; i < r->nsteps; i++)
        marker->psd_after[i] = gpu_psd_pack(&psd.after[i]);
    if (rc) {
        marker_update(GPU_BM_POWER_FAILED);
        printf("HV: GPU: power-on FAILED at step %d; nothing was powered off or retried; the GPU "
               "state is unknown: reboot the M6\n",
               psd.failed_step);
        return HV_GPU_E_POWER;
    }
    marker_update(GPU_BM_POWERED);
    state |= GPU_HS_POWERED;
    for (u32 i = 0; i < r->nsteps; i++)
        printf("HV: GPU: PSD %s (service %u) after: state %u ack 0x%x (%u polls)\n",
               r->step[i].name, psd.after[i].service, psd.after[i].state, psd.after[i].ack,
               psd.polls[i]);
    read_identity();
    handoff_publish();
    printf("HV: GPU: powered: every PSD service on; the GPU firmware session of this boot is "
           "now in use (boot marker)\n");
    if (g2 && (g2_grants & GPU_G2_GRANT_BOOT_CONTROL))
        return boot_control_step();
    return 0;
}

/* ---- G2: the grants ------------------------------------------------------------------------- */

/*
 * HV_GPU_OP_G2, after ADMIT and before POWER, once: arm the G2 guards for the PROBE that follows,
 * with the launcher's grants (owner decisions G2-D1 fw-mmio, G2-D2 boot-control).  Everything is
 * checked before anything is recorded; a refusal touches nothing (the session can still run G1).
 */
static int g2_arm(u64 grants)
{
    if (!state_ok("G2"))
        return HV_GPU_E_STATE;
    if (!admitted || hooked || power_tried || g2) {
        printf("HV: GPU: G2 refused: %s\n",
               !admitted   ? "the GPU is not admitted" :
               hooked      ? "this session has the G0 logging hooks" :
               power_tried ? "POWER already ran (G2 is armed between ADMIT and POWER)" :
                             "G2 is armed once per session");
        return HV_GPU_E_STATE;
    }
    const struct gpu_g2_recipe *r = gpu_t8152_g2_recipe();
    if (grants & ~(u64)GPU_G2_GRANTS) {
        printf("HV: GPU: G2 refused: unknown grant bits 0x%lx\n", grants);
        return HV_GPU_E_G2;
    }
    if (gpu_t8152_g2_check(r))
        return HV_GPU_E_G2;
    u64 rb, rs;
    hv_t8152_guest_ram(&rb, &rs);
    int n = 0;
    if (grants & GPU_G2_GRANT_FW_MMIO) {
        n = gpu_t8152_g2_declare(r, &adm, table, table_count, rb, rb + rs, declared,
                                 ARRAY_SIZE(declared));
        if (n < 0)
            return HV_GPU_E_G2;
    }
    u64 addr = 0;
    if ((grants & GPU_G2_GRANT_BOOT_CONTROL) &&
        gpu_t8152_g2_boot_check(r, &adm, table, table_count, &addr))
        return HV_GPU_E_G2;
    for (u32 i = 0; i < r->nroots; i++)
        if (r->root[i].offset >= adm.region[r->root[i].region].size) {
            printf("HV: GPU: G2 refused: extra UAT root %u lies outside its region\n", i);
            return HV_GPU_E_G2;
        }
    g2 = true;
    g2_grants = (u32)grants;
    declared_count = (u32)n;
    boot_addr = addr;
    handoff_publish();
    printf("HV: GPU: G2 armed (%s): fw-mmio %s (G2-D1), boot-control %s (G2-D2); whole messages "
           "only, one init-data message per role, %u refused types, sgx locked after init, UAT "
           "audit before every EP 0x%x/0x%x message\n",
           r->source, n ? "declared" : "not granted", addr ? "granted" : "not granted",
           r->nrefused, GPU_MSG_EP_GFX, GPU_MSG_EP_DOORBELL);
    for (u32 i = 0; i < declared_count; i++)
        printf("HV: GPU: fw-mmio slot %u [0x%lx, +0x%lx) %s, anchored in /arm-io/%s\n",
               declared[i].tag, declared[i].base, declared[i].size,
               declared[i].flags & GPU_RF_READ_ONLY ? "RO" : "RW", declared[i].name);
    return 0;
}

/* ---- the UAT audit (gpu_t8152_uat.h) --------------------------------------------------------- */

static void uat_setup(void)
{
    struct gpu_uat_scope *s = &uat_scope;
    memset(s, 0, sizeof(*s));
    s->pool = (struct gpu_uat_span){pool_base, pool_size};
    for (u32 i = 0; i < 4; i++) {
        s->fw_rw[i] = (struct gpu_uat_span){adm.region[i].base, adm.region[i].size};
        s->fw_ro[i] = (struct gpu_uat_span){adm.region[4 + i].base, adm.region[4 + i].size};
    }
    for (u32 r = 0; r < GPU_T8152_ROLES; r++)
        s->text[r] = (struct gpu_uat_span){adm.text[r].base, adm.text[r].size};
    s->ctx_table = (struct gpu_uat_span){adm.region[GPU_R_TTBS].base, adm.region[GPU_R_TTBS].size};
    s->tables[0] = s->pool;
    s->tables[1] = (struct gpu_uat_span){adm.region[GPU_R_PAGETABLES].base,
                                         adm.region[GPU_R_PAGETABLES].size};
    s->tables[2] = (struct gpu_uat_span){adm.region[GPU_R_SHARED_L2].base,
                                         adm.region[GPU_R_SHARED_L2].size};
    s->dram_base = adm.dram_base;
    s->dram_size = adm.dram_size;
    if (g2_grants & GPU_G2_GRANT_FW_MMIO) {
        s->fw_mmio = declared;
        s->nfw_mmio = declared_count;
    }
    const struct gpu_g2_recipe *r = gpu_t8152_g2_recipe();
    if (r->available && !gpu_t8152_g2_check(r))
        for (u32 i = 0; i < r->nroots && s->nroots < GPU_UAT_MAX_ROOTS; i++)
            if (r->root[i].offset < adm.region[r->root[i].region].size)
                s->roots[s->nroots++] = adm.region[r->root[i].region].base + r->root[i].offset;
    if (!uat_visited)
        uat_visited = malloc(UAT_VISITED_CAP * sizeof(u64));
    s->visited = uat_visited;
    s->visited_cap = uat_visited ? UAT_VISITED_CAP : 0;
    if (!uat_base.e) {
        /* Room for every entry of every firmware table page (about 1.6 MiB on the M6). */
        u32 cap = gpu_uat_baseline_cap(s);
        uat_base.e = malloc((size_t)cap * sizeof(*uat_base.e));
        uat_base.cap = uat_base.e ? cap : 0;
    }
    uat_ready = s->visited && uat_base.e;
}

static void uat_print(const char *when, const struct gpu_uat_report *rep, bool force)
{
    if (!force && __atomic_fetch_add(&uat_printed, 1, __ATOMIC_RELAXED) >= UAT_PRINT_LIMIT)
        return;
    printf("HV: GPU: UAT audit %s: %u roots, %u tables%s; leaves pool %u uat %u data %u fw-mmio %u; "
           "outliers %u (%u in the baseline): text %u dram %u mmio %u bad %u table %u; fresh %u "
           "(%u in guest-writable tables)\n",
           when, rep->roots, rep->tables, rep->capped ? " (CAPPED: incomplete)" : "",
           rep->leaves[GPU_UC_POOL], rep->leaves[GPU_UC_UAT], rep->leaves[GPU_UC_DATA],
           rep->leaves[GPU_UC_FW_MMIO], rep->outliers, rep->known, rep->leaves[GPU_UC_TEXT],
           rep->leaves[GPU_UC_DRAM], rep->leaves[GPU_UC_MMIO], rep->leaves[GPU_UC_BAD],
           rep->leaves[GPU_UC_TABLE], rep->fresh, rep->fresh_in_pool);
    for (u32 i = 0; i < rep->nfirst; i++)
        printf("HV: GPU: UAT outlier: entry 0x%lx (level %u%s) descriptor 0x%lx: %s\n",
               rep->first[i].key, rep->first[i].level,
               rep->first[i].in_pool ? ", guest-writable table" : "",
               rep->first[i].desc, gpu_uat_class_name(rep->first[i].cls));
}

/* One audit against the PROBE baseline; the report stays readable through STATUS. */
static u32 uat_check(const char *when, bool force_print)
{
    static const struct gpu_uat_ops ops = {uat_page, NULL};
    if (!uat_ready)
        return 0;
    gpu_uat_audit(&uat_scope, &ops, &uat_base, NULL, &uat_last);
    uat_audits++;
    if (uat_last.fresh > uat_fresh_max)
        uat_fresh_max = uat_last.fresh;
    uat_print(when, &uat_last, force_print || uat_last.fresh);
    return uat_last.fresh;
}

/* ---- G1: PROBE (mediated forwarding) ------------------------------------------------------- */

static void amap_acquire(void)
{
    while (__atomic_exchange_n(&amap_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void amap_release(void)
{
    __atomic_store_n(&amap_lock, 0, __ATOMIC_RELEASE);
}

static void g2_acquire(void)
{
    while (__atomic_exchange_n(&g2_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void g2_release(void)
{
    __atomic_store_n(&g2_lock, 0, __ATOMIC_RELEASE);
}

static const char *where(u32 cls, u64 addr, u64 *off)
{
    if (cls == GPU_C_MAILBOX) {
        *off = addr - ALIGN_DOWN(addr, GPU_T8152_PAGE);
        return "post-office";
    }
    *off = addr - adm.sgx.base;
    return "sgx";
}

static void log_message(u64 addr, const u64 *val, bool write, int width)
{
    u32 role;
    if (!gpu_t8152_mailbox_window(&adm, addr, &role))
        return;
    u64 off = (addr - adm.mailbox.base) & (GPU_T8152_MBOX_STRIDE - 1);
    /* Status polls are counted in the map, not logged. */
    if (off >= 0x20 ||
        __atomic_fetch_add(&msgs_logged, 1, __ATOMIC_RELAXED) >= HV_GPU_MSG_LOG_LIMIT)
        return;
    if (width == 4)
        printf("HV: GPU: %s %s msg 0x%016lx ep 0x%lx (%s)\n", role ? "GFX1" : "GFX",
               write ? "TX" : "RX", val[0], val[1],
               gpu_msg_kind_name(gpu_t8152_msg_kind(val[1], val[0])));
    else
        printf("HV: GPU: %s %s %s half 0x%016lx\n", role ? "GFX1" : "GFX", write ? "TX" : "RX",
               off & 8 ? "high" : "low", val[0]);
}

/* Count one forwarded RX or TX access of a GFX window by message kind (C6). */
static void count_message(u64 addr, const u64 *val, bool write, int width)
{
    u32 role;
    if (!gpu_t8152_mailbox_window(&adm, addr, &role))
        return;
    u64 off = (addr - adm.mailbox.base) & (GPU_T8152_MBOX_STRIDE - 1);
    if (off >= 0x20)
        return; /* status */
    u32 kind = width == 4 ? gpu_t8152_msg_kind(val[1], val[0]) : GPU_MK_HALF;
    __atomic_add_fetch(&msg_count[role][write][kind], 1, __ATOMIC_RELAXED);
}

/*
 * G2 C5 and the UAT gate: may this whole host-to-firmware message (W128 on a GFX window) go out?
 * Init data once per role; never a type the recipe refuses; on the two application endpoints only
 * when the page tables pass the audit (init data: no fresh outlier anywhere, as nothing of G2 has
 * reached the firmware yet; any other message: none in a table the guest can write, which is
 * the pool, the context table and every fw-rw region PROBE maps read-write).
 */
static bool g2_allow_tx(u32 role, const u64 *val)
{
    u32 kind = gpu_t8152_msg_kind(val[1], val[0]);
    const char *who = role ? "GFX1" : "GFX";
    if (kind == GPU_MK_INIT && (init_sent & BIT(role))) {
        __atomic_add_fetch(&guard[HV_GPU_G_INIT_AGAIN], 1, __ATOMIC_RELAXED);
        marker_update(GPU_BM_GUARD);
        printf("HV: GPU: %s TX 0x%016lx refused: a second init-data message to this role (one "
               "firmware session per boot)\n",
               who, val[0]);
        return false;
    }
    if (gpu_t8152_g2_refused(gpu_t8152_g2_recipe(), val[1], val[0])) {
        __atomic_add_fetch(&guard[HV_GPU_G_REFUSED], 1, __ATOMIC_RELAXED);
        marker_update(GPU_BM_GUARD);
        printf("HV: GPU: %s TX 0x%016lx ep 0x%lx refused: a message type the G2 recipe never "
               "forwards\n",
               who, val[0], val[1] & 0xff);
        return false;
    }
    if (gpu_t8152_g2_iop_power_down(val[1], val[0])) {
        __atomic_add_fetch(&guard[HV_GPU_G_REFUSED], 1, __ATOMIC_RELAXED);
        marker_update(GPU_BM_GUARD);
        printf("HV: GPU: %s TX 0x%016lx refused: an IOP power state other than on (waking the "
               "firmware again would be a second start)\n",
               who, val[0]);
        return false;
    }
    u32 ep = val[1] & 0xff;
    if (ep != GPU_MSG_EP_GFX && ep != GPU_MSG_EP_DOORBELL)
        return true;
    char when[64];
    snprintf(when, sizeof(when), "before %s %s", who, gpu_msg_kind_name(kind));
    uat_check(when, kind == GPU_MK_INIT);
    bool bad = !uat_ready || (kind == GPU_MK_INIT ? uat_last.fresh : uat_last.fresh_in_pool);
    if (bad) {
        __atomic_add_fetch(&guard[HV_GPU_G_AUDIT], 1, __ATOMIC_RELAXED);
        marker_update(GPU_BM_GUARD);
        printf("HV: GPU: %s TX 0x%016lx refused: %s\n", who, val[0],
               uat_ready ? "the GPU page tables map outside the pool and the admitted regions" :
                           "no UAT audit is possible (no memory)");
        return false;
    }
    return true;
}

static bool probe_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u32 cls = GPU_C_NONE, disp = GPU_A_DENIED;
    u32 act = GPU_P_DENY, act_any;
    if (g2)
        act_any = gpu_t8152_probe_decide_g2(&adm, table, table_count, addr, width, write, &cls);
    else
        act_any = gpu_t8152_probe_decide(&adm, table, table_count, addr, width, write, &cls);
    if (__atomic_load_n(&forwarding, __ATOMIC_ACQUIRE))
        act = act_any;
    u64 wrote = write ? val[0] : 0;
    u32 role = 0;
    bool gfx_window = cls == GPU_C_MAILBOX && gpu_t8152_mailbox_window(&adm, addr, &role);

    if (g2 && act == GPU_P_DENY && gfx_window && width == 3 &&
        ((addr - adm.mailbox.base) & (GPU_T8152_MBOX_STRIDE - 1)) < 0x20)
        __atomic_add_fetch(&guard[HV_GPU_G_HALF], 1, __ATOMIC_RELAXED);
    if (g2 && act != GPU_P_DENY && __atomic_load_n(&sgx_locked, __ATOMIC_ACQUIRE) &&
        (cls == GPU_C_SGX || cls == GPU_C_ASC_CTRL)) {
        /* C4: once init data went out, the firmware owns sgx (it may gate it). */
        __atomic_add_fetch(&guard[HV_GPU_G_LOCKED], 1, __ATOMIC_RELAXED);
        act = GPU_P_DENY;
        disp = GPU_A_LOCKED;
    }
    bool init = false, locked = false;
    if (g2 && act == GPU_P_PASS && gfx_window && write && width == 4) {
        g2_acquire();
        locked = true;
        if (!g2_allow_tx(role, val)) {
            act = GPU_P_DENY;
            disp = GPU_A_BLOCKED;
        } else {
            init = gpu_t8152_msg_kind(val[1], val[0]) == GPU_MK_INIT;
        }
    }

    if (act == GPU_P_PASS) {
        if (hw_rw(ctx, addr, val, write, width)) {
            disp = GPU_A_PASSED;
        } else {
            disp = GPU_A_FAULT;
            if (!write)
                val[0] = val[1] = 0;
        }
    } else if (act == GPU_P_CPU_CONTROL) {
        u64 cur[2] = {0, 0};
        if (!hw_rw(ctx, addr, cur, false, 2))
            disp = GPU_A_FAULT;
        else if ((u32)val[0] == (u32)cur[0] && (cur[0] & GPU_ASC_CPU_RUN))
            disp = GPU_A_MEDIATED; /* the value it holds, RUN set: nothing to do */
        else
            disp = GPU_A_REFUSED;
    } else if (!write) {
        val[0] = 0;
        if (width > 3)
            val[1] = 0;
    }

    if (init && disp != GPU_A_DENIED) {
        /* Sent (or faulted on the way: count it as sent).  From now on sgx is the firmware's. */
        init_sent |= BIT(role);
        __atomic_store_n(&sgx_locked, true, __ATOMIC_RELEASE);
        marker_update(0);
    }
    if (locked)
        g2_release();

    u64 *counter = disp == GPU_A_PASSED || disp == GPU_A_MEDIATED ? &forwarded :
                   disp == GPU_A_FAULT                            ? &faults :
                                                                    &denied;
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
    if (disp == GPU_A_REFUSED)
        __atomic_add_fetch(&run_refused, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&class_hits[cls < GPU_C_COUNT ? cls : 0], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&total_hits, 1, __ATOMIC_RELAXED);

    if (gfx_window)
        __atomic_add_fetch(&gfx_hits, 1, __ATOMIC_RELAXED);
    if (disp == GPU_A_PASSED && gfx_window) {
        count_message(addr, val, write, width);
        if (write && width == 4 && gpu_t8152_msg_kind(val[1], val[0]) == GPU_MK_DOORBELL) {
            __atomic_store_n(&last_db_msg, val[0], __ATOMIC_RELAXED);
            __atomic_store_n(&last_db_seq, __atomic_load_n(&forwarded, __ATOMIC_RELAXED),
                             __ATOMIC_RELAXED);
            __atomic_store_n(&last_db_role, BIT(63) | role, __ATOMIC_RELEASE);
        }
    }

    bool first;
    u64 value = write ? wrote : val[0];
    amap_acquire();
    gpu_access_note(&amap, addr, width, write, disp, cls, value, &first);
    amap_release();
    if (first) {
        u64 off;
        const char *base = where(cls, addr, &off);
        printf("HV: GPU: probe %s %s+0x%lx (%s, %d bytes) %s0x%lx: %s\n", write ? "write" : "read",
               base, off, gpu_class_name(cls), 1 << width, write ? "value " : "-> ", value,
               gpu_access_disp_name(disp));
    }
    if (disp == GPU_A_PASSED && cls == GPU_C_MAILBOX)
        log_message(addr, val, write, width);
    if (disp == GPU_A_REFUSED)
        printf("HV: GPU: CPU_CONTROL write 0x%lx refused (RUN is never cleared or toggled)\n",
               wrote);
    if (init && disp == GPU_A_PASSED)
        printf("HV: GPU: %s init data forwarded: sgx is locked for the guest from now on\n",
               role ? "GFX1" : "GFX");
    return true;
}

static void unmap_firmware(void)
{
    for (u32 i = 0; i < table_count; i++) {
        struct gpu_range *r = &table[i];
        if ((r->cls == GPU_C_FW_RW || r->cls == GPU_C_FW_RO) && (r->flags & GPU_RF_MAPPED)) {
            hv_unmap(r->base, r->size);
            r->flags &= ~GPU_RF_MAPPED;
        }
    }
}

static int probe(void)
{
    if (!state_ok("PROBE"))
        return HV_GPU_E_STATE;
    if (!admitted || hooked || probing || !(state & GPU_HS_POWERED)) {
        printf("HV: GPU: PROBE refused: %s\n",
               !admitted                  ? "the GPU is not admitted" :
               !(state & GPU_HS_POWERED)  ? "the GPU is not powered (POWER first)" :
                                            "the hooks are already in place");
        return HV_GPU_E_STATE;
    }
    probing = true;
    const struct gpu_range *mbox = NULL;
    for (u32 i = 0; i < table_count; i++)
        if (table[i].cls == GPU_C_MAILBOX)
            mbox = &table[i];
    u32 n;
    const struct gpu_probe_reg *regs = gpu_t8152_probe_sgx_reads(&n);
    for (u32 i = 0; i < n; i++) {
        const struct gpu_range *r =
            gpu_t8152_find(table, table_count, adm.sgx.base + regs[i].offset);
        if (!r || r->cls != GPU_C_SGX) {
            printf("HV: GPU: PROBE refused: forwarded register sgx+0x%x (%s) is not in an sgx "
                   "range of this table\n",
                   regs[i].offset, regs[i].what);
            return HV_GPU_E_RANGES;
        }
    }
    if (!mbox || hv_map_hook(adm.sgx.base, probe_hook, adm.sgx.size) < 0 ||
        hv_map_hook(mbox->base, probe_hook, mbox->size) < 0) {
        printf("HV: GPU: the probe hooks could not be installed\n");
        return HV_GPU_E_POLICY;
    }
    for (u32 i = 0; i < table_count; i++) {
        struct gpu_range *r = &table[i];
        if ((r->base >= adm.sgx.base && r->base - adm.sgx.base < adm.sgx.size) || r == mbox)
            r->flags |= GPU_RF_HOOKED;
        if (r->cls != GPU_C_FW_RW && r->cls != GPU_C_FW_RO)
            continue;
        int rc = r->cls == GPU_C_FW_RW ? hv_map_hw(r->base, r->base, r->size) :
                                         hv_map_hw_ro(r->base, r->base, r->size);
        if (rc < 0) {
            printf("HV: GPU: PROBE: %s [0x%lx, 0x%lx) could not be mapped\n", r->name, r->base,
                   r->base + r->size);
            hv_unmap(r->base, r->size);
            unmap_firmware();
            return HV_GPU_E_POLICY;
        }
        r->flags |= GPU_RF_MAPPED;
    }
    state |= GPU_HS_ASC_RUN | GPU_HS_MMIO_PASS | GPU_HS_FW_MAPPED;
    handoff_publish();
    /* The UAT baseline: what iBoot and the firmware mapped before the guest exists. */
    static const struct gpu_uat_ops ops = {uat_page, NULL};
    uat_setup();
    if (uat_ready) {
        gpu_uat_audit(&uat_scope, &ops, NULL, &uat_base, &uat_last);
        uat_audits++;
        uat_print("baseline (before the guest)", &uat_last, true);
        if (uat_base.overflow)
            printf("HV: GPU: UAT baseline overflow: more than %u outliers; later audits count "
                   "every outlier past them as fresh\n",
                   uat_base.cap);
    } else {
        printf("HV: GPU: no memory for the UAT audit%s\n",
               g2 ? ": every G2 message on the application endpoints will be refused" : "");
    }
    __atomic_store_n(&forwarding, true, __ATOMIC_RELEASE);
    marker_update(GPU_BM_PROBE);
    printf("HV: GPU: probe forwarding on sgx [0x%lx, 0x%lx) and post-office page 0x%lx: %u sgx "
           "ID reads, CPU_CONTROL (RUN guarded) / CPU_STATUS of both ASCs, the GFX and GFX1 "
           "windows (RX, TX, status); everything else read-as-zero; UAT regions mapped RW, DATA "
           "regions RO; IRQs not published (poll)\n",
           adm.sgx.base, adm.sgx.base + adm.sgx.size, mbox->base, n);
    if (g2)
        printf("HV: GPU: G2 guards on: 128-bit messages only, init data once per role, sgx locked "
               "after it, UAT audit before every application-endpoint message\n");
    return 0;
}

/* ---- teardown -------------------------------------------------------------------------------- */

static void print_map(void)
{
    amap_acquire();
    u32 n = amap.n, dropped = amap.dropped;
    amap_release();
    printf("HV: GPU: access map: %u entries (%u accesses not recorded), %lu forwarded, %lu denied, "
           "%lu CPU_CONTROL writes refused, %lu faults\n",
           n, dropped, forwarded, denied, run_refused, faults);
    for (u32 i = 0; i < n; i++) {
        const struct gpu_access *e = &amap.e[i];
        u64 off;
        const char *base = where(e->cls, e->addr, &off);
        printf("HV: GPU: map %s+0x%lx %c%d %s %s n=%u first 0x%lx last 0x%lx\n", base, off,
               e->write ? 'W' : 'R', 8 << e->width, gpu_class_name(e->cls),
               gpu_access_disp_name(e->disp), e->count, e->first, e->last);
    }
    for (u32 role = 0; role < GPU_T8152_ROLES; role++)
        for (u32 tx = 0; tx < 2; tx++)
            for (u32 k = 0; k < GPU_MK_COUNT; k++)
                if (msg_count[role][tx][k])
                    printf("HV: GPU: messages %s %s %s n=%lu\n", role ? "GFX1" : "GFX",
                           tx ? "TX" : "RX", gpu_msg_kind_name(k), msg_count[role][tx][k]);
    if (g2)
        printf("HV: GPU: G2 guards: %lu second init-data refused, %lu refused types, %lu refused "
               "by the UAT audit, %lu sgx accesses locked, %lu mailbox halves denied; init data "
               "sent to%s%s%s\n",
               guard[HV_GPU_G_INIT_AGAIN], guard[HV_GPU_G_REFUSED], guard[HV_GPU_G_AUDIT],
               guard[HV_GPU_G_LOCKED], guard[HV_GPU_G_HALF], init_sent & 1 ? " GFX" : "",
               init_sent & 2 ? " GFX1" : "", init_sent ? "" : " no role");
}

void hv_gpu_teardown(const char *why)
{
    __atomic_store_n(&forwarding, false, __ATOMIC_RELEASE);
    if (!admitted || torn_down || !(probing || (state & GPU_HS_POWERED) || marker))
        return;
    torn_down = true;
    unmap_firmware();
    state &= ~(GPU_HS_MMIO_PASS | GPU_HS_FW_MAPPED);
    handoff_publish();
    if (probing)
        print_map();
    if (probing) {
        g2_acquire();
        if (uat_check("at teardown", true))
            marker_update(GPU_BM_GUARD);
        g2_release();
    }
    marker_update(GPU_BM_ENDED);
    printf("HV: GPU: session ended (%s): forwarding off, firmware regions unmapped; the GPU stays "
           "powered and its firmware running for the rest of this boot (never stopped, powered "
           "off or restarted by the carrier); pool [0x%lx, 0x%lx) stays reserved (%s)\n",
           why, pool_base, pool_base + pool_size, HV_GPU_POOL_SLOT);
}

void hv_gpu_emergency_stop(void)
{
    if (__atomic_exchange_n(&forwarding, false, __ATOMIC_ACQ_REL))
        printf("HV: GPU: forwarding stopped before the reset (the GPU is not touched)\n");
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
    if (index == HV_GPU_S_BOOT)
        return boot_status();
    if (index == HV_GPU_S_FORWARDED)
        return __atomic_load_n(&forwarded, __ATOMIC_RELAXED);
    if (index == HV_GPU_S_DENIED)
        return __atomic_load_n(&denied, __ATOMIC_RELAXED);
    if (index == HV_GPU_S_RUN_REFUSED)
        return __atomic_load_n(&run_refused, __ATOMIC_RELAXED);
    if (index == HV_GPU_S_FAULTS)
        return __atomic_load_n(&faults, __ATOMIC_RELAXED);
    if (index == HV_GPU_S_MAP_COUNT || index == HV_GPU_S_MAP_DROPPED) {
        amap_acquire();
        u64 v = index == HV_GPU_S_MAP_COUNT ? amap.n : amap.dropped;
        amap_release();
        return v;
    }
    if (index == HV_GPU_S_POWER_RECIPE)
        return gpu_t8152_psd_recipe()->available ? 1 : 0;
    if (index == HV_GPU_S_G2_RECIPE) {
        const struct gpu_g2_recipe *r = gpu_t8152_g2_recipe();
        return r->available && !gpu_t8152_g2_check(r) ? 1 : 0;
    }
    if (index == HV_GPU_S_G2)
        return (g2 ? HV_GPU_G2_ARMED : 0) |
               (g2_grants & GPU_G2_GRANT_FW_MMIO ? HV_GPU_G2_FW_MMIO : 0) |
               (g2_grants & GPU_G2_GRANT_BOOT_CONTROL ? HV_GPU_G2_BOOT_GRANTED : 0) |
               (boot_done ? HV_GPU_G2_BOOT_DONE : 0) |
               (__atomic_load_n(&sgx_locked, __ATOMIC_ACQUIRE) ? HV_GPU_G2_SGX_LOCKED : 0) |
               (u64)init_sent << 8;
    if (index == HV_GPU_S_DECLARED)
        return g2_grants & GPU_G2_GRANT_FW_MMIO ? declared_count : 0;
    if (index == HV_GPU_S_BOOT_CONTROL || index == HV_GPU_S_BOOT_CONTROL + 1)
        return index == HV_GPU_S_BOOT_CONTROL ? boot_before : boot_after;
    if (index >= HV_GPU_S_MSG && index < HV_GPU_S_MSG + 0x10 * 2 * GPU_T8152_ROLES) {
        u64 i = index - HV_GPU_S_MSG, k = i & 0xf, rt = i >> 4;
        return k < GPU_MK_COUNT ? __atomic_load_n(&msg_count[rt >> 1][rt & 1][k], __ATOMIC_RELAXED)
                                : 0;
    }
    if (index >= HV_GPU_S_GUARD && index < HV_GPU_S_GUARD + HV_GPU_G_COUNT)
        return __atomic_load_n(&guard[index - HV_GPU_S_GUARD], __ATOMIC_RELAXED);
    if (index >= HV_GPU_S_AUDIT && index < HV_GPU_S_AUDIT + HV_GPU_A_COUNT) {
        u64 f = index - HV_GPU_S_AUDIT;
        const struct gpu_uat_report *a = &uat_last;
        if (f >= HV_GPU_A_LEAVES && f < HV_GPU_A_LEAVES + GPU_UC_COUNT)
            return a->leaves[f - HV_GPU_A_LEAVES];
        switch (f) {
            case HV_GPU_A_AUDITS:
                return uat_audits;
            case HV_GPU_A_ROOTS:
                return a->roots;
            case HV_GPU_A_TABLES:
                return a->tables;
            case HV_GPU_A_OUTLIERS:
                return a->outliers;
            case HV_GPU_A_KNOWN:
                return a->known;
            case HV_GPU_A_FRESH:
                return a->fresh;
            case HV_GPU_A_FRESH_POOL:
                return a->fresh_in_pool;
            case HV_GPU_A_CAPPED:
                return a->capped;
            case HV_GPU_A_BASELINE:
                return uat_base.n;
            case HV_GPU_A_BASELINE_OVF:
                return uat_base.overflow;
            case HV_GPU_A_FRESH_MAX:
                return uat_fresh_max;
            default:
                return 0;
        }
    }
    if (index >= HV_GPU_S_AUDIT_FRESH && index < HV_GPU_S_AUDIT_FRESH + 4 * GPU_UAT_REPORT_MAX) {
        u64 i = (index - HV_GPU_S_AUDIT_FRESH) / 4;
        if (i >= uat_last.nfirst)
            return HV_GPU_ERROR;
        const struct gpu_uat_outlier *o = &uat_last.first[i];
        switch ((index - HV_GPU_S_AUDIT_FRESH) % 4) {
            case 0:
                return o->key;
            case 1:
                return o->desc;
            case 2:
                return o->cls | (u64)o->level << 8 | (u64)o->in_pool << 16;
            default:
                return 0;
        }
    }
    if (index >= HV_GPU_S_PSD_BEFORE && index < HV_GPU_S_PSD_BEFORE + GPU_PSD_MAX_STEPS)
        return marker ? marker->psd_before[index - HV_GPU_S_PSD_BEFORE] : 0;
    if (index >= HV_GPU_S_PSD_AFTER && index < HV_GPU_S_PSD_AFTER + GPU_PSD_MAX_STEPS)
        return marker ? marker->psd_after[index - HV_GPU_S_PSD_AFTER] : 0;
    if (index >= HV_GPU_S_GPU_ID && index < HV_GPU_S_GPU_ID + GPU_HANDOFF_ID_WORDS)
        return gpu_id[index - HV_GPU_S_GPU_ID];
    if (index >= HV_GPU_S_CPU && index < HV_GPU_S_CPU + 2 * GPU_T8152_ROLES)
        return index - HV_GPU_S_CPU < GPU_T8152_ROLES ?
                   cpu_control[index - HV_GPU_S_CPU] :
                   cpu_status[index - HV_GPU_S_CPU - GPU_T8152_ROLES];
    if (index >= HV_GPU_S_MAP && index < HV_GPU_S_MAP + 4 * (u64)GPU_ACCESS_MAP_MAX) {
        u64 i = (index - HV_GPU_S_MAP) / 4, v = HV_GPU_ERROR;
        amap_acquire();
        if (i < amap.n) {
            const struct gpu_access *e = &amap.e[i];
            switch ((index - HV_GPU_S_MAP) % 4) {
                case 0:
                    v = e->addr;
                    break;
                case 1:
                    v = e->count;
                    break;
                case 2:
                    v = e->width | (u64)e->write << 8 | (u64)e->disp << 16 | (u64)e->cls << 24;
                    break;
                default:
                    v = e->first;
            }
        }
        amap_release();
        return v;
    }
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
        case HV_GPU_OP_POWER:
            return err(power());
        case HV_GPU_OP_PROBE:
            return err(probe());
        case HV_GPU_OP_STOP:
            hv_gpu_teardown("launcher STOP");
            return 0;
        case HV_GPU_OP_G2:
            return err(g2_arm(a1));
        default:
            return HV_GPU_ERROR;
    }
}

#ifdef HV_GPU_HOST_TEST
void hv_gpu_host_new_session(void)
{
    session_used = admitted = hooked = power_tried = probing = torn_down = forwarding = false;
    marker = NULL;
    memset(&psd, 0, sizeof(psd));
    memset(gpu_id, 0, sizeof(gpu_id));
    memset(cpu_control, 0, sizeof(cpu_control));
    memset(cpu_status, 0, sizeof(cpu_status));
    memset(&amap, 0, sizeof(amap));
    forwarded = denied = run_refused = faults = 0;
    msgs_logged = 0;
    memset(&adm, 0, sizeof(adm));
    table_count = 0;
    handoff = NULL;
    handoff_pa = pool_base = pool_size = 0;
    state = 0;
    memset(class_hits, 0, sizeof(class_hits));
    total_hits = gfx_hits = 0;
    last_hook_addr = last_hook_write = 0;
    last_db_msg = last_db_role = last_db_seq = 0;
    logged = 0;
    g2 = boot_done = sgx_locked = uat_ready = false;
    g2_grants = declared_count = init_sent = 0;
    boot_addr = 0;
    boot_before = boot_after = 0;
    memset(msg_count, 0, sizeof(msg_count));
    memset(guard, 0, sizeof(guard));
    memset(&uat_last, 0, sizeof(uat_last));
    uat_base.n = 0;
    uat_base.overflow = uat_base.sorted = false;
    uat_audits = uat_printed = uat_fresh_max = 0;
}

bool hv_gpu_host_hook(u64 addr, u64 *val, bool write, int width)
{
    return gpu_hook(NULL, addr, val, write, width);
}

bool hv_gpu_host_probe_hook(u64 addr, u64 *val, bool write, int width)
{
    return probe_hook(NULL, addr, val, write, width);
}

const void *hv_gpu_host_handoff(void)
{
    return handoff;
}

const void *hv_gpu_host_marker(void)
{
    return marker;
}
#endif
