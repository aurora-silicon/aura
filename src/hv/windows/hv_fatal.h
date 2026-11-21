/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_FATAL_H
#define HV_WINDOWS_FATAL_H

/*
 * T8152 fatal-exception context (D3 run 1).
 *
 * An SError on Apple cores is asynchronous: the PC it is taken at says where EL2 noticed it,
 * not what caused it.  D3 run 1 died at the instruction after an AT in hv_translate() with a
 * FAR left over from an earlier AHCI trap and the framebuffer converter's pointers still in
 * x12-x16, and nothing in the dump said which of those was live.  So every CPU keeps a small
 * record of what EL2 last did, and the EL2 "Unhandled exception" paths print it before the
 * reboot:
 *
 *  - the last trap from the guest (kind, ESR, guest PC, SPSR, FAR, HPFAR, when);
 *  - the hv_tick() stage the CPU was in (each stage boundary is an SError fence on T8152, so
 *    an error a stage caused is taken with that stage still recorded);
 *  - the last framebuffer converter band (in progress or done, when);
 *  - and, from hv_ahci.c, the last AHCI register access and command with its guest spans.
 *
 * The records are seqlocked so the reporting CPU can tell a stable record from one its owner
 * is rewriting.  Writing them is a few stores and one barrier per trap; nothing here prints,
 * blocks or allocates on the guest's path.  Other chips do not record (chip gate).
 */

#include "smp.h"
#include "types.h"

/* Trap kinds, as recorded by hv_fatal_note_trap(). */
#define HV_FATAL_TRAP_SYNC     'S'
#define HV_FATAL_TRAP_FIQ      'F'
#define HV_FATAL_TRAP_FIQ_FAST 'f'
#define HV_FATAL_TRAP_IRQ      'I'
#define HV_FATAL_TRAP_SERROR   'E'

/* hv_tick() stages (hv.c).  0 = not in the tick. */
#define HV_FATAL_STAGE_NONE     0
#define HV_FATAL_STAGE_FB       'c' /* hv_fb_convert_slice(HV_FB_SLICE_TICK) */
#define HV_FATAL_STAGE_REPORT   'r' /* hv_fb_report(): fb, guest diag and AHCI lines */
#define HV_FATAL_STAGE_TRACK    'k' /* hv_track_t8142_undef_vector() */
#define HV_FATAL_STAGE_VERIFY   'v' /* hv_verify_t8142_undef_vectors() */
#define HV_FATAL_STAGE_MODULES  'm' /* hv_scan_t8142_guest_modules() */
#define HV_FATAL_STAGE_PROCESS  'p' /* hv_report_t8142_process(), GIC activity */
#define HV_FATAL_STAGE_MTP      'u' /* MTP poll/report */
#define HV_FATAL_STAGE_BUGCHECK 'b' /* hv_check_t8142_bugcheck() */
#define HV_FATAL_STAGE_RECOVER  'R' /* hv_recover_t8142_sysreg_undef() */
#define HV_FATAL_STAGE_IO       'i' /* proxy iodev and vUART polling */
#define HV_FATAL_STAGE_DONE     '.' /* hv_tick() returned */

/* Seqlock word: odd while the owner rewrites the record. */
struct hv_fatal_trap {
    u64 seq;
    u64 kind;
    u64 esr, elr, spsr, far, hpfar;
    u64 cntpct;
    u64 isr; /* ISR_EL1 at the exit, before EL2 did anything: A (bit 8) = SError pending */
    /*
     * The return to the guest that ended this exit (hv_fatal_note_return(), from _hv_return
     * after a `dsb sy`): ELR_EL2 and SPSR_EL2 as the ERET uses them, ISR_EL1 then, and when.
     * Zero until that return.  ISR.A clear at the exit but set here means the EL2 work of this
     * exit raised an SError (i0 session 2: the guest SError taken at a return point).
     */
    u64 ret_elr, ret_spsr, ret_isr, ret_cntpct;
};

struct hv_fatal_fb {
    u64 seq; /* odd while hv_fb_convert_slice() runs on this CPU */
    u64 start, end, cntpct;
    /* ISR_EL1 after the closing fence: on the idle path PSTATE.A is masked, so an SError the
     * band's stores raised is pending here instead of taken (i0 session 2). */
    u64 isr;
};

struct hv_fatal_cpu {
    struct hv_fatal_trap trap;
    struct hv_fatal_fb fb;
    u64 stage;
    u64 stage_cntpct;
} __attribute__((aligned(64)));

