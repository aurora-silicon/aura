/* SPDX-License-Identifier: MIT */

#include "hv_aic_alias.h"
#include "soc.h"
#include "utils.h"

struct hv_aic_alias {
    u32 published;
    u32 physical;
};

/*
 * J813 (T8142) only.
 *
 * dockchannel-mtp carries the built-in keyboard and trackpad.  Its ADT
 * `interrupts` property is <1277>, read back from the live device tree on this
 * machine, and 1277 is not a legal Windows GSIV -- see hv_aic_alias.h.
 *
 * 995 is free.  Walking every ADT node that carries an `interrupts` property
 * gives 249 distinct lines in 32..1019, and none of them is 995; the whole
 * 990..999 block is unused.  That has to be checked rather than assumed,
 * because injection is otherwise the identity: a published number that is also
 * a live physical line would deliver some other device's interrupts to the
 * guest under this device's INTID.  J414s publishes 37..46 for its aliases,
 * but 37 is taken on this SoC, so those numbers are not portable here.
 *
 * m1n1's own reserved timer software IRQs sit at aic->nr_irq - 2 * MAX_CPUS,
 * i.e. the very top of AIC space, so they cannot collide with a low published
 * number either.
 *
 * ans is the internal NVMe storage controller, and the same problem: its line
 * is `interrupts`[`nvme-interrupt-idx`] = interrupts[4] = 1155, also above the
 * arbiter's ceiling.  996 was picked by the same walk, re-run on this machine
 * for the ans work: 443 distinct lines appear in ADT `interrupts` properties,
 * 249 of them inside [32,1024), and 1155 is claimed by /arm-io/ans alone.  Of
 * the 980..999 block only 984 is taken, so 996 is free and sits next to the
 * mtp alias where both stay reviewable.  scratchpad adt_free_gsiv.py re-runs
 * the check; it is RAM-only, because a stray MMIO read latches an L2C error on
 * this SoC.
 */
/*
 * usb-drd0 is the LEFT Type-C port (atc-phy0 port-number 1; the proxy's own
 * cable is usb-drd1 / port-number 2, whose ADT nodes m1n1 removes from the
 * guest).  m1n1 already releases this controller to the guest in USB2 host
 * mode every boot -- "USB0: releasing controller for guest" -- so the only
 * thing missing for Windows is a devnode it can bind, and a GSIV it accepts.
 *
 * Its ADT `interrupts` is <1511 1512 1513 1514 1489>; 1511 is index 0, the
 * DWC3/xHCI controller interrupt, matching the single 777 that T8103's DSDT
 * published for the same node.  All five are above the arbiter's ceiling.
 *
 * 997 is free by the same ADT walk that picked 995 and 996: of 980..999 only
 * 984 is claimed, and no live physical line is 997 -- which has to be checked
 * rather than assumed, because injection is otherwise the identity and a
 * published number that is also a real line would deliver some other device's
 * interrupts under this INTID.
 */
static const struct hv_aic_alias hv_aic_aliases_t8142[] = {
    {995, 1277}, /* dockchannel-mtp: keyboard + trackpad */
    {996, 1155}, /* ans: internal NVMe storage */
    {997, 1511}, /* usb-drd0: left Type-C port, xHCI host */
};

static const struct hv_aic_alias *hv_aic_alias_table(u32 *count)
{
    /*
     * Runtime-gated, not compile-time: the same m1n1 binary is chainloaded
     * onto other targets, and an alias is only ever correct for the SoC whose
     * device tree it was read from.
     */
    if (chip_id == T8142) {
        *count = ARRAY_SIZE(hv_aic_aliases_t8142);
        return hv_aic_aliases_t8142;
    }

    *count = 0;
    return NULL;
}

u32 hv_aic_alias_to_physical(u32 published)
{
    u32 count;
    const struct hv_aic_alias *table = hv_aic_alias_table(&count);

    for (u32 i = 0; i < count; i++) {
        if (table[i].published == published)
            return table[i].physical;
    }

    return published;
}

bool hv_aic_alias_is_owned(u32 published)
{
    u32 count;
    const struct hv_aic_alias *table = hv_aic_alias_table(&count);

    for (u32 i = 0; i < count; i++) {
        if (table[i].published == published)
            return true;
    }

    return false;
}

u32 hv_aic_alias_to_published(u32 physical)
{
    u32 count;
    const struct hv_aic_alias *table = hv_aic_alias_table(&count);

    for (u32 i = 0; i < count; i++) {
        if (table[i].physical == physical)
            return table[i].published;
    }

    return physical;
}
