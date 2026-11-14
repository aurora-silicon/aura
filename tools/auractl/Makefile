# SPDX-License-Identifier: MIT
# The guest-side tool. ENTRY picks the entry_<name>.c that hands a request to the guest's kernel
# for aura's hvc (entry.h, docs/auractl.md):
#   none    refuses every call; only `auractl print` works (no aura needed).
#   iokit   issues the hvc through AuractlDevice.kext (make ENTRY=iokit; needs the kext loaded).
# On macOS the tool is arm64, not arm64e: macOS refuses a third-party arm64e binary without
# -arm64e_preview_abi. The kext, by contrast, is arm64e (the kernel's ABI): `make kext`.
CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -g -Wall -Wextra
ENTRY ?= none
ifeq ($(shell uname -s),Darwin)
CFLAGS += -arch arm64 -mmacosx-version-min=14.0
endif

# Frameworks an entry needs; empty for ENTRY=none. iokit links IOKit + CoreFoundation.
LDLIBS_iokit = -framework IOKit -framework CoreFoundation

auractl: auractl.c entry.h entry_$(ENTRY).c ../../src/auractl_abi.h
	$(CC) $(CFLAGS) -o $@ auractl.c entry_$(ENTRY).c $(LDLIBS_$(ENTRY))

# ---- AuractlDevice.kext: the iokit entry's guest-side driver (macOS only) -------------------- #
# It issues aura's control HVC for a userland process (AuractlDevice/, README.md). Built with
# clang against Kernel.framework; dev-sign + load (SIP off) per README.md.
KEXT_NAME   = AuractlDevice
KEXT_BUNDLE = $(KEXT_NAME).kext
KEXT_DIR    = AuractlDevice
# kmod_info.c carries the bundle's kmod_info (Xcode would generate it); see that file.
KEXT_SRCS   = $(KEXT_DIR)/AuractlDevice.cpp $(KEXT_DIR)/AuractlUserClient.cpp \
	$(KEXT_DIR)/kmod_info.c
KEXT_PLIST  = $(KEXT_DIR)/Info.plist
SDKROOT    ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
KARCH      ?= arm64e
# A kext needs an LC_BUILD_VERSION load command or kmutil refuses to load it. ld's -kext mode
# drops the platform load command, so -mmacosx-version-min alone does not leave one in the bundle;
# we stamp it after the link with vtool (below). KEXT_MIN is the lowest macOS the kext loads on
# (not the build SDK): keep it at or below the target's version; 14.0 loads on every current release.
KEXT_MIN    ?= 14.0
KEXT_SDKVER ?= $(shell xcrun --sdk macosx --show-sdk-version 2>/dev/null)
KERN_FLAGS  = -arch $(KARCH) -isysroot $(SDKROOT) -mmacosx-version-min=$(KEXT_MIN) \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-fno-builtin -fno-common -fno-exceptions -fno-rtti -fapple-kext \
	-mkernel -nostdinc -std=gnu++17 -Wall -Wextra

kext: $(KEXT_BUNDLE)

$(KEXT_BUNDLE): $(KEXT_SRCS) $(KEXT_PLIST) ../../src/auractl_abi.h
	@test "$$(uname -s)" = Darwin || { echo "auractl: 'kext' builds only on macOS (Kernel.framework)."; exit 1; }
	@test -n "$(SDKROOT)" || { echo "auractl: no macOS SDK found (xcrun)."; exit 1; }
	mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	$(CXX) $(KERN_FLAGS) -nostdlib -o $(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME) \
		$(KEXT_SRCS) -Xlinker -kext -lkmod -lkmodc++ -lcc_kext
	# ld -kext leaves no platform load command; kmutil wants LC_BUILD_VERSION, so stamp one in.
	xcrun vtool -set-build-version macos $(KEXT_MIN) $(KEXT_SDKVER) -replace \
		-output $(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME) \
		$(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME)
	cp $(KEXT_PLIST) $(KEXT_BUNDLE)/Contents/Info.plist
	@echo "built $(KEXT_BUNDLE) (unsigned); dev-sign + load per README.md (SIP off)."

clean:
	rm -f auractl
	rm -rf auractl.dSYM $(KEXT_BUNDLE)
.PHONY: kext clean
