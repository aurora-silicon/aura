/* SPDX-License-Identifier: MIT */
/*
 * Host driver for the G1 part of src/hv/windows/hv_gpu.c (POWER, the boot marker, PROBE, STOP),
 * compiled with HV_GPU_HOST_TEST against the stand-ins of windows_gpu_glue_stubs.h and the
 * hardware model of windows_gpu_hw_model.h.  tests/python/test_windows_t8152_gpu.py runs it with
 * the synthetic machine.  A "carrier reload" is hv_gpu_host_new_session() with the same ADT
 * bytes and the same heap pages (as an in-place chainload keeps them); a "reboot" reloads the
 * ADT file.
 *
 *   windows_gpu_probe_host ADT TEXT0_PHYS TEXT0 TEXT1_PHYS TEXT1 SGX SGX_SIZE MBOX POOL POOL_SIZE
 *                          RAM_BASE RAM_TOP WRAP0 WRAP1 VBAR0 HOLE
 */
#include "windows_gpu_glue_stubs.h"
#include "windows_gpu_hw_model.h"

#include "gpu_t8152_probe.h"
#include "gpu_t8152_uat.h"

static const char *adt_path;
static u64 sgx, sgx_size, mbox, pool, pool_size, wrap[2], vbar0, hole;
static bool allow_boot; /* G2: the boot-control register is part of the forwarded set */

/* Keep in step with machine() in tests/python/test_windows_t8152_gpu.py (aic, AON_X). */
#define AIC_BASE   0x280000000UL
#define AON_X_BASE 0x320000000UL

static void reboot(void)
{
    standin_load_adt(adt_path); /* iBoot's fresh ADT: both slots zero */
    reset_stubs();
    psd_model_reset();
    hw_model_reset();
    test_recipe.available = true;
    test_recipe.base = MODEL_PSD_BASE;
}

static void reload(void)
{
    reset_stubs(); /* hv_gpu_host_new_session(): a new carrier, same ADT and heap */
    psd_model_reset();
    for (int i = 0; i < 3; i++) /* the GPU stays as the last carrier left it */
        psdm[i].state = test_recipe.step[i].target;
    hw_model_reset();
}

static void slot(const char *name, u64 v[2], bool write)
{
    int mm = adt_path_offset(adt, "/chosen/memory-map");
    assert(mm >= 0);
    if (write) {
        assert(adt_setprop(adt, mm, name, v, 16) == 16);
    } else {
        u32 len;
        const void *p = adt_getprop(adt, mm, name, &len);
        assert(p && len == 16);
        memcpy(v, p, 16);
    }
}

static void admit(void)
{
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == 0);
}

static u64 hook_rw(u64 addr, u64 value, bool write, int width)
{
    u64 v[2] = {value, 0x5a5a};
    assert(hv_gpu_host_probe_hook(addr, v, write, width));
    return v[0];
}

/* The one rule: every hardware access the hooks made is in the forwarded set. */
static void check_hw_log(void)
{
    assert(hw_unexpected == 0);
    assert(cpu_control_writes == 0);
    for (int i = 0; i < hw_count && i < 512; i++) {
        struct hw_access *a = &hwlog[i];
        bool ok = false;
        for (int k = 0; k < 7; k++)
            ok |= a->addr == sgx + model_id_off[k] && !a->write && a->width == 2;
        for (int r = 0; r < 2; r++) {
            u64 w = mbox + 0x100 * (u64)(r + 1);
            ok |= (a->addr == wrap[r] + 0x44 || a->addr == wrap[r] + 0x48) && !a->write;
            ok |= a->addr == w + 0x20 && !a->write && a->width == 2;
            ok |= (a->addr == w || a->addr == w + 8) && !a->write;
            ok |= (a->addr == w + 0x10 || a->addr == w + 0x18) && a->write;
        }
        ok |= allow_boot && a->addr == sgx + MODEL_BOOT_OFF && a->width == 2;
        if (!ok) {
            printf("unexpected hardware access 0x%lx w%d %c\n", a->addr, a->width,
                   a->write ? 'W' : 'R');
            assert(0);
        }
    }
}

static const u8 *page(void)
{
    return hv_gpu_host_handoff();
}

static u32 page32(u32 off)
{
    u32 v;
    memcpy(&v, page() + off, 4);
    return v;
}

/* ---- G2 helpers --------------------------------------------------------------------------- */

/* A range of the class table, by class and tag (STATUS HV_GPU_S_RANGE). */
static u64 range_base(u32 cls, u32 tag)
{
    u64 n = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGES, 0);
    for (u64 i = 0; i < n; i++) {
        u64 k = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i + 2, 0);
        if ((k & 0xffff) == cls && (k >> 16) == tag)
            return (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i, 0);
    }
    assert(0 && "no such range");
    return 0;
}

static u64 st(u64 index)
{
    return (u64)op(HV_GPU_OP_STATUS, index, 0);
}

static u64 msgs(int role, int tx, u32 kind)
{
    return st(HV_GPU_S_MSG + 0x10 * (u64)(2 * role + tx) + kind);
}

/* One whole message to a GFX window; returns whether it reached the hardware. */
static bool send(int role, u64 msg, u64 ep)
{
    int before = tx_count;
    u64 v[2] = {msg, ep};
    assert(hv_gpu_host_probe_hook(mbox + 0x100 * (u64)(role + 1) + 0x10, v, true, 4));
    return tx_count == before + 1 && tx_log[before % 8][0] == msg;
}

static u32 g2_page_ranges(u64 *first_declared)
{
    u32 n = page32(GPU_HO_RANGE_COUNT);
    *first_declared = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGES, 0);
    return n;
}

