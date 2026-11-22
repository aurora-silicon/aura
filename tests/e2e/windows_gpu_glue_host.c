/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host driver for src/hv/windows/hv_gpu.c (G0 carrier glue), compiled with HV_GPU_HOST_TEST
 * against stand-ins for the stage-2 policy and the HV map calls, which record what the glue
 * asks for (windows_gpu_glue_stubs.h).  tests/python/test_windows_t8152_gpu.py runs it with the
 * synthetic machine.
 *
 *   windows_gpu_glue_host ADT TEXT0_PHYS TEXT0 TEXT1_PHYS TEXT1 SGX SGX_SIZE MBOX POOL POOL_SIZE
 *                         RAM_BASE RAM_TOP
 */
#include "windows_gpu_glue_stubs.h"
#include "windows_gpu_hw_model.h"

int main(int argc, char **argv)
{
    assert(argc == 13);
    standin_load_adt(argv[1]);
    for (int i = 0; i < 2; i++) {
        texts[i].phys = strtoull(argv[2 + 2 * i], NULL, 0);
        texts[i].data = slurp(argv[3 + 2 * i], &texts[i].size);
    }
    u64 sgx = strtoull(argv[6], NULL, 0), sgx_size = strtoull(argv[7], NULL, 0);
    u64 mbox = strtoull(argv[8], NULL, 0);
    u64 pool = strtoull(argv[9], NULL, 0), pool_size = strtoull(argv[10], NULL, 0);
    ram_base = strtoull(argv[11], NULL, 0);
    ram_top = strtoull(argv[12], NULL, 0);
    cur_boot_args.phys_base = ram_base;
    cur_boot_args.mem_size = ram_top - ram_base;

    /* F32: the state each op needs; refusals here spend nothing. */
    reset_stubs();
    armed = false;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);
    armed = true;
    ram_set = false;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);
    ram_set = true;
    entered = true;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);
    entered = false;
    assert(op(HV_GPU_OP_HOOK, 0, 0) == HV_GPU_E_STATE);
    assert(op(HV_GPU_OP_ADMIT, pool + 0x4000, pool_size) == HV_GPU_E_POOL);
    assert(op(HV_GPU_OP_ADMIT, pool, 0) == HV_GPU_E_POOL);
    assert(op(HV_GPU_OP_ADMIT, ram_top, pool_size) == HV_GPU_E_POOL);
    assert(op(HV_GPU_OP_ADMIT, ram_base, ram_top - ram_base) == HV_GPU_E_POOL);
    assert(op(HV_GPU_OP_ADMIT, ram_top - pool_size, 2 * pool_size) == HV_GPU_E_POOL);
    assert(fw_allow_calls == 0 && deny_calls == 0 && allow_calls == 0 && hook_calls == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_STATE, 0) == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_HANDOFF_IPA, 0) == 0);

    /* ADMIT: deny TEXT, register 4 RW + 4 RO identity exceptions, map the handoff page RO. */
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == 0);
    assert(deny_calls == 2 && fw_allow_calls == 8 && allow_calls == 1 && map_ro_calls == 1);
    assert(hook_calls == 0);
    for (int i = 0; i < 8; i++) {
        assert(fw[i].ro == (i >= 4));
        assert(fw[i].pa >= fw[i].lo && fw[i].pa + fw[i].size <= fw[i].hi);
        assert(fw[i].pa >= ram_top || fw[i].pa + fw[i].size <= ram_base);
    }
    for (int i = 0; i < 2; i++)
        assert(denies[i].from == texts[i].phys && denies[i].size == texts[i].size);
    u64 ipa = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_HANDOFF_IPA, 0);
    assert(ipa && ipa == (u64)hv_gpu_host_handoff() && !(ipa & 0x3fff));
    assert(allow_ipa == ipa && allow_pa == ipa && allow_size == 0x4000 && allow_ro);
    assert(map_from == ipa && map_to == ipa && map_size == 0x4000);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_HANDOFF_SIZE, 0) == 0x4000);
    handoff_ok(GPU_HS_ADMITTED);
    /* Mu's PrePi reads the page with the MMU off: every build is cleaned to the PoC. */
    assert(clean_calls == 2 && clean_addr == hv_gpu_host_handoff() && clean_len == 0x4000);
    assert(live_exceptions == 9 && rollback_calls == 0 && unmap_calls == 0);
    u64 n = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGES, 0);
    assert(n > 12 && n <= GPU_T8152_MAX_RANGES);
    u64 last_cls = (u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * (n - 1) + 2, 0);
    assert(last_cls == GPU_C_HANDOFF);
    assert((u64)op(HV_GPU_OP_STATUS, HV_GPU_S_RANGE + 4 * n, 0) == HV_GPU_ERROR);
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE); /* once per session */
    assert(fw_allow_calls == 8);

    /* F33: HOOK covers exactly the sgx window and the post-office page, once. */
    assert(op(HV_GPU_OP_HOOK, 0, 0) == 0);
    assert(hook_calls == 2);
    assert(hooks[0].from == sgx && hooks[0].size == sgx_size);
    assert(hooks[1].from == mbox && hooks[1].size == 0x4000);
    assert(op(HV_GPU_OP_HOOK, 0, 0) == HV_GPU_E_STATE);
    handoff_ok(GPU_HS_ADMITTED | GPU_HS_HOOKS_LOG);
    assert(clean_calls == 3); /* the refreshed page is cleaned too */
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_STATE, 0) == (GPU_HS_ADMITTED | GPU_HS_HOOKS_LOG));

    /* F34: read-as-zero, write-ignored, counted by class. */
    u64 val[2] = {0xdead, 0xbeef};
    assert(hv_gpu_host_hook(sgx + 0x10, val, false, 2) && val[0] == 0);
    val[0] = 0x1234;
    assert(hv_gpu_host_hook(sgx + 0x20, val, true, 3) && val[0] == 0x1234);
    val[0] = val[1] = 0x5555;
    assert(hv_gpu_host_hook(mbox + 0x104, val, false, 4) && val[0] == 0 && val[1] == 0);
    assert(hv_gpu_host_hook(mbox + 0x300, val, true, 2));
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_ACCESSES, 0) == 4);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_MAILBOX_GFX, 0) == 1);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_CLASS + GPU_C_SGX, 0) == 2);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_CLASS + GPU_C_MAILBOX, 0) == 2);
    for (int i = 0; i < 40; i++)
        hv_gpu_host_hook(sgx + 0x4000, val, false, 2); /* the log stops at the limit */
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_ACCESSES, 0) == 44);

    /* A policy refusal fails ADMIT, spends the session and takes back every exception. */
    reset_stubs();
    fw_allow_fail_at = 3;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_POLICY);
    assert(rollback_calls == 1 && live_exceptions == 0 && map_ro_calls == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_HANDOFF_IPA, 0) == 0);
    assert(op(HV_GPU_OP_HOOK, 0, 0) == HV_GPU_E_STATE);
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);

    /* The handoff page refused, or its map failed: nothing of the admission stays allowed. */
    reset_stubs();
    allow_fail = true;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_POLICY);
    assert(fw_allow_calls == 8 && rollback_calls == 1 && live_exceptions == 0);
    assert(map_ro_calls == 0 && unmap_calls == 0);
    assert(op(HV_GPU_OP_STATUS, HV_GPU_S_HANDOFF_IPA, 0) == 0);
    reset_stubs();
    map_fail = true;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_POLICY);
    assert(map_ro_calls == 1 && unmap_calls == 1 && rollback_calls == 1 && live_exceptions == 0);
    assert(unmaps[0].from == map_from && unmaps[0].size == 0x4000);
    assert(op(HV_GPU_OP_HOOK, 0, 0) == HV_GPU_E_STATE);
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);

    /* An admission refusal (wrong board) spends it too, and maps nothing. */
    reset_stubs();
    board_id = 0x24;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_ADMIT);
    assert(fw_allow_calls == 0 && deny_calls == 0 && allow_calls == 0 && map_ro_calls == 0);
    board_id = 0x22;
    assert(op(HV_GPU_OP_ADMIT, pool, pool_size) == HV_GPU_E_STATE);

    /* Unknown ops and status indices. */
    assert((u64)op(9, 0, 0) == HV_GPU_ERROR);
    assert((u64)op(HV_GPU_OP_STATUS, 0x77, 0) == HV_GPU_ERROR);
    puts("GPU GLUE OK");
    return 0;
}
