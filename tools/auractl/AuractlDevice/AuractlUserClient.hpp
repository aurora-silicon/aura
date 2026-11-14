/* SPDX-License-Identifier: MIT */
/*
 * AuractlUserClient -- the IOUserClient the auractl tool opens.
 *
 * It has one external method:
 *
 *   0 enter(reqVA) -> x0 : issue aura's control HVC for the calling process.
 *
 * The tool (through tools/auractl/entry_iokit.c) passes the user virtual address of its struct
 * auractl_req. The method runs synchronously on the calling thread, so the user process's EL0
 * translation (TTBR0_EL1) is still current, and executes
 *
 *     w0 = AURACTL_HVC_MAGIC, x1 = reqVA, hvc #AURACTL_HVC_IMM
 *
 * returning aura's x0 (0 or a Darwin errno from src/auractl_abi.h) as the scalar output. aura
 * reads and writes the request and its buffers through that same EL0 translation; the tool wires
 * them (mlock) before the call, so they are resident and writable.
 *
 * Design goal: as little kext code as can be wrong. The kext performs no copyin/copyout and keeps
 * no state; it is the single privileged instruction userland cannot run itself.
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

    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;

private:
    AuractlDevice *fProvider = nullptr;
    task_t         fTask     = nullptr;

    /* Dispatch-table trampoline for selector 0 (enter). */
    static IOReturn sEnter(OSObject *target, void *reference, IOExternalMethodArguments *args);
};

#endif /* AURACTL_USER_CLIENT_HPP */
