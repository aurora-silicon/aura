/* SPDX-License-Identifier: MIT */
/*
 * AuractlDevice IOService implementation. See AuractlDevice.hpp.
 *
 * The driver matches IOResources (so it instantiates once, with no ADT node) and publishes itself
 * so the auractl tool can IOServiceOpen() it and reach the AuractlUserClient. It touches no
 * hardware: the only work is in the user client's single HVC method.
 */

#include "AuractlDevice.hpp"

#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(AuractlDevice, IOService)

bool AuractlDevice::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    /* Publish so IOServiceOpen() from the auractl tool finds us and opens the user client. */
    registerService();
    IOLog("AuractlDevice: ready (guest-side entry for aura's auractl HVC)\n");
    return true;
}

void AuractlDevice::stop(IOService *provider)
{
    super::stop(provider);
}
