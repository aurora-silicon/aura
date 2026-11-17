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
 * m1n1's own reserved timer software IRQs sit at aic->nr_irq - 2 * 24 (the Mu ABI),
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

/*
 * T8152 (J804) D4: usb-drd1's xHCI line 1692 is published as 995 and usb-drd3's 1834 as 997
 * (index 0 of each node's ADT `interrupts`, level-high).  A walk of the J804 ADT found 520
 * distinct lines, 242 inside [32,1020), and none in 980..999; 995/997 are also distinct from
 * the carrier's virtual AHCI SPI 990 and from 996, kept for ANS.  An entry is live only while
 * hv_usb.c holds that port attached (hv_aic_alias_t8152_usb): with USB off, 995 is not owned,
 * so a guest GICD_ISENABLER write for it can never unmask AIC 1692.
 */
static const struct hv_aic_alias hv_aic_aliases_t8152_usb[] = {
    {995, 1692}, /* usb-drd1: left-rear USB-C, dwc3 xHCI */
    {997, 1834}, /* usb-drd3: right USB-C, dwc3 xHCI */
};
/*
 * The live table: one fixed slot per port, a withdrawn slot holds ~0 in both fields (never an
 * INTID or an AIC line).  Slots are rewritten in place, published number last on the way in
 * and first on the way out, so a CPU reading the table while the storm guard in hv_usb.c
 * withdraws one port never sees an entry move or a half-built one claim ownership.
 */
#define T8152_ALIAS_NONE 0xffffffffu
static struct hv_aic_alias hv_aic_aliases_t8152[ARRAY_SIZE(hv_aic_aliases_t8152_usb)] = {
    {T8152_ALIAS_NONE, T8152_ALIAS_NONE},
    {T8152_ALIAS_NONE, T8152_ALIAS_NONE},
};

static void t8152_alias_slot(u32 slot, bool live)
{
    struct hv_aic_alias *e = &hv_aic_aliases_t8152[slot];
    if (live) {
        __atomic_store_n(&e->physical, hv_aic_aliases_t8152_usb[slot].physical, __ATOMIC_RELEASE);
        __atomic_store_n(&e->published, hv_aic_aliases_t8152_usb[slot].published,
                         __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&e->published, T8152_ALIAS_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&e->physical, T8152_ALIAS_NONE, __ATOMIC_RELEASE);
    }
}

void hv_aic_alias_t8152_usb(u32 port_mask)
{
    t8152_alias_slot(0, port_mask & BIT(1));
    t8152_alias_slot(1, port_mask & BIT(3));
}

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
    /*
     * T8152 (J804) publishes only what a milestone attached: D4's USB lines while hv_usb.c
     * holds the port (above), nothing otherwise.  Planned, from the same walk (of 960..1019
     * only 1009, 1011, 1013 and 1015..1017 are taken, so 980..999 is free):
     *
     *   M4  996 -> 1349  ans (NVMe), the line the J804 DTS uses
     *   D4  995 -> 1692  usb-drd1, 997 -> 1834  usb-drd3 (live now, attach-gated)
     *   M7  MTP 1500/1501
     *
     * Re-run the free-line walk on the live ADT before each run.  A published
     * number must also never be one of aura's reserved timer software lines at
     * aic->nr_irq - 2 * 24 .. aic->nr_irq - 1 (2352..2399 with 0x960).
     */
    if (chip_id == T8152) {
        *count = ARRAY_SIZE(hv_aic_aliases_t8152);
        return hv_aic_aliases_t8152;
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

bool hv_aic_alias_lookup_physical(u32 physical, u32 *published)
{
    u32 count;
    const struct hv_aic_alias *table = hv_aic_alias_table(&count);

    for (u32 i = 0; i < count; i++) {
        if (table[i].physical == physical) {
            *published = table[i].published;
            return true;
        }
    }
    return false;
}
