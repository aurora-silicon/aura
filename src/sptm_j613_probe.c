/* SPDX-License-Identifier: MIT */

/*
 * One-boot J613 host probes.  These run before the normal Linux payload and
 * never open, patch or enter an Apple image.
 */

#include "../build/build_cfg.h"

#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "gxf.h"
#include "malloc.h"
#include "memory.h"
#include "soc.h"
#include "sptm_virt.h"
#include "sptm_j613_probe.h"
#include "string.h"
#include "utils.h"

#define ID_AA64MMFR1_VH GENMASK(11, 8)

extern const char *const aura_version;

static struct {
    bool valid, nv2, vh, fgt, gxf, admission;
    u64 mmfr0, mmfr1, mmfr2, aidr, currentel, midr, mpidr;
#ifdef J613_HV_SYNTHETIC
    bool attempted, nv_roundtrip, guarded_roundtrip;
    u64 gl, want;
#endif
} receipt;

struct nv2_probe_ctx {
    u64 entry, sp_el1, vncr, avncr;
    u64 hcr_set, hcr_clr, arg, vbar_el1, spsr;
    u64 out_x0, out_x1, done, saved_hcr, saved_vbar;
    u64 cnthctl, hfgitr, which, saved_cnthctl, saved_hfgitr;
};

void nv2_probe_call(struct nv2_probe_ctx *ctx);

static u64 j613_gl_roundtrip(u64 a, u64 b, u64 c, u64 d)
{
    return a ^ b ^ c ^ d ^ 0x613;
}

static bool j613_nv2_roundtrip(void)
{
    /* movz x1,#0x5a5a; eor x0,x0,x1; hvc #1 */
    static const u32 code[] = {0xd28b4b41, 0xca010000, 0xd4000022};
    u8 *mem = memalign(SZ_16K, 5 * SZ_16K);
    if (!mem)
        return false;
    memset(mem, 0, 5 * SZ_16K);
    memcpy(mem, code, sizeof(code));
    dc_cvac_range(mem, sizeof(code));
    ic_ivau_range(mem, sizeof(code));
    sysop("dsb sy");
    sysop("isb");

    struct nv2_probe_ctx ctx = {
        .entry = (u64)mem,
        .sp_el1 = (u64)mem + SZ_16K,
        .vncr = (u64)mem + 2 * SZ_16K,
        .avncr = (u64)mem + 3 * SZ_16K,
        .hcr_set = HCR_RW | HCR_NV | HCR_NV2,
        .hcr_clr = HCR_TGE | HCR_NV1,
        .arg = 0x6130,
    };
    nv2_probe_call(&ctx);
    bool pass = ctx.done == 1 && ctx.out_x0 == (0x6130 ^ 0x5a5a);
    free(mem);
    return pass;
}

void sptm_j613_probe_run(void)
{
    receipt.mmfr0 = mrs(SYS_ID_AA64MMFR0_EL1);
    receipt.mmfr1 = mrs(SYS_ID_AA64MMFR1_EL1);
    receipt.mmfr2 = mrs(SYS_ID_AA64MMFR2_EL1);
    receipt.aidr = mrs(AIDR_EL1);
    receipt.currentel = mrs(CurrentEL);
    receipt.midr = mrs(MIDR_EL1);
    receipt.mpidr = mrs(MPIDR_EL1);
    receipt.nv2 = FIELD_GET(ID_AA64MMFR2_NV, receipt.mmfr2) >= 2;
    receipt.vh = FIELD_GET(ID_AA64MMFR1_VH, receipt.mmfr1) != 0;
    receipt.fgt = FIELD_GET(ID_AA64MMFR0_FGT, receipt.mmfr0) != 0;
    receipt.gxf = supports_gxf();
    receipt.admission = sptm_virt_requested();
    receipt.valid = true;

#ifdef J613_HV_SYNTHETIC
    if (chip_id == T8122 && receipt.currentel == 8 && receipt.nv2 && receipt.vh && receipt.fgt &&
        receipt.gxf) {
        receipt.attempted = true;
        receipt.nv_roundtrip = j613_nv2_roundtrip();
        u64 saved_hcr = mrs(HCR_EL2);
        receipt.gl = gl1_call(j613_gl_roundtrip, 0x1111, 0x2222, 0x3333, 0x4444);
        receipt.want = 0x1111 ^ 0x2222 ^ 0x3333 ^ 0x4444 ^ 0x613;
        receipt.guarded_roundtrip = receipt.gl == receipt.want;
        msr(HCR_EL2, saved_hcr);
        sysop("isb");
    }
#endif
    sptm_j613_probe_replay();
}

void sptm_j613_probe_replay(void)
{
    if (!receipt.valid)
        return;

    printf("J613-HV BEGIN stage=%s profile=J613/25G83 soc=T8122 build=%s\n",
#ifdef J613_HV_SYNTHETIC
           "synthetic",
#else
           "monitor",
#endif
           aura_version);
    printf("J613-HV IDS currentel=0x%lx midr=0x%lx mpidr=0x%lx aidr=0x%lx "
           "mmfr0=0x%lx mmfr1=0x%lx mmfr2=0x%lx\n",
           receipt.currentel, receipt.midr, receipt.mpidr, receipt.aidr, receipt.mmfr0,
           receipt.mmfr1, receipt.mmfr2);
    printf("J613-HV DECISION chip=%s el2=%s nv2=%s vhe=%s fgt=%s aidr_gxf=%s "
           "mmu_sprr=%s aura_gxf=%s admission=%s\n",
           chip_id == T8122 ? "pass" : "fail", receipt.currentel == 8 ? "pass" : "fail",
           receipt.nv2 ? "pass" : "fail", receipt.vh ? "pass" : "fail",
           receipt.fgt ? "pass" : "fail", (receipt.aidr & AIDR_EL1_GXF) ? "pass" : "fail",
           cpu_features->mmu_sprr ? "pass" : "fail", receipt.gxf ? "pass" : "fail",
           receipt.admission ? "pass" : "fail");

#ifdef J613_HV_SYNTHETIC
    if (!receipt.attempted) {
        printf("J613-HV SYNTHETIC result=SKIP reason=feature-admission\n");
    } else {
        printf("J613-HV SYNTHETIC vncr_eret=%s gl1_gl2=%s gl_ret=0x%lx expected=0x%lx "
               "result=%s\n",
               receipt.nv_roundtrip ? "pass" : "fail",
               receipt.guarded_roundtrip ? "pass" : "fail", receipt.gl, receipt.want,
               receipt.nv_roundtrip && receipt.guarded_roundtrip ? "PASS" : "FAIL");
    }
#endif
    printf("J613-HV END action=continue-default-linux\n");
}
