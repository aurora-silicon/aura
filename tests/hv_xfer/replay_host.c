/* SPDX-License-Identifier: MIT */
/*
 * Host-side runtime for the HvXfer replay, compiled with the HOST headers.
 *
 * The real hv_vm.c touches four EL1/EL2 system registers on the paths the
 * replay runs: hv_pt_init() reads ID_AA64MMFR0_EL1 and writes VTCR_EL2 and
 * VTTBR_EL2; hv_translate() reads SCTLR_EL12 to see whether the guest MMU is
 * on. At EL0 those trap as SIGILL. The handler below emulates exactly those
 * (and refuses anything else), so the real code runs unmodified:
 *   ID_AA64MMFR0_EL1.PARange = 0b0100 (44-bit, the hv_pt_init() ceiling)
 *   SCTLR_EL12 = 0 (guest MMU off: stage-1 is identity, IPA == VA)
 *   MSR to VTCR_EL2 / VTTBR_EL2: recorded and ignored
 */

#define _DARWIN_C_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

/* MRS/MSR system-register field (op0:op1:CRn:CRm:op2 = insn[20:5]). */
#define SYSREG(op0, op1, crn, crm, op2)                                                            \
    ((((op0) & 1) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
#define SR_ID_AA64MMFR0_EL1 SYSREG(3, 0, 0, 7, 0)
#define SR_SCTLR_EL12       SYSREG(3, 5, 1, 0, 0)
#define SR_VTCR_EL2         SYSREG(3, 4, 2, 1, 2)
#define SR_VTTBR_EL2        SYSREG(3, 4, 2, 1, 0)

static FILE *el2_log;
static unsigned long sysreg_emulated, sysreg_refused;

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vfprintf(el2_log ? el2_log : stderr, fmt, ap);
    va_end(ap);
    if (el2_log)
        fflush(el2_log);
    return ret;
}

int replay_open_log(const char *path)
{
    el2_log = fopen(path, "w");
    return el2_log ? 0 : -1;
}

void *memalign(size_t align, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size))
        return NULL;
    return p;
}

void __assert_fail(const char *assertion, const char *file, unsigned int line,
                   const char *function)
{
    debug_printf("EL2 ASSERTION FAILED: %s at %s:%u (%s)\n", assertion, file, line, function);
    fprintf(stderr, "EL2 ASSERTION FAILED: %s at %s:%u (%s)\n", assertion, file, line, function);
    abort();
}

static void sigill_handler(int sig, siginfo_t *info, void *uctx)
{
    ucontext_t *uc = uctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    uint32_t insn = *(const uint32_t *)pc;
    uint32_t sysreg = (insn >> 5) & 0x7fff;
    uint32_t rt = insn & 31;

    (void)info;
    if ((insn & 0xfff00000u) == 0xd5300000u) { /* MRS Xt, <sysreg> */
        uint64_t value;
        if (sysreg == SR_ID_AA64MMFR0_EL1)
            value = 4; /* PARange 0b0100: 44 bits */
        else if (sysreg == SR_SCTLR_EL12)
            value = 0;
        else
            goto refuse;
        if (rt < 29)
            uc->uc_mcontext->__ss.__x[rt] = value;
        else if (rt == 29)
            uc->uc_mcontext->__ss.__fp = value;
        else if (rt == 30)
            uc->uc_mcontext->__ss.__lr = value;
        sysreg_emulated++;
        uc->uc_mcontext->__ss.__pc = pc + 4;
        return;
    }
    if ((insn & 0xfff00000u) == 0xd5100000u &&
        (sysreg == SR_VTCR_EL2 || sysreg == SR_VTTBR_EL2)) { /* MSR <sysreg>, Xt */
        sysreg_emulated++;
        uc->uc_mcontext->__ss.__pc = pc + 4;
        return;
    }
refuse:
    sysreg_refused++;
    fprintf(stderr, "replay: unexpected privileged instruction 0x%08x at 0x%llx\n", insn,
            (unsigned long long)pc);
    signal(sig, SIG_DFL);
}

int replay_install_sigill(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGILL, &sa, NULL);
}

unsigned long replay_sysreg_stats(int which)
{
    return which ? sysreg_refused : sysreg_emulated;
}
