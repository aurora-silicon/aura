/**
 * Copyright (c) 2025, NTASP authors.
 *
 * Module Name:
 *     hv_vgic.c
 *
 * Abstract:
 *     The vGIC virtual device code for the m1n1 hypervisor.
 *
 *
 * Environment:
 *     m1n1 in hypervisor mode.
 *
 * License:
 *     SPDX-License-Identifier: (BSD-2-Clause-Patent OR MIT)
 *
 *     Inspiration borrowed from the KVM vGIC driver in the Linux source tree. Original copyright
 * notice below.
 *
 */

#include "hv.h"
#include "aic.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "heapblock.h"
#include "hv_aic_alias.h"
#include "hv_vgic.h"
#include "malloc.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "usb.h"
#include "utils.h"

/*
 * Turn the AIC on the first time the guest asks for a device interrupt.
 *
 * The controller is off at this point: iBoot hands over with it clear and Mu's
 * ExitBootServices callback clears it again, so a line can be unmasked and
 * asserted -- both observable in MASK_SET and HW_STATE -- and still reach no
 * CPU at all.  That was the state J813's keyboard sat in with its driver
 * started and its FIFO full.
 *
 * A GICD_ISENABLER write for an SPI is the right trigger: it is the guest
 * saying it wants a specific device line delivered, which on this machine is
 * also the moment m1n1 becomes responsible for delivering it.  Doing it here
 * rather than at hv_start() keeps the controller off for the whole of Mu, which
 * owns its own interrupts and must not have them redirected to EL2.
 *
 * SGIs and PPIs deliberately do not arm it.  They are not AIC lines.
 */
static bool hv_vgic_aic_armed = false;

static void hv_vgic_arm_device_delivery(u32 irq_num)
{
    if (hv_vgic_aic_armed || irq_num < 32 || chip_id != T8142)
        return;

    hv_vgic_aic_armed = true;
    aic_set_enabled(true);
    printf("HV vGIC: guest enabled SPI %u; AIC master enable now %d "
           "(hcr 0x%lx on CPU %d)\n",
           irq_num, (int)aic_is_enabled(), mrs(HCR_EL2), (int)smp_id());
}

/**
 * General idea of how this should work:
 *
 * Apple Silicon chips since the M1 implement the GIC CPU interface registers in hardware, meaning
 * only the distributor, the core specific redistributors, and (potentially) an ITS need to be
 * emulated by m1n1.
 *
 * As such, this file implements most of the code needed to make this possible. The emulated
 * distributor/redistributors will need to meet a few constraints (namely it's limited by what the
 * GIC CPU interface supports)
 *
 * Apple's vGIC CPU interface has the following characteristics (on M1 and M2):
 * - 32 levels of virtual priority and preemption priority (5 preemption/priority bits)
 * - 16 bits of virtual interrupt ID bits (meaning up to 65535 interrupts are supported
 * theoretically, however practically limited by the number of IRQs the AIC supports)
 * - supports guest-generated SEIs upon writing to GIC registers in a bad way
 *   (note that an errata here exists on pre-M3 SoCs that can result in a host SError - we implement
 * special handling for this.)
 * - 3 level affinity (aff2/aff1/aff0 valid, aff3 invalid/reserved as 0)
 * - legacy operation is not supported (ICC_SRE_EL2.SRE is reserved, set to 1) (no GICv2 operations)
 * - TDIR bit is supported (FEAT_GICv3_TDIR)
 * - extended SPI and PPI ranges are *not* supported on M1/M2 (and their Pro counterparts, even if
 * the SoC itself has > 16 cores)
 * - 8 list registers
 * - direct injection of virtual interrupts are not supported (not a GICv4, and by extension, no
 * NMIs supported)
 * - IRQ/FIQ bypass are not supported
 *
 *
 * The mappings are different for platforms with 36-bit vs 42-bit physical addressing, with 36-bit
 * platforms tentatively having the distributor being mapped to 0xF00000000, redistributors at
 * offset +0x10000000 and 42-bit platforms having the distributor at 0x5000000000, redistributors at
 * offset +0x100000000
 *
 * A major note about processor affinities: since AICv2 platforms don't support setting core
 * affinities easily, the tentative solution is to do routing to any virtual CPU once we receive an
 * IRQ, we can't assume that the core that got the IRQ is the one that needs to be signaled. (for
 * FIQs, because they're core specific, we'll know which core needs to be signaled in those cases.)
 *
 * windows-native-aic (branch windows-native-aic, config.h: ENABLE_NATIVE_AIC_PASSTHROUGH):
 * when that flag is on, the GICD/GICR/ITS hooks remain the firmware and early-Windows
 * startup carrier. hv_aic.c observes the real AIC2 CONFIG enable write after the HAL
 * extension replaces the carrier callbacks; each CPU then disables its virtual CPU
 * interface and clears HCR_EL2.IMO before native AIC delivery begins.
 *
 */
#ifdef ENABLE_VGIC_MODULE
#define DIST_BASE_36_BIT   0xF00000000
#define REDIST_BASE_36_BIT 0xF10000000
#define DIST_BASE_42_BIT   0x5000000000
#define REDIST_BASE_42_BIT 0x5100000000
//
// This is tentative - depends on if direct MSIs or ITS translated IRQs end up being easier to
// implement.
//
#define ITS_BASE_36_BIT 0xF20000000
#define ITS_BASE_42_BIT 0x5200000000

#ifndef ENABLE_VGIC_LOGGING
#define ENABLE_VGIC_LOGGING 0
#endif

#if ENABLE_VGIC_LOGGING
#define vgic_log(...) printf(__VA_ARGS__)
#else
#define vgic_log(...)                                                                              \
    do {                                                                                           \
    } while (0)
#endif

vgicv3_dist *distributor;
vgicv3_vcpu_redist *redistributors;
vgicv3_its *interrupt_translation_service;
static u64 dist_base, redist_base, its_base;
static u16 num_cpus;
/* Guest GICR frames are dense, but Apple ADT CPU IDs can be sparse. */
static u8 redist_cpu_ids[MAX_CPUS];
static bool vgic_inited;
/* ICC_IGRPEN1_EL1 is banked per PE, not distributor-global state. */
static u64 igrpen1[MAX_CPUS];
/*
 * Number of implemented ICH_LR<n>_EL2 list registers. Derived at init from
 * ICH_VTR_EL2.ListRegs instead of assuming eight: touching an unimplemented LR
 * faults, and scanning eight when fewer exist is wasted hot-path work. Clamped
 * to eight because hv_vgic3_read_lr/write_lr only encode ICH_LR0..ICH_LR7.
 */
static u32 vgic_nr_lrs = 8;

static bool handle_vgic_its_access(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 relative_addr;
    bool register_handled;
    bool unimplemented_reg_accessed;
    u32 reg_num;
    relative_addr = addr - its_base;
    register_handled = false;
    unimplemented_reg_accessed = false;
    reg_num = 0;
    if (write) {
        switch (relative_addr) {
            case GITS_CTLR:
                interrupt_translation_service->its_ctl_region.gits_ctl_reg = *val;
                register_handled = true;
                break;
            case GITS_BASER0 ... GITS_BASER7:
                reg_num = (relative_addr - GITS_BASER0) / 8;
                interrupt_translation_service->its_ctl_region.gits_baser[reg_num] = *val;
                register_handled = true;
                break;
            default:
                //
                // we're dealing with a register that is banked n times, we need to get to the if
                // statements.
                //
                break;
        }

    } else {
        switch (relative_addr) {
            case GITS_CTLR:
                *val = interrupt_translation_service->its_ctl_region.gits_ctl_reg;
                register_handled = true;
                break;
            case GITS_BASER0 ... GITS_BASER7:
                reg_num = (relative_addr - GITS_BASER0) / 8;
                *val = interrupt_translation_service->its_ctl_region.gits_baser[reg_num];
                register_handled = true;
                break;
            default:
                //
                // we're dealing with a register that is banked n times, we need to get to the if
                // statements.
                //
                break;
        }
    }

    vgic_log("HV vGIC DEBUG [INFO] [ITS]: 0x%llx = 0x%llx ", relative_addr, *val);
    if (write) {
        vgic_log("[Written]");
    } else {
        vgic_log("[Read]");
    }
    if (unimplemented_reg_accessed) {
        vgic_log("[Unimplemented]\n");
    } else {
        vgic_log("\n");
    }
    return register_handled;
}

