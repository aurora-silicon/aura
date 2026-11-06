/* SPDX-License-Identifier: MIT */
#include "windows_compat.h"
#include "aic.h"
#include "aic_regs.h"
#include "smp.h"
#include "utils.h"
/* Current Aura's helper preserves callee-saved registers across unsafe WFI. */
extern void _deep_wfi_helper(void);
void cpu_wfi_stateless(void)
{
    sysop("dsb sy");
    sysop("isb");
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
