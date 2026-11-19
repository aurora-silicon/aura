/* SPDX-License-Identifier: MIT */

/*
 * I1: hardware glue for the T8152 MTP keyboard.  See hv_mtp_t8152.h for the rules and
 * mtp_bringup.c for the protocol.  Written on this branch; no third-party source.
 */

#include "hv_mtp_t8152.h"
#include "adt.h"
#include "asc.h"
#include "exception.h"
#include "hv.h"
#include "hv_input.h"
#include "hv_t8152.h"
#include "memory.h"
#include "mtp_bringup.h"
#include "rtkit.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#define MTP_ASC_REG2      0x314700000UL
#define MTP_POLL_US       1000u
#define MTP_PROBE_MAX_MS  10000u
#define MTP_LOG_PER_S     50u

static struct mtp_bringup mb;
static asc_dev_t *asc;
static rtkit_dev_t *rtk;
static u64 area_pa;
static bool session_used, attached;
static bool live; /* the keyboard is up and nothing latched (read without the servicer flag) */
static u8 servicer;
static u64 next_poll;
static u64 freq_mhz;
static bool probing;
static u32 log_budget;
static u64 log_window;

/* Counters for the report and STATUS. */
static u32 mmio_refused, mmio_faults, mem_refused;
static u64 poll_max_us, polls, last_frames;
static u32 reported_kbd, reported_bad;
static bool reported_latch;

static u64 ticks(void)
{
    return mrs(CNTPCT_EL0);
}

static u64 now_us(void *ctx)
{
    UNUSED(ctx);
    return ticks() / freq_mhz;
}

/* ---- allow-listed MMIO ------------------------------------------------------------------ */

static bool pmgr_reg(u64 addr)
{
    return addr >= MTP_T8152_PMGR_PS &&
           addr < MTP_T8152_PMGR_PS + MTP_T8152_PMGR_COUNT * MTP_T8152_PMGR_STRIDE &&
           !((addr - MTP_T8152_PMGR_PS) % MTP_T8152_PMGR_STRIDE);
}

static bool read_allowed(u64 addr)
{
    return addr == MTP_T8152_ASC_CPU_CTRL || addr == MTP_T8152_DART_TTBR ||
           addr == MTP_T8152_DART_TCR || pmgr_reg(addr);
}

static u32 op_read32(void *ctx, u64 addr)
{
    UNUSED(ctx);
    if (!read_allowed(addr)) {
        mmio_refused++;
        printf("HV: input: MTP read of 0x%lx refused (not on the allow-list)\n", addr);
        return ~0u; /* fails every check that reads it */
    }
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    u32 v = read32(addr);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    if (exc_count != before) {
        mmio_faults++;
        printf("HV: input: MTP read of 0x%lx faulted\n", addr);
        return ~0u;
    }
    return v;
}

static void op_write32(void *ctx, u64 addr, u32 value)
{
    UNUSED(ctx);
    if (!pmgr_reg(addr)) {
        mmio_refused++;
        printf("HV: input: MTP write of 0x%x to 0x%lx refused (not on the allow-list)\n", value,
               addr);
        return;
    }
    printf("HV: input: PMGR MTP state 0x%lx: write 0x%x (target on)\n", addr, value);
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    write32(addr, value);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    if (exc_count != before) {
        mmio_faults++;
        printf("HV: input: PMGR write to 0x%lx faulted\n", addr);
    }
}

/* ---- the stream-4 tables ---------------------------------------------------------------- */

static bool table_mem(u64 pa)
{
    if (pa >= mb.root && pa < mb.root + MTP_DART_PAGE)
        return true;
    u64 tables = area_pa + MTP_AREA_TABLES;
    return pa >= tables && pa < tables + MTP_DART_MAX_TABLES * MTP_DART_PAGE;
}

static u64 op_mem_read(void *ctx, u64 pa)
{
    UNUSED(ctx);
    if ((pa & 7) || !table_mem(pa)) {
        mem_refused++;
        return 0;
    }
    /* Only this side and iBoot write these tables: the line is cleaned as it is read. */
    dc_civac_range((void *)pa, 8);
    return *(volatile u64 *)pa;
}

static void op_mem_write(void *ctx, u64 pa, u64 value)
{
    UNUSED(ctx);
    if ((pa & 7) || !table_mem(pa)) {
        mem_refused++;
        return;
    }
    *(volatile u64 *)pa = value;
    dc_cvac_range((void *)pa, 8);
}

static void op_dart_sync(void *ctx)
{
    UNUSED(ctx);
    sysop("dsb sy");
}