//
// Description:
//   the vGIC guest interrupt handler for distributor writes.
//
// Return values:
//   true - access has been handled successfully, even if the access itself is either bad or not
//   permitted. false - access was not handled successfully.
//
static bool handle_vgic_dist_access(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 relative_addr;
    bool register_handled;
    bool unimplemented_reg_accessed;
    relative_addr = addr - dist_base;
    register_handled = false;
    unimplemented_reg_accessed = false;
    if (write) {
        //
        // The guest attempted to write a register.
        // Handle it based on what they're trying to write, and preserve the value if
        // the value is going to a RW register.
        // Emit a warning (to become an error later) if the guest is attempting to write a register
        // that doesn't exist or is read only.
        //

        //
        // This switch statement covers all the unique one of a kind registers.
        //
        switch (relative_addr) {
            case GIC_DIST_CTLR:
                //
                // GICD_CTLR has fields that we cannot change (due to the underlying physical
                // environment or constraints) and fields we can change, so check for RO fields here
                // first.
                //
                u32 gicd_ctlr_new_val = (u32)(*val);
                vgic_log("HV vGIC DEBUG: guest writing GICD_CTLR = 0x%x, old value 0x%x\n",
                         gicd_ctlr_new_val, distributor->gicd_ctl_reg);
                bool is_rwp_to_be_set = false;
                if (((gicd_ctlr_new_val & GENMASK(30, 8)) != 0) ||
                    ((gicd_ctlr_new_val & BIT(5)) != 0) ||
                    ((gicd_ctlr_new_val & GENMASK(3, 2)) != 0)) {
                    //
                    // these bits are RES0 - clear out this bitmask.
                    //
                    gicd_ctlr_new_val &= ~(GENMASK(30, 8));
                    gicd_ctlr_new_val &= ~(GENMASK(3, 2));
                    gicd_ctlr_new_val &= ~(BIT(5));
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to write RES0 bits in "
                             "GICD_CTLR, discarding\n");
                }

                if ((gicd_ctlr_new_val & BIT(6)) == 0) {
                    //
                    // the guest is trying to set DS = 0. we do not support this so ensure that bit
                    // 6 is always written, however we need to emit a warning because this means
                    // that our GIC configuration is wrong.
                    //
                    gicd_ctlr_new_val |= BIT(6);
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to set DS = 0, discarding\n");
                }
                if ((gicd_ctlr_new_val & BIT(4)) == 0) {
                    //
                    // the guest is trying to set ARE = 0. we do not support this so ensure that bit
                    // 4 is always written, however we need to emit a warning because this means
                    // that our GIC configuration is wrong.
                    //
                    gicd_ctlr_new_val |= BIT(4);
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to set ARE = 0, discarding\n");
                }
                if ((((gicd_ctlr_new_val & BIT(7)) != 0) &&
                     ((distributor->gicd_ctl_reg & BIT(7)) == 0)) ||
                    (((gicd_ctlr_new_val & BIT(7)) == 0) &&
                     ((distributor->gicd_ctl_reg & BIT(7)) != 0))) {
                    //
                    // the guest is trying to set EN1WF either way. we need to know about any
                    // attempt to change this, as it affects IRQ behavior. we also need to flag that
                    // RWP needs to be set to 1.
                    //
                    is_rwp_to_be_set = true;
                    vgic_log("HV vGIC DEBUG [INFO]: guest is changing EN1WF\n");
                }
                if (((gicd_ctlr_new_val & BIT(1)) == 0) &&
                    ((distributor->gicd_ctl_reg & BIT(1)) != 0)) {
                    //
                    // the guest is trying to set EnableGrp1 = 0. we need to know about any attempt
                    // to set this, as it affects IRQ behavior. we also need to flag that RWP needs
                    // to be set to 1.
                    //
                    is_rwp_to_be_set = true;
                    vgic_log("HV vGIC DEBUG [INFO]: guest is setting EnableGrp1 = 0\n");
                }
                if (((gicd_ctlr_new_val & BIT(0)) == 0) &&
                    ((distributor->gicd_ctl_reg & BIT(0)) != 0)) {
                    //
                    // the guest is trying to set EnableGrp1 = 0. we need to know about any attempt
                    // to set this, as it affects IRQ behavior. we also need to flag that RWP needs
                    // to be set to 1.
                    //
                    is_rwp_to_be_set = true;
                    vgic_log("HV vGIC DEBUG [INFO]: guest is setting EnableGrp0 = 0\n");
                }

                //
                // RWP (Register Write Pending bit) - this bit is a tad bit special - it's RO, but
                // it has to be set if bits 0 or 1 are transitioning from 1 to 0.
                //
                if (is_rwp_to_be_set == true) {
                    //
                    // set RWP here - then start propagating the effects immediately after.
                    //
                    gicd_ctlr_new_val |= BIT(31);
                }

                distributor->gicd_ctl_reg = gicd_ctlr_new_val;
                if (is_rwp_to_be_set == true) {
                    //
                    // TODO: start the changes signaled by RWP.
                    //
                    // hv_vgicv3_apply_gic_dist_changes(gicd_ctlr_new_val);
                }
                register_handled = true;
                break;
            case GIC_DIST_TYPER:
            case GIC_DIST_TYPER2:
            case GIC_DIST_IIDR:
                //
                // these registers are totally RO, so leave their values unchanged.
                //
                vgic_log("HV vGIC DEBUG [WARN]: guest attempted to change a read-only register "
                         "(0x%x), discarding\n",
                         relative_addr);
                register_handled = true;
                break;
            case GIC_DIST_STATUSR:
                //
                // GICD_STATUSR is a bit special, software must write 1 to ack an error, which then
                // *clears* the bit. Note that [31:4] are always RES0.
                //
                u32 gicd_statusr_new_val = (u32)(*val);
                u32 gicd_statusr_current_val = distributor->gicd_err_sts;
                if ((gicd_statusr_new_val & GENMASK(31, 4)) != 0) {
                    gicd_statusr_new_val &= ~(GENMASK(31, 4));
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to write RES0 bits in "
                             "GICD_STATUSR, discarding\n");
                }
                if (((gicd_statusr_new_val & BIT(3)) != 0) &
                    ((gicd_statusr_current_val & BIT(3)) != 0)) {
                    gicd_statusr_current_val &= ~(BIT(3));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing WROD bit in GICD_STATUSR\n");
                }
                if (((gicd_statusr_new_val & BIT(2)) != 0) &
                    ((gicd_statusr_current_val & BIT(2)) != 0)) {
                    gicd_statusr_current_val &= ~(BIT(2));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing RWOD bit in GICD_STATUSR\n");
                }
                if (((gicd_statusr_new_val & BIT(1)) != 0) &
                    ((gicd_statusr_current_val & BIT(1)) != 0)) {
                    gicd_statusr_current_val &= ~(BIT(1));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing WRD bit in GICD_STATUSR\n");
                }
                if (((gicd_statusr_new_val & BIT(0)) != 0) &
                    ((gicd_statusr_current_val & BIT(0)) != 0)) {
                    gicd_statusr_current_val &= ~(BIT(0));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing RRD bit in GICD_STATUSR\n");
                }
                distributor->gicd_err_sts = gicd_statusr_current_val;
                register_handled = true;
                break;
            //
            // right now, MBIS is disabled - so these four registers are reserved.
            //
            case GIC_DIST_SETSPI_NSR:
            case GIC_DIST_CLRSPI_NSR:
            case GIC_DIST_CLRSPI_SR:
            case GIC_DIST_SETSPI_SR:
                register_handled = true;
                break;

            case GIC_DIST_SGIR:
                //
                // This register is reserved too, since affinity routing is always enabled.
                //
                register_handled = true;
                break;

            case GIC_DIST_IROUTER32 ... GIC_DIST_IROUTER1019:
                u32 reg_num;
                u64 mpidr = 0;
                u32 cpu_num;
                reg_num = (relative_addr - GIC_DIST_IROUTER32) / 8;
                distributor->gicd_interrupt_router_regs[reg_num] = *val;

                mpidr |= (u64)MPIDR_AFF0(*val);
                mpidr |= (u64)MPIDR_AFF1(*val) << 8;
                mpidr |= (u64)MPIDR_AFF2(*val) << 16;
                mpidr |= (u64)MPIDR_AFF3(*val) << 32;
                cpu_num = smp_get_id(mpidr);

                aic_set_affinity(reg_num + 32, cpu_num);
                vgic_log("HV vGIC DEBUG [INFO] [Distributor]: interrupt routing register %d = %d\n",
                         reg_num, cpu_num);
                register_handled = true;
                break;
            default:
                //
                // we're dealing with a register that is banked n times, we need to get to the if
                // statements.
                //
                break;
        }

        //
        // Fair warning this code is probably dicey...
        //
        if ((register_handled == false) && (relative_addr >= GIC_DIST_IGROUPR0) &&
            (relative_addr <= GIC_DIST_IGROUPR31)) {
            //
            // the guest is trying to change the group of a given interrupt.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_IGROUPR0) / 4;

            //
            // TODO: bank GICD_IGROUPR0 for cores 0-7 - GIC spec requires it - but since we're
            // booting with 1 core atm, we can ignore this for now.
            //

            distributor->gicd_interrupt_group_regs[reg_num] = *val;
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISENABLER0) &&
                   (relative_addr <= GIC_DIST_ISENABLER31)) {
            //
            // enables an IRQ to be forwarded to a CPU interface.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ISENABLER0) / 4;
            u32 value_is_enabler, value_ic_enabler, current_val;
            u32 irq_num;
            value_is_enabler = distributor->gicd_interrupt_set_enable_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_enable_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 1 in
            // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //

            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) == 0)) {
                    value_is_enabler |= BIT(i);
                    value_ic_enabler |= BIT(i);
                    irq_num = (32 * reg_num) + i;

                    /*
                     * Only for a line m1n1 actually owns.  The alias helpers
                     * are identity on a miss, so unmasking unconditionally
                     * turned every guest SPI into an unmask of the
                     * same-numbered real AIC line -- see
                     * hv_aic_alias_is_owned().
                     */
                    if (hv_aic_alias_is_owned(irq_num)) {
                        aic_set_mask(hv_aic_alias_to_physical(irq_num), false);
                        hv_vgic_arm_device_delivery(irq_num);
                        vgic_log("HV vGIC DEBUG [Info] [AIC]: unmasking irq %d (physical %d)\n",
                                 irq_num, hv_aic_alias_to_physical(irq_num));
                    }
                }
            }
            if (reg_num == 0) {

            } else {
                distributor->gicd_interrupt_set_enable_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_enable_regs[reg_num] = value_ic_enabler;
            }

            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICENABLER0) &&
                   (relative_addr <= GIC_DIST_ICENABLER31)) {
            //
            // disables an IRQ to be forwarded to a CPU interface.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ICENABLER0) / 4;
            u32 irq_num;
            u32 value_is_enabler, value_ic_enabler, current_val;
            value_is_enabler = distributor->gicd_interrupt_set_enable_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_enable_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 0 in
            // GICD_ISENABLER[0:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //
            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_ic_enabler & BIT(i)) != 0)) {
                    value_is_enabler &= ~BIT(i);
                    value_ic_enabler &= ~BIT(i);
                    irq_num = (32 * reg_num) + i;

                    /*
                     * Mask, not unmask.  This passed `false` -- the same
                     * argument as the ISENABLER path above -- so a guest
                     * disabling an interrupt left the physical line enabled.
                     * That was inert while nothing was ever delivered; now that
                     * SPIs reach the guest it would mean a device the OS has
                     * switched off keeps asserting into a CPU interface that no
                     * longer expects it.
                     */
                    if (hv_aic_alias_is_owned(irq_num)) {
                        aic_set_mask(hv_aic_alias_to_physical(irq_num), true);
                        vgic_log("HV vGIC DEBUG [Info] [AIC]: masking irq %d (physical %d)\n",
                                 irq_num, hv_aic_alias_to_physical(irq_num));
                    }
                }
            }
            if (reg_num == 0) {

            } else {
                distributor->gicd_interrupt_set_enable_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_enable_regs[reg_num] = value_ic_enabler;
            }
            register_handled = true;
            //
            // ICENABLER register writes require RWP dependent things to be updated, set the bit.
            //
            distributor->gicd_ctl_reg |= BIT(31);
            //
            // TODO: propagate the changes
            //

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISPENDR0) &&
                   (relative_addr <= GIC_DIST_ISPENDR31)) {
            //
            // sets an IRQ to pending
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ISPENDR0) / 4;
            u32 value_is_enabler, value_ic_enabler, current_val;
            value_is_enabler = distributor->gicd_interrupt_set_pending_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_pending_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 1 in
            // GICD_ICENABLER[1:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //

            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) == 0)) {
                    value_is_enabler |= BIT(i);
                    value_ic_enabler |= BIT(i);
                    irq_num = (32 * reg_num) + i;
                    //
                    // TODO: do this
                    //
                    vgic_log("HV vGIC DEBUG [ERROR]: ISPENDR not implemented for irq %d\n",
                             irq_num);
                }
            }
            if (reg_num == 0) {
                //
                // don't attempt to write these registers, since affinity routing is always on.
                //
            } else {
                distributor->gicd_interrupt_set_pending_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_pending_regs[reg_num] = value_ic_enabler;
            }
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICPENDR0) &&
                   (relative_addr <= GIC_DIST_ICPENDR31)) {
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ICPENDR0) / 4;
            u32 value_is_enabler, value_ic_enabler, current_val;
            value_is_enabler = distributor->gicd_interrupt_set_pending_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_pending_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 0 in
            // GICD_ISENABLER[0:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //
            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_ic_enabler & BIT(i)) != 0)) {
                    value_is_enabler &= ~BIT(i);
                    value_ic_enabler &= ~BIT(i);
                    irq_num = (32 * reg_num) + i;
                    //
                    // TODO: do this
                    //
                    vgic_log("HV vGIC DEBUG [ERROR]: ICPENDR not implemented for irq %d\n",
                             irq_num);
                }
            }
            if (reg_num == 0) {

            } else {
                distributor->gicd_interrupt_set_pending_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_pending_regs[reg_num] = value_ic_enabler;
            }
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISACTIVER0) &&
                   (relative_addr <= GIC_DIST_ISACTIVER31)) {
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ISACTIVER0) / 4;
            u32 value_is_enabler, value_ic_enabler, current_val;
            value_is_enabler = distributor->gicd_interrupt_set_active_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_active_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 0 in
            // GICD_ISACTIVER[0:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //
            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_ic_enabler & BIT(i)) == 0)) {
                    value_is_enabler &= ~BIT(i);
                    value_ic_enabler &= ~BIT(i);
                    irq_num = (32 * reg_num) + i;
                    //
                    // TODO: do this
                    //
                    vgic_log("HV vGIC DEBUG [ERROR]: ISACTIVER not implemented for irq %d\n",
                             irq_num);
                }
            }
            if (reg_num == 0) {

            } else {
                distributor->gicd_interrupt_set_active_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_active_regs[reg_num] = value_ic_enabler;
            }
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICACTIVER0) &&
                   (relative_addr <= GIC_DIST_ICACTIVER31)) {
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ICACTIVER0) / 4;
            u32 value_is_enabler, value_ic_enabler, current_val;
            value_is_enabler = distributor->gicd_interrupt_set_active_regs[reg_num];
            value_ic_enabler = distributor->gicd_interrupt_clear_active_regs[reg_num];
            current_val = *val;

            //
            // if 1 is written to the bits in these registers, they need to read 0 in
            // GICD_ISACTIVER[0:31] as well. also this is banked for the first 8 processor cores -
            // so changes must reflect across all of them.
            //
            // There has to be a way more efficient way of doing this...
            //
            for (u32 i = 0; i < 32; i++) {
                if (((current_val & BIT(i)) != 0) && ((value_ic_enabler & BIT(i)) != 0)) {
                    value_is_enabler &= ~BIT(i);
                    value_ic_enabler &= ~BIT(i);
                    irq_num = (32 * reg_num) + i;
                    //
                    // TODO: do this
                    //
                    vgic_log("HV vGIC DEBUG [ERROR]: ICACTIVER not implemented for irq %d\n",
                             irq_num);
                }
            }
            if (reg_num == 0) {

            } else {
                distributor->gicd_interrupt_set_active_regs[reg_num] = value_is_enabler;
                distributor->gicd_interrupt_clear_active_regs[reg_num] = value_ic_enabler;
            }
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_IPRIORITYR0) &&
                   (relative_addr <= GIC_DIST_IPRIORITYR254)) {
            //
            // GICD_IPRIORITYR is a byte array: byte N is the priority of INTID N,
            // and a guest may program a single interrupt with one strb.  Rounding
            // the address down to a word and storing the whole access as a u32 --
            // which is what this did -- writes the value to the wrong INTID and
            // zeroes its three neighbours.
            //
            // Measured on J813: Windows programs the MTP SPI with priority 0x40 at
            // GICD_IPRIORITYR+995, i.e. word 248 byte 3.  The value landed on INTID
            // 992 and INTID 995 kept 0.  Zero is the *highest* priority the GIC
            // defines, so when a core acknowledged 995 the CPU interface's running
            // priority became 0 and hv_gic_cpuif_select()'s `prio >= s->running`
            // test then rejected every other interrupt -- including the SGI used
            // for processor rendezvous (priority 0x10) and the core's own timer.
            // Windows waited 300 s for a core that could no longer answer and
            // bugchecked 0x1DB, then asked PSCI for SYSTEM_RESET.
            //
            // The redistributor path already honoured the sub-word offset; this is
            // the same handling, and additionally honours the access width so a
            // halfword or doubleword write updates exactly the bytes it covers.
            //
            u32 first_intid = (u32)(relative_addr - GIC_DIST_IPRIORITYR0);
            u32 nbytes = 1u << width;
            u8 *prio = (u8 *)distributor->gicd_interrupt_priority_regs;

            for (u32 i = 0; i < nbytes; i++) {
                if ((first_intid + i) >= sizeof(distributor->gicd_interrupt_priority_regs))
                    break;
                prio[first_intid + i] = (u8)(*val >> (8 * i));
            }
            vgic_log(
                "HV vGIC DEBUG [INFO] [Distributor]: interrupt priority INTID %d..%d = 0x%llx\n",
                first_intid, first_intid + nbytes - 1, *val);
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ITARGETSR0) &&
                   (relative_addr <= GIC_DIST_ITARGETSR254)) {
            //
            // These are RES0 - since affinity routing is always enabled on Apple platforms.
            //
            vgic_log("HV vGIC DEBUG [WARN]: GICD_ITARGETS registers are RES0 - discarding write\n");
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICFGR0) &&
                   (relative_addr <= GIC_DIST_ICFGR63)) {
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ICFGR0) / 4;
            //
            // Unimplemented for now (we only support the timer interrupt right now - and those are
            // managed by the redistributors)
            //
            distributor->gicd_interrupt_config_regs[reg_num] = *val;
            vgic_log("HV vGIC DEBUG [INFO] [Distributor]: interrupt configuration register %d = "
                     "0x%llx\n",
                     reg_num, *val);
            register_handled = true;
            // unimplemented_reg_accessed = true;
        } else if (register_handled == false) {
            //
            // the register is unknown (or unimplemented) - print a warning.
            //
            vgic_log("HV vGIC DEBUG [ERR] - guest attempted to access unknown register 0x%llx\n",
                     relative_addr);
            register_handled = true;
            unimplemented_reg_accessed = true;
        }
    } else {
        //
        // The guest is attempting to read a register.
        // Handle it appropriately. Emit a warning (to become an error later) if a register is write
        // only or doesn't exist
        //
        switch (relative_addr) {
            case GIC_DIST_CTLR:
                *val = distributor->gicd_ctl_reg;
                register_handled = true;
                break;
            case GIC_DIST_TYPER:
                *val = distributor->gicd_type_reg;
                register_handled = true;
                break;
            case GIC_DIST_TYPER2:
                *val = distributor->gicd_type_reg_2;
                register_handled = true;
                break;
            case GIC_DIST_IIDR:
                *val = distributor->gicd_imp_id_reg;
                register_handled = true;
                break;
            case GIC_DIST_STATUSR:
                *val = distributor->gicd_err_sts;
                register_handled = true;
                break;
            case GIC_DIST_SETSPI_NSR:
            case GIC_DIST_CLRSPI_NSR:
            case GIC_DIST_CLRSPI_SR:
            case GIC_DIST_SETSPI_SR:
            case GIC_DIST_SGIR:
                *val = 0; // these registers are write only so force return 0 to the guest.
                register_handled = true;
                break;
            case 0xffe8: // make Hal happy
                *val = 0xff;
                register_handled = true;
                break;
            case GIC_DIST_IROUTER32 ... GIC_DIST_IROUTER1019:
                u32 reg_num;
                reg_num = (relative_addr - GIC_DIST_IROUTER32) / 8;
                *val = distributor->gicd_interrupt_router_regs[reg_num];
                register_handled = true;
                break;
            default:
                //
                // we're dealing with a register that is banked n times, we need to get to the if
                // statements.
                //
                break;
        }
        if ((register_handled == false) && (relative_addr >= GIC_DIST_IGROUPR0) &&
            (relative_addr <= GIC_DIST_IGROUPR31)) {
            //
            // the guest is trying to change the group of a given interrupt.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_IGROUPR0) / 4;

            //
            // TODO: bank GICD_IGROUPR0 for cores 0-7 - GIC spec requires it - but since we're
            // booting with 1 core atm, we can ignore this for now.
            //

            *val = distributor->gicd_interrupt_group_regs[reg_num];
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISENABLER0) &&
                   (relative_addr <= GIC_DIST_ISENABLER31)) {
            //
            // enables an IRQ to be forwarded to a CPU interface.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ISENABLER0) / 4;
            *val = distributor->gicd_interrupt_set_enable_regs[reg_num];
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICENABLER0) &&
                   (relative_addr <= GIC_DIST_ICENABLER31)) {
            //
            // disables an IRQ to be forwarded to a CPU interface.
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ICENABLER0) / 4;
            *val = distributor->gicd_interrupt_clear_enable_regs[reg_num];
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISPENDR0) &&
                   (relative_addr <= GIC_DIST_ISPENDR31)) {
            //
            // sets an IRQ to pending
            //
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ISPENDR0) / 4;
            *val = distributor->gicd_interrupt_set_pending_regs[reg_num];
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICPENDR0) &&
                   (relative_addr <= GIC_DIST_ICPENDR31)) {
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ICPENDR0) / 4;
            *val = distributor->gicd_interrupt_clear_pending_regs[reg_num];
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ISACTIVER0) &&
                   (relative_addr <= GIC_DIST_ISACTIVER31)) {
            //
            //
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ISACTIVER0) / 4;
            *val = distributor->gicd_interrupt_set_active_regs[reg_num];
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICACTIVER0) &&
                   (relative_addr <= GIC_DIST_ICACTIVER31)) {
            //
            // clears the pending state from an IRQ
            //
            u32 reg_num, irq_num;
            reg_num = (relative_addr - GIC_DIST_ICACTIVER0) / 4;
            *val = distributor->gicd_interrupt_clear_active_regs[reg_num];
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_IPRIORITYR0) &&
                   (relative_addr <= GIC_DIST_IPRIORITYR254)) {
            //
            // Byte-addressable, and read back exactly the way it is written --
            // see the write path for why the sub-word offset is load-bearing.
            //
            u32 first_intid = (u32)(relative_addr - GIC_DIST_IPRIORITYR0);
            u32 nbytes = 1u << width;
            const u8 *prio = (const u8 *)distributor->gicd_interrupt_priority_regs;
            u64 out = 0;

            for (u32 i = 0; i < nbytes; i++) {
                if ((first_intid + i) >= sizeof(distributor->gicd_interrupt_priority_regs))
                    break;
                out |= (u64)prio[first_intid + i] << (8 * i);
            }
            *val = out;
            vgic_log(
                "HV vGIC DEBUG [INFO] [Distributor]: interrupt priority INTID %d..%d = 0x%llx\n",
                first_intid, first_intid + nbytes - 1, *val);
            register_handled = true;
        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ITARGETSR0) &&
                   (relative_addr <= GIC_DIST_ITARGETSR254)) {
            //
            // These are RES0 - since affinity routing is always enabled on Apple platforms.
            //
            *val = 0;
            register_handled = true;

        } else if ((register_handled == false) && (relative_addr >= GIC_DIST_ICFGR0) &&
                   (relative_addr <= GIC_DIST_ICFGR63)) {
            u32 reg_num;
            reg_num = (relative_addr - GIC_DIST_ICFGR0) / 4;
            //
            // Unimplemented for now (we only support the timer interrupt right now - and those are
            // managed by the redistributors)
            //
            *val = distributor->gicd_interrupt_config_regs[reg_num];
            vgic_log("HV vGIC DEBUG [INFO] [Distributor]: interrupt configuration register %d = "
                     "0x%llx\n",
                     reg_num, *val);
            register_handled = true;
            // unimplemented_reg_accessed = true;
        } else if (register_handled == false) {
            //
            // the register is unknown (or unimplemented) - print a warning.
            //
            vgic_log("HV vGIC DEBUG [ERR] - guest attempted to access unknown register 0x%llx\n",
                     relative_addr);
            register_handled = true;
            unimplemented_reg_accessed = true;
        }
    }
    vgic_log("HV vGIC DEBUG [INFO] [Distributor]: 0x%llx = 0x%llx ", relative_addr, *val);
    if (write) {
        vgic_log("[Written]");
    } else {
        vgic_log("[Read]");
    }
    if (unimplemented_reg_accessed) {
        vgic_log("[Unimplemented]\n");
    } else {
        vgic_log("\n");
    }
    return register_handled;
}

