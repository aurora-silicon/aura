/* SPDX-License-Identifier: MIT */
#include "windows_compat.h"
#include "aic.h"
#include "aic_regs.h"
#include "hv.h"
#include "smp.h"
#include "soc.h"
#include "utils.h"
/* Current Aura's helper preserves callee-saved registers across unsafe WFI. */
extern void _deep_wfi_helper(void);
void cpu_wfi_stateless(void)
{
    sysop("dsb sy");
    sysop("isb");
    if (chip_id == T8152 && !hv_t8152_native_wfi()) {
        /*
         * Phase 3 measurement (a) found WFI_WIPES on T8152 (a timer-woken WFI
         * clears GPRs, NZCV and SIMD): no WFI.  Spin until an interrupt is
         * pending (what would end a WFI), bounded at 1 ms so a missing wake
         * source cannot hang the core.
         */
        u64 freq = mrs(CNTFRQ_EL0); /* nonzero: hv_t8152_preflight() */
        u64 deadline = mrs(CNTPCT_EL0) + (freq ? freq / 1000 : 24000);
        while (!mrs(ISR_EL1) && mrs(CNTPCT_EL0) < deadline)
            sysop("yield");
        sysop("isb");
        return;
    }
    _deep_wfi_helper();
    sysop("isb");
}
int smp_cpu_count(void)
{
    int count = 0;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (smp_is_alive(cpu))
            count++;
    return count;
}
int smp_get_id(u64 mpidr)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (smp_is_alive(cpu) && (smp_get_mpidr(cpu) & 0xff00ffffffUL) == (mpidr & 0xff00ffffffUL))
            return cpu;
    return -1;
}
void aic_set_mask(int irq, bool active)
{
    if (!aic || !aic->max_irq || irq < 0 || (u32)irq >= aic->max_irq * aic->nr_die)
        return;
    u32 die = irq / aic->max_irq;
    u32 local = irq % aic->max_irq;
    u64 offset = active ? aic->regs.mask_set : aic->regs.mask_clr;
    u32 stride = active ? aic->intmaskset_stride : aic->intmaskclear_stride;
    write32(aic->base + offset + die * stride + (local / 32) * 4, BIT(local % 32));
}
void aic_set_affinity(int irq, int cpu)
{
    if (!aic || aic->version != 1 || irq < 0 || (u32)irq >= aic->nr_irq || cpu < 0 || cpu >= 32)
        return;
    write32(aic->base + aic->regs.tgt_cpu + irq * 4, BIT(cpu));
}
/* AIC2/3 IRQ_CFG word of a line (die and target group), for logs; ~0 if there is none. */
u32 aic_irq_config(int irq)
{
    if (!aic || aic->version < 2 || !aic->max_irq || irq < 0 ||
        (u32)irq >= aic->max_irq * aic->nr_die)
        return ~0u;
    u32 die = irq / aic->max_irq;
    u32 local = irq % aic->max_irq;
    return read32(aic->base + aic->regs.config + die * aic->extintrcfg_stride + 4 * local);
}
void aic_set_enabled(bool enabled)
{
    if (!aic || aic->version < 2)
        return;
    mask32(aic->base + AIC23_GLOBAL_CFG, AIC23_GLOBAL_CFG_ENABLE,
           enabled ? AIC23_GLOBAL_CFG_ENABLE : 0);
}
bool aic_is_enabled(void)
{
    return aic && aic->version >= 2 &&
           (read32(aic->base + AIC23_GLOBAL_CFG) & AIC23_GLOBAL_CFG_ENABLE);
}
