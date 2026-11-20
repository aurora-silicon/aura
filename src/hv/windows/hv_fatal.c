/* SPDX-License-Identifier: MIT */

/*
 * T8152 fatal-exception context.  See hv_fatal.h for what is recorded and why, and
 * windows-m6/D3-RESULTS.md for the run that asked for it.  Written on this branch from the
 * Arm ARM register descriptions (ESR_ELx ISS for SError, PAR_EL1, ISR_EL1, DISR_EL1,
 * HPFAR_EL2) and the AAPCS64 frame-record layout; no third-party source.
 *
 * Containment.  The carrier does not try to resume after an SError taken at EL2.  The Arm
 * ARM allows a handler to continue only when the syndrome classifies the error (IDS = 0,
 * DFSC = 0b010001, AET = corrected or restartable) and the handler knows which state was
 * affected.  D3 run 1's syndrome was ESR 0xbe000000: EC 0x2f with ISS 0, i.e. no
 * classification at all, and Apple's own error registers are locked from this EL.  So the
 * report says what the syndrome was and the caller reboots as before.
 */

#include "hv_fatal.h"
#include "hv_ahci.h"
#include "hv_gpu.h"
#include "hv_input.h"
#include "hv_scanout.h"
#include "hv_serror.h"
#include "hv_usb.h"
#include "soc.h"
#include "utils.h"

struct hv_fatal_cpu hv_fatal_cpus[MAX_CPUS];
bool hv_fatal_armed;
static u32 hv_fatal_reporting;
/* hv_windows_guest_serror_report() is the caller: the carrier goes on (the host decides). */
static bool hv_fatal_guest_caller;

/* ---- writers (hot paths: a few stores and one barrier, T8152 only) ------------------------ */

void hv_fatal_note_trap(u64 kind, u64 esr, u64 elr, u64 spsr, u64 far)
{
    if (chip_id != T8152)
        return;

    struct hv_fatal_trap *t = &hv_fatal_cpus[smp_id()].trap;
    u64 ec = (esr >> 26) & 0x3f;

    hv_fatal_seq_begin(&t->seq);
    t->kind = kind;
    t->esr = esr;
    t->elr = elr;
    t->spsr = spsr;
    t->far = far;
    /* ESR_EC_IABORT_LOWER 0x20, ESR_EC_DABORT_LOWER 0x24: the only exits HPFAR describes. */
    t->hpfar = (kind == HV_FATAL_TRAP_SYNC && (ec == 0x20 || ec == 0x24)) ? mrs(HPFAR_EL2) : 0;
    t->cntpct = mrs(CNTPCT_EL0);
    t->isr = mrs(ISR_EL1);
    t->ret_elr = t->ret_spsr = t->ret_isr = t->ret_cntpct = 0;
    hv_fatal_seq_end(&t->seq);
}

void hv_fatal_note_return(void)
{
    if (chip_id != T8152)
        return;
    /* PSTATE.A is masked here: the fence makes an error from this exit's EL2 accesses pending
     * (ISR_EL1.A) instead of letting it arrive after the ERET, where it reads as the guest's. */
    sysop("dsb sy");
    struct hv_fatal_trap *t = &hv_fatal_cpus[smp_id()].trap;
    hv_fatal_seq_begin(&t->seq);
    t->ret_elr = mrs(ELR_EL2);
    t->ret_spsr = mrs(SPSR_EL2);
    t->ret_isr = mrs(ISR_EL1);
    t->ret_cntpct = mrs(CNTPCT_EL0);
    hv_fatal_seq_end(&t->seq);
}

void hv_fatal_trap_snapshot(struct hv_fatal_trap *out)
{
    *out = (struct hv_fatal_trap){0};
    if (chip_id != T8152)
        return;
    struct hv_fatal_trap *t = &hv_fatal_cpus[smp_id()].trap;
    /* This CPU owns the record and is not inside it (it is in an exception vector). */
    hv_fatal_read(&t->seq, t, out, sizeof(*out));
}

void hv_fatal_stage(u64 stage)
{
    if (chip_id != T8152)
        return;

    sysop("dsb sy");
    sysop("isb");
    struct hv_fatal_cpu *c = &hv_fatal_cpus[smp_id()];
    c->stage = stage;
    c->stage_cntpct = mrs(CNTPCT_EL0);
}