static bool op_root_ok(void *ctx, u64 root)
{
    UNUSED(ctx);
    u64 map_hi = ram_base + mem_size_actual;
    bool ok = !(root & (MTP_DART_PAGE - 1)) &&
              hv_t8152_carrier_ram(root, root + MTP_DART_PAGE, ram_base, map_hi);
    printf("HV: input: dart-sbr stream %u root 0x%lx %s\n", MTP_T8152_SID, root,
           ok ? "accepted" : "refused (not carrier-mapped RAM, or inside guest RAM/store/area)");
    return ok;
}

/* ---- RTKit ------------------------------------------------------------------------------ */

static void app_handler(void *ctx, u8 ep, u64 msg)
{
    UNUSED(ctx);
    mtp_bringup_app_msg(&mb, ep, msg);
}

static bool op_rtkit_start(void *ctx, u64 pool_dva, u32 timeout_ms)
{
    UNUSED(ctx);
    rtk = rtkit_init("mtp-t8152", asc, NULL, NULL, NULL, false);
    if (!rtk || !rtkit_set_buffer_pool_premapped(rtk, area_pa + MTP_AREA_POOL, pool_dva,
                                                  MTP_AREA_POOL_SIZE) ||
        !rtkit_set_app_handler(rtk, app_handler, NULL) || !rtkit_set_early_ap_power(rtk, true)) {
        printf("HV: input: MTP RTKit set-up refused\n");
        return false;
    }
    printf("HV: input: MTP RUN (ASC_CPU_CONTROL bit 4), RTKit boot, pool DVA 0x%lx\n", pool_dva);
    if (!rtkit_boot_timed(rtk, timeout_ms * 1000)) {
        struct rtkit_rejection r;
        if (rtkit_get_first_rejection(rtk, &r))
            printf("HV: input: MTP RTKit boot failed: rejected ep 0x%lx msg 0x%lx (reason %d)\n",
                   r.msg1, r.msg0, (int)r.reason);
        else
            printf("HV: input: MTP RTKit boot failed (timeout or handshake)\n");
        return false;
    }
    if (!rtkit_start_ep(rtk, MTP_SCM_EP)) {
        printf("HV: input: MTP RTKit endpoint 0x20 did not start\n");
        return false;
    }
    return true;
}

static int op_rtkit_service(void *ctx, int max)
{
    UNUSED(ctx);
    return rtkit_service_quiet(rtk, max);
}

static bool op_scm_send(void *ctx, u64 msg)
{
    UNUSED(ctx);
    return rtkit_send_quiet(rtk, MTP_SCM_EP, msg);
}

/* ---- logging (pre-entry only: ATTACH and PROBE) -------------------------------------------- */

static bool log_ok(void)
{
    if (!probing)
        return false;
    u64 t = ticks();
    if (t - log_window >= freq_mhz * 1000000) {
        log_window = t;
        log_budget = MTP_LOG_PER_S;
    }
    if (!log_budget)
        return false;
    log_budget--;
    return true;
}

static void hex(const u8 *p, u32 n)
{
    for (u32 i = 0; i < n; i++)
        printf("%02x", p[i]);
}

static void op_frame(void *ctx, const struct mtp_msg *m)
{
    UNUSED(ctx);
    if (!log_ok())
        return;
    printf("HV: input: mtp frame ch 0x%02x if %u seq %u flags 0x%02x ret 0x%x len %u: ", m->channel,
           m->iface, m->seq, m->flags, m->retcode, m->len);
    hex(m->data, m->len > 64 ? 64 : m->len);
    printf("%s\n", m->len > 64 ? "..." : "");
}

static void op_keyboard(void *ctx, u8 mods, const u8 keys[KBD_BITMAP_BYTES], u16 consumer)
{
    UNUSED(ctx);
    if (log_ok()) {
        printf("HV: input: mtp keyboard -> mods 0x%02x consumer 0x%x keys", mods, consumer);
        for (u32 u = 0; u < KBD_BITMAP_BYTES * 8; u++)
            if (keys[u / 8] & (1u << (u % 8)))
                printf(" 0x%02x", u);
        printf("\n");
    }
    hv_input_internal_keyboard(mods, keys, consumer);
}

static const struct mtp_ops ops = {
    .ctx = NULL,
    .now_us = now_us,
    .read32 = op_read32,
    .write32 = op_write32,
    .dart_mem = {NULL, op_mem_read, op_mem_write},
    .dart_sync = op_dart_sync,
    .root_ok = op_root_ok,
    .rtkit_start = op_rtkit_start,
    .rtkit_service = op_rtkit_service,
    .scm_send = op_scm_send,
    .keyboard = op_keyboard,
    .frame = op_frame,
};