//
// Description:
//   the vGIC guest interrupt handler for redistributor writes.
//
// Return values:
//   true - access has been handled successfully, even if the access itself is either bad or not
//   permitted. false - access was not handled successfully.
//
static bool handle_vgic_redist_access(struct exc_info *ctx, u64 addr, u64 *val, bool write,
                                      int width)
{
    u64 frame_offset;
    u64 relative_addr;
    bool register_handled;
    bool unimplemented_reg_accessed;
    frame_offset = addr - redist_base;
    u16 frame = (u16)(frame_offset / 0x20000);
    if (frame >= num_cpus)
        return false;
    relative_addr = frame_offset % 0x20000;
    register_handled = false;
    unimplemented_reg_accessed = false;
    u8 cpu_num;
    u32 value_is_enabler, value_ic_enabler, current_val;
    u32 irq_num;
    u32 reg_num;
    u32 reg_offset;
    UNUSED(ctx);
    value_ic_enabler = 0;
    value_is_enabler = 0;
    current_val = 0;
    irq_num = 0;
    reg_num = 0;
    reg_offset = 0;

    /* The addressed redistributor frame selects the bank. */
    cpu_num = redist_cpu_ids[frame];
    if (cpu_num >= MAX_CPUS)
        return false;
    if (write) {
        //
        // The guest attempted to write a register.
        // Handle it based on what they're trying to write, and preserve the value if
        // the value is going to a RW register.
        // Emit a warning (to become an error later) if the guest is attempting to write a register
        // that doesn't exist or is read only.
        //

        switch (relative_addr) {
            //
            // RD region
            //
            case GIC_REDIST_CTLR:
                u32 gicr_ctlr_new_val = (u32)(*val);
                vgic_log("HV vGIC DEBUG: guest writing GICR_CTLR = 0x%x, old value 0x%x\n",
                         gicr_ctlr_new_val, redistributors[cpu_num].rd_region.gicr_ctl_reg);
                bool is_uwp_to_be_set = false;
                bool is_rwp_to_be_set = false;
                //
                // like RWP in the distributor's case, the redistributor has it's own version of
                // this type of bit (UWP), where certain actions will trigger updates (IPIs in this
                // case.) we have to deal with this once the CPU interface is brought up. the
                // redistributors also have their own RWP bits which need to be handled similarly

                //
                // bits 30-27 and 23-4 are RES0 so discard writes.
                //
                if (((gicr_ctlr_new_val & GENMASK(30, 27)) != 0) ||
                    ((gicr_ctlr_new_val & GENMASK(23, 4)) != 0)) {
                    //
                    // these bits are RES0 - clear out this bitmask.
                    //
                    gicr_ctlr_new_val &= ~(GENMASK(30, 27));
                    gicr_ctlr_new_val &= ~(GENMASK(23, 4));
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to write RES0 bits in "
                             "GICR_CTLR, discarding\n");
                }

                //
                // since DS = 1 - bit 26 (DPG1S) is RAZ/WI
                //
                if (((gicr_ctlr_new_val)&BIT(26)) != 0) {
                    //
                    // clear the bit
                    //
                    gicr_ctlr_new_val &= ~BIT(26);
                }

                //
                // setting or clearing bits 25 and 24 (DPG1NS and DPG0) will trigger an RWP change.
                //
                if (((gicr_ctlr_new_val ^ redistributors[cpu_num].rd_region.gicr_ctl_reg) &
                     (BIT(25) | BIT(24))) != 0) {
                    //
                    // signal that RWP is going to be changed.
                    //
                    is_rwp_to_be_set = true;
                }

                //
                // bits 2 and 1 are RO - so discard writes to those bits.
                //
                if (((gicr_ctlr_new_val & BIT(2)) == 0) || ((gicr_ctlr_new_val & BIT(1)) == 0)) {
                    //
                    // guest is attempting to clear these RO bits - discard the write.
                    //
                    gicr_ctlr_new_val |= (BIT(2) | BIT(1));
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to write read-only bits in "
                             "GICR_CTLR, discarding\n");
                }

                //
                // EnableLPIs if cleared will trigger an RWP write.
                //
                if (((gicr_ctlr_new_val & BIT(0)) == 0) ||
                    ((redistributors[cpu_num].rd_region.gicr_ctl_reg & BIT(0)) != 0)) {
                    is_rwp_to_be_set = true;
                }

                //
                // start propagating the effects of the RWP changes.
                //
                if (is_rwp_to_be_set == true) {
                    //
                    // set RWP here - then start propagating the effects immediately after.
                    //
                    gicr_ctlr_new_val |= BIT(31);
                }

                redistributors[cpu_num].rd_region.gicr_ctl_reg = gicr_ctlr_new_val;
                if (is_rwp_to_be_set == true) {
                    //
                    // TODO: start the changes signaled by RWP.
                    //
                    // hv_vgicv3_apply_gic_redist_changes(gicr_ctlr_new_val);
                }

                redistributors[cpu_num].rd_region.gicr_ctl_reg = gicr_ctlr_new_val;
                register_handled = true;
                break;
            case GIC_REDIST_IIDR:
            case GIC_REDIST_TYPER:
            case GIC_REDIST_MPAMIDR:
                //
                // these are simple - the registers are read only so discard any write attempts.
                //
                vgic_log("HV vGIC DEBUG [WARN]: guest attempted to change a read-only register "
                         "(0x%x), discarding\n",
                         relative_addr);
                register_handled = true;
                break;
            case GIC_REDIST_STATUSR:
                //
                // GICR_STATUSR is a bit special, software must write 1 to ack an error, which then
                // *clears* the bit. Note that [31:4] are always RES0.
                //
                u32 gicr_statusr_new_val = (u32)(*val);
                u32 gicr_statusr_current_val = redistributors[cpu_num].rd_region.gicr_status_reg;
                if ((gicr_statusr_new_val & GENMASK(31, 4)) != 0) {
                    gicr_statusr_new_val &= ~(GENMASK(31, 4));
                    vgic_log("HV vGIC DEBUG [WARN]: guest attempted to write RES0 bits in "
                             "GICD_STATUSR, discarding\n");
                }
                if (((gicr_statusr_new_val & BIT(3)) != 0) &
                    ((gicr_statusr_current_val & BIT(3)) != 0)) {
                    gicr_statusr_current_val &= ~(BIT(3));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing WROD bit in GICD_STATUSR\n");
                }
                if (((gicr_statusr_new_val & BIT(2)) != 0) &
                    ((gicr_statusr_current_val & BIT(2)) != 0)) {
                    gicr_statusr_current_val &= ~(BIT(2));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing RWOD bit in GICD_STATUSR\n");
                }
                if (((gicr_statusr_new_val & BIT(1)) != 0) &
                    ((gicr_statusr_current_val & BIT(1)) != 0)) {
                    gicr_statusr_current_val &= ~(BIT(1));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing WRD bit in GICD_STATUSR\n");
                }
                if (((gicr_statusr_new_val & BIT(0)) != 0) &
                    ((gicr_statusr_current_val & BIT(0)) != 0)) {
                    gicr_statusr_current_val &= ~(BIT(0));
                    vgic_log("HV vGIC DEBUG [INFO]: clearing RRD bit in GICD_STATUSR\n");
                }
                redistributors[cpu_num].rd_region.gicr_status_reg = gicr_statusr_current_val;
                register_handled = true;
                break;
            case GIC_REDIST_WAKER:
                redistributors[cpu_num].rd_region.gicr_wake_reg = *val;
                register_handled = true;
                break;
            case GIC_REDIST_PARTIDR:
                redistributors[cpu_num].rd_region.gicr_partidr = *val;
                register_handled = true;
                break;
            case GIC_REDIST_SETLPIR:
                redistributors[cpu_num].rd_region.gicr_setlpir = *val;
                //
                // TODO: actually do the action here.
                //
                vgic_log("HV vGIC DEBUG [WARN]: GICR_SETLPIR is currently unimplemented!\n");
                unimplemented_reg_accessed = true;
                register_handled = true;
                break;
            case GIC_REDIST_CLRLPIR:
                redistributors[cpu_num].rd_region.gicr_clrlpir = *val;
                //
                // TODO: actually do the action here.
                //
                vgic_log("HV vGIC DEBUG [WARN]: GICR_CLRLPIR is currently unimplemented!\n");
                unimplemented_reg_accessed = true;
                register_handled = true;
                break;
            case GIC_REDIST_PROPBASER:
                redistributors[cpu_num].rd_region.gicr_propbaser = *val;
                register_handled = true;
                break;
            case GIC_REDIST_PENDBASER:
                redistributors[cpu_num].rd_region.gicr_pendbaser = *val;
                register_handled = true;
                break;
            case GIC_REDIST_INVLPIR:
                redistributors[cpu_num].rd_region.gicr_invlpir = *val;
                //
                // TODO: implement this. note that for INTID bits, bits 31:16 are unused since
                // IDbits = 16 for us.
                //
                vgic_log("HV vGIC DEBUG [WARN]: GICR_INVLPIR is currently unimplemented!\n");
                unimplemented_reg_accessed = true;
                register_handled = true;
                break;
            case GIC_REDIST_INVALLR:
                //
                // Any write to this register will invalidate all LPI config data - but the bits
                // themselves are RES0.
                // TODO: implement this.
                //
                redistributors[cpu_num].rd_region.gicr_invallr = 0;
                vgic_log("HV vGIC DEBUG [WARN]: GICR_INVALLR is currently unimplemented!\n");
                unimplemented_reg_accessed = true;
                register_handled = true;
                break;
            case GIC_REDIST_SYNCR:
                //
                // this register is read only - but has special handling. currently unimplemented.
                //
                vgic_log("HV vGIC DEBUG [WARN]: GICR_SYNCR is currently unimplemented!\n");
                unimplemented_reg_accessed = true;
                register_handled = true;
                break;
            //
            // SGI region
            //
            case GIC_REDIST_IGROUPR0:
                redistributors[cpu_num].sgi_region.gicr_igroupr0 = *val;
                register_handled = true;
                break;
            case GIC_REDIST_ISENABLER0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_isactiver0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icactiver0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICR_ICENABLER0 as well. also this is banked for the first 8 processor cores - so
                // changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) == 0)) {
                        value_is_enabler |= BIT(i);
                        value_ic_enabler |= BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                // TODO: should we also write to gicr_isenabler0?
                redistributors[cpu_num].sgi_region.gicr_isenabler0 = *val;
                register_handled = true;
                break;
            case GIC_REDIST_ICENABLER0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_isactiver0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icactiver0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores
                // - so changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) != 0)) {
                        value_is_enabler &= ~BIT(i);
                        value_ic_enabler &= ~BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                register_handled = true;
                break;
            case GIC_REDIST_ISPENDR0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_ispendr0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icpendr0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores
                // - so changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) == 0)) {
                        value_is_enabler |= BIT(i);
                        value_ic_enabler |= BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                register_handled = true;
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                break;
            case GIC_REDIST_ICPENDR0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_ispendr0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icpendr0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores
                // - so changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) != 0)) {
                        value_is_enabler &= ~BIT(i);
                        value_ic_enabler &= ~BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                register_handled = true;
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                break;
            case GIC_REDIST_ISACTIVER0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_isactiver0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icactiver0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores
                // - so changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) == 0)) {
                        value_is_enabler |= BIT(i);
                        value_ic_enabler |= BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                register_handled = true;
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                break;
            case GIC_REDIST_ICACTIVER0:
                // u32 value_is_enabler, value_ic_enabler, current_val;
                // u32 irq_num;
                value_is_enabler = redistributors[cpu_num].sgi_region.gicr_isactiver0;
                value_ic_enabler = redistributors[cpu_num].sgi_region.gicr_icactiver0;
                current_val = *val;

                //
                // if 1 is written to the bits in these registers, they need to read 1 in
                // GICD_ICENABLER[0:31] as well. also this is banked for the first 8 processor cores
                // - so changes must reflect across all of them.
                //
                // There has to be a way more efficient way of doing this...
                //

                for (u32 i = 0; i < 32; i++) {
                    if (((current_val & BIT(i)) != 0) && ((value_is_enabler & BIT(i)) != 0)) {
                        value_is_enabler &= ~BIT(i);
                        value_ic_enabler &= ~BIT(i);
                        irq_num = i;
                        //
                        // TODO: do the AIC operation associated with this.
                        //
                    }
                }
                register_handled = true;
                redistributors[cpu_num].sgi_region.gicr_isactiver0 = value_is_enabler;
                redistributors[cpu_num].sgi_region.gicr_icactiver0 = value_ic_enabler;
                break;
            case GIC_REDIST_ICFGR0:
                redistributors[cpu_num].sgi_region.gicr_icfgr0 = *val;
                register_handled = true;
                break;
            case GIC_REDIST_ICFGR1:
                redistributors[cpu_num].sgi_region.gicr_icfgr1 = *val;
                register_handled = true;
                break;
            case GIC_REDIST_IGRPMODR0:
                redistributors[cpu_num].sgi_region.gicr_igrpmodr0 = *val;
                register_handled = true;
                break;
            case GIC_REDIST_NSACR:
                redistributors[cpu_num].sgi_region.gicr_nsacr = *val;
                register_handled = true;
                break;
            case GIC_REDIST_IPRIORITYR0 ... GIC_REDIST_IPRIORITYR3 + 3:
                // u32 reg_num;
                reg_num = (relative_addr - GIC_REDIST_IPRIORITYR0) / 4;
                reg_offset = (relative_addr - GIC_REDIST_IPRIORITYR0) % 4;

                if (reg_offset == 0)
                    redistributors[cpu_num].sgi_region.gicr_sgi_ipriority_reg[reg_num] = *val;
                else {
                    // TODO: handle width
                    u8 *reg_u8 =
                        (u8 *)&redistributors[cpu_num].sgi_region.gicr_sgi_ipriority_reg[reg_num];
                    reg_u8 += reg_offset;
                    *reg_u8 = *val & 0xFF;
                }
                register_handled = true;
                break;
            case GIC_REDIST_IPRIORITYR4 ... GIC_REDIST_IPRIORITYR7 + 3:
                // u32 reg_num;
                reg_num = (relative_addr - GIC_REDIST_IPRIORITYR4) / 4;
                reg_offset = (relative_addr - GIC_REDIST_IPRIORITYR4) % 4;
                if (reg_offset == 0)
                    redistributors[cpu_num].sgi_region.gicr_ppi_ipriority_reg[reg_num] = *val;
                else {
                    // TODO: handle width
                    u8 *reg_u8 =
                        (u8 *)&redistributors[cpu_num].sgi_region.gicr_ppi_ipriority_reg[reg_num];
                    reg_u8 += reg_offset;
                    *reg_u8 = *val & 0xFF;
                }
                register_handled = true;
                break;
            default:
                //
                // an unimplemented register.
                //
                unimplemented_reg_accessed = true;
                break;
        }
    } else {
        //
        // The guest is attempting to read a register.
        // Handle it appropriately. Emit a warning (to become an error later) if a register is write
        // only or doesn't exist
        //

        switch (relative_addr) {
            //
            // RD region
            //
            case GIC_REDIST_CTLR:
                *val = redistributors[cpu_num].rd_region.gicr_ctl_reg;
                register_handled = true;
                break;
            case GIC_REDIST_IIDR:
                *val = redistributors[cpu_num].rd_region.gicr_iidr;
                register_handled = true;
                break;
            case GIC_REDIST_TYPER:
                *val = redistributors[cpu_num].rd_region.gicr_type_reg;
                register_handled = true;
                break;
            case GIC_REDIST_STATUSR:
                *val = redistributors[cpu_num].rd_region.gicr_status_reg;
                register_handled = true;
                break;
            case GIC_REDIST_WAKER:
                *val = redistributors[cpu_num].rd_region.gicr_wake_reg;
                register_handled = true;
                break;
            case GIC_REDIST_MPAMIDR:
                *val = redistributors[cpu_num].rd_region.gicr_mpamidr;
                register_handled = true;
                break;
            case GIC_REDIST_PARTIDR:
                *val = redistributors[cpu_num].rd_region.gicr_partidr;
                register_handled = true;
                break;
            case GIC_REDIST_SETLPIR:
            case GIC_REDIST_CLRLPIR:
            case GIC_REDIST_INVLPIR:
            case GIC_REDIST_INVALLR:
                //
                // these registers are write-only so reads in our case will return 0 (only
                // meaningful action here is writes)
                *val = 0;
                register_handled = true;
                break;
            case GIC_REDIST_PROPBASER:
                *val = redistributors[cpu_num].rd_region.gicr_propbaser;
                register_handled = true;
                break;
            case GIC_REDIST_PENDBASER:
                *val = redistributors[cpu_num].rd_region.gicr_pendbaser;
                register_handled = true;
                break;
            case GIC_REDIST_SYNCR:
                *val = redistributors[cpu_num].rd_region.gicr_iidr;
                register_handled = true;
                break;
            //
            // SGI region
            //
            case GIC_REDIST_IGROUPR0:
                *val = redistributors[cpu_num].sgi_region.gicr_igroupr0;
                register_handled = true;
                break;
            case GIC_REDIST_ISENABLER0:
                *val = redistributors[cpu_num].sgi_region.gicr_isenabler0;
                register_handled = true;
                break;
            case GIC_REDIST_ICENABLER0:
                *val = redistributors[cpu_num].sgi_region.gicr_icenabler0;
                register_handled = true;
                break;
            case GIC_REDIST_ISPENDR0:
                *val = redistributors[cpu_num].sgi_region.gicr_ispendr0;
                register_handled = true;
                break;
            case GIC_REDIST_ICPENDR0:
                *val = redistributors[cpu_num].sgi_region.gicr_icpendr0;
                register_handled = true;
                break;
            case GIC_REDIST_ISACTIVER0:
                *val = redistributors[cpu_num].sgi_region.gicr_isactiver0;
                register_handled = true;
                break;
            case GIC_REDIST_ICACTIVER0:
                *val = redistributors[cpu_num].sgi_region.gicr_icactiver0;
                register_handled = true;
                break;
            case GIC_REDIST_ICFGR0:
                *val = redistributors[cpu_num].sgi_region.gicr_icfgr0;
                register_handled = true;
                break;
            case GIC_REDIST_ICFGR1:
                *val = redistributors[cpu_num].sgi_region.gicr_icfgr1;
                register_handled = true;
                break;
            case GIC_REDIST_IGRPMODR0:
                *val = redistributors[cpu_num].sgi_region.gicr_igrpmodr0;
                register_handled = true;
                break;
            case GIC_REDIST_NSACR:
                *val = redistributors[cpu_num].sgi_region.gicr_nsacr;
                register_handled = true;
                break;
            case GIC_REDIST_IPRIORITYR0 ... GIC_REDIST_IPRIORITYR3 + 3:
                // u32 reg_num;
                reg_num = (relative_addr - GIC_REDIST_IPRIORITYR0) / 4;
                reg_offset = (relative_addr - GIC_REDIST_IPRIORITYR0) % 4;

                if (reg_offset == 0)
                    *val = redistributors[cpu_num].sgi_region.gicr_sgi_ipriority_reg[reg_num];
                else {
                    // TODO: handle width
                    u8 *reg_u8 =
                        (u8 *)&redistributors[cpu_num].sgi_region.gicr_sgi_ipriority_reg[reg_num];
                    reg_u8 += reg_offset;
                    *val = *reg_u8;
                }
                register_handled = true;
                break;
            case GIC_REDIST_IPRIORITYR4 ... GIC_REDIST_IPRIORITYR7 + 3:
                // u32 reg_num;
                reg_num = (relative_addr - GIC_REDIST_IPRIORITYR4) / 4;
                reg_offset = (relative_addr - GIC_REDIST_IPRIORITYR4) % 4;
                if (reg_offset == 0)
                    redistributors[cpu_num].sgi_region.gicr_ppi_ipriority_reg[reg_num] = *val;
                else {
                    // TODO: handle width
                    u8 *reg_u8 =
                        (u8 *)&redistributors[cpu_num].sgi_region.gicr_ppi_ipriority_reg[reg_num];
                    reg_u8 += reg_offset;
                    *val = *reg_u8;
                }
                register_handled = true;
                break;
            default:
                //
                // an unimplemented register.
                //
                unimplemented_reg_accessed = true;
                break;
        }
    }
    vgic_log("HV vGIC DEBUG [INFO] [Redistributor]: 0x%llx = 0x%llx ", relative_addr, *val);
    if (write) {
        vgic_log("[Written]");
    } else {
        vgic_log("[Read]");
    }
    if (unimplemented_reg_accessed) {
        vgic_log("[Unimplemented]\n");
    } else {
        vgic_log("\n");
    }
    return register_handled;
}