void hv_fatal_fb_begin(u64 start, u64 end)
{
    if (chip_id != T8152)
        return;

    /* Fence before opening the record, as hv_fatal_stage() does: the idle path in
     * hv_exc_sync() calls the converter with no fence before it, and an SError left pending by
     * the trap handled just before must be taken here, while the record still says "done",
     * not inside the band where it would be blamed on the scanout stores. */
    sysop("dsb sy");
    sysop("isb");
    struct hv_fatal_fb *f = &hv_fatal_cpus[smp_id()].fb;
    hv_fatal_seq_begin(&f->seq);
    f->start = start;
    f->end = end;
    f->cntpct = mrs(CNTPCT_EL0);
}

void hv_fatal_fb_end(void)
{
    if (chip_id != T8152)
        return;

    sysop("dsb sy");
    sysop("isb");
    struct hv_fatal_fb *f = &hv_fatal_cpus[smp_id()].fb;
    f->isr = mrs(ISR_EL1);
    hv_fatal_seq_end(&f->seq);
}

/* ---- pure helpers (host-tested) ----------------------------------------------------------- */

int hv_fatal_unwind(u64 fp, u64 lo, u64 hi, u64 *ret, int max)
{
    int n = 0;

    while (n < max) {
        if (fp < lo || fp >= hi || hi - fp < 16 || (fp & 7))
            break;
        const volatile u64 *rec = (const volatile u64 *)fp;
        u64 next = rec[0], lr = rec[1];
        if (!lr)
            break;
        ret[n++] = lr;
        if (next <= fp)
            break;
        fp = next;
    }
    return n;
}

const char *hv_fatal_fst_name(u64 fst)
{
    switch (fst & 0x3f) {
        case 0x00 ... 0x03:
            return "address size fault";
        case 0x04 ... 0x07:
            return "translation fault";
        case 0x08 ... 0x0b:
            return "access flag fault";
        case 0x0c ... 0x0f:
            return "permission fault";
        case 0x10:
            return "synchronous external abort";
        case 0x13 ... 0x17:
            return "synchronous external abort on the table walk";
        case 0x18:
            return "parity/ECC error";
        case 0x1b ... 0x1f:
            return "parity/ECC error on the table walk";
        case 0x21:
            return "alignment fault";
        case 0x30:
            return "TLB conflict abort";
        default:
            return "other fault status";
    }
}

const char *hv_fatal_serror_class(u64 esr)
{
    if (((esr >> 26) & 0x3f) != 0x2f)
        return "not an SError syndrome";
    if (esr & BIT(24))
        return "IMPLEMENTATION DEFINED syndrome (IDS=1): no architectural classification";
    u64 dfsc = esr & 0x3f;
    if (dfsc == 0)
        return "uncategorized (DFSC 0): no architectural classification";
    if (dfsc != 0x11)
        return "reserved DFSC";
    switch ((esr >> 10) & 7) {
        case 0:
            return "asynchronous SError, uncontainable (AET UC)";
        case 1:
            return "asynchronous SError, unrecoverable (AET UEU)";
        case 2:
            return "asynchronous SError, restartable (AET UEO)";
        case 3:
            return "asynchronous SError, recoverable (AET UER)";
        case 6:
            return "asynchronous SError, corrected (AET CE)";
        default:
            return "asynchronous SError, reserved AET";
    }
}

/*
 * Where the guest SError was taken relative to the previous exit of the same CPU.  The return
 * point of that exit is ELR_EL2 as _hv_return recorded it; without that record (an exit that
 * never returned through _hv_return), the instruction after a handled synchronous trap (HVC
 * already has ELR_EL2 past it) or the interrupted instruction for an IRQ, FIQ or SError.
 *
 * Taken exactly at the return point means the guest executed nothing between the ERET and the
 * SError: the error was pending when EL2 returned.  ISR_EL1.A at the exit and at the return
 * then say whether it was raised before the exit (the guest's own access just before it, or
 * earlier EL2 work) or by the EL2 work of that exit.
 */
