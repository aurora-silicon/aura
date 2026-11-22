/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Software GICv3 CPU interface.  See hv_gic_cpuif.h for the model and its
 * references (Arm IHI 0069; Arm DDI 0487).  Pure logic: host-tested by
 * tests/python/test_windows_t8152_gic_cpuif.py.
 */

#include "hv_gic_cpuif.h"

static bool bitmap_test(const u32 *map, u32 intid)
{
    return (__atomic_load_n(&map[intid / 32], __ATOMIC_ACQUIRE) & (1u << (intid % 32))) != 0;
}

static void bitmap_set(u32 *map, u32 intid)
{
    __atomic_fetch_or(&map[intid / 32], 1u << (intid % 32), __ATOMIC_ACQ_REL);
}

static void bitmap_clear(u32 *map, u32 intid)
{
    __atomic_fetch_and(&map[intid / 32], ~(1u << (intid % 32)), __ATOMIC_ACQ_REL);
}

static void cpuif_reset_registers(struct gicv3_cpuif *c)
{
    /* IHI 0069 reset values: PMR 0 (everything masked), binary points at their minimum. */
    c->ap0r0 = 0;
    c->ap1r0 = 0;
    c->pmr = 0;
    c->bpr0 = GICV3_CPUIF_BPR0_MIN;
    c->bpr1 = GICV3_CPUIF_BPR1_MIN;
    c->eoimode = false;
    c->cbpr = false;
    c->igrpen0 = false;
    c->igrpen1 = false;
}

void gicv3_cpuif_reset(struct gicv3_cpuif *c)
{
    for (u32 word = 0; word < GICV3_CPUIF_WORDS; word++) {
        __atomic_store_n(&c->pending[word], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&c->active[word], 0, __ATOMIC_RELEASE);
    }
    cpuif_reset_registers(c);
    c->valid = true;
}

void gicv3_cpuif_power_on(struct gicv3_cpuif *c)
{
    for (u32 word = 0; word < GICV3_CPUIF_WORDS; word++)
        __atomic_store_n(&c->active[word], 0, __ATOMIC_RELEASE);
    cpuif_reset_registers(c);
    c->valid = true;
}

bool gicv3_cpuif_set_pending(struct gicv3_cpuif *c, u32 intid)
{
    if (intid >= GICV3_CPUIF_NR_INTID)
        return false;
    bitmap_set(c->pending, intid);
    return true;
}

void gicv3_cpuif_clear_pending(struct gicv3_cpuif *c, u32 intid)
{
    if (intid < GICV3_CPUIF_NR_INTID)
        bitmap_clear(c->pending, intid);
}

bool gicv3_cpuif_is_pending(const struct gicv3_cpuif *c, u32 intid)
{
    return intid < GICV3_CPUIF_NR_INTID && bitmap_test(c->pending, intid);
}

bool gicv3_cpuif_is_active(const struct gicv3_cpuif *c, u32 intid)
{
    return intid < GICV3_CPUIF_NR_INTID && bitmap_test(c->active, intid);
}

static u8 cpuif_priority(gicv3_cpuif_priority_fn prio, void *opaque, u32 intid)
{
    return prio(opaque, intid) & GICV3_CPUIF_PRI_MASK;
}

/*
 * The effective Group 1 binary point.  With ICC_CTLR_EL1.CBPR = 1, ICC_BPR0_EL1
 * determines Group 1 preemption too, and ICC_BPR1_EL1 reads as BPR0 + 1,
 * saturated at 7 (IHI 0069, ICC_BPR1_EL1).
 */
static u8 cpuif_bpr1(const struct gicv3_cpuif *c)
{
    if (c->cbpr)
        return c->bpr0 >= 7 ? 7 : (u8)(c->bpr0 + 1);
    return c->bpr1;
}

/*
 * Group priority of a Group 1 interrupt: the priority bits above the binary
 * point.  ICC_BPR1_EL1 = N splits priority[7:N] / priority[N-1:0] (Group 0's
 * ICC_BPR0_EL1 = N splits at N + 1), so the Group 1 mask is 0xff << N.
 */
static u8 cpuif_group_priority1(const struct gicv3_cpuif *c, u8 priority)
{
    return priority & (u8)(0xff << cpuif_bpr1(c)) & GICV3_CPUIF_PRI_MASK;
}

u8 gicv3_cpuif_running_priority(const struct gicv3_cpuif *c)
{
    u32 active = c->ap0r0 | c->ap1r0;

    if (!active)
        return GICV3_CPUIF_IDLE_PRI;
    return (u8)(__builtin_ctz(active) << GICV3_CPUIF_PRI_SHIFT);
}

