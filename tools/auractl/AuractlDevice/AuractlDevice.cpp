/* SPDX-License-Identifier: MIT */
/*
 * AuractlDevice IOService implementation. See AuractlDevice.hpp for the design.
 *
 * Every MMIO access below is commented with the ABI register it touches. All register reads/writes
 * go through the uncached DEVICE mapping of the control page so hypervisor trap ordering holds.
 */

#include "AuractlDevice.hpp"
#include "AuractlUserClient.hpp"
#include "sptm_virt_auractl_abi.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

#define super IOService
OSDefineMetaClassAndStructors(AuractlDevice, IOService)

/*
 * The register file is natural-width: the 64-bit registers sit at 8-byte-aligned offsets, the
 * 32-bit registers at 4-byte-aligned offsets, and the hypervisor decode keys on the exact offset
 * (there are no sub-word cases). So every access must use the register's own width, or the guest
 * makes a misaligned device access (which faults) or lands on an undecoded offset.
 */
static bool auractlReg64(uint32_t off)
{
    switch (off) {
        case AURACTL_REG_ARG0:
        case AURACTL_REG_ARG1:
        case AURACTL_REG_NREC:
        case AURACTL_REG_DROPPED:
        case AURACTL_REG_FIFO_DATA:
        case AURACTL_REG_FIFO_AVAIL:
            return true;
        default:
            return false;
    }
}

bool AuractlDevice::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    /*
     * MVP: map the fixed guest-physical control page as DEVICE memory. withAddressRange over the
     * physical page, direction InOut (we read status/FIFO and write ARG/CMD). Uncached so MMIO
     * ordering holds -- the HV must see each access as a distinct trap.
     *
     * IOMemoryDescriptor::withAddressRange with a kernel task of NULL and kIOMemoryMapperNone
     * treats the address as a physical range (the aura page is a reserved guest-physical hole).
     */
    IOOptionBits opts = kIODirectionInOut | kIOMemoryMapperNone;
    fRegMem = IOMemoryDescriptor::withAddressRange(
        (mach_vm_address_t)AURACTL_MMIO_BASE, /* OUR fixed guest-physical base from the ABI header */
        (mach_vm_size_t)AURACTL_PAGE_SIZE, opts, NULL /* kernel_task: physical */);
    if (!fRegMem) {
        IOLog("AuractlDevice: could not describe control page at 0x%llx\n",
              (unsigned long long)AURACTL_MMIO_BASE);
        return false;
    }

    if (fRegMem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog("AuractlDevice: prepare() failed\n");
        fRegMem->release();
        fRegMem = nullptr;
        return false;
    }

    /* Map into the kernel uncached for the scalar-method fallback path. */
    fRegMap = fRegMem->map(kIOMapInhibitCache);
    if (fRegMap)
        fRegs = (volatile uint8_t *)fRegMap->getVirtualAddress();

    /* Probe MAGIC so a misconfigured base fails loudly at load rather than at first command. */
    if (fRegs) {
        uint32_t magic = *(volatile uint32_t *)(fRegs + AURACTL_REG_MAGIC); /* AURACTL_REG_MAGIC */
        if (magic != AURACTL_MAGIC) {
            IOLog("AuractlDevice: no device at 0x%llx (MAGIC=0x%08x, expected 0x%08x)\n",
                  (unsigned long long)AURACTL_MMIO_BASE, magic, AURACTL_MAGIC);
            /* Not fatal: the userland tool re-probes and reports cleanly. Keep the service up so
             * the operator can read status and diagnose. */
        } else {
            IOLog("AuractlDevice: control page mapped, MAGIC ok (version %u)\n",
                  *(volatile uint32_t *)(fRegs + AURACTL_REG_VERSION)); /* AURACTL_REG_VERSION */
        }
    }

    /* Publish so IOServiceOpen() from the auractl tool can find us and open the user client. */
    registerService();
    return true;
}

void AuractlDevice::stop(IOService *provider)
{
    if (fRegMap) {
        fRegMap->release();
        fRegMap = nullptr;
        fRegs   = nullptr;
    }
    if (fRegMem) {
        fRegMem->complete(kIODirectionInOut);
        fRegMem->release();
        fRegMem = nullptr;
    }
    super::stop(provider);
}

bool AuractlDevice::offsetValid(uint32_t off) const
{
    /* Inside the register file, aligned to the register's own width (8-byte regs 8-aligned, else
     * 4-aligned) so the device never sees a misaligned access. */
    if (off >= AURACTL_REG_MAX)
        return false;
    return (off & (auractlReg64(off) ? 0x7u : 0x3u)) == 0;
}

bool AuractlDevice::readReg(uint32_t off, uint64_t *out)
{
    if (!fRegs || !out || !offsetValid(off))
        return false;
    /* DEVICE memory, uncached: read each register at its natural width. */
    if (auractlReg64(off))
        *out = *(volatile uint64_t *)(fRegs + off);
    else
        *out = *(volatile uint32_t *)(fRegs + off);
    return true;
}

bool AuractlDevice::writeReg(uint32_t off, uint64_t val)
{
    if (!fRegs || !offsetValid(off))
        return false;
    if (auractlReg64(off))
        *(volatile uint64_t *)(fRegs + off) = val; /* uncached MMIO write */
    else
        *(volatile uint32_t *)(fRegs + off) = (uint32_t)val;
    return true;
}

uint32_t AuractlDevice::doCommand(uint32_t cmd, uint64_t arg0, uint64_t arg1)
{
    if (!fRegs)
        return 0;
    /* ARG0, ARG1, then the CMD doorbell, then re-read STATUS -- the ABI command sequence. */
    *(volatile uint64_t *)(fRegs + AURACTL_REG_ARG0) = arg0; /* AURACTL_REG_ARG0 */
    *(volatile uint64_t *)(fRegs + AURACTL_REG_ARG1) = arg1; /* AURACTL_REG_ARG1 */
    *(volatile uint32_t *)(fRegs + AURACTL_REG_CMD)  = cmd;  /* AURACTL_REG_CMD doorbell */
    return *(volatile uint32_t *)(fRegs + AURACTL_REG_STATUS); /* AURACTL_REG_STATUS */
}