const char *hv_fatal_serror_relation(const struct hv_fatal_trap *prev, u64 elr, u64 age_us,
                                     int *code)
{
    int dummy;
    if (!code)
        code = &dummy;
    if (!prev || !prev->cntpct) {
        *code = HV_SERR_REL_NONE;
        return "no earlier exit recorded on this CPU";
    }
    u64 ret = prev->elr;
    if (prev->ret_cntpct)
        ret = prev->ret_elr;
    else if (prev->kind == HV_FATAL_TRAP_SYNC && ((prev->esr >> 26) & 0x3f) != 0x16)
        ret += 4;
    bool at_exit = prev->isr & BIT(8);
    bool at_return = prev->ret_cntpct && (prev->ret_isr & BIT(8));
    if (elr != ret) {
        if (at_return) {
            *code = HV_SERR_REL_MASKED_RAN;
            return at_exit ? "already pending when the guest exited and still at the return; "
                             "taken later, after the guest unmasked SErrors: raised before "
                             "that exit"
                           : "pending when EL2 returned but not at the exit, taken later after "
                             "the guest unmasked SErrors: raised by the EL2 work of that exit";
        }
        *code = at_exit ? HV_SERR_REL_BEFORE_EXIT : HV_SERR_REL_GUEST_RAN;
        if (at_exit)
            return "the guest ran on past that exit, and an SError was already pending at it: "
                   "it was raised before that exit and only taken later";
        return "the guest ran on past that exit: raised while the guest ran (its own access, "
               "or a late completion of earlier work)";
    }
    if (at_exit) {
        *code = HV_SERR_REL_BEFORE_EXIT;
        return "taken at that exit's return point and already pending when it exited: raised "
               "before that exit (guest access, or earlier EL2 work)";
    }
    if (at_return) {
        *code = HV_SERR_REL_EL2_WORK;
        return "taken at that exit's return point; not pending when the guest exited, pending "
               "when EL2 returned: raised by the EL2 work of that exit";
    }
    if (age_us > 10000) {
        *code = HV_SERR_REL_STALE;
        return "at that exit's return point but long after it: the guest may have re-run that "
               "PC; not attributable";
    }
    if (prev->ret_cntpct) {
        *code = HV_SERR_REL_EL2_LATE;
        return "taken at that exit's return point (the guest ran nothing), not pending at the "
               "return fence: an EL2 access of that exit whose error arrived after the fence";
    }
    *code = HV_SERR_REL_EL2_LATE;
    return "taken at that exit's return point: pending when EL2 returned to the guest (EL2 "
           "work in that exit with PSTATE.A masked, or a guest access just before the exit)";
}

/* ---- the report --------------------------------------------------------------------------- */

static const char *mode_name(u64 spsr)
{
    switch (spsr & 0xf) {
        case 0x0:
            return "EL0t";
        case 0x4:
            return "EL1t";
        case 0x5:
            return "EL1h";
        case 0x8:
            return "EL2t";
        case 0x9:
            return "EL2h";
        default:
            return "?";
    }
}

static u64 age_us(u64 now, u64 then, u64 freq)
{
    if (!then || !freq || then > now)
        return 0;
    return (now - then) * 1000000 / freq;
}

static u64 hpfar_ipa(u64 hpfar, u64 far)
{
    /* HPFAR_EL2.FIPA, bits [47:4], holds IPA[55:12]. */
    return (((hpfar >> 4) & GENMASK(43, 0)) << 12) | (far & 0xfff);
}

static void report_par(u64 par, const char *when)
{
    if (par & BIT(0)) {
        u64 fst = (par >> 1) & 0x3f;
        printf("HV: fatal: PAR_EL1 0x%lx %s: F=1 FST 0x%lx (%s, level %ld)%s%s\n", par, when, fst,
               hv_fatal_fst_name(fst), fst == 0x13 ? -1L : (long)(fst & 3),
               (par & BIT(9)) ? ", stage 2" : ", stage 1",
               (par & BIT(8)) ? " during the stage-1 walk" : "");
    } else {
        printf("HV: fatal: PAR_EL1 0x%lx %s: F=0 PA 0x%lx attr 0x%lx\n", par, when,
               par & GENMASK(51, 12), par >> 56);
    }
}

