/* SPDX-License-Identifier: MIT */
/*
 * AuractlDevice -- a minimal IOKit driver that lets the auractl tool, running in the macOS guest
 * under aura's SPTM hypervisor, reach aura's control call (src/auractl_abi.h, docs/auractl.md).
 *
 * The control call is an HVC: the guest's kernel issues `hvc #AURACTL_HVC_IMM` with w0 =
 * AURACTL_HVC_MAGIC and x1 = the calling process's address of a struct auractl_req, and aura reads
 * and writes that request through the caller's own EL0 translation. Userland cannot issue an HVC,
 * so this kext does it on the calling thread's behalf: AuractlUserClient's single external method
 * runs synchronously on the caller's thread (so TTBR0_EL1 is still the caller's EL0 map) and
 * executes the one HVC instruction, passing the request's user virtual address straight through.
 *
 * There is NO second ABI and no MMIO device: this driver is only the guest-side entry that carries
 * a request to aura's hvc. It maps nothing and keeps no state. The matching entry for the tool is
 * tools/auractl/entry_iokit.c (`make ENTRY=iokit`).
 *
 * Lab tool: the HVC is answered only by an AURACTL build of aura (AURACTL=1). Load it only in the
 * macOS guest under such an aura, with SIP off (dev-signed). See README.md.
 *
 * Clean-room: our own names and code; the ABI is Dj's shared src/auractl_abi.h. No Apple source.
 */

#ifndef AURACTL_DEVICE_HPP
#define AURACTL_DEVICE_HPP

#include <IOKit/IOService.h>

class AuractlDevice : public IOService {
    OSDeclareDefaultStructors(AuractlDevice)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};

#endif /* AURACTL_DEVICE_HPP */