/**
 * @brief hv_vgicv3_init_dist_registers
 *
 * Sets up the initial values for the distributor registers.
 *
 * For registers that deal with unsupported features, set them to 0 and just never interact with
 * them
 *
 * For write only registers, set them to 0, and emulate the effect upon attempting to write that
 * register. Read-only registers, set their value here and don't let the guest touch their values.
 *
 */
void hv_vgicv3_init_dist_registers(void)
{
    memset(distributor, 0, sizeof(vgicv3_dist));
    //
    // For now - taking the easy route of saying that at least 1024 IRQs are supported on all
    // platforms.
    //
    distributor->gicd_ctl_reg = (BIT(6) | BIT(4) | BIT(1) | BIT(0));
    //
    // GIC type will be defined as the following:
    // - No extended SPIs (Update on 6/10/2025: maz in Asahi IRC says we can probably expose
    // extended SPIs? could also look into some other hacks for > 1024 IRQ platforms)
    // - Affinity level 0 can go up to 15
    // - 1 of N SPI interrupts are supported (kind of how AIC2 can behave?)
    // - Affinity 3 invalid
    // - 16 interrupt ID bits (to match what the CPU interface supports)
    // - LPIs/MSIs supported (MSIs not using an ITS)
    //
    distributor->gicd_type_reg = (BIT(22) | BIT(21) | BIT(20) | BIT(19) | BIT(17) | BIT(4) |
                                  BIT(3) | BIT(2) | BIT(1) | BIT(0));
    distributor->gicd_imp_id_reg = (BIT(10) | BIT(5) | BIT(4) | BIT(3) | BIT(1) | BIT(0));
    distributor->gicd_type_reg_2 = 0;
    distributor->gicd_err_sts = 0;
    return;
}

