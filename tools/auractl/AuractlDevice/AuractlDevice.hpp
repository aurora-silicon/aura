/* SPDX-License-Identifier: MIT */
/*
 * AuractlDevice -- a minimal IOKit driver that exposes aura's synthetic in-guest tracing-control
 * MMIO page to a userland tool (auractl) running inside the macOS guest under aura's SPTM
 * hypervisor.
 *
 * The control page is a one-page (AURACTL_PAGE_SIZE) synthetic MMIO device that the hypervisor
 * traps (hv_map_hook / SPTE_HOOK). It is NOT backed by RAM or any Apple block; every register is
 * implemented by the HV from its own state. See sptm_virt_auractl_abi.h for the register file.
 *
 * MVP mapping: we map the fixed guest-physical page AURACTL_MMIO_BASE as DEVICE memory (uncached,
 * kIOMapInhibitCache) so MMIO ordering holds. SIP is disabled on the J616s macOS, so a dev-signed
 * kext may map a fixed physical page directly -- no ADT dependency.
 *
 * Discoverable alternative (documented, not the MVP): add an "aura,auractl" node to the guest ADT
 * and match it as an IOService, taking the reg from the node instead of the fixed address. The
 * stage-2 hook fires on the access regardless of how the page is mapped.
 *
 * Clean-room: our own names, ABI and layout. No Apple source.
 */

#ifndef AURACTL_DEVICE_HPP
#define AURACTL_DEVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

class AuractlDevice : public IOService {
    OSDeclareDefaultStructors(AuractlDevice)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /*
     * Accessors used by the user client. The register page is mapped once in start() and shared,
     * read-only to userland (clientMemoryForType) or driven here via the scalar methods.
     */
    IOMemoryDescriptor *registerMemory(void) const { return fRegMem; }

    /* Scalar-method fallback helpers (used when userland device mapping is refused). */
    bool     readReg(uint32_t off, uint64_t *out);
    bool     writeReg(uint32_t off, uint64_t val);
    uint32_t doCommand(uint32_t cmd, uint64_t arg0, uint64_t arg1);

private:
    /* Backing descriptor for the fixed physical control page, mapped uncached (DEVICE memory). */
    IOMemoryDescriptor *fRegMem = nullptr;
    /* In-kernel mapping of the page for the scalar-method path. */
    IOMemoryMap        *fRegMap = nullptr;
    volatile uint8_t   *fRegs   = nullptr;

    bool offsetValid(uint32_t off) const;
};

#endif /* AURACTL_DEVICE_HPP */