static void g2_tests(void)
{
    const u64 grants = GPU_G2_GRANT_FW_MMIO | GPU_G2_GRANT_BOOT_CONTROL;

    /* ---- G2-1: order and state rules; a refusal spends nothing ---------------------------- */
    reboot();
    uat_mem_reset();
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_STATE); /* before ADMIT */
    assert(st(HV_GPU_S_G2_RECIPE) == 1);
    admit();
    assert(op(HV_GPU_OP_G2, 0x4, 0) == HV_GPU_E_G2); /* unknown grant */
    test_g2.available = false;
    assert(st(HV_GPU_S_G2_RECIPE) == 0);
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_G2);
    test_g2.available = true;
    test_g2.nwindows = 0; /* malformed */
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_G2 && st(HV_GPU_S_G2_RECIPE) == 0);
    test_g2.nwindows = 5;
    entered = true;
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_STATE);
    entered = false;
    assert(op(HV_GPU_OP_G2, 0, 0) == 0); /* not spent by the refusals */
    assert(st(HV_GPU_S_G2) == HV_GPU_G2_ARMED);
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_STATE); /* once */
    reboot();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0);
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_STATE); /* after POWER */
    reboot();
    admit();
    assert(op(HV_GPU_OP_HOOK, 0, 0) == 0);
    assert(op(HV_GPU_OP_G2, 0, 0) == HV_GPU_E_STATE); /* in a G0 log session */
    assert(psd_writes == 0 && boot_reads == 0 && boot_writes == 0);
    puts("G2 ORDER OK");

    /* ---- G2-2: fw-mmio declarations (G2-D1) ------------------------------------------------ */
    reboot();
    admit();
    u64 table_n;
    assert(g2_page_ranges(&table_n) == table_n); /* nothing declared before G2 */
    struct gpu_g2_window saved = test_g2.window[0];
    test_g2.window[0].offset = 0x100000; /* past the end of aic reg 0 */
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == HV_GPU_E_G2);
    test_g2.window[0] = saved;
    test_g2.window[0].node = "nope";
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == HV_GPU_E_G2);
    test_g2.window[0] = saved;
    saved = test_g2.window[1];
    test_g2.window[1].offset = wrap[0] - sgx; /* the CPU_CONTROL page of an ASC wrapper */
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == HV_GPU_E_G2);
    test_g2.window[1].offset = vbar0 - sgx; /* an iop-vbar page */
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == HV_GPU_E_G2);
    test_g2.window[1] = saved;
    saved = test_g2.window[4];
    test_g2.window[4].reg_last = 0; /* the window needs the hull of both regs */
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == HV_GPU_E_G2);
    test_g2.window[4] = saved;
    handoff_ok(GPU_HS_ADMITTED);
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_FW_MMIO, 0) == 0);
    assert(st(HV_GPU_S_DECLARED) == 5);
    assert(st(HV_GPU_S_G2) == (HV_GPU_G2_ARMED | HV_GPU_G2_FW_MMIO));
    handoff_ok(GPU_HS_ADMITTED);
    assert(g2_page_ranges(&table_n) == table_n + 5);
    static const u32 tags[5] = {2, 5, 6, 7, 8};
    for (u32 i = 0; i < 5; i++) {
        const u8 *e = page() + GPU_HANDOFF_HEADER + (table_n + i) * GPU_HANDOFF_RANGE;
        u64 base, size;
        u32 cls, tag, flags;
        memcpy(&base, e, 8);
        memcpy(&size, e + 8, 8);
        memcpy(&cls, e + 16, 4);
        memcpy(&tag, e + 20, 4);
        memcpy(&flags, e + 24, 4);
        assert(cls == GPU_C_FW_MMIO && tag == tags[i]);
        assert(flags == (GPU_RF_DECLARED | (test_g2.window[i].writable ? 0 : GPU_RF_READ_ONLY)));
        assert(size == test_g2.window[i].size);
        if (i == 0)
            assert(base == AIC_BASE + 0x8000);
        if (i == 2)
            assert(base == sgx + 0x1800);
        if (i == 4)
            assert(base == AON_X_BASE && size == 0x14000);
    }
    reboot();
    admit();
    assert(op(HV_GPU_OP_G2, 0, 0) == 0 && st(HV_GPU_S_DECLARED) == 0);
    assert(g2_page_ranges(&table_n) == table_n); /* not granted: nothing declared */
    puts("G2 DECLARE OK");

    /* ---- G2-3: the boot-control step (G2-D2) ----------------------------------------------- */
    reboot();
    admit();
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_BOOT_CONTROL, 0) == 0);
    assert(boot_reads == 0 && boot_writes == 0); /* nothing before POWER */
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0);
    assert(boot_reads == 2 && boot_writes == 1 && model_boot == MODEL_BOOT_EXPECT);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED | GPU_HS_SGX_BOOT);
    assert(page32(GPU_HO_BOOT_CONTROL) == MODEL_BOOT_EXPECT);
    const struct gpu_boot_marker *m = hv_gpu_host_marker();
    assert(gpu_t8152_marker_valid(m) && (m->state & GPU_BM_BOOT_CONTROL) && (m->state & GPU_BM_G2));
    assert(m->boot_control_before == MODEL_BOOT_COLD && m->boot_control_after == MODEL_BOOT_EXPECT);
    assert(st(HV_GPU_S_BOOT_CONTROL) == MODEL_BOOT_COLD &&
           st(HV_GPU_S_BOOT_CONTROL + 1) == MODEL_BOOT_EXPECT);
    assert(st(HV_GPU_S_G2) & HV_GPU_G2_BOOT_DONE);
    /* A value the recipe does not accept: nothing written, POWER reports it, GPU stays on. */
    reboot();
    model_boot = 0x123;
    admit();
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_BOOT_CONTROL, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT_CONTROL);
    assert(boot_reads == 1 && boot_writes == 0 && psd_all_on());
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED);
    assert(page32(GPU_HO_BOOT_CONTROL) == 0);
    m = hv_gpu_host_marker();
    assert((m->state & GPU_BM_GUARD) && !(m->state & GPU_BM_BOOT_CONTROL));
    /* The wrong value reads back: reported, not retried. */
    reboot();
    model_boot_readback = MODEL_BOOT_COLD;
    admit();
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_BOOT_CONTROL, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT_CONTROL);
    assert(boot_writes == 1 && boot_reads == 2);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED);
    /* A register outside a pmgr hole is refused at G2, before anything is touched. */
    reboot();
    admit();
    test_g2.boot_offset = 0x100000;
    assert(op(HV_GPU_OP_G2, GPU_G2_GRANT_BOOT_CONTROL, 0) == HV_GPU_E_G2);
    test_g2.boot_offset = MODEL_BOOT_OFF;
    /* Not granted (G2 or G1): never read, never written. */
    assert(op(HV_GPU_OP_G2, 0, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && boot_reads == 0 && boot_writes == 0);
    reboot();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && boot_reads == 0 && boot_writes == 0);
    puts("G2 BOOT OK");

    /* ---- G2-4/5: message guards and the UAT audit ------------------------------------------- */
    reboot();
    uat_mem_reset();
    admit();
    u64 ctx = range_base(GPU_C_FW_RW, GPU_R_TTBS);
    u64 pt = range_base(GPU_C_FW_RW, GPU_R_PAGETABLES);
    u64 l2r = range_base(GPU_C_FW_RW, GPU_R_SHARED_L2);
    u64 data = range_base(GPU_C_FW_RO, GPU_R_DATA);
    u64 text = range_base(GPU_C_FW_TEXT, 0);
    /* Before the guest: the firmware's own GFX1 root maps one MMIO page (a baseline outlier). */
    uat_put(pt + 0x40000, 2, (pt + 0x8000) | 3);
    uat_put(pt + 0x8000, 0, (pt + 0xc000) | 3);
    uat_put(pt + 0xc000, 0, 0x3500000000UL | 0x403);
    assert(op(HV_GPU_OP_G2, grants, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && op(HV_GPU_OP_PROBE, 0, 0) == 0);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_AUDITS) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_BASELINE) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_ROOTS) == 1 && st(HV_GPU_S_AUDIT + HV_GPU_A_TABLES) == 3);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED | GPU_HS_SGX_BOOT | GPU_HS_ASC_RUN |
               GPU_HS_MMIO_PASS | GPU_HS_FW_MAPPED);
    int hw0 = hw_count;

    /* Whole messages only: the 64-bit halves never reach the hardware. */
    hook_rw(mbox + 0x110, 0x1, true, 3);
    hook_rw(mbox + 0x118, 0x0, true, 3);
    assert(hook_rw(mbox + 0x200, 0, false, 3) == 0);
    assert(hw_count == hw0 && st(HV_GPU_S_GUARD + HV_GPU_G_HALF) == 3);
    /* Management and RX traffic: forwarded and counted by kind. */
    assert(send(1, 0x0020000000000c0cUL, 0x0));
    assert(msgs(1, 1, GPU_MK_MGMT) == 1);
    rx_push(0, 0x0009000000000000UL, 0x20);
    u64 r128[2] = {0, 0};
    assert(hv_gpu_host_probe_hook(mbox + 0x100, r128, false, 4) && r128[0] == 0x0009000000000000UL);
    assert(msgs(0, 0, GPU_MK_INIT_ACK) == 1);
    /* sgx is the guest's until init data goes out. */
    assert(hook_rw(sgx + model_id_off[0], 0, false, 2) == model_id[0]);

    /* The guest builds its tables: context 0, a kernel-low VM in the pool. */
    u64 p0 = pool, p1 = pool + 0x4000, p2 = pool + 0x8000;
    uat_put(ctx, 0, p0 | 1);
    uat_put(ctx, 1, pt | 1);
    uat_put(p0, 7, p1 | 3);
    uat_put(p1, 0, p2 | 3);
    uat_put(p2, 0, (pool + 0x100000) | 0x403);
    uat_put(p2, 1, l2r | 0x403);
    uat_put(p2, 2, data | 0x403);
    uat_put(p2, 3, (AIC_BASE + 0x8000) | 0x403); /* a declared window */
    uat_put(p1, 1, (pool + 0x2000000) | 0x401);  /* a 32 MiB block inside the pool */
    /* A fresh outlier in a guest table: init data is refused, so is a doorbell. */
    uat_put(p2, 4, text | 0x403);
    assert(!send(0, 0x00810c20001a8000UL, 0x20));
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_AUDIT) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1 && st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    assert(st(HV_GPU_S_AUDIT_FRESH) == (p2 | 4) && st(HV_GPU_S_AUDIT_FRESH + 1) == (text | 0x403));
    assert((st(HV_GPU_S_AUDIT_FRESH + 2) & 0xff) == GPU_UC_TEXT);
    assert(!send(0, 0x0089000000000000UL, 0x21));
    assert(!(st(HV_GPU_S_G2) & HV_GPU_G2_SGX_LOCKED));
    uat_put(p2, 4, 0);
    /* Clean: init data to GFX goes out; sgx is locked from then on. */
    assert(send(0, 0x00810c20001a8000UL, 0x20));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 0 && st(HV_GPU_S_AUDIT + HV_GPU_A_KNOWN) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_LEAVES + GPU_UC_POOL) == 2);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_LEAVES + GPU_UC_UAT) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_LEAVES + GPU_UC_DATA) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_LEAVES + GPU_UC_FW_MMIO) == 1);
    assert(st(HV_GPU_S_G2) & (HV_GPU_G2_SGX_LOCKED | HV_GPU_G2_INIT_GFX));
    assert(msgs(0, 1, GPU_MK_INIT) == 1);
    hw0 = hw_count;
    assert(hook_rw(sgx + model_id_off[0], 0, false, 2) == 0);
    assert(hook_rw(wrap[0] + 0x44, 0, false, 2) == 0);
    hook_rw(wrap[0] + 0x44, 0x10, true, 2);
    assert(hw_count == hw0 && st(HV_GPU_S_GUARD + HV_GPU_G_LOCKED) == 3);
    /* One init-data message per role. */
    assert(!send(0, 0x00810c20001a8000UL, 0x20));
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_INIT_AGAIN) == 1);
    assert(send(1, 0x00810c20001b0000UL, 0x20));
    assert(st(HV_GPU_S_G2) & HV_GPU_G2_INIT_GFX1);
    assert(!send(1, 0x00810c20001b0000UL, 0x20));
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_INIT_AGAIN) == 2);
    /* The recipe's refused type never goes out; the post-init note does. */
    assert(!send(0, (u64)MODEL_REFUSED_T << 48, MODEL_REFUSED_EP));
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_REFUSED) == 1);
    /* No IOP power state but on (RTKit management type 6): sleep or off, then a wake, would be a
     * second firmware start.  On itself goes out. */
    assert(!send(1, 0x0060000000000010UL, 0x0) && !send(0, 0x0060000000000000UL, 0x0));
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_REFUSED) == 3);
    assert(send(1, 0x0060000000000020UL, 0x0));
    assert(send(0, 0x0089000000000000UL, 0x21) && send(1, 0x0089000000000000UL, 0x21));
    assert(msgs(0, 1, GPU_MK_POWER_END) == 1 && msgs(1, 1, GPU_MK_POWER_END) == 1);
    /* A fresh outlier in a firmware table: the page-table region is mapped RW for the guest, so
     * it may be the guest's: the doorbell is refused until it goes. */
    uat_put(pt + 0xc000, 1, 0x3500004000UL | 0x403);
    assert(!send(0, 0x0084000000000011UL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    assert(st(HV_GPU_S_AUDIT_FRESH) == ((pt + 0xc000) | 1));
    assert((st(HV_GPU_S_AUDIT_FRESH + 2) >> 16) == 1);
    uat_put(pt + 0xc000, 1, 0);
    assert(send(0, 0x0084000000000011UL, 0x21));
    assert(msgs(0, 1, GPU_MK_FWCTL) == 1);
    /* A context root pointing at a guest-built table in a spare page of the page-table region. */
    uat_put(ctx, 4, (pt + 0x20000) | 1);
    uat_put(pt + 0x20000, 0, (pt + 0x24000) | 3);
    uat_put(pt + 0x24000, 0, (pt + 0x28000) | 3);
    uat_put(pt + 0x28000, 0, 0x3500008000UL | 0x403);
    assert(!send(0, 0x008300000000000aUL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    uat_put(ctx, 4, 0);
    /* One pool page as an L3 table under context 0 and as the L2 table of context 6: its page
     * leaves would be the hardware's L3 tables on the second path, which the walk never reads. */
    uat_put(p2, 6, (pool + 0x6c000) | 0x403);   /* a pool leaf at L3 ... */
    uat_put(pool + 0x6c000, 0, text | 0x403);   /* ... and, read as an L3 table, a text leaf */
    uat_put(ctx, 6, (pool + 0x70000) | 1);
    uat_put(pool + 0x70000, 0, p2 | 3);         /* p2 again, now at L2 */
    assert(!send(0, 0x008300000000000aUL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    assert((st(HV_GPU_S_AUDIT_FRESH + 2) & 0xff) == GPU_UC_BAD);
    assert(st(HV_GPU_S_AUDIT_FRESH) == ((pool + 0x70000) | 0));
    uat_put(ctx, 6, 0);
    uat_put(p2, 6, 0);
    /* A guest table pointing outside the table regions, or at a page that cannot be read. */
    uat_put(p1, 2, text | 3);
    assert(!send(0, 0x008300000000000aUL, 0x21));
    assert((st(HV_GPU_S_AUDIT_FRESH + 2) & 0xff) == GPU_UC_TABLE ||
           (st(HV_GPU_S_AUDIT_FRESH + 6) & 0xff) == GPU_UC_TABLE);
    uat_put(p1, 2, 0);
    uat_unreadable = pool + 0x20000;
    uat_put(p1, 3, (pool + 0x20000) | 3);
    assert(!send(0, 0x008300000000000aUL, 0x21));
    uat_put(p1, 3, 0);
    /* A malformed descriptor (a block at the last level). */
    uat_put(p2, 5, (pool + 0x200000) | 0x401);
    assert(!send(0, 0x008300000000000aUL, 0x21));
    uat_put(p2, 5, 0);
    assert(send(0, 0x008300000000000aUL, 0x21));
    assert(msgs(0, 1, GPU_MK_DOORBELL) == 1);
    assert(st(HV_GPU_S_GUARD + HV_GPU_G_AUDIT) == 8);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_MAX) >= 1);

    /* The access map shows the new dispositions. */
    u64 count = st(HV_GPU_S_MAP_COUNT);
    bool saw_locked = false, saw_blocked = false;
    for (u64 i = 0; i < count; i++) {
        u64 a = st(HV_GPU_S_MAP + 4 * i), k = st(HV_GPU_S_MAP + 4 * i + 2);
        u32 disp = (k >> 16) & 0xff;
        saw_locked |= a == sgx + model_id_off[0] && disp == GPU_A_LOCKED;
        saw_blocked |= a == mbox + 0x110 && disp == GPU_A_BLOCKED && (k & 0xff) == 4;
    }
    assert(saw_locked && saw_blocked);
    allow_boot = true; /* this session's boot-control step (G2-D2 granted) */
    check_hw_log();
    allow_boot = false;

    /* STOP: the teardown audit sees a firmware-table outlier left behind; the marker says so. */
    uat_put(pt + 0xc000, 1, 0x3500004000UL | 0x403);
    assert(op(HV_GPU_OP_STOP, 0, 0) == 0);
    m = hv_gpu_host_marker();
    assert((m->state & GPU_BM_ENDED) && (m->state & GPU_BM_GUARD) && (m->state & GPU_BM_G2));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1);
    assert(m->g2 == (HV_GPU_G2_ARMED | grants << 1));
    puts("G2 GUARD OK");

    /* ---- G2-6: a G1 probe session keeps its halves and records the audit only -------------- */
    reboot();
    uat_mem_reset();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && op(HV_GPU_OP_PROBE, 0, 0) == 0);
    hw0 = hw_count;
    hook_rw(mbox + 0x110, 0x1, true, 3);
    assert(hw_count == hw0 + 1 && st(HV_GPU_S_GUARD + HV_GPU_G_HALF) == 0);
    uat_put(range_base(GPU_C_FW_RW, GPU_R_TTBS), 0, pool | 1);
    uat_put(pool, 7, text | 3);
    assert(send(0, 0x00810c20001a8000UL, 0x20)); /* G1 has no guards */
    assert(st(HV_GPU_S_G2) == 0 && st(HV_GPU_S_AUDIT + HV_GPU_A_AUDITS) == 1);
    assert(op(HV_GPU_OP_STOP, 0, 0) == 0);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_AUDITS) == 2 && st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1);
    puts("G2 G1-SESSION OK");

    /* ---- G2-7: a large firmware baseline, and a stale guest-table entry at PROBE ------------ */
    reboot();
    uat_mem_reset();
    admit();
    pt = range_base(GPU_C_FW_RW, GPU_R_PAGETABLES);
    ctx = range_base(GPU_C_FW_RW, GPU_R_TTBS);
    /* The firmware's GFX1 root maps 3,000 MMIO pages through two of its own L3 tables. */
    uat_put(pt + 0x40000, 2, (pt + 0x8000) | 3);
    uat_put(pt + 0x8000, 0, (pt + 0xc000) | 3);
    uat_put(pt + 0x8000, 1, (pt + 0x10000) | 3);
    for (u32 i = 0; i < 3000; i++)
        uat_put(pt + (i < 2048 ? 0xc000 : 0x10000), i % 2048,
                (0x3500000000UL + 0x4000UL * (2999 - i)) | 0x403);
    /* A stale context-table root left in the pool by an earlier boot, pointing at text. */
    uat_put(ctx, 10, (pool + 0x40000) | 1);
    uat_put(pool + 0x40000, 0, text | 3);
    assert(op(HV_GPU_OP_G2, 0, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && op(HV_GPU_OP_PROBE, 0, 0) == 0);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_BASELINE) == 3000);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_BASELINE_OVF) == 0);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_KNOWN) == 3000 && st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1);
    /* The stale guest entry is never inherited: init data waits until the guest clears it. */
    assert(!send(0, 0x00810c20001a8000UL, 0x20));
    uat_put(ctx, 10, 0);
    assert(send(0, 0x00810c20001a8000UL, 0x20));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_KNOWN) == 3000 && st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 0);
    /* One firmware leaf changes: found among the 3,000 known ones; the table is in a region the
     * guest can write, so the message waits until it is back. */
    uat_put(pt + 0x10000, 100, 0x3600000000UL | 0x403);
    assert(!send(0, 0x0089000000000000UL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 1 && st(HV_GPU_S_AUDIT + HV_GPU_A_KNOWN) == 2999);
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    assert(st(HV_GPU_S_AUDIT_FRESH) == ((pt + 0x10000) | 100));
    uat_put(pt + 0x10000, 100, (0x3500000000UL + 0x4000UL * (2999 - 2148)) | 0x403);
    assert(send(0, 0x0089000000000000UL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH) == 0 && st(HV_GPU_S_AUDIT + HV_GPU_A_KNOWN) == 3000);
    /* The host-owned shared-L2 table: what it held at PROBE is known, anything new refuses. */
    l2r = range_base(GPU_C_FW_RW, GPU_R_SHARED_L2);
    uat_put(ctx, 1, pt | 1);
    uat_put(pt, 2, l2r | 3);
    assert(send(0, 0x0084000000000011UL, 0x21));
    uat_put(l2r, 7, 0x3700000000UL | 0x401); /* a 32 MiB block of MMIO */
    assert(!send(0, 0x008300000000000aUL, 0x21));
    assert(st(HV_GPU_S_AUDIT + HV_GPU_A_FRESH_POOL) == 1);
    assert((st(HV_GPU_S_AUDIT_FRESH + 2) >> 16) == 1 || (st(HV_GPU_S_AUDIT_FRESH + 6) >> 16) == 1);
    uat_put(l2r, 7, 0);
    assert(send(0, 0x008300000000000aUL, 0x21));
    assert(op(HV_GPU_OP_STOP, 0, 0) == 0);
    puts("G2 BASELINE OK");
}