void hv_vgicv3_assign_redist_affinity_value(u16 cpu_num, bool last_cpu)
{
    u32 cpu_affinity_value;
    uint64_t mpidr_val;
    uint64_t gicr_typer;
    mpidr_val = smp_get_mpidr(cpu_num);
    //
    // Affinity level 3 is always 0.
    //
    cpu_affinity_value = (0 << 24);
    //
    // Affinity level 2 signifies if we're targeting a P-core or E-core cluster.
    // (0x0 for an E-core, 0x1 for a P-core)
    //
    cpu_affinity_value |= ((mpidr_val >> 16) & 0xFF) << 16;
    //
    // Affinity level 1 signifies the cluster number on the local die (for multi-die systems it's
    // cluster_num + (die_num * 8)).
    //
    cpu_affinity_value |= ((mpidr_val >> 8) & 0xFF) << 8;
    //
    // Affinity level 0 is the core number on the local cluster.
    //
    cpu_affinity_value |= ((mpidr_val) & 0xFF);
    gicr_typer = (((uint64_t)cpu_affinity_value) << 32);
    //
    // Apple silicon platforms (at least the M1 and M2 and the Pro counterparts) do not support the
    // extended PPI/SPI ranges so bits 31:27 remain 0. If M3 or M4 do support the extended ranges,
    // check the Chip ID here and toggle those bits. (Unlikely, as even though M1 Ultra has > 16
    // cores, we do not have those ranges on that platform, which means we probably will need to
    // have a solution for those platforms.)
    //
    // We're also sharing a common LPI configuration table across all the vCPUs.
    //
    // Put the processor/CPU number in the right field here,
    // the way we're doing it here ensures we have a one to one mapping with how m1n1 identifies the
    // CPUs.
    //
    gicr_typer |= (cpu_num << 8);

    //
    // Leave out MPAM support for now - we can't assume the CPU supports it.
    // Let processors opt out of interrupts though. (bit 5, GICR_TYPER.DPGS bit)
    //
    gicr_typer |= BIT(5);
    if (last_cpu == true) {
        //
        // this is the last redistributor, set bit 4 to indicate this.
        //
        gicr_typer |= BIT(4);
    }

    //
    // If we need an ITS, we need to comment out the bottom line, since we wouldn't be supporting
    // direct LPI injection to redistributors.
    //
    gicr_typer |= BIT(3);
    //
    // say that we have physical LPIs to be safe.
    //
    gicr_typer |= BIT(0);

    //
    // write back this feature set to our vGIC registers.
    //
    redistributors[cpu_num].rd_region.gicr_type_reg = gicr_typer;

    return;
}

