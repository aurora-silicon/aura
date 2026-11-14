/* SPDX-License-Identifier: MIT */
/*
 * AuractlUserClient implementation. See AuractlUserClient.hpp for the design and selector map.
 *
 * Defensive throughout: validate the task, the selector, scalar counts, and every register offset
 * (within the page and 8-byte aligned) before touching MMIO.
 */

#include "AuractlUserClient.hpp"
#include "sptm_virt_auractl_abi.h"

#include <IOKit/IOLib.h>

/* Selector map -- keep in sync with auractl.c's enum. */
enum {
    kAuractlMethodMapRegisters = 0,
    kAuractlMethodDoCommand    = 1,
    kAuractlMethodReadReg      = 2,
    kAuractlMethodWriteReg     = 3,
    kAuractlMethodCount
};

#define super IOUserClient
OSDefineMetaClassAndStructors(AuractlUserClient, IOUserClient)

bool AuractlUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    return true;
}

bool AuractlUserClient::start(IOService *provider)
{
    fProvider = OSDynamicCast(AuractlDevice, provider);
    if (!fProvider)
        return false;
    if (!super::start(provider))
        return false;
    return true;
}

IOReturn AuractlUserClient::clientClose(void)
{
    terminate();
    return kIOReturnSuccess;
}

/* Hand the control page to userland, uncached and read/write, so the tool drives the registers. */
IOReturn AuractlUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                IOMemoryDescriptor **memory)
{
    if (type != kAuractlMethodMapRegisters)
        return kIOReturnUnsupported;
    if (!fProvider || !fProvider->registerMemory())
        return kIOReturnNotReady;

    IOMemoryDescriptor *mem = fProvider->registerMemory();
    mem->retain(); /* the caller (IOUserClient map path) releases it */
    *memory  = mem;
    /* Inhibit cache so the userland mapping is DEVICE memory and MMIO ordering holds. */
    *options = kIOMapInhibitCache;
    return kIOReturnSuccess;
}

/* ---- scalar method trampolines -------------------------------------------------------------- */

IOReturn AuractlUserClient::sDoCommand(OSObject *target, void *reference,
                                       IOExternalMethodArguments *args)
{
    (void)reference;
    AuractlUserClient *self = OSDynamicCast(AuractlUserClient, target);
    if (!self || !self->fProvider)
        return kIOReturnNotReady;
    if (args->scalarInputCount != 3 || args->scalarOutputCount != 1)
        return kIOReturnBadArgument;

    uint32_t cmd  = (uint32_t)args->scalarInput[0];
    uint64_t arg0 = args->scalarInput[1];
    uint64_t arg1 = args->scalarInput[2];
    if (cmd >= AURACTL_CMD_MAX)
        return kIOReturnBadArgument;

    args->scalarOutput[0] = self->fProvider->doCommand(cmd, arg0, arg1);
    return kIOReturnSuccess;
}

IOReturn AuractlUserClient::sReadReg(OSObject *target, void *reference,
                                     IOExternalMethodArguments *args)
{
    (void)reference;
    AuractlUserClient *self = OSDynamicCast(AuractlUserClient, target);
    if (!self || !self->fProvider)
        return kIOReturnNotReady;
    if (args->scalarInputCount != 1 || args->scalarOutputCount != 1)
        return kIOReturnBadArgument;

    uint32_t off = (uint32_t)args->scalarInput[0];
    uint64_t val = 0;
    if (!self->fProvider->readReg(off, &val)) /* readReg validates off: in-page + 8-byte aligned */
        return kIOReturnBadArgument;

    args->scalarOutput[0] = val;
    return kIOReturnSuccess;
}

IOReturn AuractlUserClient::sWriteReg(OSObject *target, void *reference,
                                      IOExternalMethodArguments *args)
{
    (void)reference;
    AuractlUserClient *self = OSDynamicCast(AuractlUserClient, target);
    if (!self || !self->fProvider)
        return kIOReturnNotReady;
    if (args->scalarInputCount != 2 || args->scalarOutputCount != 0)
        return kIOReturnBadArgument;

    uint32_t off = (uint32_t)args->scalarInput[0];
    uint64_t val = args->scalarInput[1];
    if (!self->fProvider->writeReg(off, val)) /* writeReg validates off */
        return kIOReturnBadArgument;

    return kIOReturnSuccess;
}

IOReturn AuractlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch, OSObject *target,
                                           void *reference)
{
    static const IOExternalMethodDispatch kMethods[kAuractlMethodCount] = {
        /* 0 mapRegisters: no scalar method; handled via clientMemoryForType. */
        { NULL, 0, 0, 0, 0 },
        /* 1 doCommand(cmd, arg0, arg1) -> status */
        { &AuractlUserClient::sDoCommand, 3, 0, 1, 0 },
        /* 2 readReg(off) -> value */
        { &AuractlUserClient::sReadReg, 1, 0, 1, 0 },
        /* 3 writeReg(off, value) -> () */
        { &AuractlUserClient::sWriteReg, 2, 0, 0, 0 },
    };

    if (selector >= kAuractlMethodCount)
        return kIOReturnUnsupported;
    if (selector == kAuractlMethodMapRegisters)
        return kIOReturnUnsupported; /* use IOConnectMapMemory(type=0), not a scalar call */

    dispatch = (IOExternalMethodDispatch *)&kMethods[selector];
    target   = this;
    return super::externalMethod(selector, args, dispatch, target, reference);
}
