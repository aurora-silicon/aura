/* SPDX-License-Identifier: MIT */

#ifndef HV_GIC_CPUIF_H
#define HV_GIC_CPUIF_H

#include "types.h"

/*
 * A software GICv3 CPU interface: the per-PE ICC_*_EL1 register file and its
 * acknowledge / priority-drop / deactivate state machine, kept entirely in
 * memory.  It is pure logic (no system-register access, no locking, no AIC):
 * the carrier feeds it trapped guest accesses and pending interrupts, and asks
 * it whether the virtual IRQ line (HCR_EL2.VI) must be asserted.
 *
 * Used on T8152 (J804), where the guest runs with ICH_HCR_EL2 = En | TC | TALL0
 * | TALL1 and HCR_EL2.IMO/FMO set.  Phase 3 measurement (f), "trapall", on E, P
 * and M cores and the boot CPU, showed ICC_PMR_EL1, ICC_CTLR_EL1, ICC_IAR1_EL1
 * and ICC_IGRPEN1_EL1 trapping to EL2 with EC 0x18 and ICC_SRE_EL1 not
 * trapping.  That the other ICC_* registers (EOIR1, DIR, HPPIR1, BPR0/1, RPR,
 * AP0Rn/AP1Rn, IGRPEN0, the Group 0 twins and the SGI generation registers)
 * trap as well follows from IHI 0069 for those trap bits; it was not measured.
 *
 * Architecture references (public): Arm IHI 0069 (GIC architecture
 * specification, GICv3/v4), the ICC_*_EL1 register descriptions and the
 * chapters on interrupt prioritization (priority mask, running priority,
 * preemption and the binary point) and on priority drop and deactivation
 * (ICC_CTLR_EL1.EOImode).  Encodings are the AArch64 system-register encodings
 * of the ICC_* registers in Arm DDI 0487 (Arm ARM).
 *
 * Model choices, all within what IHI 0069 permits:
 *  - 5 priority bits and 5 preemption bits, as the hardware reports
 *    (ICH_VTR_EL2 0x90580007: PRIbits = PREbits = 0b100), so priority values
 *    are compared on bits [7:3] and one active-priority register per group
 *    (AP0R0, AP1R0) holds the 32 preemption levels; AP0R1-3 and AP1R1-3 are
 *    RAZ/WI;
 *  - 16 INTID bits, no extended ranges, no SEIs, Aff3 not valid (CTLR reads
 *    PRIbits 4, IDbits 0, SEIS 0, A3V 0, RSS 0, ExtRange 0);
 *  - one security state (the carrier's distributor reports GICD_CTLR.DS = 1)
 *    and every interrupt is delivered as Group 1.  Group 0 would be signalled
 *    as FIQ, which Windows must never receive, so IAR0/HPPIR0 read 1023 and
 *    ICC_SGI0R_EL1 / ICC_ASGI1R_EL1 generate nothing;
 *  - no NMIs (FEAT_GICv3_NMI): ICC_NMIAR1_EL1 reads the spurious INTID 1023,
 *    never INTID 0, should a guest read it anyway;
 *  - INTIDs 0..1019 (SGIs, PPIs, SPIs).  No LPIs.
 */

#define GICV3_CPUIF_NR_INTID  1020
#define GICV3_CPUIF_WORDS     ((GICV3_CPUIF_NR_INTID + 31) / 32)
#define GICV3_SPURIOUS_INTID  1023
#define GICV3_CPUIF_PRI_BITS  5
#define GICV3_CPUIF_PRI_SHIFT (8 - GICV3_CPUIF_PRI_BITS)
#define GICV3_CPUIF_PRI_MASK  ((u8)(0xff << GICV3_CPUIF_PRI_SHIFT))
/* Minimum binary points for 5 preemption bits (IHI 0069, ICC_BPR0_EL1/ICC_BPR1_EL1). */
#define GICV3_CPUIF_BPR0_MIN (7 - GICV3_CPUIF_PRI_BITS)
#define GICV3_CPUIF_BPR1_MIN (GICV3_CPUIF_BPR0_MIN + 1)
#define GICV3_CPUIF_IDLE_PRI 0xff

/* ICC_SRE_EL1 as the hardware reads it on T8152 (measurement (f)): SRE | DFB | DIB. */
#define GICV3_CPUIF_SRE_VALUE 0x7

/*
 * ICC_* registers are all op0 = 3, op1 = 0.  The model keys them by
 * (CRn << 7) | (CRm << 3) | op2.
 */
#define GICV3_CPUIF_REG(crn, crm, op2) (((u32)(crn) << 7) | ((u32)(crm) << 3) | (u32)(op2))