/* ---- identity --------------------------------------------------------------------------- */

static bool reg_is(const char *path, int idx, u64 want)
{
    int trace[8];
    u64 addr, size;
    if (adt_path_offset_trace(adt, path, trace) < 0 ||
        adt_get_reg(adt, trace, "reg", idx, &addr, &size) < 0 || addr != want) {
        printf("HV: input: ADT %s reg[%d] is not 0x%lx\n", path, idx, want);
        return false;
    }
    return true;
}

static bool identity(void)
{
    int mtp = adt_path_offset(adt, "/arm-io/mtp");
    int dart = adt_path_offset(adt, "/arm-io/dart-sbr");
    int mapper = adt_path_offset(adt, "/arm-io/dart-sbr/mapper-mtp");
    int kbd = adt_path_offset(adt, "/arm-io/mtp/iop-mtp-nub/keyboard");
    u32 parent = 0, phandle = ~0u, sid = ~0u;
    u64 vm_base = 0;
    if (chip_id != T8152 || board_id != 0x22 || mtp < 0 || dart < 0 || mapper < 0 || kbd < 0) {
        printf("HV: input: MTP identity: not J804, or the ADT lacks mtp, dart-sbr, mapper-mtp or "
               "the keyboard nub\n");
        return false;
    }
    if (!adt_is_compatible(adt, mtp, "iop,ascwrap-v8") ||
        ADT_GETPROP(adt, mtp, "iommu-parent", &parent) != sizeof(parent) ||
        ADT_GETPROP(adt, mapper, "AAPL,phandle", &phandle) != sizeof(phandle) ||
        ADT_GETPROP(adt, mapper, "reg", &sid) != sizeof(sid) || parent != phandle ||
        sid != MTP_T8152_SID || ADT_GETPROP(adt, dart, "vm-base", &vm_base) != sizeof(vm_base) ||
        vm_base != MTP_T8152_DVA_BASE) {
        printf("HV: input: MTP identity: compatible, iommu-parent (stream %u) or dart-sbr vm-base "
               "0x%lx differ from J804's\n",
               sid, vm_base);
        return false;
    }
    return reg_is("/arm-io/mtp", 0, MTP_T8152_ASC) && reg_is("/arm-io/mtp", 2, MTP_ASC_REG2) &&
           reg_is("/arm-io/dart-sbr", 0, MTP_T8152_DART);
}

/* ---- the area --------------------------------------------------------------------------- */

/*
 * The device-written rings become Normal-NC in every alias of the carrier's map (the way
 * mmu_remap_smp_shared() treats shared memory), after their lines are cleaned: both ring
 * indices share one line, so a cached write-back of the consumer would also write back a stale
 * producer.  The table pages stay cacheable (the device only reads them; every write is cleaned).
 */
static void prepare_area(void)
{
    u64 rings = area_pa + MTP_AREA_TX, size = MTP_AREA_RINGS_END - MTP_AREA_TX;
    dc_civac_range((void *)rings, size);
    sysop("dsb sy");
    mmu_add_mapping(rings, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW);
    mmu_add_mapping(rings | REGION_RWX_EL0, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW_EL0);
    mmu_add_mapping(rings | REGION_RW_EL0, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW_EL0);
    mmu_add_mapping(rings | REGION_RX_EL1, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW_EL0);
    memset((void *)rings, 0, size);
    u64 tables = area_pa + MTP_AREA_TABLES;
    memset((void *)tables, 0, MTP_DART_MAX_TABLES * MTP_DART_PAGE);
    dc_civac_range((void *)tables, MTP_DART_MAX_TABLES * MTP_DART_PAGE);
    sysop("dsb sy");
}