static void report_cpu(int cpu, int self, u64 now, u64 freq)
{
    struct hv_fatal_cpu *c = &hv_fatal_cpus[cpu];
    struct hv_fatal_trap t;
    struct hv_fatal_fb f;
    u64 ts = hv_fatal_read(&c->trap.seq, &c->trap, &t, sizeof(t));
    u64 fs = hv_fatal_read(&c->fb.seq, &c->fb, &f, sizeof(f));
    u64 stage = c->stage, stage_at = c->stage_cntpct;

    if (!t.cntpct && !f.cntpct && !stage_at)
        return;

    const char *mark = cpu == self ? "*" : " ";
    if (t.cntpct) {
        const char *state = ts == ~0UL ? " (changing)" : (ts & 1) ? " (being written)" : "";
        printf("HV: fatal: cpu%2d%s last trap %c %lu us ago%s: ESR 0x%lx PC 0x%lx %s", cpu, mark,
               (char)t.kind, age_us(now, t.cntpct, freq), state, t.esr, t.elr, mode_name(t.spsr));
        if (t.kind == HV_FATAL_TRAP_SYNC && t.hpfar)
            printf(" FAR 0x%lx IPA 0x%lx", t.far, hpfar_ipa(t.hpfar, t.far));
        if (t.isr & BIT(8))
            printf("; SError pending at that exit");
        if (t.ret_cntpct)
            printf("; returned to PC 0x%lx %lu us later%s", t.ret_elr,
                   t.ret_cntpct > t.cntpct ? age_us(t.ret_cntpct, t.cntpct, freq) : 0,
                   (t.ret_isr & BIT(8)) ? ", SError pending at the return" : "");
        printf("\n");
    }
    printf("HV: fatal: cpu%2d%s EL2", cpu, mark);
    if (stage_at)
        printf(" stage %c %lu us ago;", (char)stage, age_us(now, stage_at, freq));
    if (f.cntpct)
        printf(" fb rows %lu..%lu %s %lu us ago;", f.start, f.end,
               fs == ~0UL ? "changing"
               : (fs & 1) ? "IN PROGRESS"
                          : "done",
               age_us(now, f.cntpct, freq));
    if (f.cntpct && !(fs & 1) && (f.isr & BIT(8)))
        printf(" SError pending after that band;");
    printf(" crumbs ");
    u64 crumbs = hv_wdt_breadcrumbs_read(cpu);
    for (int i = 56; i >= 0; i -= 8) {
        char ch = (crumbs >> i) & 0xff;
        if (ch)
            printf("%c", ch);
    }
    printf("\n");
}

void hv_windows_fatal_report(u64 *regs, const char *what, u64 par, const char *par_when)
{
    if (chip_id != T8152 || !hv_fatal_armed)
        return;
    if (__atomic_exchange_n(&hv_fatal_reporting, 1, __ATOMIC_ACQ_REL)) {
        printf("HV: fatal: %s inside the fatal report; skipping it\n", what);
        return;
    }

    int self = smp_id();
    u64 now = mrs(CNTPCT_EL0), freq = mrs(CNTFRQ_EL0);
    u64 esr = mrs(ESR_EL2), far = mrs(FAR_EL2), hpfar = mrs(HPFAR_EL2);
    u64 isr = mrs(ISR_EL1);

    /*
     * Order: what the handler already holds, then the per-CPU and AHCI records (the part that
     * says what caused the error), then the frame walk, and the optional registers last.
     * DISR_EL1 and the _EL12 set are the reads most likely to trap on these cores; if one
     * does, the re-entry guard ends the report there and the records are already out.
     */
    printf("HV: fatal: %s on cpu %d; T8152 carrier context follows\n", what, self);
    report_par(par, par_when);
    printf("HV: fatal: ESR_EL2 0x%lx: %s\n", esr, hv_fatal_serror_class(esr));
    printf("HV: fatal: ISR_EL1 0x%lx (A=%ld I=%ld F=%ld)\n", isr, (long)((isr >> 8) & 1),
           (long)((isr >> 7) & 1), (long)((isr >> 6) & 1));
    printf("HV: fatal: FAR_EL2 0x%lx HPFAR_EL2 0x%lx (IPA 0x%lx); only aborts write these, so "
           "they are stale after a FIQ, IRQ or SError\n",
           far, hpfar, hpfar_ipa(hpfar, far));

    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        report_cpu(cpu, self, now, freq);

    hv_ahci_fatal_report(now, freq);
    hv_usb_fatal_report();
    hv_gpu_fatal_report();
    hv_input_fatal_report();
    hv_scanout_fatal_report();

    if (regs) {
        /* The EL2 frame sits on this CPU's stack (64 KiB); the interrupted frames are above. */
        u64 frames[12];
        u64 lo = (u64)regs;
        int n = hv_fatal_unwind(regs[29], lo, lo + 0x10000, frames, 12);
        printf("HV: fatal: EL2 lr rel 0x%lx; frames (rel):", regs[30] - (u64)_base);
        for (int i = 0; i < n; i++)
            printf(" 0x%lx", frames[i] - (u64)_base);
        printf("\n");
    }

    /* DISR_EL1 exists only with FEAT_RAS (ID_AA64PFR0_EL1.RAS, bits [31:28]); else it UNDEFs. */
    if ((mrs(ID_AA64PFR0_EL1) >> 28) & 0xf)
        printf("HV: fatal: DISR_EL1 0x%lx\n", mrs(DISR_EL1));
    else
        printf("HV: fatal: DISR_EL1 absent (no FEAT_RAS)\n");
    printf("HV: fatal: guest EL1 ELR_EL12 0x%lx ESR_EL12 0x%lx FAR_EL12 0x%lx SPSR_EL12 0x%lx\n",
           mrs(ELR_EL12), mrs(ESR_EL12), mrs(FAR_EL12), mrs(SPSR_EL12));
    if (hv_fatal_guest_caller)
        printf("HV: fatal: %s: the host decides what follows (run_guest --windows-serror: abort "
               "exits the guest, continue resumes it)\n",
               what);
    else
        printf("HV: fatal: not contained: the carrier does not resume after this (%s)\n", what);
}

