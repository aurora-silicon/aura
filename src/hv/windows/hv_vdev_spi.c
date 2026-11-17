/* SPDX-License-Identifier: MIT */

#include "hv_vdev_spi.h"

/* See hv_vdev_spi.h.  Arm IHI 0069, GICD_IROUTER<n>. */
int vdev_spi_route(u64 irouter, int self, int fallback, const u64 *mpidr, u64 candidates, int ncpu)
{
    if (ncpu > 64)
        ncpu = 64;
    if (fallback < 0 || fallback >= ncpu || !(candidates & BIT(fallback)))
        fallback = -1;

    if (irouter & VDEV_SPI_IROUTER_IRM) {
        if (self >= 0 && self < ncpu && (candidates & BIT(self)))
            return self;
        return fallback;
    }

    u64 want = irouter & VDEV_SPI_AFF_MASK;
    for (int cpu = 0; cpu < ncpu; cpu++) {
        if (!(candidates & BIT(cpu)))
            continue;
        if ((mpidr[cpu] & VDEV_SPI_AFF_MASK) == want)
            return cpu;
    }
    return fallback;
}

bool vdev_spi_should_pend(bool level, bool enabled)
{
    return level && enabled;
}

struct vdev_spi_plan vdev_spi_plan(bool pend, int target, u64 pending, u64 active)
{
    struct vdev_spi_plan plan = {-1, 0};

    if (!pend) {
        plan.clear = pending;
        return plan;
    }
    if (active) {
        plan.clear = pending & ~active;
        return plan;
    }
    if (target < 0 || target >= 64)
        return plan;
    plan.raise = target;
    plan.clear = pending & ~BIT(target);
    return plan;
}

const u32 vdev_spi_intids[VDEV_SPI_COUNT] = {HV_VDEV_SPI_AHCI, HV_VDEV_SPI_INPUT};

u32 vdev_spi_device(u32 intid)
{
    switch (intid) {
        case HV_VDEV_SPI_AHCI:
            return VDEV_SPI_DEV_AHCI;
        case HV_VDEV_SPI_INPUT:
            return VDEV_SPI_DEV_INPUT;
        default:
            return 0;
    }
}

bool vdev_spi_is_virtual(u32 intid)
{
    return vdev_spi_device(intid) != 0;
}

bool vdev_spi_live(u32 intid, u32 devices)
{
    return (vdev_spi_device(intid) & devices) != 0;
}
