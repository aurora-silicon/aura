/* SPDX-License-Identifier: MIT */
/*
 * AuractlUserClient implementation. See AuractlUserClient.hpp for the design.
 *
 * The one method issues aura's control HVC on the calling thread. aura answers only an AURACTL
 * build (AURACTL=1); on any other aura the HVC is reflected to the guest, so load this only under
 * an AURACTL aura (README.md).
 */

#include "AuractlUserClient.hpp"
#include "../../../src/auractl_abi.h"

#include <IOKit/IOLib.h>

/* The HVC immediate is a literal in the instruction, so assert the header still agrees with it. */
static_assert(AURACTL_HVC_IMM == 0xac71, "hvc immediate must match src/auractl_abi.h");

enum { kAuractlMethodEnter = 0, kAuractlMethodCount };

#define super IOUserClient
OSDefineMetaClassAndStructors(AuractlUserClient, IOUserClient)

/*
 * Issue `hvc #0xac71` with w0 = magic and x1 = the caller's request address, and return aura's x0.
 * Runs on the calling user thread, so AT S1E0R/W inside aura resolves x1 as that process would.
 */
static uint64_t auractl_do_hvc(uint64_t reqVA)
{
    register uint64_t x0 asm("x0") = AURACTL_HVC_MAGIC;
    register uint64_t x1 asm("x1") = reqVA;
    asm volatile("hvc #0xac71" : "+r"(x0) : "r"(x1) : "memory", "cc");
    return x0;
}

bool AuractlUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    /*
     * Refuse non-admin openers. Nothing stops a userland process from IOServiceOpen()ing us
     * directly, which bypasses the auractl tool's own root check; from there it could arm hooks,
     * drain the ring, read aura's console, and on a non-AURACTL aura panic the guest. Require the
     * opening task to hold administrator privilege (root / admin group), so the kext is no weaker
     * than the tool.
     */
    if (clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    fTask = owningTask;
    return true;
}

bool AuractlUserClient::start(IOService *provider)
{
    fProvider = OSDynamicCast(AuractlDevice, provider);
    if (!fProvider)
        return false;
    return super::start(provider);
}

IOReturn AuractlUserClient::clientClose(void)
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn AuractlUserClient::sEnter(OSObject *target, void *reference,
                                   IOExternalMethodArguments *args)
{
    (void)reference;
    AuractlUserClient *self = OSDynamicCast(AuractlUserClient, target);
    if (!self || !self->fProvider)
        return kIOReturnNotReady;
    if (args->scalarInputCount != 1 || args->scalarOutputCount != 1)
        return kIOReturnBadArgument;

    uint64_t reqVA = args->scalarInput[0];
    if (!reqVA || (reqVA & 0x7)) /* the request block is 8-byte aligned (src/auractl_abi.h) */
        return kIOReturnBadArgument;

    args->scalarOutput[0] = auractl_do_hvc(reqVA);
    return kIOReturnSuccess;
}

IOReturn AuractlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch, OSObject *target,
                                           void *reference)
{
    static const IOExternalMethodDispatch kMethods[kAuractlMethodCount] = {
        /* 0 enter(reqVA) -> x0 */
        {&AuractlUserClient::sEnter, 1, 0, 1, 0},
    };

    if (selector >= kAuractlMethodCount)
        return kIOReturnUnsupported;

    dispatch = (IOExternalMethodDispatch *)&kMethods[selector];
    target   = this;
    return super::externalMethod(selector, args, dispatch, target, reference);
}
