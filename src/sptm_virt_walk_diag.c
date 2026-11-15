/* SPDX-License-Identifier: MIT */
/*
 * sptm_virt_walk_diag.c -- dev-only diagnostic for the "acct patch runs stock on the ESP self-boot"
 * question (AURACTL-9..16). Suggested by Dj's HV bot (agents seq 1259).
 *
 * The acct auractl entry is a 5-instruction patch in XNU __TEXT_EXEC. aura writes it to the physical
 * BootKC-rx word (BootKC-rx.pa + text_off) on both boot paths. On the host-chainload path the running
 * kernel executes the PATCHED bytes (auractl works, AURACTL-5). On the ESP self-boot the running
 * kernel executes STOCK bytes (auractl status -> ENOENT), even though read32(patch_pa) == patched and
 * the data cache was flushed + the I-cache invalidated before entry. So the executing kernel's
 * __TEXT_EXEC is fetched from a physical source other than aura's patched copy.
 *
 * This diagnostic measures exactly that, from aura, once the guest kernel's stage-1 MMU is live:
 * translate the patch SITE's guest VA through XNU's own stage-1 AND aura's stage-2 (hv_translate ->
 * AT S12E1R, the same mechanism the MMIO tracer and auractl use for guest VAs) and read the word the
 * CPU would actually fetch there. Compare that runtime PA with the PA aura patched:
 *
 *   - runtime_pa != patch_pa  -> XNU executes a DIFFERENT physical copy; aura must patch that copy
 *                                (or whatever SPTM/TXM re-copies/re-maps __TEXT_EXEC to).
 *   - runtime_pa == patch_pa but word_at_runtime == stock -> something rewrote the word after aura's
 *                                patch (a late SPTM/TXM text restore / CTRR relock); patch later, or
 *                                trap the write via stage 2 and re-apply.
 *
 * Trigger: the reset-guard WDT hook (sptm_virt_rg_hook), armed unconditionally on this SoC. XNU's
 * watchdog driver pets /arm-io/wdt repeatedly once the kernel is up, so the hook fires from XNU's own
 * EL1 regime after its stage-1 MMU is on -- exactly the context AT S12E1R needs, and with no
 * dependence on the acct patch (which is the broken thing we are diagnosing) or on any auractl entry.
 *
 * Readout, tetherless (the ESP self-boot has no console and no working auractl entry): the result is
 * written, tagged with WALK_MAGIC, into a page of aura's carve-out heap. aura's carve-out is a
 * reserved-memory region; Linux does not scrub it, and DRAM survives a warm reboot, so after a
 * one-shot Macintosh HD self-boot the result is recovered from Aurora Linux by scanning the reserved
 * carve-out range in /dev/mem for WALK_MAGIC (tools/walk-diag-read.sh). The same fields are also
 * pushed into the S2R trace ring, and printed to the aura console for the host-chainload reference
 * run where the tether carries console.
 *
 * Clean-room: every value here is ours and named. Dev-only: compiled under AURACTL_WALK_DIAG, never in
 * a shipped build (the Makefile gates it on SPTM_VIRT_ESP=1 AURACTL=1 and it is never set for Stage 1).
 */

#include "adt.h"
#include "cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "s2r_dcp_pkt.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "utils.h"

#ifdef AURACTL_WALK_DIAG

/* Set from the manifest `walk <site_va> <text_off>` directive (esp_plan.c / esp.c). */
u64 sptm_virt_walk_site_va;  /* guest VA of the first patched acct instruction */
u64 sptm_virt_walk_text_off; /* its offset in BootKC __TEXT_EXEC (for logging) */
/* Set by sptm_virt_esp_boot from the acct ESP_XNU patch: the PA aura actually wrote. */
u64 sptm_virt_walk_patch_pa;

/* "ACTWALK1" -- the tag the Linux-side reader scans the reserved carve-out for. */
#define WALK_MAGIC 0x31574b4c41544341UL

/*
 * Result layout at the scratch page (also the S2R ring tags). Fixed, so walk-diag-read.sh can decode
 * it without the build. All u64.
 *   +0x00 WALK_MAGIC
 *   +0x08 site_va           guest VA that was translated
 *   +0x10 runtime_pa        PA XNU's live stage1+stage2 maps site_va to (0 = translate faulted)
 *   +0x18 patch_pa          PA aura patched (BootKC-rx + text_off)
 *   +0x20 word_at_runtime   first 4 bytes XNU would fetch at site_va (low 32 bits)
 *   +0x28 word_at_patch_pa  first 4 bytes at aura's patched PA (sanity: should be the patched insn)
 *   +0x30 par               PAR_EL1 from the S12E1R translation (attributes/fault detail)
 *   +0x38 verdict           1 = runtime==patch & word matches (patch reaches the kernel)
 *                           2 = runtime_pa != patch_pa (different physical copy)
 *                           3 = runtime_pa == patch_pa but word_at_runtime == stock (rewritten)
 */