int gicv3_cpuif_hppi(const struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio, void *opaque)
{
    int best = -1;
    u32 best_prio = 0x100;

    for (u32 word = 0; word < GICV3_CPUIF_WORDS; word++) {
        /* An interrupt that is active and pending cannot be acknowledged again. */
        u32 candidates = __atomic_load_n(&c->pending[word], __ATOMIC_ACQUIRE) &
                         ~__atomic_load_n(&c->active[word], __ATOMIC_ACQUIRE);

        while (candidates) {
            u32 bit = (u32)__builtin_ctz(candidates);
            candidates &= ~(1u << bit);

            u32 intid = word * 32 + bit;
            if (intid >= GICV3_CPUIF_NR_INTID)
                break;
            u32 p = cpuif_priority(prio, opaque, intid);
            /* Equal priorities: the lowest INTID (IMPLEMENTATION DEFINED choice). */
            if (p < best_prio) {
                best = (int)intid;
                best_prio = p;
            }
        }
    }

    return best;
}

int gicv3_cpuif_deliverable(const struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio,
                            void *opaque)
{
    if (!c->valid || !c->igrpen1)
        return -1;

    int intid = gicv3_cpuif_hppi(c, prio, opaque);
    if (intid < 0)
        return -1;

    u8 p = cpuif_priority(prio, opaque, (u32)intid);
    /* Signalled only if higher priority (numerically lower) than PMR ... */
    if (p >= (c->pmr & GICV3_CPUIF_PRI_MASK))
        return -1;
    /* ... and its group priority is higher than the running priority (preemption). */
    if (cpuif_group_priority1(c, p) >= gicv3_cpuif_running_priority(c))
        return -1;

    return intid;
}

static void cpuif_deactivate(struct gicv3_cpuif *c, u32 intid, struct gicv3_cpuif_effect *fx)
{
    if (intid >= GICV3_CPUIF_NR_INTID || !bitmap_test(c->active, intid))
        return;
    bitmap_clear(c->active, intid);
    fx->deactivated = (s32)intid;
}

/*
 * Priority drop: clear the highest-priority set bit across the active priority
 * registers, which restores the running priority of the preempted context.
 */
static void cpuif_priority_drop(struct gicv3_cpuif *c)
{
    u32 active = c->ap0r0 | c->ap1r0;

    if (!active)
        return;
    u32 bit = 1u << __builtin_ctz(active);
    if (c->ap0r0 & bit)
        c->ap0r0 &= ~bit;
    else
        c->ap1r0 &= ~bit;
}

static u64 cpuif_ctlr(const struct gicv3_cpuif *c)
{
    return ((u64)(GICV3_CPUIF_PRI_BITS - 1) << GICV3_CTLR_PRIBITS_SHIFT) |
           (c->eoimode ? GICV3_CTLR_EOIMODE : 0) | (c->cbpr ? GICV3_CTLR_CBPR : 0);
}

static u32 cpuif_eoi_intid(u64 value)
{
    /* INTID field, bits [23:0] with 24 INTID bits; this interface has 16. */
    return (u32)(value & 0xffffff);
}