/*
 * CPUs that actually reached the spin table, counted exactly the way
 * hv_vgicv3_init_redist_registers() decides to build a frame for one.
 *
 * The redistributor count has to describe the machine, not the ADT.  On T8142
 * one secondary does not come up, so the ADT's ten CPUs yield nine live ones,
 * and requiring parity turned that into a reboot before Mu ran a single
 * instruction -- the panic below, which is what the T8142 "carrier resets the
 * machine" deferral in hv.c was actually reporting.  Sizing to the live count
 * keeps the carrier usable on a partially-started machine and returns to ten
 * frames on its own once every core starts.
 */
static u16 hv_vgic_live_cpu_count(void)
{
    u16 live = 0;

    for (u16 cpu = 0; cpu < MAX_CPUS; cpu++)
        if (cpu == (u16)boot_cpu_idx || smp_is_alive(cpu))
            live++;

    return live;
}

void hv_vgicv3_init_redist_registers(void)
{
    u16 frame = 0;

    memset(redistributors, 0, sizeof(vgicv3_vcpu_redist) * MAX_CPUS);
    memset(redist_cpu_ids, 0xff, sizeof(redist_cpu_ids));
    for (u16 cpu = 0; cpu < MAX_CPUS; cpu++) {
        /* The boot CPU never sets the secondary spin-table alive flag. */
        if (cpu != (u16)boot_cpu_idx && !smp_is_alive(cpu))
            continue;
        if (frame >= num_cpus)
            panic("HV vGIC: active CPU count exceeds ADT CPU count\n");

        redist_cpu_ids[frame] = (u8)cpu;
        redistributors[cpu].rd_region.gicr_ctl_reg = (BIT(2) | BIT(1));
        redistributors[cpu].rd_region.gicr_iidr =
            (BIT(10) | BIT(5) | BIT(4) | BIT(3) | BIT(1) | BIT(0));
        //
        // assign affinity values to redistributors.
        //
        hv_vgicv3_assign_redist_affinity_value(cpu, frame + 1 == num_cpus);
        redistributors[cpu].rd_region.gicr_status_reg = 0;
        redistributors[cpu].rd_region.gicr_wake_reg =
            (BIT(2) | BIT(1)); // GICR_WAKER reset values, currently not using bits 31 or 0.
        //
        // Generate and set the LPI configuration table here.
        // (Right now this is ignored just to test if stuff is working since we have no MSIs and
        // LPIs are disabled right now.)
        //
        frame++;
    }
    /*
     * Not fatal.  A frame short of the ADT count means a core did not start,
     * which is a CPU-bring-up defect to fix in smp.c -- not a reason to reboot
     * the machine out from under firmware that has not run yet.  The guest
     * stops walking at the frame whose GICR_TYPER carries Last, so it simply
     * sees the cores that exist.
     */
    if (frame != num_cpus)
        printf("HV vGIC: %u redistributors for %u ADT CPUs -- %u core(s) never started\n", frame,
               num_cpus, num_cpus - frame);
}

