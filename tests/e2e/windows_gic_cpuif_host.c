/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host replay of the software GICv3 CPU interface (src/hv/windows/hv_gic_cpuif.c),
 * compiled unchanged.  Expected values follow Arm IHI 0069 (ICC_*_EL1 register
 * descriptions; prioritization, preemption, binary point; priority drop and
 * deactivation).  No hardware, no guest.  Prints one JSON line per case.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "hv_gic_cpuif.h"

static u8 prio_table[GICV3_CPUIF_NR_INTID];

static u8 prio(void *opaque, u32 intid)
{
    assert(opaque == (void *)prio_table);
    assert(intid < GICV3_CPUIF_NR_INTID);
    return prio_table[intid];
}

static struct gicv3_cpuif cpu;
static struct gicv3_cpuif_effect fx;

static u64 rd(u32 reg)
{
    u64 v = 0xdeadbeef;
    assert(gicv3_cpuif_access(&cpu, prio, prio_table, reg, true, &v, &fx));
    return v;
}

static void wr(u32 reg, u64 v)
{
    assert(gicv3_cpuif_access(&cpu, prio, prio_table, reg, false, &v, &fx));
}

static void fresh(void)
{
    memset(&cpu, 0x5a, sizeof(cpu)); /* garbage: reset must define everything it uses */
    gicv3_cpuif_reset(&cpu);
    memset(prio_table, 0, sizeof(prio_table));
}

static void pass(const char *name)
{
    printf("{\"case\": \"%s\", \"result\": \"passed\"}\n", name);
}

/* Windows-style CPU interface bring-up: CTLR, PMR, BPR1, IGRPEN1. */
static void enable(u8 pmr)
{
    wr(GICV3_ICC_CTLR, 0);
    wr(GICV3_ICC_PMR, pmr);
    wr(GICV3_ICC_BPR1, 0);
    wr(GICV3_ICC_IGRPEN1, 1);
}

static void case_reset_values(void)
{
    fresh();
    assert(rd(GICV3_ICC_PMR) == 0);
    assert(rd(GICV3_ICC_BPR0) == 2);
    assert(rd(GICV3_ICC_BPR1) == 3);
    /* PRIbits = 4 (5 bits), IDbits 0, SEIS 0, A3V 0, EOImode 0, CBPR 0. */
    assert(rd(GICV3_ICC_CTLR) == 0x400);
    assert(rd(GICV3_ICC_RPR) == 0xff);
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID);
    assert(rd(GICV3_ICC_HPPIR1) == GICV3_SPURIOUS_INTID);
    assert(rd(GICV3_ICC_SRE) == 0x7);
    assert(rd(GICV3_ICC_IGRPEN0) == 0 && rd(GICV3_ICC_IGRPEN1) == 0);
    /* A never-reset interface resets itself on first access. */
    memset(&cpu, 0, sizeof(cpu));
    assert(rd(GICV3_ICC_BPR1) == 3 && cpu.valid);
    pass("reset_values");
}

static void case_group1_disabled(void)
{
    fresh();
    wr(GICV3_ICC_PMR, 0xf0);
    gicv3_cpuif_set_pending(&cpu, 27);
    /* IGRPEN1 = 0: nothing is signalled, IAR1 is spurious, HPPIR1 still reports it. */
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID);
    assert(rd(GICV3_ICC_HPPIR1) == 27);
    assert(gicv3_cpuif_is_pending(&cpu, 27));
    wr(GICV3_ICC_IGRPEN1, 1);
    assert(fx.igrpen1_enabled);
    wr(GICV3_ICC_IGRPEN1, 1);
    assert(!fx.igrpen1_enabled); /* only the 0 -> 1 edge */
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 27);
    pass("group1_disabled");
}

static void case_pmr_mask(void)
{
    fresh();
    prio_table[30] = 0xa0;
    gicv3_cpuif_set_pending(&cpu, 30);
    enable(0xa0);
    /* Equal to the mask: not signalled (priority must be strictly higher). */
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID);
    wr(GICV3_ICC_PMR, 0xa8);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 30);
    /* Only 5 priority bits: PMR bits [2:0] are RAZ/WI. */
    wr(GICV3_ICC_PMR, 0xff);
    assert(rd(GICV3_ICC_PMR) == 0xf8);
    /* ... and an interrupt priority's low bits are ignored: 0xa7 compares as 0xa0. */
    prio_table[30] = 0xa7;
    wr(GICV3_ICC_PMR, 0xa0);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    pass("pmr_mask");
}

