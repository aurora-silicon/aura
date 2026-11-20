/* SPDX-License-Identifier: MIT */

#ifndef HV_VDEV_SPI_H
#define HV_VDEV_SPI_H

#include "types.h"

/*
 * Shared peripheral interrupts of devices the carrier emulates in software (the D1 AHCI disk
 * and the I0 input device), on the T8152 software GICv3 CPU interface.  Such an SPI has no AIC
 * line behind it: its level is whatever the device model says, and the carrier makes it
 * pending on one CPU interface exactly while that level is high and the guest has enabled it.
 *
 * Pure decisions only (no system registers, no locks, no AIC); the carrier glue that marks
 * the interrupt pending and kicks the CPU is hv_t8152_gic_spi_level() in hv_exc.c.
 *
 * References: Arm IHI 0069 (GICv3), GICD_IROUTER<n> (Interrupt_Routing_Mode, Aff3..Aff0),
 * GICD_ISENABLER<n>, and the rules for level-sensitive interrupts (an interrupt stays
 * pending while its input is asserted; acknowledging it makes it active and pending).
 */

/* The AHCI disk's SPI: GSIV 990, level, active-high.  No AIC line; never an AIC alias. */
#define HV_VDEV_SPI_AHCI 990u
/* I0: the input device's SPI: GSIV 991, level, active-high.  No AIC line; never an alias. */
#define HV_VDEV_SPI_INPUT 991u
/* G6-S: the scanout service's SPI (vblank, flip done, flip error): GSIV 992, level. */
#define HV_VDEV_SPI_SCANOUT 992u

/*
 * The devices behind them, as a mask of the configure flags that are set.  An emulated SPI is
 * live only while its own device is configured, so a launch without the input device runs
 * exactly the D1 code for 990 and never touches 991 (and the reverse).
 */
#define VDEV_SPI_DEV_AHCI  BIT(0)
#define VDEV_SPI_DEV_INPUT BIT(1)
#define VDEV_SPI_DEV_SCANOUT BIT(2)

/* GICD_IROUTER<n> fields (IHI 0069). */
#define VDEV_SPI_IROUTER_IRM BIT(31)
#define VDEV_SPI_AFF_MASK    0xff00ffffffUL /* Aff3 [39:32], Aff2..Aff0 [23:0] */

/*
 * The CPU index an SPI with routing register `irouter` is delivered to.
 *  - IRM = 1 (any participating PE): `self`, the CPU whose action changed the level, if it is
 *    a candidate, else `fallback`;
 *  - IRM = 0: the candidate whose MPIDR affinity equals IROUTER's Aff3.Aff2.Aff1.Aff0, else
 *    `fallback` (IHI 0069 leaves an unmatched route UNKNOWN; this model never drops it).
 * Candidates are the CPU indices i < ncpu with bit i of `candidates` set; mpidr[i] is the
 * MPIDR of CPU i.  `fallback` must itself be a candidate; -1 if there is none.
 */
int vdev_spi_route(u64 irouter, int self, int fallback, const u64 *mpidr, u64 candidates, int ncpu);

/* Is the SPI to be pending: its input asserted and the guest has it enabled at the GICD. */
bool vdev_spi_should_pend(bool level, bool enabled);

/*
 * What to do with an SPI's per-CPU state after its level, enable or routing may have changed.
 * A GICv3 SPI has one state machine: it is pending on at most one PE, and while it is active
 * on a PE it is not signalled anywhere else (IHI 0069, interrupt handling state machine; an
 * asserted level-sensitive interrupt that is active is "active and pending" and is delivered
 * again after deactivation).  Bit i of `pending` / `active` is CPU i's interface state.
 *  - not to be pending (`pend` false): clear it everywhere, raise nowhere;
 *  - active on some CPU: raise nowhere, clear any pending copy on a CPU where it is not
 *    active (the deactivation re-applies the level and delivers it then);
 *  - otherwise: raise on `target` (if >= 0) and clear it on every other CPU.
 */
struct vdev_spi_plan {
    int raise;   /* CPU to make it pending on, or -1 */
    u64 clear;   /* CPUs to clear the pending state on (never includes `raise`) */
};
struct vdev_spi_plan vdev_spi_plan(bool pend, int target, u64 pending, u64 active);

/* Is INTID a carrier-emulated SPI (as opposed to an AIC alias or a PPI/SGI)? */
bool vdev_spi_is_virtual(u32 intid);

/* The VDEV_SPI_DEV_* device that owns a carrier-emulated SPI; 0 for any other INTID. */
u32 vdev_spi_device(u32 intid);

/* Is INTID a carrier-emulated SPI whose device is in `devices` (VDEV_SPI_DEV_* mask)? */
bool vdev_spi_live(u32 intid, u32 devices);

/* The carrier-emulated SPIs, for the re-evaluation loops (claim, distributor writes). */
#define VDEV_SPI_COUNT 3
extern const u32 vdev_spi_intids[VDEV_SPI_COUNT];

#endif
