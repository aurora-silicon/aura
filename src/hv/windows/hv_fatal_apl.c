/* SPDX-License-Identifier: MIT */

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

#include "../../cpu_regs.h"
#include "../../exception.h"
#include "../../utils.h"

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