static void case_ack_eoi(void)
{
    fresh();
    prio_table[18] = 0xa0;
    enable(0xf0);
    gicv3_cpuif_set_pending(&cpu, 18);
    assert(rd(GICV3_ICC_IAR1) == 18);
    assert(fx.acknowledged == 18 && fx.deactivated == -1);
    assert(!gicv3_cpuif_is_pending(&cpu, 18) && gicv3_cpuif_is_active(&cpu, 18));
    assert(rd(GICV3_ICC_RPR) == 0xa0);
    assert(rd(GICV3_ICC_AP1R(0)) == (1u << (0xa0 >> 3)));
    /* Nothing else: spurious, and the line stays low. */
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID && fx.acknowledged == -1);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    wr(GICV3_ICC_EOIR1, 18);
    assert(fx.deactivated == 18);
    assert(!gicv3_cpuif_is_active(&cpu, 18));
    assert(rd(GICV3_ICC_RPR) == 0xff && rd(GICV3_ICC_AP1R(0)) == 0);
    pass("ack_eoi");
}

static void case_active_and_pending(void)
{
    fresh();
    prio_table[17] = 0x80;
    enable(0xf0);
    gicv3_cpuif_set_pending(&cpu, 17);
    assert(rd(GICV3_ICC_IAR1) == 17);
    /* The level re-asserts while the handler runs: active and pending. */
    gicv3_cpuif_set_pending(&cpu, 17);
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID);
    assert(rd(GICV3_ICC_HPPIR1) == GICV3_SPURIOUS_INTID);
    wr(GICV3_ICC_EOIR1, 17);
    assert(fx.deactivated == 17 && gicv3_cpuif_is_pending(&cpu, 17));
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 17);
    pass("active_and_pending");
}

static void case_preemption_nesting(void)
{
    fresh();
    prio_table[40] = 0xa0; /* outer: a device */
    prio_table[41] = 0xa0; /* same priority: may not preempt */
    prio_table[2] = 0x80;  /* SGI 2, higher: preempts */
    enable(0xf0);
    gicv3_cpuif_set_pending(&cpu, 40);
    assert(rd(GICV3_ICC_IAR1) == 40);
    gicv3_cpuif_set_pending(&cpu, 41);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    gicv3_cpuif_set_pending(&cpu, 2);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 2);
    assert(rd(GICV3_ICC_IAR1) == 2);
    assert(rd(GICV3_ICC_RPR) == 0x80);
    /* Completing the nested one restores the outer running priority, not idle. */
    wr(GICV3_ICC_EOIR1, 2);
    assert(fx.deactivated == 2);
    assert(rd(GICV3_ICC_RPR) == 0xa0);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1); /* 41 still blocked */
    wr(GICV3_ICC_EOIR1, 40);
    assert(fx.deactivated == 40 && rd(GICV3_ICC_RPR) == 0xff);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 41);
    pass("preemption_nesting");
}

static void case_binary_point(void)
{
    fresh();
    prio_table[50] = 0xa0;
    prio_table[51] = 0x98; /* group priority 0x80 with BPR1 = 5 */
    prio_table[52] = 0xa8; /* group priority 0xa0 with BPR1 = 5 */
    enable(0xf0);
    wr(GICV3_ICC_BPR1, 5);
    assert(rd(GICV3_ICC_BPR1) == 5);
    gicv3_cpuif_set_pending(&cpu, 50);
    assert(rd(GICV3_ICC_IAR1) == 50);
    gicv3_cpuif_set_pending(&cpu, 52);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1); /* same group priority */
    gicv3_cpuif_set_pending(&cpu, 51);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 51); /* higher group priority */
    /* Minimum binary points (5 preemption bits): BPR0 2, BPR1 3. */
    wr(GICV3_ICC_BPR1, 0);
    assert(rd(GICV3_ICC_BPR1) == 3);
    wr(GICV3_ICC_BPR0, 1);
    assert(rd(GICV3_ICC_BPR0) == 2);
    /* CBPR: BPR1 reads BPR0 + 1 (saturated at 7), writes ignored. */
    wr(GICV3_ICC_CTLR, GICV3_CTLR_CBPR);
    assert(rd(GICV3_ICC_CTLR) == (0x400 | GICV3_CTLR_CBPR));
    wr(GICV3_ICC_BPR0, 4);
    assert(rd(GICV3_ICC_BPR1) == 5);
    wr(GICV3_ICC_BPR1, 7);
    assert(rd(GICV3_ICC_BPR1) == 5);
    wr(GICV3_ICC_BPR0, 7);
    assert(rd(GICV3_ICC_BPR1) == 7);
    pass("binary_point");
}