static void log_attach(void)
{
    printf("HV: input: MTP: ASC_CPU_CONTROL 0x%x, TTBR[4] 0x%x, TCR[4] 0x%x, root 0x%lx; DVAs tx "
           "0x%lx rx 0x%lx wake 0x%lx pool 0x%lx; %u table(s), %u PMGR write(s)\n",
           mb.asc_ctrl, mb.ttbr, mb.tcr, mb.root, mb.tx_dva, mb.rx_dva, mb.wake_dva, mb.pool_dva,
           mb.dart.used, mb.pmgr_writes);
    printf("HV: input: MTP PMGR states before:");
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
        printf(" %x", mb.ps_before[i]);
    printf("\n");
    printf("HV: input: MTP steps (ms from start): dart %u power %u rtkit %u scm %u init %u "
           "kbd %u\n",
           mb.step_ms[MTP_ST_DART], mb.step_ms[MTP_ST_POWER], mb.step_ms[MTP_ST_RTKIT],
           mb.step_ms[MTP_ST_SCM], mb.step_ms[MTP_ST_INIT], mb.step_ms[MTP_ST_KBD_READY]);
    for (u32 i = 0; i < MTP_MAX_IFACES; i++) {
        const struct mtp_iface_info *f = &mb.comm.iface[i];
        if (f->seen)
            printf("HV: input: MTP INIT iface %u \"%s\"%s%s descriptor %u bytes%s%s\n", i, f->name,
                   f->complete ? "" : " (incomplete)", f->gpio ? " gpio" : "", f->desc_len,
                   f->desc_truncated ? " (truncated)" : "", f->ready ? " READY" : "");
    }
    if (mb.kbd_iface >= 0) {
        const struct mtp_iface_info *k = &mb.comm.iface[mb.kbd_iface];
        for (u32 at = 0; at < k->desc_len; at += 32) {
            printf("HV: input: MTP keyboard descriptor +%03x: ", at);
            hex(k->desc + at, k->desc_len - at > 32 ? 32 : k->desc_len - at);
            printf("\n");
        }
        if (mb.kbd.valid)
            printf("HV: input: MTP keyboard layout: report ID %u, %u data bytes, %u key "
                   "field(s), Fn %s (bit %u), %u other field(s)\n",
                   mb.kbd.report_id, mb.kbd.data_bytes, mb.kbd.n_fields,
                   mb.kbd.has_fn ? "present" : "absent", mb.kbd.fn_bit, mb.kbd.ignored_fields);
    }
    printf("HV: input: MTP %s (state 0x%x, reason %u) in %u ms; GPIO refused %u, frames %u, "
           "checksum errors %u, mmio refused %u faulted %u, table refusals %u\n",
           mb.reason ? "bring-up stopped" : "up (keyboard)", mb.state, mb.reason,
           mb.step_ms[mb.state & ~MTP_ST_FAILED], mb.gpio_refused, mb.frames, mb.csum_errors,
           mmio_refused, mmio_faults, mem_refused);
    if (mb.reason == MTP_E_RUNNING)
        printf("HV: input: MTP already running: reboot through iBoot for a fresh MTP\n");
    /* From DART on, stream-4 entries point into the area; from RTKit on, the MTP runs. */
    if ((mb.state & ~MTP_ST_FAILED) >= MTP_ST_DART)
        printf("HV: input: stream-4 entries%s point into the area: leave this session by a "
               "reboot\n",
               (mb.state & ~MTP_ST_FAILED) >= MTP_ST_POWER ? " and a running MTP" : "");
}

bool hv_mtp_t8152_attach(u64 area, u64 size, bool fn_fkeys)
{
    if (session_used) {
        printf("HV: input: MTP attach refused: once per carrier session\n");
        return false;
    }
    session_used = true;
    freq_mhz = mrs(CNTFRQ_EL0) / 1000000;
    if (!freq_mhz)
        freq_mhz = 24;
    mb.kbd_iface = -1;
    if (!identity()) {
        mb.state = MTP_ST_FAILED;
        mb.reason = MTP_E_IDENTITY;
        return false;
    }
    if (size != MTP_AREA_SIZE || (area & (MTP_AREA_ALIGN - 1)) ||
        hv_t8152_reserve_input_area(area, area + size) < 0) {
        printf("HV: input: MTP area 0x%lx+0x%lx refused (8 MiB, 2 MiB aligned, above guest RAM)\n",
               area, size);
        mb.state = MTP_ST_FAILED | MTP_ST_IDENTITY;
        mb.reason = MTP_E_AREA;
        return false;
    }
    area_pa = area;
    asc = asc_init("/arm-io/mtp");
    if (!asc) {
        mb.state = MTP_ST_FAILED | MTP_ST_IDENTITY;
        mb.reason = MTP_E_IDENTITY;
        return false;
    }
    prepare_area();
    attached = true;
    probing = true; /* log the bring-up's frames too */
    log_budget = MTP_LOG_PER_S;
    log_window = ticks();
    int rc = mtp_bringup_attach(&mb, &ops, area, (void *)area,
                                fn_fkeys ? KBD_FN_FKEYS : KBD_FN_MEDIA);
    probing = false;
    log_attach();
    __atomic_store_n(&live, rc == MTP_E_NONE, __ATOMIC_RELEASE);
    return rc == MTP_E_NONE;
}

