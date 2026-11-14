# SPDX-License-Identifier: MIT
# The guest-side tool. ENTRY picks the entry_<name>.c that hands a request to the guest's kernel
# for aura's hvc (entry.h, docs/auractl.md); this tree has only "none", which refuses every call.
# On macOS the tool is arm64, not arm64e: macOS refuses a third-party arm64e binary without
# -arm64e_preview_abi.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
ENTRY ?= none
ifeq ($(shell uname -s),Darwin)
CFLAGS += -arch arm64 -mmacosx-version-min=14.0
endif

auractl: auractl.c entry.h entry_$(ENTRY).c ../../src/auractl_abi.h
	$(CC) $(CFLAGS) -o $@ auractl.c entry_$(ENTRY).c

clean:
	rm -f auractl
	rm -rf auractl.dSYM
.PHONY: clean