static void case_eoimode1(void)
{
    fresh();
    prio_table[60] = 0x60;
    enable(0xf0);
    wr(GICV3_ICC_CTLR, GICV3_CTLR_EOIMODE);
    assert(rd(GICV3_ICC_CTLR) == (0x400 | GICV3_CTLR_EOIMODE));
    gicv3_cpuif_set_pending(&cpu, 60);
    assert(rd(GICV3_ICC_IAR1) == 60);
    /* EOIR1 is only the priority drop ... */
    wr(GICV3_ICC_EOIR1, 60);
    assert(fx.deactivated == -1 && gicv3_cpuif_is_active(&cpu, 60));
    assert(rd(GICV3_ICC_RPR) == 0xff);
    /* ... DIR deactivates. */
    wr(GICV3_ICC_DIR, 60);
    assert(fx.deactivated == 60 && !gicv3_cpuif_is_active(&cpu, 60));
    /* With EOImode 0, DIR is not a deactivation. */
    wr(GICV3_ICC_CTLR, 0);
    gicv3_cpuif_set_pending(&cpu, 60);
    assert(rd(GICV3_ICC_IAR1) == 60);
    wr(GICV3_ICC_DIR, 60);
    assert(fx.deactivated == -1 && gicv3_cpuif_is_active(&cpu, 60));
    wr(GICV3_ICC_EOIR1, 60);
    assert(fx.deactivated == 60);
    pass("eoimode1");
}

static void case_special_and_group0(void)
{
    fresh();
    prio_table[33] = 0x40;
    enable(0xf0);
    gicv3_cpuif_set_pending(&cpu, 33);
    assert(rd(GICV3_ICC_IAR1) == 33);
    /* EOIR1 of a special INTID changes nothing. */
    wr(GICV3_ICC_EOIR1, GICV3_SPURIOUS_INTID);
    assert(fx.deactivated == -1 && rd(GICV3_ICC_RPR) == 0x40);
    wr(GICV3_ICC_EOIR1, 33 | (0xffull << 32)); /* bits above the INTID field ignored */
    assert(fx.deactivated == 33);
    /* No Group 0 interrupts. */
    gicv3_cpuif_set_pending(&cpu, 34);
    assert(rd(GICV3_ICC_IAR0) == GICV3_SPURIOUS_INTID);
    assert(rd(GICV3_ICC_HPPIR0) == GICV3_SPURIOUS_INTID);
    assert(gicv3_cpuif_is_pending(&cpu, 34));
    wr(GICV3_ICC_IGRPEN0, 1);
    assert(rd(GICV3_ICC_IGRPEN0) == 1);
    wr(GICV3_ICC_EOIR0, 34);
    assert(fx.deactivated == -1);
    /* Active priority registers: AP0R0/AP1R0 save and restore, the others RAZ/WI. */
    wr(GICV3_ICC_AP0R(0), 0x10);
    assert(rd(GICV3_ICC_AP0R(0)) == 0x10 && rd(GICV3_ICC_RPR) == (4 << 3));
    wr(GICV3_ICC_AP0R(0), 0);
    for (int n = 1; n < 4; n++) {
        wr(GICV3_ICC_AP0R(n), ~0ull);
        wr(GICV3_ICC_AP1R(n), ~0ull);
        assert(rd(GICV3_ICC_AP0R(n)) == 0 && rd(GICV3_ICC_AP1R(n)) == 0);
    }
    /* Read-only written, write-only read. */
    wr(GICV3_ICC_IAR1, 34);
    wr(GICV3_ICC_RPR, 0);
    assert(rd(GICV3_ICC_EOIR1) == 0 && rd(GICV3_ICC_DIR) == 0 && rd(GICV3_ICC_SGI1R) == 0);
    /* SGI0R / ASGI1R generate nothing; SGI1R is handed to the carrier. */
    wr(GICV3_ICC_SGI0R, 1ull << 40);
    assert(!fx.sgi1r);
    wr(GICV3_ICC_ASGI1R, 1ull << 40);
    assert(!fx.sgi1r);
    wr(GICV3_ICC_SGI1R, (3ull << 24) | 1);
    assert(fx.sgi1r && fx.sgi1r_value == ((3ull << 24) | 1));
    /* ICC_NMIAR1_EL1: no NMI, so the spurious INTID, never INTID 0; nothing is acknowledged. */
    assert(rd(GICV3_ICC_NMIAR1) == GICV3_SPURIOUS_INTID && fx.acknowledged == -1);
    assert(gicv3_cpuif_is_pending(&cpu, 34));
    /* An encoding the model does not implement is the caller's decision. */
    u64 v = 0;
    assert(!gicv3_cpuif_access(&cpu, prio, prio_table, GICV3_CPUIF_REG(12, 10, 0), true, &v, &fx));
    assert(!gicv3_cpuif_access(&cpu, prio, prio_table, GICV3_CPUIF_REG(4, 6, 1), false, &v, &fx));
    pass("special_and_group0");
}

