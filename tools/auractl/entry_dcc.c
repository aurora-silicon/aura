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
 * booted with the DCC transport armed. On any other host the tagged read-back fails its check and
 * the call reports ENODEV rather than acting on stray debug-channel state.
 *
 * Anyone may ring the doorbell, so aura gates everything past the read-only status behind a
 * per-boot cookie it only honours when presented through DBGDTRTX_EL0 just before the doorbell.
 * The cookie is in the guest's boot-args, which only root can read (sysctl kern.bootargs): so
 * `auractl status` works for any user, but arming, recording and reading records need root. A
 * non-root caller presents 0 and aura refuses the gated command with EPERM.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include "entry.h"

#define AURACTL_DCC_TAG 0xAC71ULL /* top 16 bits of the doorbell and of aura's errno read-back */

/* The per-boot cookie, cached. Two sources, in order:
 *   1. AURACTL_COOKIE in the environment (hex), for paths where kern.bootargs is not legible --
 *      e.g. macOS 27 redacts the boot-args from user space even for root under secure boot, and a
 *      tetherless self-boot has no serial to read aura's logged value from. The operator who built
 *      the ESP bundle already knows the cookie it put in the manifest, so it can be handed in here.
 *   2. The guest boot-args ("aura-actl-ck=<hex>" in sysctl kern.bootargs), where those are legible.
 * 0 if neither yields one, which leaves only the status command (every gated command refused). */
static uint64_t dcc_cookie(void)
{
    static uint64_t cookie;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *env = getenv("AURACTL_COOKIE");
        if (env && *env) {
            cookie = strtoull(env, NULL, 16);
        } else {
            char buf[4096];
            size_t len = sizeof(buf) - 1;
            if (sysctlbyname("kern.bootargs", buf, &len, NULL, 0) == 0) {
                buf[len] = 0;
                const char *p = strstr(buf, "aura-actl-ck=");
                if (p)
                    cookie = strtoull(p + strlen("aura-actl-ck="), NULL, 16);
            }
        }
    }
    return cookie;
}

int auractl_enter(struct auractl_req *req)
{
    uint64_t va = (uint64_t)(uintptr_t)req;
    /* A macOS user virtual address is 47-bit, so the top 16 bits are free to carry the tag that
     * tells aura a doorbell from any other DCC traffic. */
    uint64_t door = (AURACTL_DCC_TAG << 48) | (va & 0xffffffffffffULL);
    uint64_t out = 0;

    /* Present the cookie first: aura latches it per core and the doorbell that immediately follows
     * consumes it. The status command needs no cookie, so presenting 0 there is fine. */
    __asm__ volatile("msr DBGDTRTX_EL0, %0" ::"r"(dcc_cookie()) : "memory");
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
