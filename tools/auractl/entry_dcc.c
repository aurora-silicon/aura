// SPDX-License-Identifier: MIT
/*
 * The DCC guest entry: reach aura with no kernel help at all. A plain, unprivileged process
 * writes the request's own address, tagged, to DBGDTR_EL0 — an EL0-accessible debug register —
 * and aura, which traps that access to EL2 (MDCR_EL2.TDCC, src/nv2.c), reads and runs the request
 * through this process's own EL0 translation and leaves the errno to be read back from the same
 * register. No kext, no entitlement, no root (entry.h, docs/auractl.md). Build with
 * `make ENTRY=dcc`.
 *
 * This is the transport to use where the IOKit entry cannot: under SPTM-virt a kext may be
 * unloadable (SIP on, or a host-fed BootKC that kmutil cannot extend). The DCC path needs nothing
 * the guest kernel has to cooperate with — only that the aura it runs under is an AURACTL build
 * (which sets MDCR_EL2.TDCC). On any other host the tagged read-back fails its check and the call
 * reports ENODEV rather than acting on stray debug-channel state.
 */
#include "entry.h"

#define AURACTL_DCC_TAG 0xAC71ULL /* top 16 bits of the doorbell and of aura's errno read-back */

int auractl_enter(struct auractl_req *req)
{
    uint64_t va = (uint64_t)(uintptr_t)req;
    /* A macOS user virtual address is 47-bit, so the top 16 bits are free to carry the tag that
     * tells aura a doorbell from any other DCC traffic. */
    uint64_t door = (AURACTL_DCC_TAG << 48) | (va & 0xffffffffffffULL);
    uint64_t out = 0;

    /* The write is the doorbell: aura runs the whole command synchronously in this one trap and
     * writes the request block back through our own translation. The read returns its errno. */
    __asm__ volatile("msr DBGDTR_EL0, %0" ::"r"(door) : "memory");
    __asm__ volatile("mrs %0, DBGDTR_EL0" : "=r"(out)::"memory");

    /* aura tags the read-back; an untagged value means no aura trap (bare hardware's DCC), so the
     * call never happened. */
    if ((out >> 48) != AURACTL_DCC_TAG)
        return AURACTL_ENODEV;
    return (int)(int32_t)(out & 0xffffffffULL);
}

const char *auractl_entry_name(void)
{
    return "dcc";
}
