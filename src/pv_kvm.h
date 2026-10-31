/* SPDX-License-Identifier: MIT */
#ifndef AURA_PV_KVM_H
#define AURA_PV_KVM_H

#include "types.h"

/* ABI v0 is opt-in with the `aura-pv` boot argument. */
#define AURA_PV_ABI_MAJOR     0
#define AURA_PV_ABI_MINOR     1
#define AURA_PV_FUNC_UID      0xa000
#define AURA_PV_FUNC_VERSION  0xa001
#define AURA_PV_FUNC_GET_CAPS 0xa002
#define AURA_PV_FUNC_RUN      0xa010

#define AURA_PV_STATUS_OK       0
#define AURA_PV_STATUS_INVAL    (-22)
#define AURA_PV_STATUS_FAULT    (-14)
#define AURA_PV_STATUS_NOSYS    (-38)
#define AURA_PV_STATUS_BUSY     (-16)
#define AURA_PV_STATUS_SEQUENCE (-75)

#define AURA_PV_FEATURE_BOUNDED_INT BIT(0)
#define AURA_PV_FEATURE_HOST_STATE  BIT(1)
#define AURA_PV_FEATURE_STAGE2      BIT(2)
#define AURA_PV_FEATURE_VMID_TLBI   BIT(3)
#define AURA_PV_FEATURE_TIMER       BIT(4)

#define AURA_PV_GUEST_MAGIC   0x4155524150475630ULL /* AURAPGV0 */
#define AURA_PV_LAYOUT_CAPS   0x43504130U           /* CPA0 */
#define AURA_PV_LAYOUT_RUN    0x52554e30U           /* RUN0 */
#define AURA_PV_OUTPUT_RESULT BIT(0)
#define AURA_PV_OUTPUT_HOST   BIT(1)
#define AURA_PV_OUTPUT_ERROR  BIT(2)
#define AURA_PV_MAX_PROGRAM   8
#define AURA_PV_PAGE_BYTES    4096
#define AURA_PV_SENTINEL_X19  0x1919191919191919ULL
#define AURA_PV_SENTINEL_X20  0x2020202020202020ULL

struct aura_pv_caps_v0 {
    u32 major;
    u32 minor;
    u32 bytes;
    u32 layout_id;
    u64 features;
    u64 max_control_bytes;
    u64 shared_alignment;
    u64 vmid_bits;
    u64 ipa_bits;
    u64 s2_granule;
    u64 host_el;
    u64 reserved_base;
    u64 reserved_end;
};

struct aura_pv_control_v0 {
    u32 major;
    u32 minor;
    u32 bytes;
    u32 layout_id;
    u64 request_seq;
    u64 response_seq;
    u64 input_dirty;
    u64 output_valid;
    u64 guest_magic;
    u64 program_words;
    u64 initial;
    u64 result;
    u64 program[AURA_PV_MAX_PROGRAM];
    u64 host_sctlr_before;
    u64 host_sctlr_after;
    u64 host_cntkctl_before;
    u64 host_cntkctl_after;
    u64 host_sentinel_status;
    u64 exit_reason;
    u64 exit_flags;
    u8 reserved[AURA_PV_PAGE_BYTES - 200];
};

_Static_assert(sizeof(struct aura_pv_caps_v0) == 88, "PV caps ABI size");
_Static_assert(sizeof(struct aura_pv_control_v0) == AURA_PV_PAGE_BYTES, "PV control ABI size");

extern bool pv_kvm_active;
bool pv_kvm_requested(void);
void pv_kvm_prepare(void);
/* Called only for a host EL1 HVC with an immediate of zero. */
bool pv_kvm_handle_hvc(u64 *regs);

#endif