/**
 * @brief hv_vgicv3_init_list_registers
 *
 * Enables the platform's list registers for use by the guest OS.
 *
 */
void hv_vgicv3_init_list_registers(void)
{
    msr(ICH_LR0_EL2, 0);
    msr(ICH_LR1_EL2, 0);
    msr(ICH_LR2_EL2, 0);
    msr(ICH_LR3_EL2, 0);
    msr(ICH_LR4_EL2, 0);
    msr(ICH_LR5_EL2, 0);
    msr(ICH_LR6_EL2, 0);
    msr(ICH_LR7_EL2, 0);
}

/**
 * @brief hv_vgicv3_enable_virtual_interrupts
 *
 * Enables virtual interrupts for the guest.
 *
 * Note that actual interrupts are always handled by m1n1, then passed onto the vGIC which will
 * signal the virtual interrupt to the OS.
 *
 * @return
 * 0 - success
 * -1 - there was an error.
 */

int hv_vgicv3_enable_virtual_interrupts(void)
{
    // set VMCR to reset values, then enable virtual group 0 and 1 interrupts
    msr(ICH_VMCR_EL2, 0);
    msr(ICH_VMCR_EL2, (BIT(1)));
    // bit 0 enables the virtual CPU interface registers
    // AMO/IMO/FMO set by m1n1 on boot
    msr(ICH_HCR_EL2, (BIT(0) | BIT(2)));

    return 0;
}

u8 hv_vgic3_get_priority_cpu(int cpu, u64 intd)
{
    u64 reg_num = 0;
    u64 reg_offset = 0;
    u8 *reg_val = NULL;

    if (intd <= 15) {
        reg_num = intd / 4;
        reg_offset = intd % 4;
        reg_val = (u8 *)&redistributors[cpu].sgi_region.gicr_sgi_ipriority_reg[reg_num];
    } else if (intd >= 16 && intd <= 31) {
        reg_num = (intd - 16) / 4;
        reg_offset = (intd - 16) % 4;
        reg_val = (u8 *)&redistributors[cpu].sgi_region.gicr_ppi_ipriority_reg[reg_num];
    } else {
        //
        // GICD_IPRIORITYR<n> (offset 0x400 + 4n) holds INTIDs 4n..4n+3, so the
        // byte for INTID N lives at byte index N of the array. The MMIO handler
        // stores at reg_num = (offset - GIC_DIST_IPRIORITYR0) / 4 == N / 4 with
        // no -32 bias; read it back the same way. The previous (intd - 32) / 4
        // returned the priority programmed for INTID (intd - 32).
        //
        // 255 words cover INTIDs 0..1019 (GICD_IPRIORITYR254); anything above
        // that has no backing slot, so return the reset priority.
        //
        if (intd > 1019)
            return 0;
        reg_num = intd / 4;
        reg_offset = intd % 4;
        reg_val = (u8 *)&distributor->gicd_interrupt_priority_regs[reg_num];
    }
    reg_val += reg_offset;

    return *reg_val;
}

u8 hv_vgic3_get_priority(u64 intd)
{
    return hv_vgic3_get_priority_cpu(smp_id(), intd);
}

u16 hv_vgic3_num_cpus(void)
{
    return num_cpus;
}

int hv_vgic3_cpu_for_frame(u16 frame)
{
    if (frame >= num_cpus || redist_cpu_ids[frame] >= MAX_CPUS)
        return -1;
    return redist_cpu_ids[frame];
}

u32 hv_vgic3_num_lrs(void)
{
    return vgic_nr_lrs;
}

int hv_vgic3_get_free_lr(void)
{
    /* ELRSR bits above the implemented count are RES0, but mask defensively so
     * a free-LR index is never reported for a register we cannot address. */
    u64 elrsr = mrs(ICH_ELRSR_EL2) & (((u64)1 << vgic_nr_lrs) - 1);
    if (!elrsr)
        return -1;
    return __builtin_ctzll(elrsr);
}

u64 hv_vgic3_read_lr(u32 lr_num)
{
    switch (lr_num) {
        case 0:
            return mrs(ICH_LR0_EL2);
            break;
        case 1:
            return mrs(ICH_LR1_EL2);
            break;
        case 2:
            return mrs(ICH_LR2_EL2);
            break;
        case 3:
            return mrs(ICH_LR3_EL2);
            break;
        case 4:
            return mrs(ICH_LR4_EL2);
            break;
        case 5:
            return mrs(ICH_LR5_EL2);
            break;
        case 6:
            return mrs(ICH_LR6_EL2);
            break;
        case 7:
            return mrs(ICH_LR7_EL2);
            break;
    }
    return 0;
}

void hv_vgic3_write_lr(u32 lr_num, u64 lr_val)
{
    switch (lr_num) {
        case 0:
            msr(ICH_LR0_EL2, lr_val);
            break;
        case 1:
            msr(ICH_LR1_EL2, lr_val);
            break;
        case 2:
            msr(ICH_LR2_EL2, lr_val);
            break;
        case 3:
            msr(ICH_LR3_EL2, lr_val);
            break;
        case 4:
            msr(ICH_LR4_EL2, lr_val);
            break;
        case 5:
            msr(ICH_LR5_EL2, lr_val);
            break;
        case 6:
            msr(ICH_LR6_EL2, lr_val);
            break;
        case 7:
            msr(ICH_LR7_EL2, lr_val);
            break;
    }
    sysop("isb");
}

void hv_vgic3_inject_irq(u32 vintid, u8 priority, bool active, bool pending, bool hw_status,
                         u64 hw_irq)
{
    u64 val = 0;
    val |= (u64)(vintid & ICH_LR_VIRTUAL_MASK) << ICH_LR_VIRTUAL_SHIFT;
    val |= (u64)(priority & ICH_LR_PRIORITY_MASK) << ICH_LR_PRIORITY_SHIFT;
    val |= ICH_LR_GRP1;

    if (active)
        val |= ICH_LR_STATE_ACTIVE;
    if (pending)
        val |= ICH_LR_STATE_PENDING;
    if (hw_status) {
        val |= ICH_LR_HW;
        //
        // pINTID is a 13-bit field (bits [44:32]); an unmasked value would
        // spill into RES0 bits and, from bit 48 up, corrupt priority/group/
        // HW/state. Callers must not pass a physical INTID above 0x1fff.
        //
        val |= (hw_irq & ICH_LR_PHYSICAL_MASK) << ICH_LR_PHYSICAL_SHIFT;
    } else {
        val |= ICH_LR_MAINTENANCE_IRQ;
    }

    /*
     * A GIC interrupt has one state machine per INTID.  Re-posting an INTID
     * that is already pending or active must update that LR to
     * active+pending, not allocate a second LR carrying the same INTID.  The
     * latter lets Windows accept the same timer/SGI recursively before EOI,
     * which corrupts its IRQL/context-switch state during AP startup.
     */
    for (u32 lr = 0; lr < vgic_nr_lrs; lr++) {
        u64 lr_val = hv_vgic3_read_lr(lr);
        if (!(lr_val & (ICH_LR_STATE_PENDING | ICH_LR_STATE_ACTIVE)))
            continue;
        if (((lr_val >> ICH_LR_VIRTUAL_SHIFT) & ICH_LR_VIRTUAL_MASK) !=
            (vintid & ICH_LR_VIRTUAL_MASK))
            continue;

        if (pending)
            lr_val |= ICH_LR_STATE_PENDING;
        if (active)
            lr_val |= ICH_LR_STATE_ACTIVE;
        hv_vgic3_write_lr(lr, lr_val);
        sysop("isb");
        return;
    }

    int free_lr = hv_vgic3_get_free_lr();
    if (free_lr < 0)
        return;
    hv_vgic3_write_lr(free_lr, val);
    sysop("isb");
}

int hv_vgic3_do_iar1(void)
{
    u8 found_priority = 0xff;
    int found_lr = -1;
    for (int lr = 0; lr < (int)vgic_nr_lrs; lr++) {
        u64 lr_val = hv_vgic3_read_lr(lr);
        if ((lr_val & ICH_LR_STATE_PENDING) && !(lr_val & ICH_LR_STATE_ACTIVE)) {
            u8 priority = (lr_val >> ICH_LR_PRIORITY_SHIFT) & ICH_LR_PRIORITY_MASK;
            if (priority < found_priority) {
                found_lr = lr;
                found_priority = priority;
            }
        }
    }

    if (found_lr != -1) {
        u64 lr_val = hv_vgic3_read_lr(found_lr);
        lr_val &= ~ICH_LR_STATE_PENDING;
        lr_val |= ICH_LR_STATE_ACTIVE;
        hv_vgic3_write_lr(found_lr, lr_val);
        return (lr_val >> ICH_LR_VIRTUAL_SHIFT) & ICH_LR_VIRTUAL_MASK;
    }

    return 0x3FF;
}

void hv_vgic3_do_eoir1(u64 reg)
{
    u32 intd = reg & ICH_LR_VIRTUAL_MASK;
    for (int lr = 0; lr < (int)vgic_nr_lrs; lr++) {
        u64 lr_val = hv_vgic3_read_lr(lr);
        // vgic_log("CHECKING LR: 0x%lx %d %d %d\n", lr_val, intd, (lr_val >> ICH_LR_VIRTUAL_SHIFT)
        // & ICH_LR_VIRTUAL_MASK, lr_val & ICH_LR_STATE_ACTIVE);
        if (((lr_val >> ICH_LR_VIRTUAL_SHIFT) & ICH_LR_VIRTUAL_MASK) == intd &&
            (lr_val & ICH_LR_STATE_ACTIVE)) {
            // vgic_log("DOING EOIR 0x%lx, found LR%d: 0x%lx, setting to 0\n", reg, lr, lr_val);
            if (lr_val & ICH_LR_STATE_PENDING) {
                lr_val &= ~ICH_LR_STATE_ACTIVE;
                hv_vgic3_write_lr(lr, lr_val);
            } else {
                hv_vgic3_write_lr(lr, 0);
            }
            sysop("isb");
            return;
        }
    }
}