bool gicv3_cpuif_access(struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio, void *opaque, u32 reg,
                        bool is_read, u64 *value, struct gicv3_cpuif_effect *fx)
{
    u64 v = is_read ? 0 : *value;

    fx->acknowledged = -1;
    fx->deactivated = -1;
    fx->sgi1r = false;
    fx->sgi1r_value = 0;
    fx->igrpen1_enabled = false;

    if (!c->valid)
        gicv3_cpuif_reset(c);

    switch (reg) {
        case GICV3_ICC_PMR:
            if (is_read)
                v = c->pmr;
            else
                c->pmr = (u8)v & GICV3_CPUIF_PRI_MASK;
            break;

        case GICV3_ICC_BPR0:
            if (is_read) {
                v = c->bpr0;
            } else {
                u8 bpr = (u8)(v & 7);
                c->bpr0 = bpr < GICV3_CPUIF_BPR0_MIN ? GICV3_CPUIF_BPR0_MIN : bpr;
            }
            break;

        case GICV3_ICC_BPR1:
            if (is_read) {
                v = cpuif_bpr1(c);
            } else if (!c->cbpr) {
                u8 bpr = (u8)(v & 7);
                c->bpr1 = bpr < GICV3_CPUIF_BPR1_MIN ? GICV3_CPUIF_BPR1_MIN : bpr;
            }
            break;

        case GICV3_ICC_CTLR:
            if (is_read) {
                v = cpuif_ctlr(c);
            } else {
                c->eoimode = (v & GICV3_CTLR_EOIMODE) != 0;
                c->cbpr = (v & GICV3_CTLR_CBPR) != 0;
            }
            break;

        case GICV3_ICC_SRE:
            /* System-register interface only; ICC_SRE_EL1 does not normally trap here. */
            if (is_read)
                v = GICV3_CPUIF_SRE_VALUE;
            break;

        case GICV3_ICC_IGRPEN0:
            if (is_read)
                v = c->igrpen0;
            else
                c->igrpen0 = (v & 1) != 0;
            break;

        case GICV3_ICC_IGRPEN1:
            if (is_read) {
                v = c->igrpen1;
            } else {
                bool was = c->igrpen1;
                c->igrpen1 = (v & 1) != 0;
                fx->igrpen1_enabled = c->igrpen1 && !was;
            }
            break;

        case GICV3_ICC_AP0R(0):
            if (is_read)
                v = c->ap0r0;
            else
                c->ap0r0 = (u32)v;
            break;

        case GICV3_ICC_AP1R(0):
            if (is_read)
                v = c->ap1r0;
            else
                c->ap1r0 = (u32)v;
            break;

        case GICV3_ICC_AP0R(1):
        case GICV3_ICC_AP0R(2):
        case GICV3_ICC_AP0R(3):
        case GICV3_ICC_AP1R(1):
        case GICV3_ICC_AP1R(2):
        case GICV3_ICC_AP1R(3):
            /* Only needed for more than 5 preemption bits. */
            break;

        case GICV3_ICC_RPR:
            if (is_read)
                v = gicv3_cpuif_running_priority(c);
            break;

        case GICV3_ICC_IAR1: {
            if (!is_read)
                break;
            int intid = gicv3_cpuif_deliverable(c, prio, opaque);
            if (intid < 0) {
                v = GICV3_SPURIOUS_INTID;
                break;
            }
            u8 p = cpuif_priority(prio, opaque, (u32)intid);
            bitmap_clear(c->pending, (u32)intid);
            bitmap_set(c->active, (u32)intid);
            c->ap1r0 |= 1u << (cpuif_group_priority1(c, p) >> GICV3_CPUIF_PRI_SHIFT);
            fx->acknowledged = intid;
            v = (u64)intid;
            break;
        }

        case GICV3_ICC_HPPIR1: {
            if (!is_read)
                break;
            /* Highest pending Group 1 interrupt, not filtered by PMR or running priority. */
            int intid = gicv3_cpuif_hppi(c, prio, opaque);
            v = intid < 0 ? GICV3_SPURIOUS_INTID : (u64)intid;
            break;
        }

        case GICV3_ICC_IAR0:
        case GICV3_ICC_HPPIR0:
        case GICV3_ICC_NMIAR1:
            /* No Group 0 interrupt and no NMI is ever pending here. */
            if (is_read)
                v = GICV3_SPURIOUS_INTID;
            break;

        case GICV3_ICC_EOIR1: {
            if (is_read)
                break;
            u32 intid = cpuif_eoi_intid(v);
            /* Writes of the special INTIDs 1020-1023 have no effect. */
            if (intid >= GICV3_CPUIF_NR_INTID)
                break;
            cpuif_priority_drop(c);
            if (!c->eoimode)
                cpuif_deactivate(c, intid, fx);
            break;
        }

        case GICV3_ICC_EOIR0:
            /* Group 0 is never acknowledged, so there is nothing to complete. */
            break;

        case GICV3_ICC_DIR:
            /* Deactivation is separate from priority drop only with EOImode = 1. */
            if (!is_read && c->eoimode)
                cpuif_deactivate(c, cpuif_eoi_intid(v), fx);
            break;

        case GICV3_ICC_SGI1R:
            if (!is_read) {
                fx->sgi1r = true;
                fx->sgi1r_value = v;
            }
            break;

        case GICV3_ICC_SGI0R:
        case GICV3_ICC_ASGI1R:
            /* Group 0 / other-security-state SGIs: none in a DS = 1, Group 1 only model. */
            break;

        default:
            return false;
    }

    if (is_read)
        *value = v;
    return true;
}

u32 gicv3_sgi1r_intid(u64 sgi1r)
{
    return (u32)((sgi1r >> 24) & 0xf);
}

bool gicv3_sgi1r_targets(u64 sgi1r, u64 mpidr, bool is_sender)
{
    /* ICC_SGI1R_EL1: TargetList [15:0], Aff1 [23:16], INTID [27:24], Aff2 [39:32], IRM [40],
     * RS [47:44], Aff3 [55:48]. */
    if ((sgi1r >> 40) & 1)
        return !is_sender;

    u64 aff0 = mpidr & 0xff;
    if (((mpidr >> 8) & 0xff) != ((sgi1r >> 16) & 0xff) ||
        ((mpidr >> 16) & 0xff) != ((sgi1r >> 32) & 0xff) ||
        ((mpidr >> 32) & 0xff) != ((sgi1r >> 48) & 0xff))
        return false;
    if ((aff0 >> 4) != ((sgi1r >> 44) & 0xf))
        return false;
    return (sgi1r & (1u << (aff0 & 0xf))) != 0;
}

struct gicv3_timer_gate gicv3_timer_gate(u64 ctl, bool held)
{
    struct gicv3_timer_gate g = {0};
    bool imask = (ctl & GICV3_TIMER_CTL_IMASK) != 0;
    bool fired = (ctl & (GICV3_TIMER_CTL_ENABLE | GICV3_TIMER_CTL_ISTATUS)) ==
                 (GICV3_TIMER_CTL_ENABLE | GICV3_TIMER_CTL_ISTATUS);

    held = held && imask; /* IMASK clear: the guest rewrote CTL, nothing is held */
    g.asserted = fired && (!imask || held);
    if (g.asserted) {
        g.set_imask = !imask;
        g.held = true;
    } else {
        g.clear_imask = held;
        g.held = false;
    }
    return g;
}