#define GICV3_ICC_PMR      GICV3_CPUIF_REG(4, 6, 0)
#define GICV3_ICC_IAR0     GICV3_CPUIF_REG(12, 8, 0)
#define GICV3_ICC_EOIR0    GICV3_CPUIF_REG(12, 8, 1)
#define GICV3_ICC_HPPIR0   GICV3_CPUIF_REG(12, 8, 2)
#define GICV3_ICC_BPR0     GICV3_CPUIF_REG(12, 8, 3)
#define GICV3_ICC_AP0R(n)  GICV3_CPUIF_REG(12, 8, 4 + (n))
#define GICV3_ICC_AP1R(n)  GICV3_CPUIF_REG(12, 9, (n))
#define GICV3_ICC_NMIAR1   GICV3_CPUIF_REG(12, 9, 5)
#define GICV3_ICC_DIR      GICV3_CPUIF_REG(12, 11, 1)
#define GICV3_ICC_RPR      GICV3_CPUIF_REG(12, 11, 3)
#define GICV3_ICC_SGI1R    GICV3_CPUIF_REG(12, 11, 5)
#define GICV3_ICC_ASGI1R   GICV3_CPUIF_REG(12, 11, 6)
#define GICV3_ICC_SGI0R    GICV3_CPUIF_REG(12, 11, 7)
#define GICV3_ICC_IAR1     GICV3_CPUIF_REG(12, 12, 0)
#define GICV3_ICC_EOIR1    GICV3_CPUIF_REG(12, 12, 1)
#define GICV3_ICC_HPPIR1   GICV3_CPUIF_REG(12, 12, 2)
#define GICV3_ICC_BPR1     GICV3_CPUIF_REG(12, 12, 3)
#define GICV3_ICC_CTLR     GICV3_CPUIF_REG(12, 12, 4)
#define GICV3_ICC_SRE      GICV3_CPUIF_REG(12, 12, 5)
#define GICV3_ICC_IGRPEN0  GICV3_CPUIF_REG(12, 12, 6)
#define GICV3_ICC_IGRPEN1  GICV3_CPUIF_REG(12, 12, 7)

/* ICC_CTLR_EL1 fields (IHI 0069). */
#define GICV3_CTLR_CBPR         (1u << 0)
#define GICV3_CTLR_EOIMODE      (1u << 1)
#define GICV3_CTLR_PRIBITS_SHIFT 8

struct gicv3_cpuif {
    /*
     * Pending and active state per INTID.  Other CPUs set pending bits (SGIs),
     * so every bitmap access is atomic; everything else belongs to the PE that
     * owns this interface and is only touched from its own traps.
     */
    u32 pending[GICV3_CPUIF_WORDS];
    u32 active[GICV3_CPUIF_WORDS];
    /* Active priorities, one bit per preemption level (ICC_AP0R0 / ICC_AP1R0). */
    u32 ap0r0;
    u32 ap1r0;
    u8 pmr;
    u8 bpr0;
    u8 bpr1;
    bool eoimode;
    bool cbpr;
    bool igrpen0;
    bool igrpen1;
    bool valid;
};

/* The guest-programmed priority of an INTID on this PE (GICR/GICD_IPRIORITYR). */
typedef u8 (*gicv3_cpuif_priority_fn)(void *opaque, u32 intid);

struct gicv3_cpuif_effect {
    s32 acknowledged;    /* INTID moved pending -> active by this access, or -1 */
    s32 deactivated;     /* INTID whose active state this access cleared, or -1 */
    bool sgi1r;          /* an ICC_SGI1R_EL1 write: the carrier routes it */
    u64 sgi1r_value;
    bool igrpen1_enabled; /* this access turned Group 1 on (0 -> 1) */
};

/* Architectural reset state; pending and active state cleared. */
void gicv3_cpuif_reset(struct gicv3_cpuif *c);
/*
 * The PE is starting again (PSCI CPU_ON): the CPU interface loses its state,
 * but interrupts already pending at its redistributor stay pending.
 */
void gicv3_cpuif_power_on(struct gicv3_cpuif *c);

bool gicv3_cpuif_set_pending(struct gicv3_cpuif *c, u32 intid);
void gicv3_cpuif_clear_pending(struct gicv3_cpuif *c, u32 intid);
bool gicv3_cpuif_is_pending(const struct gicv3_cpuif *c, u32 intid);
bool gicv3_cpuif_is_active(const struct gicv3_cpuif *c, u32 intid);

/* Highest-priority interrupt that is pending and not active, or -1. */
int gicv3_cpuif_hppi(const struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio, void *opaque);
/*
 * The INTID a read of ICC_IAR1_EL1 would acknowledge now, or -1: Group 1
 * enabled, and the HPPI's priority higher than ICC_PMR_EL1 and its group
 * priority higher than the running priority.  The carrier asserts HCR_EL2.VI
 * exactly while this is not -1.
 */
int gicv3_cpuif_deliverable(const struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio,
                            void *opaque);
/* ICC_RPR_EL1: the priority of the highest active preemption level, or 0xff. */
u8 gicv3_cpuif_running_priority(const struct gicv3_cpuif *c);

/*
 * One trapped guest access to the register keyed `reg` (GICV3_ICC_*).  For a
 * read the result is stored in *value; for a write *value is the guest's
 * value.  Returns false for an encoding the model does not implement (the
 * caller decides).  A read of a write-only register reads 0 and a write to a
 * read-only register is ignored.
 */
bool gicv3_cpuif_access(struct gicv3_cpuif *c, gicv3_cpuif_priority_fn prio, void *opaque, u32 reg,
                        bool is_read, u64 *value, struct gicv3_cpuif_effect *fx);

/*
 * Does an ICC_SGI1R_EL1 write of `sgi1r` target the PE with this MPIDR?
 * IRM = 1 is "every PE but the sender"; otherwise Aff3.Aff2.Aff1 must match and
 * Aff0 must be in the target list at range selector RS (IHI 0069, ICC_SGI1R_EL1).
 */
bool gicv3_sgi1r_targets(u64 sgi1r, u64 mpidr, bool is_sender);
u32 gicv3_sgi1r_intid(u64 sgi1r);

#endif