static void case_ties_and_range(void)
{
    fresh();
    enable(0xf0);
    prio_table[700] = 0x20;
    prio_table[33] = 0x20;
    prio_table[1019] = 0x10;
    gicv3_cpuif_set_pending(&cpu, 700);
    gicv3_cpuif_set_pending(&cpu, 33);
    assert(rd(GICV3_ICC_HPPIR1) == 33); /* equal priority: lowest INTID */
    assert(gicv3_cpuif_set_pending(&cpu, 1019));
    assert(rd(GICV3_ICC_HPPIR1) == 1019);
    assert(!gicv3_cpuif_set_pending(&cpu, 1020));
    assert(!gicv3_cpuif_set_pending(&cpu, 0xffffffff));
    gicv3_cpuif_clear_pending(&cpu, 5000);
    assert(!gicv3_cpuif_is_pending(&cpu, 1020) && !gicv3_cpuif_is_active(&cpu, 1023));
    pass("ties_and_range");
}

static void case_power_on(void)
{
    fresh();
    prio_table[3] = 0x10;
    enable(0xf0);
    gicv3_cpuif_set_pending(&cpu, 3);
    assert(rd(GICV3_ICC_IAR1) == 3);
    gicv3_cpuif_set_pending(&cpu, 5);
    gicv3_cpuif_power_on(&cpu);
    /* Interface state from reset, the redistributor's pending SGI kept. */
    assert(!gicv3_cpuif_is_active(&cpu, 3) && gicv3_cpuif_is_pending(&cpu, 5));
    assert(rd(GICV3_ICC_RPR) == 0xff && rd(GICV3_ICC_IGRPEN1) == 0 && rd(GICV3_ICC_PMR) == 0);
    assert(rd(GICV3_ICC_IAR1) == GICV3_SPURIOUS_INTID);
    enable(0xf0);
    assert(rd(GICV3_ICC_IAR1) == 5);
    pass("power_on");
}

static void case_sgi1r_targets(void)
{
    const u64 me = 0x10100, p1 = 0x10101, m2 = 0x40102, e0 = 0x0;
    u64 list = (1ull << 16) | (1ull << 32) | 0x3; /* Aff2 1, Aff1 1, Aff0 {0,1} */

    assert(gicv3_sgi1r_targets(list, me, true)); /* a target list may name the sender */
    assert(gicv3_sgi1r_targets(list, p1, false));
    assert(!gicv3_sgi1r_targets(list, m2, false) && !gicv3_sgi1r_targets(list, e0, false));
    u64 m_list = (1ull << 16) | (4ull << 32) | (1u << 2); /* Aff2 4, Aff1 1, Aff0 2 */
    assert(gicv3_sgi1r_targets(m_list, m2, false) && !gicv3_sgi1r_targets(m_list, me, false));
    /* IRM: everyone but the sender. */
    u64 irm = (1ull << 40) | (7ull << 24);
    assert(!gicv3_sgi1r_targets(irm, me, true) && gicv3_sgi1r_targets(irm, e0, false));
    assert(gicv3_sgi1r_intid(irm) == 7);
    /* Range selector: Aff0 16..31 needs RS 1. */
    u64 rs1 = (1ull << 44) | (1u << 2);
    assert(gicv3_sgi1r_targets(rs1, 0x12, false) && !gicv3_sgi1r_targets(rs1, 0x2, false));
    /* Aff3 must match too. */
    assert(!gicv3_sgi1r_targets(0x1, 0x100000000ull, false));
    pass("sgi1r_targets");
}