extern struct hv_fatal_cpu hv_fatal_cpus[MAX_CPUS];
/* Set at T8152 guest entry (hv_start()), cleared when it returns. */
extern bool hv_fatal_armed;

static inline void hv_fatal_seq_begin(u64 *seq)
{
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void hv_fatal_seq_end(u64 *seq)
{
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELEASE);
}

/*
 * Copy a seqlocked record written by another CPU.  Up to four tries for a stable even copy;
 * returns the sequence word of the copy it kept: even = stable, odd = the owner was inside
 * the record (in progress), or ~0 if it changed under every try.  `seq` lies inside `src`.
 */
static inline u64 hv_fatal_read(const u64 *seq, const void *src, void *dst, u64 len)
{
    for (int i = 0; i < 4; i++) {
        u64 s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        const volatile u8 *s = src;
        u8 *d = dst;
        for (u64 j = 0; j < len; j++)
            d[j] = s[j];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u64 s1 = __atomic_load_n(seq, __ATOMIC_RELAXED);
        if (s0 == s1 && (!(s0 & 1) || i == 3))
            return s0;
    }
    return ~0UL;
}

/*
 * Writers, each for the calling CPU's own record and a no-op off T8152.
 *
 * hv_fatal_note_trap(): every guest exit, right after hv_get_context().  HPFAR_EL2 is read
 * only for a lower-EL instruction or data abort, where it is defined.
 *
 * hv_fatal_stage(): an hv_tick() stage boundary.  `dsb sy; isb` first: an SError the previous
 * stage caused becomes pending at the DSB and is taken there with that stage still recorded,
 * not somewhere later.
 *
 * hv_fatal_fb_begin()/_end(): around one converter band.  Both fence the same way: _begin()
 * before it opens the record, so an SError pending from earlier work (the idle path calls the
 * converter with no fence of its own) is taken while the record still says "done"; _end()
 * before it closes it, so one from the band's scanout stores is taken while the record still
 * says "in progress".
 */
void hv_fatal_note_trap(u64 kind, u64 esr, u64 elr, u64 spsr, u64 far);
/*
 * hv_fatal_note_return(): _hv_return, every return to the guest (all four vectors, fast paths
 * included), after the C handler and before the guest registers are reloaded.  `dsb sy` first,
 * so a device access EL2 made in this exit has completed and its error is pending in ISR_EL1
 * (PSTATE.A is masked at EL2 here) before the record reads it.  T8152 only.
 */
void hv_fatal_note_return(void);
void hv_fatal_stage(u64 stage);
void hv_fatal_fb_begin(u64 start, u64 end);
void hv_fatal_fb_end(void);

/*
 * Walk AAPCS64 frame records from `fp`.  Every record must lie inside [lo, hi), be 8-byte
 * aligned and sit above the previous one, so a corrupt chain stops instead of faulting.
 * Stores at most `max` return addresses; returns how many.
 */
int hv_fatal_unwind(u64 fp, u64 lo, u64 hi, u64 *ret, int max);

/* Text for a PAR_EL1 fault status code (PAR_EL1.FST, F = 1). */
const char *hv_fatal_fst_name(u64 fst);

/*
 * What an SError syndrome (ESR_ELx, EC 0x2f) says about containment.  Never "containable"
 * here: the carrier contains nothing (see hv_fatal.c); this only classifies the syndrome.
 */
const char *hv_fatal_serror_class(u64 esr);

/*
 * The report.  Called by exception.c on the EL2 "Unhandled exception" paths and by the
 * unhandled-guest-exception reset, before the reboot.  `regs` is the EL2 exception frame (NULL
 * when the frame is the guest's).  `par` is a PAR_EL1 value the caller read and `par_when`
 * says when, for the report line: "at entry" from the EL2 handlers (read first thing), "before
 * the proxy's translations" from _hv_exc_proxy() (its own ATs overwrite PAR).  Prints nothing
 * unless the chip is T8152 and a guest was entered; prints once per boot (a fault inside the
 * report itself falls through to the caller's reboot).
 */
void hv_windows_fatal_report(u64 *regs, const char *what, u64 par, const char *par_when);