void hv_windows_guest_serror_report(const u64 *regs, const u64 *sp, u64 esr, u64 elr, u64 spsr,
                                    u64 far, u64 afsr1, const struct hv_fatal_trap *prev,
                                    u64 par)
{
    if (chip_id != T8152 || !hv_fatal_armed)
        return;
    if (__atomic_load_n(&hv_fatal_reporting, __ATOMIC_ACQUIRE)) {
        printf("HV: fatal: guest SError inside the fatal report; skipping it\n");
        return;
    }

    int self = smp_id();
    u64 now = mrs(CNTPCT_EL0), freq = mrs(CNTFRQ_EL0);
    u64 age = prev && prev->cntpct ? age_us(now, prev->cntpct, freq) : 0;
    u64 isr = mrs(ISR_EL1);

    /* What the vector read, before anything else: the record lines below can only add. */
    printf("HV: fatal: guest SError on cpu %d: ESR_EL2 0x%lx (%s); guest PC 0x%lx %s SPSR "
           "0x%lx; FAR_EL2 0x%lx HPFAR_EL2 0x%lx (both stale: an SError writes neither)\n",
           self, esr, hv_fatal_serror_class(esr), elr, mode_name(spsr), spsr, far,
           mrs(HPFAR_EL2));
    printf("HV: fatal: guest SError on cpu %d: AFSR1_EL2 0x%lx (IMPLEMENTATION DEFINED, raw); "
           "ISR_EL1 now 0x%lx (A=%ld: %s)\n",
           self, afsr1, isr, (long)((isr >> 8) & 1),
           (isr & BIT(8)) ? "another SError is already pending" : "nothing else pending");
    if (prev && prev->cntpct) {
        printf("HV: fatal: cpu%2d previous exit %c %lu us before: ESR 0x%lx PC 0x%lx %s", self,
               (char)prev->kind, age, prev->esr, prev->elr, mode_name(prev->spsr));
        if (prev->kind == HV_FATAL_TRAP_SYNC && prev->hpfar)
            printf(" FAR 0x%lx IPA 0x%lx", prev->far, hpfar_ipa(prev->hpfar, prev->far));
        printf("; ISR_EL1 at it 0x%lx\n", prev->isr);
        if (prev->ret_cntpct)
            printf("HV: fatal: cpu%2d that exit returned to PC 0x%lx %s (SPSR 0x%lx, A %s) %lu us "
                   "before the SError; ISR_EL1 after the return fence 0x%lx\n",
                   self, prev->ret_elr, mode_name(prev->ret_spsr), prev->ret_spsr,
                   (prev->ret_spsr & BIT(8)) ? "masked" : "unmasked",
                   age_us(now, prev->ret_cntpct, freq), prev->ret_isr);
        else
            printf("HV: fatal: cpu%2d that exit has no return record\n", self);
    }
    int code;
    const char *why = hv_fatal_serror_relation(prev, elr, age, &code);
    printf("HV: fatal: cpu%2d SError vs previous exit: verdict %d: %s\n", self, code, why);

    hv_fatal_guest_caller = true;
    hv_windows_fatal_report(NULL, "guest SError", par, "at the guest SError");
    hv_fatal_guest_caller = false;

    if (regs) {
        for (int i = 0; i < 31; i += 4) {
            printf("HV: fatal: guest x%d-x%d:", i, i + 3 < 30 ? i + 3 : 30);
            for (int j = i; j < i + 4 && j < 31; j++)
                printf(" %016lx", regs[j]);
            printf("\n");
        }
        if (sp)
            printf("HV: fatal: guest SP_EL0 %016lx SP_EL1 %016lx\n", sp[0], sp[1]);
        /* Last: it reads guest page tables and code (guest RAM only), the riskiest part. */
        hv_serror_attribute(regs, sp, esr, elr, spsr);
    }
    __atomic_store_n(&hv_fatal_reporting, 0, __ATOMIC_RELEASE);
}
