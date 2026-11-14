/* SPDX-License-Identifier: MIT */
/*
 * AuractlUserClient -- the IOUserClient the auractl tool talks to.
 *
 * Design goal: as little kext code as possible can be wrong. The primary transport is a read/write
 * userland mapping of the control page (clientMemoryForType), so the tool drives the register file
 * directly. A tiny set of scalar methods is a fallback for environments that refuse to map device
 * memory to userland.
 *
 * External method selectors (kept in sync with auractl.c's enum):
 *   0 mapRegisters  -- no scalar args; the tool uses IOConnectMapMemory(type=0) for the page.
 *   1 doCommand     -- in (cmd, arg0, arg1) -> out (status): kext runs the ARG/CMD/STATUS sequence.
 *   2 readReg       -- in (off)             -> out (value):  off within page, 8-byte aligned.
 *   3 writeReg      -- in (off, value)      -> out ():       off within page, 8-byte aligned.
 */

#ifndef AURACTL_USER_CLIENT_HPP
#define AURACTL_USER_CLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "AuractlDevice.hpp"

class AuractlUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AuractlUserClient)

public:
    virtual bool     initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    virtual bool     start(IOService *provider) override;
    virtual IOReturn clientClose(void) override;

    /* Dispatch table entry point. */
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;

    /*
     * clientMemoryForType(type==0): hand the control page to userland. Returned uncached (DEVICE)
     * so the tool's MMIO ordering holds. Marked read/write -- the tool must be able to write ARG/CMD.
     */
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) override;

private:
    AuractlDevice *fProvider = nullptr;
    task_t         fTask     = nullptr;

    /* Static trampolines in the dispatch table. */
    static IOReturn sDoCommand(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sReadReg(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sWriteReg(OSObject *target, void *reference, IOExternalMethodArguments *args);
};

#endif /* AURACTL_USER_CLIENT_HPP */
