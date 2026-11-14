// SPDX-License-Identifier: MIT
/*
 * The IOKit guest entry: hand a request to AuractlDevice.kext, which issues aura's control HVC on
 * this thread (AuractlDevice/, entry.h, docs/auractl.md). Build with `make ENTRY=iokit`.
 *
 * The kext runs the one `hvc #AURACTL_HVC_IMM` instruction a userland process cannot, on this same
 * thread, so aura reads and writes the request through this process's own EL0 translation. The
 * tool (auractl.c setup()) has already wired req and its buffers with mlock, so aura finds them
 * resident and writable.
 */
#include "entry.h"

#include <IOKit/IOKitLib.h>

#define AURACTL_METHOD_ENTER 0 /* AuractlUserClient selector 0: enter(reqVA) -> x0 */

int auractl_enter(struct auractl_req *req)
{
    io_service_t svc =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AuractlDevice"));
    if (!svc)
        return AURACTL_ENODEV; /* kext not loaded, or no AuractlDevice published */

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS)
        return AURACTL_EPERM; /* not permitted to open the user client (needs root, SIP off) */

    uint64_t in = (uint64_t)(uintptr_t)req;
    uint64_t out = 0;
    uint32_t out_cnt = 1;
    kr = IOConnectCallScalarMethod(conn, AURACTL_METHOD_ENTER, &in, 1, &out, &out_cnt);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS)
        return AURACTL_ENODEV; /* the HVC did not complete through the kext */

    return (int)out; /* 0 or aura's Darwin errno (src/auractl_abi.h) */
}

const char *auractl_entry_name(void)
{
    return "iokit";
}
