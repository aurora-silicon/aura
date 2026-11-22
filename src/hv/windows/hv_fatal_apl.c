/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * G6 r5: Apple's implementation-defined error-status registers for the T8152 fatal report
 * (hv_fatal.h).  Written on this branch; the register encodings are the ones cpu_regs.h already
 * names (exception.c's print_regs() reads the same set when the bank is known unlocked).
 *
 * features_m6 does not set apple_sysregs_unlocked, so whether these exist at EL2 on M6 is not
 * measured.  Each read therefore runs alone under the exception guard: GUARD_MARK skips a
 * trapping MRS and writes its marker to the destination register, and exc_count says that it
 * trapped.  GUARD_SILENT keeps the guard's own report off the console (this runs inside the
 * fatal report).  Nothing is written: a write to L2C_ERR_STS acknowledges the latched error,
 * which would destroy the evidence and change what the guest sees next.
 */

#include "hv_fatal.h"

#include "../../adt.h"
#include "../../cpu_regs.h"
#include "../../exception.h"
#include "../../mcc.h"
#include "../../utils.h"

#include "string.h"

static void guard_on(void)
{
    exc_guard = GUARD_MARK | GUARD_SILENT;
    sysop("isb");
}

static void guard_off(void)
{
    sysop("isb");
    exc_guard = GUARD_OFF;
}

bool hv_fatal_apl_err_read(unsigned int which, const char **name, u64 *val)
{
    static const char *const p_names[HV_FATAL_APL_REGS] = {
        "L2C_ERR_STS", "L2C_ERR_ADR", "L2C_ERR_INF", "LSU_ERR_STS", "FED_ERR_STS", "MMU_ERR_STS"};
    static const char *const e_names[HV_FATAL_APL_REGS] = {"L2C_ERR_STS",   "L2C_ERR_ADR",
                                                           "L2C_ERR_INF",   "E_LSU_ERR_STS",
                                                           "E_FED_ERR_STS", "E_MMU_ERR_STS"};

    *val = 0;
    if (which >= HV_FATAL_APL_REGS) {
        *name = "?";
        return false;
    }
    bool e = is_ecore();
    *name = e ? e_names[which] : p_names[which];

    int before = exc_count;
    u64 v = 0;
    /* Only the one MRS runs between guard_on() and guard_off(). */
    guard_on();
    switch (which) {
        case 0:
            v = mrs(SYS_IMP_APL_L2C_ERR_STS);
            break;
        case 1:
            v = mrs(SYS_IMP_APL_L2C_ERR_ADR);
            break;
        case 2:
            v = mrs(SYS_IMP_APL_L2C_ERR_INF);
            break;
        case 3:
            if (e)
                v = mrs(SYS_IMP_APL_E_LSU_ERR_STS);
            else
                v = mrs(SYS_IMP_APL_LSU_ERR_STS);
            break;
        case 4:
            if (e)
                v = mrs(SYS_IMP_APL_E_FED_ERR_STS);
            else
                v = mrs(SYS_IMP_APL_FED_ERR_STS);
            break;
        default:
            if (e)
                v = mrs(SYS_IMP_APL_E_MMU_ERR_STS);
            else
                v = mrs(SYS_IMP_APL_MMU_ERR_STS);
            break;
    }
    guard_off();

    if (exc_count != before)
        return false;
    *val = v;
    return true;
}

/*
 * G6 r6: where L2C_ERR_ADR points.  Its address field is taken as bits 41:0.  That is an
 * inference, not a documented layout: T8152 DRAM lies below 2^42, the register's upper bits
 * were set in a G6 run, and its low 42 bits named a line inside an ADT carve-out.
 * The smallest /chosen/carveout-memory-map entry that holds the address is named; the
 * usable-RAM entry holds all of normal RAM, so an address there is named by it.  Whether the
 * address lies in a carve-out mcc_unmap_carveouts() took out of aura's EL2 map (mcc_carveouts[],
 * region-id-4 and region-id-2 on T8152) is said too.  Only ADT memory and that table are read
 * (no device, no system register), so this is safe inside the fatal report.
 */
void hv_fatal_apl_report_adr(u64 adr)
{
    u64 pa = adr & GENMASK(41, 0);
    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");

    printf("HV: fatal: L2C_ERR_ADR bits 41:0 (taken as the physical address): 0x%lx", pa);
    if (node < 0) {
        printf(", carve-out map missing\n");
        return;
    }

    const char *best = NULL;
    u64 best_start = 0, best_size = 0;
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        u64 r[2];
        if (prop->size != sizeof(r))
            continue;
        memcpy(r, prop->value, sizeof(r));
        if (!r[1] || pa < r[0] || pa - r[0] >= r[1])
            continue;
        if (!best || r[1] < best_size) {
            best = prop->name;
            best_start = r[0];
            best_size = r[1];
        }
    }
    bool unmapped = false;
    for (size_t i = 0; i < mcc_carveout_count; i++)
        if (mcc_carveouts[i].size && pa >= mcc_carveouts[i].base &&
            pa - mcc_carveouts[i].base < mcc_carveouts[i].size)
            unmapped = true;
    if (!best)
        printf(", in no carve-out%s\n", unmapped ? " (EL2-unmapped by mcc)" : "");
    else
        printf(", in carve-out %s [0x%lx, 0x%lx) at +0x%lx%s\n", best, best_start,
               best_start + best_size, pa - best_start, unmapped ? " (EL2-unmapped by mcc)" : "");
}