static void *pender(void *arg)
{
    u32 base = (u32)(unsigned long)arg;
    for (u32 intid = base; intid < GICV3_CPUIF_NR_INTID; intid += 4)
        gicv3_cpuif_set_pending(&cpu, intid);
    return NULL;
}

static void case_concurrent_pending(void)
{
    pthread_t threads[4];

    fresh();
    for (unsigned long t = 0; t < 4; t++)
        assert(pthread_create(&threads[t], NULL, pender, (void *)t) == 0);
    for (int t = 0; t < 4; t++)
        pthread_join(threads[t], NULL);
    for (u32 intid = 0; intid < GICV3_CPUIF_NR_INTID; intid++)
        assert(gicv3_cpuif_is_pending(&cpu, intid));
    pass("concurrent_pending");
}

/* A Windows-like sequence: timer at one priority, an IPI above it, a device below it. */
static void case_windows_like(void)
{
    fresh();
    prio_table[18] = 0x30; /* virtual timer (GTDT GSIV 18) */
    prio_table[1] = 0x20;  /* IPI SGI */
    prio_table[996] = 0x70;
    gicv3_cpuif_set_pending(&cpu, 18); /* firmware residue before the OS enables Group 1 */
    enable(0xf0);
    assert(rd(GICV3_ICC_IAR1) == 18);
    /* IRQL raise above the timer: PMR 0x30 masks the device and the timer, not the IPI. */
    wr(GICV3_ICC_PMR, 0x30);
    gicv3_cpuif_set_pending(&cpu, 996);
    gicv3_cpuif_set_pending(&cpu, 1);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 1);
    assert(rd(GICV3_ICC_IAR1) == 1);
    wr(GICV3_ICC_EOIR1, 1);
    assert(rd(GICV3_ICC_RPR) == 0x30);
    wr(GICV3_ICC_EOIR1, 18);
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1); /* 996 still under PMR */
    wr(GICV3_ICC_PMR, 0xf0);
    assert(rd(GICV3_ICC_IAR1) == 996);
    wr(GICV3_ICC_EOIR1, 996);
    assert(fx.deactivated == 996 && rd(GICV3_ICC_RPR) == 0xff);
    pass("windows_like");
}

/*
 * gicv3_timer_gate(): the generic timer's output (Arm DDI 0487: asserted while
 * ENABLE = 1, ISTATUS = 1, IMASK = 0) with the carrier's own IMASK hold told apart
 * from the guest's.
 */
#define T_EN GICV3_TIMER_CTL_ENABLE
#define T_IM GICV3_TIMER_CTL_IMASK
#define T_IS GICV3_TIMER_CTL_ISTATUS

static void case_timer_gate(void)
{
    struct gicv3_timer_gate g;

    /* Disabled, or enabled and not expired: nothing, whatever IMASK or the hold says. */
    for (int held = 0; held < 2; held++) {
        g = gicv3_timer_gate(0, held);
        assert(!g.asserted && !g.set_imask && !g.held);
        g = gicv3_timer_gate(T_EN, held);
        assert(!g.asserted && !g.set_imask && !g.clear_imask && !g.held);
        g = gicv3_timer_gate(T_IS, held); /* ISTATUS with ENABLE clear: not a timer event */
        assert(!g.asserted && !g.held);
    }
    /* Fired and unmasked: asserted, the carrier masks the source and holds it. */
    g = gicv3_timer_gate(T_EN | T_IS, false);
    assert(g.asserted && g.set_imask && !g.clear_imask && g.held);
    /* Next exit, still expired, IMASK is the carrier's: still asserted, no write. */
    g = gicv3_timer_gate(T_EN | T_IM | T_IS, true);
    assert(g.asserted && !g.set_imask && !g.clear_imask && g.held);
    /* M3 run 2: the guest's own ENABLE | IMASK over an expired CVAL (CTL 7). Not asserted. */
    g = gicv3_timer_gate(T_EN | T_IM | T_IS, false);
    assert(!g.asserted && !g.set_imask && !g.clear_imask && !g.held);
    /* Guest moved CVAL forward under the hold: level drops, the carrier unmasks again. */
    g = gicv3_timer_gate(T_EN | T_IM, true);
    assert(!g.asserted && !g.set_imask && g.clear_imask && !g.held);
    /* Guest rewrote CTL (IMASK clear) under the hold: the hold is gone. */
    g = gicv3_timer_gate(T_EN, true);
    assert(!g.asserted && !g.clear_imask && !g.held);
    g = gicv3_timer_gate(T_EN | T_IS, true);
    assert(g.asserted && g.set_imask && g.held);
    /* Guest stopped the timer (CTL 2) under the hold: not asserted, hold released. */
    g = gicv3_timer_gate(T_IM, true);
    assert(!g.asserted && g.clear_imask && !g.held);
    pass("timer_gate");
}

