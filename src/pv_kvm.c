/* SPDX-License-Identifier: MIT */
#include "pv_kvm.h"
#include "arm_cpu_regs.h"
#include "boot_options.h"
#include "cpu_regs.h"
#include "memory.h"
#include "nv2.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define SYS_CNTKCTL_EL12 sys_reg(3, 5, 14, 1, 0)

bool pv_kvm_active;

static bool pv_range_valid(u64 pa, u64 bytes)
{
    u64 end, ro_start, ro_end;
    if (!bytes || bytes > AURA_PV_PAGE_BYTES || (pa & 63) || pa > UINT64_MAX - bytes)
        return false;
    end = pa + bytes;
    if (!cur_boot_args.mem_size || cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
        return false;
    if (pa < cur_boot_args.phys_base || end > cur_boot_args.phys_base + cur_boot_args.mem_size)
        return false;

    /* The monitor's image is identity-mapped while PV is active. Never let a
     * host-provided physical pointer overwrite monitor code, data or stacks. */
    if ((u64)_base < end && pa < (u64)_end)
        return false;

    /* T8140's firmware-protected RAM must remain read-only to the monitor as
     * well as to the host. The helper returns zero when the range is not
     * present, so this check is portable across supported chips. */
    if (memory_fw_ro_range(&ro_start, &ro_end) > 0 && ro_start < end && pa < ro_end)
        return false;

    return true;
}

static void pv_set_error(struct aura_pv_control_v0 *c, u64 seq, s64 status)
{
    c->response_seq = seq;
    c->output_valid = AURA_PV_OUTPUT_ERROR;
    c->exit_reason = (u64)status;
}

bool pv_kvm_requested(void)
{
    return aura_pv_enabled() && !nv2_requested();
}

void pv_kvm_prepare(void)
{
    pv_kvm_active = pv_kvm_requested();
    if (pv_kvm_active)
        printf("PV-KVM: opt-in ABI v%u.%u enabled; bounded RUN only\n", AURA_PV_ABI_MAJOR,
               AURA_PV_ABI_MINOR);
}

static bool pv_uid(u64 *regs)
{
    regs[0] = AURA_PV_STATUS_OK;
    regs[1] = 0x4155524f52502b56ULL; /* AURORP+V */
    regs[2] = 0x504b564d2d454c31ULL; /* PKVM-EL1 */
    regs[3] = 0x8e2c1f4a7bd09361ULL;
    return true;
}

static bool pv_version(u64 *regs)
{
    regs[0] = AURA_PV_STATUS_OK;
    regs[1] = AURA_PV_ABI_MAJOR;
    regs[2] = AURA_PV_ABI_MINOR;
    regs[3] = AURA_PV_LAYOUT_RUN;
    return true;
}

static bool pv_caps(u64 *regs)
{
    struct aura_pv_caps_v0 *caps;
    u64 pa = regs[1], bytes = regs[2];
    if (!pv_range_valid(pa, bytes) || bytes < sizeof(*caps)) {
        regs[0] = AURA_PV_STATUS_FAULT;
        return true;
    }
    caps = (struct aura_pv_caps_v0 *)(uintptr_t)pa;
    memset(caps, 0, sizeof(*caps));
    caps->major = AURA_PV_ABI_MAJOR;
    caps->minor = AURA_PV_ABI_MINOR;
    caps->bytes = sizeof(*caps);
    caps->layout_id = AURA_PV_LAYOUT_CAPS;
    caps->features = AURA_PV_FEATURE_BOUNDED_INT | AURA_PV_FEATURE_HOST_STATE;
    caps->max_control_bytes = AURA_PV_PAGE_BYTES;
    caps->shared_alignment = 64;
    caps->host_el = 1;
    caps->reserved_base = (u64)_base;
    caps->reserved_end = (u64)_end;
    dc_cvac_range(caps, sizeof(*caps));
    regs[0] = AURA_PV_STATUS_OK;
    regs[1] = caps->features;
    regs[2] = caps->bytes;
    regs[3] = 0;
    return true;
}

static bool pv_run(u64 *regs)
{
    struct aura_pv_control_v0 *c;
    u64 pa = regs[1], bytes = regs[2], seq = regs[3];
    u64 host_sctlr, host_cntkctl, value;
    u64 words;

    if (!pv_range_valid(pa, bytes) || bytes < sizeof(*c)) {
        regs[0] = AURA_PV_STATUS_FAULT;
        return true;
    }
    c = (struct aura_pv_control_v0 *)(uintptr_t)pa;
    dc_civac_range(c, sizeof(*c));
    if (c->major != AURA_PV_ABI_MAJOR || c->layout_id != AURA_PV_LAYOUT_RUN ||
        c->bytes != sizeof(*c) || c->guest_magic != AURA_PV_GUEST_MAGIC || c->request_seq != seq) {
        pv_set_error(c, seq, AURA_PV_STATUS_INVAL);
        dc_cvac_range(c, sizeof(*c));
        regs[0] = AURA_PV_STATUS_INVAL;
        return true;
    }

    host_sctlr = mrs(SYS_SCTLR_EL12);
    host_cntkctl = mrs(SYS_CNTKCTL_EL12);
    c->host_sctlr_before = host_sctlr;
    c->host_cntkctl_before = host_cntkctl;
    c->host_sentinel_status = (regs[19] == AURA_PV_SENTINEL_X19 ? BIT(0) : 0) |
                              (regs[20] == AURA_PV_SENTINEL_X20 ? BIT(1) : 0);
    words = c->program_words;
    if (!words || words > AURA_PV_MAX_PROGRAM) {
        pv_set_error(c, seq, AURA_PV_STATUS_INVAL);
        goto out;
    }

    /* Bounded integer guest model: one initial value and at most eight ADD_IMM operations. */
    value = c->initial;
    for (u64 i = 0; i < words; i++) {
        if (UINT64_MAX - value < c->program[i]) {
            pv_set_error(c, seq, AURA_PV_STATUS_INVAL);
            goto out;
        }
        value += c->program[i];
    }
    c->result = value;
    c->exit_reason = 0; /* bounded guest completed */
    c->exit_flags = words;
    c->output_valid = AURA_PV_OUTPUT_RESULT | AURA_PV_OUTPUT_HOST;

out:
    c->host_sctlr_after = mrs(SYS_SCTLR_EL12);
    c->host_cntkctl_after = mrs(SYS_CNTKCTL_EL12);
    if (c->host_sctlr_after != host_sctlr || c->host_cntkctl_after != host_cntkctl)
        c->host_sentinel_status |= BIT(2);
    c->response_seq = seq;
    dc_cvac_range(c, sizeof(*c));
    regs[0] = (c->output_valid & AURA_PV_OUTPUT_ERROR) ? (s64)c->exit_reason : AURA_PV_STATUS_OK;
    regs[1] = c->result;
    regs[2] = c->host_sentinel_status;
    regs[3] = c->response_seq;
    return true;
}

bool pv_kvm_handle_hvc(u64 *regs)
{
    if (!pv_kvm_active)
        return false;
    switch (regs[0]) {
        case AURA_PV_FUNC_UID:
            return pv_uid(regs);
        case AURA_PV_FUNC_VERSION:
            return pv_version(regs);
        case AURA_PV_FUNC_GET_CAPS:
            return pv_caps(regs);
        case AURA_PV_FUNC_RUN:
            return pv_run(regs);
        default:
            return false;
    }
}