bool hv_mtp_t8152_live(void)
{
    return __atomic_load_n(&live, __ATOMIC_ACQUIRE);
}

static void service(void)
{
    u64 t0 = ticks();
    int rc = mtp_bringup_poll(&mb, 32);
    u64 dt = (ticks() - t0) / freq_mhz;
    if (dt > poll_max_us)
        poll_max_us = dt;
    polls++;
    if (rc < 0)
        __atomic_store_n(&live, false, __ATOMIC_RELEASE);
    else
        last_frames += (u64)rc;
}

void hv_mtp_t8152_poll(void)
{
    if (!hv_mtp_t8152_live() || __atomic_test_and_set(&servicer, __ATOMIC_ACQUIRE))
        return;
    u64 t = ticks();
    if (t >= next_poll) {
        next_poll = t + MTP_POLL_US * freq_mhz;
        service();
    }
    __atomic_clear(&servicer, __ATOMIC_RELEASE);
}

s64 hv_mtp_t8152_probe(u32 ms)
{
    if (!hv_mtp_t8152_live())
        return -1;
    if (ms > MTP_PROBE_MAX_MS)
        ms = MTP_PROBE_MAX_MS;
    u64 before = mb.frames, end = ticks() + (u64)ms * 1000 * freq_mhz;
    probing = true;
    log_budget = MTP_LOG_PER_S;
    log_window = ticks();
    while (ticks() < end && hv_mtp_t8152_live()) {
        if (__atomic_test_and_set(&servicer, __ATOMIC_ACQUIRE))
            continue;
        service();
        __atomic_clear(&servicer, __ATOMIC_RELEASE);
        udelay(MTP_POLL_US);
    }
    probing = false;
    hv_mtp_t8152_report();
    return hv_mtp_t8152_live() ? (s64)(mb.frames - before) : -1;
}

u64 hv_mtp_t8152_status(u32 index)
{
    struct rtkit_notification_counts n = {0, 0, 0};
    switch (index) {
        case 23:
            return mb.state;
        case 24:
            return mb.reason;
        case 25:
            return (u64)mb.kbd_reports | (u64)mb.kbd_bad << 32;
        case 26:
            return 0; /* trackpad: I2 */
        case 27:
            if (rtk)
                rtkit_get_notification_counts(rtk, &n);
            return (n.app & 0xffffffff) | (u64)(mb.doorbells) << 32;
        case 28:
            return poll_max_us;
        case 29:
            return (u64)mb.csum_errors | (u64)mb.ring_errors << 16 | (u64)mb.oversize << 32 |
                   (u64)mb.parse_errors << 48;
        case 30:
            return 0; /* SMC pcIO writes: none in this step (must be 0 or 2 at I2) */
        case 31:
            return mb.gpio_refused;
        default:
            return ~0ull;
    }
}

void hv_mtp_t8152_report(void)
{
    if (!attached)
        return;
    bool latched = (mb.state & MTP_ST_FAILED) && mb.reason >= MTP_E_RTKIT_FAILED;
    u32 bad = mb.kbd_bad + mb.csum_errors + mb.parse_errors + mb.ring_errors;
    if (mb.kbd_reports == reported_kbd && bad == reported_bad && latched == reported_latch &&
        !probing)
        return;
    reported_kbd = mb.kbd_reports;
    reported_bad = bad;
    reported_latch = latched;
    struct rtkit_notification_counts n = {0, 0, 0};
    if (rtk)
        rtkit_get_notification_counts(rtk, &n);
    printf("HV: input: mtp kbd %u tp 0 bad %u (csum %u ring %u) frames %u gpio %u; svc max %lu us "
           "polls %lu; rtk app %lu oslog %lu%s\n",
           mb.kbd_reports, bad, mb.csum_errors, mb.ring_errors, mb.frames, mb.gpio_refused,
           poll_max_us, polls, n.app, n.oslog_notifications,
           latched ? "; LATCHED, keyboard released" : "");
}

void hv_mtp_t8152_fatal_report(void)
{
    if (!session_used)
        return;
    printf("HV: fatal: input: MTP state 0x%x reason %u live %d; kbd %u bad %u csum %u ring %u "
           "frames %u; svc max %lu us, polls %lu; area 0x%lx (stream 4 DMA, never guest RAM)\n",
           mb.state, mb.reason, (int)live, mb.kbd_reports, mb.kbd_bad, mb.csum_errors,
           mb.ring_errors, mb.frames, poll_max_us, polls, area_pa);
}