void hv_vgic3_set_igrpen1(u64 reg)
{
    igrpen1[smp_id()] = reg;
    if (reg == 0) {
        for (int lr = 0; lr < (int)vgic_nr_lrs; lr++)
            hv_vgic3_write_lr(lr, 0);
    }
}

u64 hv_vgic3_get_igrpen1(void)
{
    return igrpen1[smp_id()];
}

#endif

/**
 * @brief hv_vgicv3_init
 *
 * Initializes the vGIC and prepares it for use by the guest OS.
 *
 * Note that this function is only expected to be called once.
 *
 * @return
 *
 * 0 - success, vGIC is ready for use by the guest
 * -1 - an error has occurred during vGIC initialization, refer to m1n1 output log for details on
 * the specific error
 */

void hv_vgicv3_init(void)
{
#ifdef ENABLE_VGIC_MODULE
    printf("HV vGIC DEBUG: start\n");
    vgic_inited = false;
    //
    // Discover the number of implemented list registers from ICH_VTR_EL2 rather
    // than assuming eight. ICH_VTR_EL2.ListRegs (bits [4:0]) holds count-minus-one.
    // Clamp to eight: hv_vgic3_read_lr/write_lr only encode ICH_LR0..ICH_LR7, so
    // any extra implemented LRs stay unused until those helpers are extended.
    //
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /*
     * Native-AIC builds instantiate GICD/GICR only as a firmware startup and
     * topology carrier.  They deliberately never enable or consume ICH list
     * registers, so probing ICH_VTR_EL2 is both unnecessary and invalid on M5.
     */
    vgic_nr_lrs = 8;
    printf("HV vGIC DEBUG: native-AIC carrier-only mode (no ICH probe)\n");
#else
    vgic_nr_lrs = (u32)((mrs(ICH_VTR_EL2) & 0x1f) + 1);
    if (vgic_nr_lrs > 8)
        vgic_nr_lrs = 8;
#endif
    printf("HV vGIC DEBUG: %u list registers implemented\n", vgic_nr_lrs);
    //
    // First things first - set the parameters appropriately based on whether
    // we're running on a 36-bit or 42-bit platform.
    // Also for now, we are catering to "lowest common denominator" for all chips,
    // so on more powerful systems we may not be using all cores.
    //
    switch (chip_id) {
        case T8103:
        case T8112:
            dist_base = DIST_BASE_36_BIT;
            redist_base = REDIST_BASE_36_BIT;
            its_base = ITS_BASE_36_BIT;
            num_cpus = 8;
            break;
        case T6050:
            dist_base = DIST_BASE_42_BIT;
            redist_base = REDIST_BASE_42_BIT;
            its_base = ITS_BASE_42_BIT;
            num_cpus = 18;
            break;
        case T6020:
            // M2 Pro ships in two bins (10-core: 6P+4E, or 12-core: 8P+4E) --
            // a chip_id-keyed literal cannot tell them apart, so num_cpus
            // comes from the live ADT /cpus population (smp_start_secondaries()
            // has already run by this point, hv.c:64,119), not a guessed
            // constant. See docs/vgic-t6020-tables-and-init.md SS1.3.
            dist_base = DIST_BASE_42_BIT;
            redist_base = REDIST_BASE_42_BIT;
            its_base = ITS_BASE_42_BIT;
            num_cpus = (u16)smp_cpu_count();
            break;
        case T8142:
            /* Match the T8142 Mu platform PCDs used by the M5 firmware. */
            dist_base = DIST_BASE_36_BIT;
            redist_base = REDIST_BASE_36_BIT;
            its_base = ITS_BASE_36_BIT;
            /*
             * Live cores, not ADT cores.  One M5 secondary does not currently
             * start, and every use of num_cpus here -- the redistributor frame
             * count, which frame carries GICR_TYPER.Last, and the size of the
             * MMIO hook -- must describe frames that actually exist.  Sizing
             * from the ADT instead publishes a frame with nothing behind it.
             */
            num_cpus = hv_vgic_live_cpu_count();
            if (num_cpus != (u16)smp_cpu_count())
                printf("HV vGIC: sizing carrier to %u live CPUs (ADT reports %d)\n", num_cpus,
                       smp_cpu_count());
            break;
        // case T8010:
        // case T8015:
        // case T8011:
        // case T8012:
        //     dist_base = DIST_BASE_36_BIT;
        //     redist_base = REDIST_BASE_36_BIT;
        //     its_base = ITS_BASE_36_BIT;
        //     break;
        case T6000:
        case T6001:
        case T6002:
            // M1 Pro / Max / Ultra (G13X; Ultra is a multi-die part). All share
            // the 42-bit GIC bases. The core count is ADT-derived via
            // smp_cpu_count(), never a chip_id literal: M1 Pro alone ships as an
            // 8-core (6P+2E) or 10-core (8P+2E) bin under a single chip_id, so a
            // hard-coded value is wrong for at least one bin.
            //
            // CRITICAL BUG FIXED HERE: these three cases (and T6021/T6022 below)
            // previously had NO `break;` and fell through the entire T60xx/T602x
            // chain into `default:`, which prints "unsupported chip_id" and
            // `return`s -- so the vGIC never actually initialized on ANY M1
            // Pro/Max/Ultra or M2 Max/Ultra machine. Each set of parameters now
            // terminates. See docs/vgic-t6020-tables-and-init.md (same
            // ADT-derived-count reasoning as the T6020 case above).
            dist_base = DIST_BASE_42_BIT;
            redist_base = REDIST_BASE_42_BIT;
            its_base = ITS_BASE_42_BIT;
            num_cpus = (u16)smp_cpu_count();
            break;
        case T6021:
        case T6022:
            // M2 Max / M2 Ultra: same 42-bit bases, same ADT-derived count, and
            // the same missing-`break;` fall-through fixed.
            dist_base = DIST_BASE_42_BIT;
            redist_base = REDIST_BASE_42_BIT;
            its_base = ITS_BASE_42_BIT;
            num_cpus = (u16)smp_cpu_count();
            break;
        // case T6030:
        // case T6031:
        // case 0x6032:
        // case T6034:
        // case 0x6040:
        // case 0x6041:
        // case T8122:
        //     dist_base = DIST_BASE_42_BIT;
        //     redist_base = REDIST_BASE_42_BIT;
        //     its_base = ITS_BASE_42_BIT;
        //     break;
        default:
            printf("HV vGIC: unsupported chip_id 0x%x, not initializing vGIC\n", chip_id);
            return;
    }
    //
    // Step 1 - distributor setup.
    //
    printf("HV vGIC DEBUG: setting up distributor\n");
    distributor = heapblock_alloc(sizeof(vgicv3_dist));
    hv_vgicv3_init_dist_registers();
    //
    // Map the vGIC distributor into unoccupied MMIO space.
    //
    // windows-native-aic: this hv_map_hook() (and the redistributor/ITS ones below) is
    // exactly what makes the guest see an emulated GICv3 distributor at dist_base
    // instead of driving hardware directly. Under ENABLE_NATIVE_AIC_PASSTHROUGH we
    // deliberately do NOT install it: the guest is meant to talk to the real AIC (its
    // MMIO was never hooked here or anywhere else in this tree -- the only AIC-MMIO
    // hook in the codebase is the opt-in host-debugger IRQ tracer in hv_aic.c, wired up
    // on demand via proxy.c's hv_trace_irq(), never called from the boot path). The
    // distributor struct above is still allocated/initialized -- hv_vgic3_get_priority()
    // used to read it for the SPI (irq > 31) case when real AIC IRQs were translated
    // into vGIC injections; that translation is gone too (see hv_exc.c's hv_exc_irq()),
    // so in native-AIC-passthrough mode this struct is effectively unused, kept
    // allocated only to minimize the diff and avoid a second flag axis. See
    // docs/windows-native-aic.md.
    //
    printf("HV vGIC DEBUG: mapping startup-carrier distributor into guest space\n");
    hv_map_hook(dist_base, handle_vgic_dist_access, 0x10000);

    /* Redistributor setup */
    printf("HV vGIC DEBUG: setting up redistributors\n");
    /* State is indexed by sparse physical CPU ID; guest frames stay dense. */
    redistributors = heapblock_alloc(sizeof(vgicv3_vcpu_redist) * MAX_CPUS);
    hv_vgicv3_init_redist_registers();
    //
    // windows-native-aic: same reasoning as the distributor above. The redistributor
    // struct stays allocated/initialized because hv_vgic3_get_priority() still reads
    // redistributors[cpu].sgi_region.gicr_ppi_ipriority_reg for PPI-range intids -- but
    // in this design that's now moot too, since the timer no longer goes through
    // hv_vgic3_get_priority()/list-register injection at all (it's an AIC software IRQ,
    // see hv_exc.c). The guest never sees this MMIO region either way.
    //
    printf("HV vGIC DEBUG: mapping startup-carrier redistributors into guest space\n");
    hv_map_hook(redist_base, handle_vgic_redist_access, ((0x20000) * num_cpus));

    //
    // ITS setup (for MSIs - PCIe devices usually signal via these.)
    // Disabled for now, seems like direct injection into the guest is easier.
    //
    // windows-native-aic: peripherals (PCIe/MSI included) now go straight through AIC
    // like everything else -- there is no virtual ITS to present, so skip the hook.
    //
    interrupt_translation_service = heapblock_alloc(sizeof(vgicv3_its));
#ifndef ENABLE_NATIVE_AIC_PASSTHROUGH
    hv_map_hook(its_base, handle_vgic_its_access, 0x10000);
#endif

    // vGIC setup is complete.
    vgic_inited = true;
    return;
#else
    printf("HV vGIC DEBUG: Disabled\n");
    return;
#endif // ENABLE_VGIC_MODULE
}
