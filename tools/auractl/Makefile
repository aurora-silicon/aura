# SPDX-License-Identifier: MIT
#
# auractl -- in-guest tracing-control tool + AuractlDevice kext for aura's SPTM hypervisor.
#
# Targets:
#   tool        build the auractl userland tool (macOS only; links IOKit + CoreFoundation)
#   kext        build AuractlDevice.kext (macOS only; dev-sign + load notes in README.md)
#   host-check  compile ONLY the ABI + argument-parsing logic of auractl.c with IOKit #ifdef'd out
#               (works on Linux/any host; the correctness check the buildbox can run)
#   clean       remove build products
#
# The tool and kext build only on macOS (they need the macOS SDK / Kernel.framework). The non-macOS
# targets print a clear message and exit cleanly rather than failing obscurely.

UNAME        := $(shell uname)
CC           ?= clang
CXX          ?= clang++

TOOL          = auractl
TOOL_SRC      = auractl.c
ABI_HDR       = sptm_virt_auractl_abi.h

KEXT_NAME     = AuractlDevice
KEXT_BUNDLE   = $(KEXT_NAME).kext
KEXT_DIR      = AuractlDevice
KEXT_SRCS     = $(KEXT_DIR)/AuractlDevice.cpp $(KEXT_DIR)/AuractlUserClient.cpp
KEXT_PLIST    = $(KEXT_DIR)/Info.plist
KEXT_ID       = org.aurorasilicon.AuractlDevice

WARN          = -Wall -Wextra
CSTD          = -std=c11

.PHONY: all tool kext host-check clean

all: tool

# ---------------------------------------------------------------------------------------------- #
# Userland tool (macOS).                                                                          #
# ---------------------------------------------------------------------------------------------- #
ifeq ($(UNAME),Darwin)
tool: $(TOOL)

$(TOOL): $(TOOL_SRC) $(ABI_HDR)
	$(CC) $(WARN) $(CSTD) -O2 -o $@ $(TOOL_SRC) \
		-framework IOKit -framework CoreFoundation
	@echo "built $(TOOL)"
else
tool:
	@echo "auractl: 'tool' builds only on macOS (needs IOKit + CoreFoundation)."
	@echo "         On this host ($(UNAME)) run 'make host-check' to compile the pure logic."
endif

# ---------------------------------------------------------------------------------------------- #
# Kext (macOS). Built with clang against Kernel.framework into a bundle. xcodebuild is an          #
# alternative if you prefer a .xcodeproj; this plain recipe keeps the lane dependency-free.        #
# Dev-sign and load steps are in README.md (SIP must be off).                                      #
# ---------------------------------------------------------------------------------------------- #
# SDK / arch for the J616s (Apple silicon). Override on the command line if needed.
SDKROOT      ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
KARCH        ?= arm64e
KERN_FLAGS    = -arch $(KARCH) -isysroot $(SDKROOT) \
		-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
		-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders \
		-I. \
		-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
		-fno-builtin -fno-common -fno-exceptions -fno-rtti -fapple-kext \
		-mkernel -nostdinc -std=gnu++17 $(WARN)

ifeq ($(UNAME),Darwin)
kext: $(KEXT_BUNDLE)

$(KEXT_BUNDLE): $(KEXT_SRCS) $(KEXT_PLIST) $(ABI_HDR)
	@test -n "$(SDKROOT)" || { echo "auractl: no macOS SDK found (xcrun)."; exit 1; }
	mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	$(CXX) $(KERN_FLAGS) -o $(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME) \
		$(KEXT_SRCS) \
		-Xlinker -kext \
		-lkmod -lkmodc++ -lcc_kext
	cp $(KEXT_PLIST) $(KEXT_BUNDLE)/Contents/Info.plist
	@echo "built $(KEXT_BUNDLE) (unsigned)."
	@echo "Dev-sign + load: see README.md (codesign, then kmutil load -p $(KEXT_BUNDLE); SIP off)."
else
kext:
	@echo "auractl: 'kext' builds only on macOS (needs Kernel.framework)."
endif

# ---------------------------------------------------------------------------------------------- #
# Host-check: compile the pure ABI + argument-parsing logic with IOKit removed. Runs anywhere.     #
# This is the correctness gate the buildbox can enforce; it also runs on macOS.                    #
# ---------------------------------------------------------------------------------------------- #
host-check: $(TOOL_SRC) $(ABI_HDR)
	$(CC) -DAURACTL_NO_IOKIT $(WARN) $(CSTD) -O2 -o $(TOOL)-hostcheck $(TOOL_SRC)
	@echo "host-check OK: $(TOOL)-hostcheck built with no warnings (IOKit paths #ifdef'd out)."

clean:
	rm -f $(TOOL) $(TOOL)-hostcheck
	rm -rf $(KEXT_BUNDLE)