/*
 * Lower-EL SErrors (i0 session 2).  hv_exc_serr() forwards an SError the guest took to the
 * host, which ends the session; until this, nothing was printed and the per-CPU record of the
 * exit before it was overwritten by the SError's own.  The SError is asynchronous: the guest
 * PC says where it was taken, not what caused it, so the report sets it against the previous
 * exit of the same CPU.  An SError taken at that exit's return point within microseconds was
 * pending when EL2 returned (EL2 work done with PSTATE.A masked -- the fast trap paths and
 * the idle converter never unmask it -- or a guest access issued just before that exit); one
 * taken after the guest ran on was raised while the guest ran.
 *
 * hv_fatal_trap_snapshot(): copy this CPU's trap record; call it before the SError's own
 * hv_fatal_note_trap() overwrites it.
 * hv_fatal_serror_relation(): that comparison as text (pure; host-tested).
 * hv_windows_guest_serror_report(): the report, then hv_windows_fatal_report() for every CPU's
 * records; `regs` are the guest's x0-x30.  Unlike the EL2 paths it releases the report latch
 * afterwards: the carrier keeps running (the host exits the guest), and a later EL2 fatal
 * still needs its report.
 */
void hv_fatal_trap_snapshot(struct hv_fatal_trap *out);
/*
 * Verdict codes of hv_fatal_serror_relation(), also printed as "verdict <code>" so a log
 * search does not depend on the wording.
 */
#define HV_SERR_REL_NONE         0 /* no earlier exit recorded */
#define HV_SERR_REL_BEFORE_EXIT  1 /* pending already when the guest exited */
#define HV_SERR_REL_EL2_WORK     2 /* not pending at the exit, pending at the return: EL2 */
#define HV_SERR_REL_EL2_LATE     3 /* at the return point, guest ran nothing, after the fence */
#define HV_SERR_REL_GUEST_RAN    4 /* the guest ran on: its own access or a late completion */
#define HV_SERR_REL_MASKED_RAN   5 /* pending at the return, taken later (guest masked A) */
#define HV_SERR_REL_STALE        6 /* at the return point but long after it */
const char *hv_fatal_serror_relation(const struct hv_fatal_trap *prev, u64 elr, u64 age_us,
                                     int *code);
/*
 * `sp` are the guest's SP_EL0 and SP_EL1, `afsr1` AFSR1_EL2 at the vector (IMPLEMENTATION
 * DEFINED contents; printed raw).  After the records and registers it runs
 * hv_serror_attribute() (hv_serror.h): which guest addresses near the SError reach a
 * passthrough or identity region.
 */
void hv_windows_guest_serror_report(const u64 *regs, const u64 *sp, u64 esr, u64 elr, u64 spsr,
                                    u64 far, u64 afsr1, const struct hv_fatal_trap *prev,
                                    u64 par);

/* hv_wdt.c: this CPU's last eight breadcrumbs, oldest in the top byte. */
u64 hv_wdt_breadcrumbs_read(int cpu);

/*
 * G6 r5 (run 20261120-053521): verdict 2 named a trapped ICC_IAR1_EL1 read on cpu 6, but the EL2
 * path of a trapped ICC_* read on T8152 is the software GIC model (hv_handle_t8152_gic_cpuif()
 * and hv_update_fiq()): carrier memory and system registers, no device access.  An SError that
 * becomes pending inside such an exit was raised by an earlier access (this CPU's, or another
 * agent's) and only signalled in that window, so the verdict names the window, not the source.
 * hv_fatal_exit_device_free(): the name of the register for such an exit (an ICC_* read, or an
 * ICC_PMR_EL1 / BPR / CTLR write), else NULL (pure; host-tested).  ICC_EOIR1/DIR/SGI1R and
 * IGRPEN1 writes are not listed: completing an AIC-alias SPI unmasks its AIC line (MMIO) and the
 * first Group 1 enable on a CPU logs a line (console, so the DWC3).
 */
const char *hv_fatal_exit_device_free(u64 esr);

/*
 * hv_fatal_apl.c (carrier only): Apple's implementation-defined error-status registers,
 * read only, one at a time under the exception guard (an UNDEF is skipped and reported, never
 * fatal).  Never written: a write acknowledges (clears) the latched error.  `which` runs from 0
 * to HV_FATAL_APL_REGS - 1; false when the read trapped.  The SError of the G6 runs stays
 * pending after `continue` (a latched source), which is what these registers latch.
 */
#define HV_FATAL_APL_REGS 6
bool hv_fatal_apl_err_read(unsigned int which, const char **name, u64 *val);

/*
 * hv_fatal_apl.c (carrier only), G6 r6: one line naming the ADT carve-out that holds
 * L2C_ERR_ADR's address (bits 41:0), or saying that none does.  Reads ADT memory only.
 */
void hv_fatal_apl_report_adr(u64 adr);

#endif
