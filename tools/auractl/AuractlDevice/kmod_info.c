/* SPDX-License-Identifier: MIT */
/*
 * kmod_info for AuractlDevice.kext.
 *
 * Every kext's Mach-O needs a `kmod_info` symbol: it carries the bundle id, version and the
 * module start/stop entry points, and kextload/kmutil refuse a kext without it. Xcode synthesizes
 * this file from the "Generate kmod info" build setting; our hand-rolled `make kext` does not run
 * through Xcode, so we provide it.
 *
 * This is the standard IOKit-C++ form. The real entry points are `_start`/`_stop` in libkmodc++
 * (they run the C++ static constructors, then call _realmain/_antimain). An IOKit driver is
 * brought up through its IOService match, not a module main, so _realmain/_antimain stay NULL, as
 * Xcode leaves them. The version string must match CFBundleVersion in Info.plist.
 *
 * Everything here must have C linkage so the symbols are `_kmod_info`, `__start`, etc. that
 * libkmodc++ and the loader look for. The Makefile builds the kext with a C++ driver, which may
 * compile a .c file as C++, so wrap the whole file in extern "C" when compiling as C++.
 */
#include <mach/mach_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(org.aurorasilicon.AuractlDevice, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int                _kext_apple_cc = __APPLE_CC__;

#ifdef __cplusplus
}
#endif