int main(int argc, char **argv)
{
    assert(argc == 17);
    adt_path = argv[1];
    for (int i = 0; i < 2; i++) {
        texts[i].phys = strtoull(argv[2 + 2 * i], NULL, 0);
        texts[i].data = slurp(argv[3 + 2 * i], &texts[i].size);
    }
    sgx = strtoull(argv[6], NULL, 0);
    sgx_size = strtoull(argv[7], NULL, 0);
    mbox = strtoull(argv[8], NULL, 0);
    pool = strtoull(argv[9], NULL, 0);
    pool_size = strtoull(argv[10], NULL, 0);
    ram_base = strtoull(argv[11], NULL, 0);
    ram_top = strtoull(argv[12], NULL, 0);
    wrap[0] = strtoull(argv[13], NULL, 0);
    wrap[1] = strtoull(argv[14], NULL, 0);
    vbar0 = strtoull(argv[15], NULL, 0);
    hole = strtoull(argv[16], NULL, 0);
    cur_boot_args.phys_base = ram_base;
    cur_boot_args.mem_size = ram_top - ram_base;
    model_sgx = sgx;
    model_wrap[0] = wrap[0];
    model_wrap[1] = wrap[1];
    model_mbox = mbox;

    /* ---- G1-1: order and state rules ---------------------------------------------------- */
    reboot();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_STATE); /* before ADMIT */
    assert(op(HV_GPU_OP_PROBE, 0, 0) == HV_GPU_E_STATE);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_POWER_RECIPE, 0) == 1);
    admit();
    assert(op(HV_GPU_OP_PROBE, 0, 0) == HV_GPU_E_STATE); /* before POWER */
    entered = true;
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_STATE);
    entered = false;
    assert(psd_reads == 0 && psd_writes == 0);
    /* G0 logging hooks and G1 exclude each other. */
    assert(op(HV_GPU_OP_HOOK, 0, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_STATE);
    assert(psd_reads == 0 && psd_writes == 0);
    puts("G1 ORDER OK");

    /* ---- G1-2: the happy path ----------------------------------------------------------- */
    reboot();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0);
    /* Exactly one request per service, in recipe order, with the recipe's values. */
    assert(psd_writes == 3);
    for (int i = 0; i < 3; i++)
        assert(psd_wlog[i].service == test_recipe.step[i].service &&
               psd_wlog[i].value == test_recipe.step[i].request);
    assert(psd_all_on());
    assert(psd_delay_us > 0 && psd_delay_us <= 3 * 3 * test_recipe.poll_us);
    /* The marker: claimed in the ADT, page reserved, pool reserved, record valid. */
    u64 v[2];
    slot(HV_GPU_BOOT_MARKER, v, false);
    const struct gpu_boot_marker *m = hv_gpu_host_marker();
    assert(m && v[0] == (u64)m && v[1] == GPU_BOOT_MARKER_SIZE);
    assert(reserve_calls == 1 && reserve_start == v[0] && reserve_end == v[0] + 0x4000);
    slot(HV_GPU_POOL_SLOT, v, false);
    assert(v[0] == pool && v[1] == pool_size);
    assert(gpu_t8152_marker_valid(m) && m->state == (GPU_BM_CLAIMED | GPU_BM_POWERED));
    assert(m->pool_base == pool && m->pool_size == pool_size);
    for (int i = 0; i < 3; i++) {
        assert((m->psd_before[i] & 0xff) == test_recipe.cold_idle);
        assert((m->psd_after[i] & 0xff) == test_recipe.step[i].target);
        assert(m->psd_after[i] >> 16 == test_recipe.step[i].service);
    }
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == (GPU_BM_CLAIMED | GPU_BM_POWERED));
    /* The identity reads went to the hardware and into the handoff page. */
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED);
    for (int i = 0; i < 7; i++) {
        assert(page32(GPU_HO_GPU_ID + 4 * i) == model_id[i]);
        assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_GPU_ID + i, 0) == model_id[i]);
    }
    for (int r = 0; r < 2; r++) {
        assert(page32(GPU_HO_CPU_CONTROL + 4 * r) == 0x10);
        assert(page32(GPU_HO_CPU_STATUS + 4 * r) == model_cpu_status[r]);
    }
    check_hw_log();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_STATE); /* once per session */
    assert(psd_writes == 3);

    /* PROBE: hooks on exactly the window and the page, firmware regions mapped. */
    int hooks_before = hook_calls;
    assert(op(HV_GPU_OP_PROBE, 0, 0) == 0);
    assert(hook_calls == hooks_before + 2);
    assert(hooks[hooks_before].from == sgx && hooks[hooks_before].size == sgx_size);
    assert(hooks[hooks_before + 1].from == mbox && hooks[hooks_before + 1].size == 0x4000);
    assert(fwmap_calls == 8);
    u64 n = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGES, 0);
    int rw = 0, ro = 0;
    for (u64 i = 0; i < n; i++) {
        u64 base = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i, 0);
        u64 size = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i + 1, 0);
        u64 cls = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i + 2, 0) & 0xffff;
        u64 flags = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * i + 3, 0);
        if (cls != GPU_C_FW_RW && cls != GPU_C_FW_RO) {
            assert(!(flags & GPU_RF_MAPPED) || cls == GPU_C_HANDOFF);
            continue;
        }
        bool found = false;
        for (int k = 0; k < fwmap_calls; k++)
            if (fwmaps[k].from == base && fwmaps[k].to == base && fwmaps[k].size == size &&
                fwmaps[k].ro == (cls == GPU_C_FW_RO))
                found = true;
        assert(found && (flags & GPU_RF_MAPPED));
        rw += cls == GPU_C_FW_RW;
        ro += cls == GPU_C_FW_RO;
    }
    assert(rw == 4 && ro == 4);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED | GPU_HS_ASC_RUN | GPU_HS_MMIO_PASS |
               GPU_HS_FW_MAPPED);
    assert(hv_gpu_host_marker() == m && (m->state & GPU_BM_PROBE));
    int hw0 = hw_count;

    /* Forwarded: the ID words and the core mask, 32-bit reads only. */
    for (int i = 0; i < 7; i++)
        assert(hook_rw(sgx + model_id_off[i], 0, false, 2) == model_id[i]);
    assert(hw_count == hw0 + 7);
    /* Denied, hardware untouched: unlisted ID offsets, other widths, writes, holes, iop-vbar,
     * the rest of the wrapper, CPU_STATUS writes, other post-office windows, RX writes, TX
     * reads, misaligned accesses. */
    hw0 = hw_count;
    assert(hook_rw(sgx + 0xd04004, 0, false, 2) == 0);
    assert(hook_rw(sgx + 0xd04000, 0, false, 3) == 0);
    assert(hook_rw(sgx + 0xd04000, 0x1, true, 2) == 0x1);
    assert(hook_rw(sgx + 0xd28000, 0, false, 3) == 0); /* G2 territory */
    assert(hook_rw(hole, 0, false, 2) == 0);
    assert(hook_rw(hole, 0x3, true, 2) == 0x3);
    assert(hook_rw(vbar0, 0, false, 3) == 0);
    assert(hook_rw(wrap[0] + 0x4000, 0, false, 2) == 0);
    assert(hook_rw(wrap[0] + 0x40, 0, false, 2) == 0);
    assert(hook_rw(wrap[0] + 0x48, 0x1, true, 2) == 0x1);
    assert(hook_rw(wrap[1] + 0x44, 0, false, 3) == 0);
    assert(hook_rw(mbox + 0x300, 0, false, 2) == 0);   /* another IOP's window */
    assert(hook_rw(mbox + 0x320, 0, false, 2) == 0);
    assert(hook_rw(mbox + 0x110, 0, false, 4) == 0);   /* TX read */
    assert(hook_rw(mbox + 0x100, 0x7, true, 4) == 0x7); /* RX write */
    assert(hook_rw(mbox + 0x124, 0, false, 2) == 0);
    assert(hook_rw(mbox + 0x180, 0, false, 2) == 0);   /* past the 0x80 window */
    assert(hook_rw(sgx + 0xd04002, 0, false, 1) == 0); /* misaligned within an ID word */
    assert(hw_count == hw0);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_DENIED, 0) == 18);

    /* CPU_CONTROL: reads forwarded; a write of the held value with RUN set is accepted and not
     * performed; any other write (clear RUN, toggle, extra bits) is refused and not performed. */
    assert(hook_rw(wrap[0] + 0x44, 0, false, 2) == 0x10);
    assert(hook_rw(wrap[1] + 0x48, 0, false, 2) == model_cpu_status[1]);
    hook_rw(wrap[0] + 0x44, 0x10, true, 2);
    hook_rw(wrap[0] + 0x44, 0x0, true, 2);
    hook_rw(wrap[1] + 0x44, 0x11, true, 2);
    hook_rw(wrap[1] + 0x44, 0x2, true, 2);
    assert(cpu_control_writes == 0 && model_cpu_control[0] == 0x10 &&
           model_cpu_control[1] == 0x10);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RUN_REFUSED, 0) == 3);
    /* If RUN were clear, even rewriting the held value is refused (the carrier never sets it). */
    model_cpu_control[1] = 0x0;
    hook_rw(wrap[1] + 0x44, 0x0, true, 2);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RUN_REFUSED, 0) == 4);
    model_cpu_control[1] = 0x10;

    /* The GFX windows: status, RX (128-bit or two halves), TX (128-bit or two halves). */
    rx_push(1, 0x0010000000000c0cUL, 0x0); /* a HELLO-shaped management message on GFX1 */
    rx_push(1, 0x0080000000000003UL, 0x0);
    assert(hook_rw(mbox + 0x220, 0, false, 2) == 2);
    u64 r128[2] = {0, 0};
    assert(hv_gpu_host_probe_hook(mbox + 0x200, r128, false, 4));
    assert(r128[0] == 0x0010000000000c0cUL && r128[1] == 0);
    assert(hook_rw(mbox + 0x200, 0, false, 3) == 0x0080000000000003UL);
    assert(hook_rw(mbox + 0x208, 0, false, 3) == 0);
    u64 t128[2] = {0x0020000000000c0cUL, 0x0};
    assert(hv_gpu_host_probe_hook(mbox + 0x210, t128, true, 4));
    assert(tx_count == 1 && tx_log[0][0] == 0x0020000000000c0cUL);
    hook_rw(mbox + 0x110, 0x1, true, 3);
    hook_rw(mbox + 0x118, 0x0, true, 3);
    assert(hook_rw(mbox + 0x120, 0, false, 2) == 0);
    check_hw_log();

    /* A forwarded access that faults reads as zero and is counted. */
    fault_addr = sgx + model_id_off[0];
    assert(hook_rw(sgx + model_id_off[0], 0, false, 2) == 0);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_FAULTS, 0) == 1);
    fault_addr = 0;

    /* The access map: one entry per (address, width, direction, outcome), exported by STATUS. */
    u64 count = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_MAP_COUNT, 0);
    assert(count > 20 && count < GPU_ACCESS_MAP_MAX);
    bool saw_id = false, saw_refused = false, saw_hole = false;
    for (u64 i = 0; i < count; i++) {
        u64 a = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_MAP + 4 * i, 0);
        u64 c = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_MAP + 4 * i + 1, 0);
        u64 k = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_MAP + 4 * i + 2, 0);
        u32 disp = (k >> 16) & 0xff, cls = (u32)(k >> 24);
        if (a == sgx + 0xd04000 && disp == GPU_A_PASSED && (k & 0xff) == 2) {
            saw_id = true;
            assert(c == 1 && cls == GPU_C_SGX);
        }
        if (a == wrap[0] + 0x44 && disp == GPU_A_REFUSED)
            saw_refused = (k >> 8 & 1) && cls == GPU_C_ASC_CTRL;
        if (a == hole && disp == GPU_A_DENIED)
            saw_hole = cls == GPU_C_SGX_HOLE;
    }
    assert(saw_id && saw_refused && saw_hole);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_MAP + 4 * count, 0) == HV_GPU_ERROR);

    /* STOP: forwarding off, firmware regions unmapped, marker ENDED; the GPU is not touched. */
    int writes = psd_writes, reads = psd_reads, unm = unmap_calls;
    assert(op(HV_GPU_OP_STOP, 0, 0) == 0);
    assert(unmap_calls == unm + 8);
    assert(psd_writes == writes && psd_reads == reads && cpu_control_writes == 0);
    assert(m->state & GPU_BM_ENDED && gpu_t8152_marker_valid(m));
    assert(m->forwarded > 0 && m->denied >= 18 && m->run_refused == 4 && m->faults == 1);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_POWERED | GPU_HS_ASC_RUN);
    hw0 = hw_count;
    assert(hook_rw(sgx + model_id_off[0], 0, false, 2) == 0); /* denied now */
    assert(hw_count == hw0);
    assert(op(HV_GPU_OP_STOP, 0, 0) == 0 && unmap_calls == unm + 8); /* idempotent */
    hv_gpu_teardown("hv_start after STOP");
    assert(unmap_calls == unm + 8);
    puts("G1 PROBE OK");

    /* ---- G1-3: a carrier reload in the same boot cannot power the GPU again ---------------- */
    reload();
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) ==
           (GPU_BM_CLAIMED | GPU_BM_POWERED | GPU_BM_PROBE | GPU_BM_ENDED));
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    assert(psd_writes == 0 && psd_reads == 0); /* refused before any PMGR access */
    assert(op(HV_GPU_OP_PROBE, 0, 0) == HV_GPU_E_STATE);
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_STATE); /* and once per session */
    /* G0 modes still work in that session (they never touch the GPU). */
    reload();
    admit();
    assert(op(HV_GPU_OP_HOOK, 0, 0) == 0);
    /* The old marker page overwritten (a bigger carrier image): the slot still forbids. */
    reload();
    slot(HV_GPU_BOOT_MARKER, v, false);
    memset((void *)v[0], 0xa5, 64);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == HV_GPU_BOOT_FOREIGN);
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    assert(psd_writes == 0 && psd_reads == 0);
    puts("G1 RELOAD OK");

    /* ---- G1-4: a reboot clears it ------------------------------------------------------- */
    reboot();
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == 0);
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0);
    puts("G1 REBOOT OK");

    /* ---- G1-5: a set slot that is not ours, and a missing slot --------------------------- */
    reboot();
    v[0] = ram_base + 0x1000;
    v[1] = 0x4000;
    slot(HV_GPU_BOOT_MARKER, v, true);
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    assert(psd_writes == 0 && psd_reads == 0);
    reboot();
    v[0] = pool;
    v[1] = 0x100;
    slot(HV_GPU_POOL_SLOT, v, true); /* the pool slot is taken: nothing claimed, nothing on */
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    assert(psd_writes == 0);
    slot(HV_GPU_BOOT_MARKER, v, false);
    assert(!v[0] && !v[1]);
    puts("G1 SLOT OK");

    /* ---- G1-6: a service already on without a marker ------------------------------------ */
    reboot();
    psdm[1].state = test_recipe.step[1].target;
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    assert(psd_writes == 0);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == (GPU_BM_CLAIMED | GPU_BM_FOREIGN));
    puts("G1 FOREIGN OK");

    /* ---- G1-7: no acknowledge, or the wrong state: stop there, power nothing off --------- */
    reboot();
    psdm[1].never_ack = true;
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_POWER);
    assert(psd_writes == 2 && psd_wlog[1].service == test_recipe.step[1].service);
    assert(psd_delay_us >= (u64)test_recipe.polls * test_recipe.poll_us);
    assert(psdm[0].state == test_recipe.step[0].target && psdm[2].state == test_recipe.cold_idle);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) ==
           (GPU_BM_CLAIMED | GPU_BM_POWER_FAILED));
    assert(op(HV_GPU_OP_PROBE, 0, 0) == HV_GPU_E_STATE);
    reload();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_BOOT);
    reboot();
    psdm[2].ack_state = 9;
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_POWER);
    assert(psd_writes == 3);
    puts("G1 FAIL OK");

    /* ---- G1-8: no recipe, or a block outside every pmgr range: nothing touched or spent -- */
    reboot();
    test_recipe.available = false;
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_POWER_RECIPE, 0) == 0);
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_RECIPE);
    test_recipe.available = true;
    test_recipe.base = MODEL_PSD_BASE + 0x100000;
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_RECIPE);
    test_recipe.base = MODEL_PSD_BASE;
    test_recipe.read_order = 0; /* the read order is recipe data: a recipe without it is malformed */
    assert(op(HV_GPU_OP_POWER, 0, 0) == HV_GPU_E_RECIPE);
    test_recipe.read_order = GPU_PSD_READ_ACK_FIRST;
    assert(psd_reads == 0 && psd_writes == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0); /* not spent: the fixed recipe goes through */
    /* A sample reads the two registers in the recipe's order, whichever it is. */
    {
        static const struct gpu_psd_ops mops = {hv_gpu_host_read32, hv_gpu_host_write32,
                                                hv_gpu_host_delay};
        struct gpu_psd_sample smp[GPU_PSD_MAX_STEPS];
        psd_model_reset();
        gpu_t8152_psd_sample(&test_recipe, &mops, smp);
        assert(psd_reads == 6 && psd_rlog[0] == test_recipe.ack_off &&
               psd_rlog[1] == test_recipe.state_off);
        psd_model_reset();
        test_recipe.read_order = GPU_PSD_READ_STATE_FIRST;
        gpu_t8152_psd_sample(&test_recipe, &mops, smp);
        assert(psd_reads == 6 && psd_rlog[0] == test_recipe.state_off &&
               psd_rlog[1] == test_recipe.ack_off && smp[0].state == test_recipe.cold_idle);
        test_recipe.read_order = GPU_PSD_READ_ACK_FIRST;
    }
    puts("G1 RECIPE OK");

    /* ---- G1-9: the reset path stops forwarding without touching the GPU ------------------ */
    reboot();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0 && op(HV_GPU_OP_PROBE, 0, 0) == 0);
    assert(hook_rw(sgx + model_id_off[6], 0, false, 2) == model_id[6]);
    writes = psd_writes;
    hv_gpu_emergency_stop();
    hw0 = hw_count;
    assert(hook_rw(sgx + model_id_off[6], 0, false, 2) == 0 && hw_count == hw0);
    assert(psd_writes == writes && cpu_control_writes == 0);
    /* A failed firmware map leaves nothing mapped and no forwarding. */
    reboot();
    admit();
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0);
    fwmap_fail_at = 5;
    unm = unmap_calls;
    assert(op(HV_GPU_OP_PROBE, 0, 0) == HV_GPU_E_POLICY);
    assert(unmap_calls == unm + 6); /* the failed one and the five before it */
    hw0 = hw_count;
    assert(hook_rw(sgx + model_id_off[0], 0, false, 2) == 0 && hw_count == hw0);
    puts("G1 STOP OK");

    g2_tests();
    puts("GPU PROBE OK");
    return 0;
}