/*
 * The carrier loop of hv_t8152_gic_timer_update(), replayed against the model: on
 * every "exit" the level is sampled and INTID 18 is pending exactly while it is high
 * and enabled.  The guest sequence: CTL = ENABLE | IMASK before any CVAL; then CVAL
 * and CTL = ENABLE; each tick moves CVAL forward before EOI.
 */
static u64 hw_ctl;
static bool hw_held;
static bool ppi_enabled;

static void exit_sample(void)
{
    struct gicv3_timer_gate g = gicv3_timer_gate(hw_ctl, hw_held);

    if (g.set_imask)
        hw_ctl |= T_IM;
    if (g.clear_imask)
        hw_ctl &= ~(u64)T_IM;
    hw_held = g.held;
    if (g.asserted && ppi_enabled)
        gicv3_cpuif_set_pending(&cpu, 18);
    else
        gicv3_cpuif_clear_pending(&cpu, 18);
}

/* The counter passing CVAL (ISTATUS follows ENABLE and the compare, not IMASK). */
static void expire(bool expired)
{
    hw_ctl = expired ? (hw_ctl | T_IS) : (hw_ctl & ~(u64)T_IS);
}

static void case_timer_level_windows(void)
{
    fresh();
    prio_table[18] = 0xd0;
    hw_ctl = 0;
    hw_held = false;
    ppi_enabled = false;
    enable(0xf0);

    /* Run 2's trigger: CTL = ENABLE | IMASK over an expired CVAL. Never signalled. */
    hw_ctl = T_EN | T_IM;
    expire(true);
    ppi_enabled = true;
    for (int i = 0; i < 100; i++) {
        exit_sample();
        assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    }
    assert(hw_ctl == (T_EN | T_IM | T_IS) && !hw_held);

    /* CVAL in the future, CTL = ENABLE. Quiet until it expires. */
    expire(false);
    hw_ctl = T_EN;
    exit_sample();
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);

    for (int tick = 0; tick < 3; tick++) {
        expire(true);
        exit_sample(); /* the timer's EL2 FIQ */
        assert(hw_ctl & T_IM);
        assert(rd(GICV3_ICC_IAR1) == 18);
        exit_sample(); /* an HVC inside the ISR before the acknowledge: active and pending */
        assert(gicv3_cpuif_is_pending(&cpu, 18) && gicv3_cpuif_is_active(&cpu, 18));
        expire(false); /* the tick moves CVAL forward, no CTL write */
        exit_sample(); /* next exit: level low, carrier unmasks, pending cleared */
        assert(!(hw_ctl & T_IM) && !gicv3_cpuif_is_pending(&cpu, 18));
        wr(GICV3_ICC_EOIR1, 18);
        exit_sample();
        /* No stale duplicate after EOI. */
        assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    }

    /* A disabled PPI is not signalled even while asserted, and is once enabled. */
    ppi_enabled = false;
    expire(true);
    exit_sample();
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == -1);
    ppi_enabled = true;
    exit_sample();
    assert(gicv3_cpuif_deliverable(&cpu, prio, prio_table) == 18);
    pass("timer_level_windows");
}

int main(void)
{
    case_reset_values();
    case_group1_disabled();
    case_pmr_mask();
    case_ack_eoi();
    case_active_and_pending();
    case_preemption_nesting();
    case_binary_point();
    case_eoimode1();
    case_special_and_group0();
    case_ties_and_range();
    case_power_on();
    case_sgi1r_targets();
    case_concurrent_pending();
    case_windows_like();
    case_timer_gate();
    case_timer_level_windows();
    return 0;
}
