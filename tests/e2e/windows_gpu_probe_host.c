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

static const char *adt_path;
static u64 sgx, sgx_size, mbox, pool, pool_size, wrap[2], vbar0, hole;

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
    assert(psd_reads == 0 && psd_writes == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_BOOT, 0) == 0);
    assert(op(HV_GPU_OP_POWER, 0, 0) == 0); /* not spent: the fixed recipe goes through */
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

    puts("GPU PROBE OK");
    return 0;
}