#define WALK_OFF_MAGIC    0x00
#define WALK_OFF_SITE_VA  0x08
#define WALK_OFF_RUN_PA   0x10
#define WALK_OFF_PATCH_PA 0x18
#define WALK_OFF_WORD_RUN 0x20
#define WALK_OFF_WORD_PAT 0x28
#define WALK_OFF_PAR      0x30
#define WALK_OFF_VERDICT  0x38

static u64 walk_scratch; /* carve-out page PA, == heap pointer (identity), 0 until set up */

/* S2R ring tags so a working auractl entry can also read the result (print/decode). */
#define WALK_TAG_RUNTIME 0xACC7A11EUL /* record: pa=runtime_pa, val=word_at_runtime */
#define WALK_TAG_PATCH   0xACC7B0B0UL /* record: pa=patch_pa,   val=word_at_patch_pa */

void sptm_virt_walk_diag_setup(void)
{
    if (!sptm_virt_walk_site_va)
        return;
    /* A page of aura's carve-out heap for the tetherless readout (heap pointer == PA). */
    void *p = memalign(0x4000, 0x4000);
    if (!p) {
        printf("SPTM-virt[walk]: no carve-out page for the diagnostic\n");
        return;
    }
    memset(p, 0, 0x4000);
    walk_scratch = (u64)p;
    printf("SPTM-virt[walk]: armed: site_va 0x%lx (text_off 0x%lx), patch_pa 0x%lx, scratch 0x%lx; "
           "waiting for XNU's stage-1 to come live\n",
           sptm_virt_walk_site_va, sptm_virt_walk_text_off, sptm_virt_walk_patch_pa, walk_scratch);
}

void sptm_virt_walk_diag(void)
{
    static bool done;
    if (done || !sptm_virt_walk_site_va)
        return;

    /* AT S12E1R in the caller's (XNU's) regime: stage 1 then aura's stage 2 -> real PA, or 0 while
     * the MMU is still off / the mapping is not up yet. Keep trying on later WDT pets until it is. */
    u64 par = 0;
    u64 runtime_pa = hv_translate(sptm_virt_walk_site_va, false, false, &par);
    if (!runtime_pa)
        return;
    done = true;

    u32 word_run = read32(runtime_pa);
    u32 word_pat = sptm_virt_walk_patch_pa ? read32(sptm_virt_walk_patch_pa) : 0;

    u64 verdict;
    if (sptm_virt_walk_patch_pa && runtime_pa != sptm_virt_walk_patch_pa)
        verdict = 2; /* different physical copy */
    else if (sptm_virt_walk_patch_pa && word_run != word_pat)
        verdict = 3; /* same PA, rewritten after the patch */
    else
        verdict = 1; /* the patch reaches the running kernel */

    if (walk_scratch) {
        write64(walk_scratch + WALK_OFF_SITE_VA, sptm_virt_walk_site_va);
        write64(walk_scratch + WALK_OFF_RUN_PA, runtime_pa);
        write64(walk_scratch + WALK_OFF_PATCH_PA, sptm_virt_walk_patch_pa);
        write64(walk_scratch + WALK_OFF_WORD_RUN, word_run);
        write64(walk_scratch + WALK_OFF_WORD_PAT, word_pat);
        write64(walk_scratch + WALK_OFF_PAR, par);
        write64(walk_scratch + WALK_OFF_VERDICT, verdict);
        /* magic last, so a concurrent Linux scan never sees a half-written record. */
        write64(walk_scratch + WALK_OFF_MAGIC, WALK_MAGIC);
        dc_civac_range((void *)walk_scratch, 0x40);
        sysop("dsb sy");
    }

    /* Also into the S2R ring for a working auractl entry (host-chainload reference / future DCC). */
    sptm_virt_s2r_rec_raw(runtime_pa, word_run, WALK_TAG_RUNTIME, false);
    sptm_virt_s2r_rec_raw(sptm_virt_walk_patch_pa, word_pat, WALK_TAG_PATCH, false);

    printf("SPTM-virt[walk]: site_va 0x%lx -> runtime_pa 0x%lx (PAR 0x%lx); patch_pa 0x%lx; "
           "word@runtime 0x%08x word@patch 0x%08x; VERDICT %lu (%s)\n",
           sptm_virt_walk_site_va, runtime_pa, par, sptm_virt_walk_patch_pa, word_run, word_pat,
           verdict,
           verdict == 2   ? "DIFFERENT physical copy -- patch the copy XNU runs"
           : verdict == 3 ? "same PA, word REWRITTEN after aura's patch"
                          : "patch reaches the running kernel");
}

#endif /* AURACTL_WALK_DIAG */
