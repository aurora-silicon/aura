# SPDX-License-Identifier: MIT AND GPL-2.0-only
RUSTARCH ?= aarch64-unknown-none-softfloat
# Built-in boot logo. The stock logo is Aurora. A fork or distribution can link its own by
# putting <name>_48.bin, <name>_128.bin and <name>_256.bin (raw RGBA, see data/makelogo.py)
# in a directory and building with LOGO_DIR=<directory> BUILTIN_LOGO=<name>.
BUILTIN_LOGO ?= aurora
LOGO_DIR ?= data

ifneq ($(words $(wildcard $(LOGO_DIR)/$(BUILTIN_LOGO)_48.bin $(LOGO_DIR)/$(BUILTIN_LOGO)_128.bin $(LOGO_DIR)/$(BUILTIN_LOGO)_256.bin)),3)
$(error BUILTIN_LOGO assets $(LOGO_DIR)/$(BUILTIN_LOGO)_{48,128,256}.bin are missing)
endif

ifeq ($(shell uname),Darwin)
USE_CLANG ?= 1
$(info INFO: Building on Darwin)

ifeq ($(shell command -v llvm-config 2>/dev/null),)
BREW ?= $(shell command -v brew)
LLVMCONFIG ?= $(shell $(BREW) --prefix llvm)/bin/llvm-config
else
LLVMCONFIG ?= $(shell command -v llvm-config)
endif
TOOLCHAIN ?= $(shell $(LLVMCONFIG) --bindir)/
$(info INFO: Toolchain path: $(TOOLCHAIN))

ifeq ($(shell ls $(TOOLCHAIN)ld.lld 2>/dev/null),)
BREW ?= $(shell command -v brew)
LLDDIR ?= $(shell $(BREW) --prefix lld)/bin/
else
LLDDIR ?= $(TOOLCHAIN)
endif
ifneq ($(TOOLCHAIN),$(LLDDIR))
$(info INFO: LLD path: $(LLDDIR))
endif
endif

ifeq ($(shell uname -m),aarch64)
ARCH ?=
else
ARCH ?= aarch64-linux-gnu-
endif

ifeq ($(USE_CLANG),1)
CLANG_TARGET := $(if $(ARCH),--target=$(ARCH))
CC := $(TOOLCHAIN)clang $(CLANG_TARGET)
AS := $(TOOLCHAIN)clang $(CLANG_TARGET)
LD := $(LLDDIR)ld.lld
OBJCOPY := $(TOOLCHAIN)llvm-objcopy
CLANG_FORMAT ?= $(TOOLCHAIN)clang-format
EXTRA_CFLAGS ?=
else
CC := $(TOOLCHAIN)$(ARCH)gcc
AS := $(TOOLCHAIN)$(ARCH)gcc
LD := $(TOOLCHAIN)$(ARCH)ld
OBJCOPY := $(TOOLCHAIN)$(ARCH)objcopy
CLANG_FORMAT ?= clang-format
EXTRA_CFLAGS ?= -Wstack-usage=2048
endif

ifeq ($(V),)
QUIET := @
else
ifeq ($(V),0)
QUIET := @
else
QUIET :=
endif
endif

BASE_CFLAGS := -O2 -Wall -g -Wundef -Werror=strict-prototypes -fno-common -fno-PIE \
	-Werror=implicit-function-declaration -Werror=implicit-int \
	-Wsign-compare -Wunused-parameter -Wno-multichar \
	-ffreestanding -fpic -ffunction-sections -fdata-sections \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) -isystem sysinc \
	-Isrc -Isrc/hv \
	-fno-stack-protector -mstrict-align -march=armv8.2-a \
	$(EXTRA_CFLAGS)

CFLAGS := $(BASE_CFLAGS) -mgeneral-regs-only

CFG :=
ifeq ($(RELEASE),1)
CFG += RELEASE
endif

# Required for no_std + alloc for now
export RUSTC_BOOTSTRAP=1
RUST_LIB := librust.a
ifeq ($(BUILDSTD),1)
CARGO_FLAGS := -Z build-std=alloc,core
else
CARGO_FLAGS :=
endif

ifeq ($(CHAINLOADING),1)
CFG += CHAINLOADING
CARGO_FLAGS += --features chainload
endif

# Select an ESP-loaded image and omit the embedded Stage 1 configuration.
# Transport and handoff special cases remain scoped to their supported chips;
# the option does not imply hardware qualification on every platform.
ifeq ($(ESP_STAGE2),1)
ifneq ($(CHAINLOADING),1)
$(error ESP_STAGE2 requires CHAINLOADING=1 for boot=)
endif
CFG += ESP_STAGE2
endif

# Development builds only: a boot started by a VDM `reboot debugusb` with a host reading the
# DockChannel console opens the same 15 s proxy window as an aura-proxy request.
ifeq ($(DEBUGUSB_PROXY),1)
CFG += DEBUGUSB_PROXY
endif


LDFLAGS := -EL -maarch64elf --no-undefined -X -Bsymbolic \
	-z notext --no-apply-dynamic-relocs --orphan-handling=warn \
	-z nocopyreloc --gc-sections -pie

MINILZLIB_OBJECTS := $(patsubst %,minilzlib/%, \
	dictbuf.o inputbuf.o lzma2dec.o lzmadec.o rangedec.o xzstream.o)

TINF_OBJECTS := $(patsubst %,tinf/%, \
	adler32.o crc32.o tinfgzip.o tinflate.o tinfzlib.o)

DLMALLOC_OBJECTS := dlmalloc/malloc.o

LIBFDT_OBJECTS := $(patsubst %,libfdt/%, \
	fdt_addresses.o fdt_check.o fdt_empty_tree.o fdt_ro.o fdt_rw.o fdt_strerror.o fdt_sw.o \
	fdt_wip.o fdt.o)

CHICKENS_OBJECTS := $(patsubst %,chickens/%, \
	avalanche.o \
	blizzard.o \
	cyclone_typhoon.o \
	everest.o \
	firestorm.o \
	hurricane_zephyr.o \
	icestorm.o \
	monsoon_mistral.o \
	sawtooth.o \
	twister.o)

DCP_OBJECTS := $(patsubst %,dcp/%, \
	atc_phy.o \
	dpav_ep.o \
	dptx_phy.o \
	dptx_port_ep.o \
	parser.o \
	system_ep.o)

HV_OBJECTS := $(patsubst %,hv/%, \
	hv.o \
	hv_aic.o \
	hv_asm.o \
	hv_exc.o \
	hv_sprr.o \
	hv_virtio.o \
	hv_vm.o \
	hv_vuart.o \
	hv_wdt.o)

OBJECTS := \
	boot_storage.o \
	adt.o \
	afk.o \
	aic.o \
	asc.o \
	$(BUILTIN_LOGO)_48.o $(BUILTIN_LOGO)_128.o $(BUILTIN_LOGO)_256.o \
	chainload.o \
	chainload_asm.o \
	chickens.o \
	clk.o \
	cpufreq.o \
	cpufreq_t6040.o \
	dapf.o \
	dart.o \
	dcp.o \
	dcp_iboot.o \
	devicetree.o \
	display.o \
	dockchannel_uart.o \
	exception.o exception_asm.o \
	fb.o font.o font_retina.o \
	firmware.o \
	gxf.o gxf_asm.o \
	heapblock.o \
	i2c.o \
	iodev.o \
	iova.o \
	isp.o \
	j873.o \
	kboot.o kboot_atc.o kboot_gpu_t8122.o kboot_gpu_t8122_power.o kboot_pmp_t8122.o \
	kboot_t6020_compat.o \
	kboot_display_handoff.o kboot_display_facts.o kboot_gpu26.o kboot_gpu_t6031.o kboot_t6031.o \
	main.o boot_options.o \
	mitigations.o \
	mcc.o \
	memory.o memory_asm.o \
	nvme.o \
	payload.o \
	pcie.o pcie_fdt.o \
	pmgr.o \
	proxy.o \
	ringbuffer.o \
	rtkit.o \
	sart.o \
	sep.o \
	stage1_config.o \
	stage1_proxy.o \
	sio.o \
	smc.o \
	smp.o \
	spmi.o pmu_counter.o ace3.o \
	start.o \
	startup.o \
	string.o \
	tunables.o tunables_static.o \
	tps6598x.o tps6598x_command_core.o \
	uart.o \
	uartproxy.o \
	usb.o usb_dwc3.o usb_dwc3_hs.o usb_dwc3_ss.o usb_cdc.o usb_cdc_atc.o usb_cdc_bulk.o usb_cdc_ss_desc.o usb_cdc_state.o \
	utils.o utils_asm.o \
	vsprintf.o \
	wdt.o \
	$(CHICKENS_OBJECTS) \
	$(DCP_OBJECTS) \
	$(HV_OBJECTS) \
	$(MINILZLIB_OBJECTS) $(TINF_OBJECTS) $(DLMALLOC_OBJECTS) $(LIBFDT_OBJECTS)

FP_OBJECTS := \
	kboot_gpu.o kboot_gpu_m4.o \
	math/expf.o \
	math/exp2f_data.o \
	math/powf.o \
	math/powf_data.o

BUILD_OBJS := $(patsubst %,build/%,$(OBJECTS))
BUILD_FP_OBJS := $(patsubst %,build/%,$(FP_OBJECTS))
BUILD_RUST_LIB := $(patsubst %,build/%,$(RUST_LIB))
BUILD_ALL_OBJS := $(BUILD_OBJS) $(BUILD_FP_OBJS) $(BUILD_RUST_LIB)
NAME := aura
TARGET := aura.macho
TARGET_RAW := aura.bin

DEPDIR := build/.deps

.PHONY: all clean format invoke_cc always_rebuild
all: build/$(TARGET) build/$(TARGET_RAW) verify-logo
.PHONY: verify-logo
verify-logo: build/$(TARGET_RAW)
	$(QUIET)python3 tools/verify_builtin_logo.py $< $(BUILTIN_LOGO) --logo-dir $(LOGO_DIR)
clean:
	rm -rf build/* build/.deps
format:
	$(CLANG_FORMAT) -i src/*.c src/*.inc src/chickens/*.c src/dcp/*.c src/hv/*.c src/math/*.c src/*.h src/dcp/*.h src/hv/*.h src/math/*.h sysinc/*.h
format-check:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.inc src/chickens/*.c src/dcp/*.c src/hv/*.c src/math/*.c src/*.h src/dcp/*.h src/hv/*.h src/math/*.h sysinc/*.h
rustfmt:
	cd rust && cargo fmt
rustfmt-check:
	cd rust && cargo fmt --check

build/$(RUST_LIB): src/../build/build_cfg.h rust/src/*.rs rust/src/gpu/*.rs rust/src/gpu/hw/*.rs rust/Cargo.toml rust/Cargo.lock
	$(QUIET)echo "  RS    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cargo build $(CARGO_FLAGS) --target $(RUSTARCH) --lib --release --manifest-path rust/Cargo.toml --target-dir build
	$(QUIET)cp "build/$(RUSTARCH)/release/${RUST_LIB}" "$@"

build/%.o: src/%.S src/../build/build_cfg.h
	$(QUIET)echo "  AS    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(AS) -c $(BASE_CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

$(BUILD_FP_OBJS): build/%.o: src/%.c
	$(QUIET)echo "  CC FP $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(CC) -c $(BASE_CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

build/%.o: src/%.c build-tag build-cfg
	$(QUIET)echo "  CC    $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(CC) -c $(CFLAGS) -MMD -MF $(DEPDIR)/$(*F).d -MQ "$@" -MP -o $@ $<

# special target for usage by aura.loadobjs
invoke_cc:
	$(QUIET)$(CC) -c $(CFLAGS) -Isrc -Isrc/hv -o $(OBJFILE) $(CFILE)

build/$(NAME).elf: $(BUILD_ALL_OBJS) aura.ld
	$(QUIET)echo "  LD    $@"
	$(QUIET)$(LD) -T aura.ld $(LDFLAGS) -o $@ $(BUILD_ALL_OBJS)

build/$(NAME)-raw.elf: $(BUILD_ALL_OBJS) aura-raw.ld
	$(QUIET)echo "  LDRAW $@"
	$(QUIET)$(LD) -T aura-raw.ld $(LDFLAGS) -o $@ $(BUILD_ALL_OBJS)

build/$(NAME).macho: build/$(NAME).elf
	$(QUIET)echo "  MACHO $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

ifeq ($(LOGO),)
build/$(NAME).bin: build/$(NAME)-raw.elf
	$(QUIET)echo "  RAW   $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

else
build/$(NAME)-payload-base.bin: build/$(NAME)-raw.elf
	$(QUIET)echo "  RAW   $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@

build/$(NAME).bin: build/$(NAME)-payload-base.bin build/$(LOGO).logo
	$(QUIET)echo "  RAW   $@"
	$(QUIET)cat $^ > $@
endif

.INTERMEDIATE: build-tag build-cfg
build-tag src/../build/build_tag.h &:
	$(QUIET)mkdir -p build
	$(QUIET)./version.sh > build/build_tag.tmp
	$(QUIET)cmp -s build/build_tag.h build/build_tag.tmp 2>/dev/null || \
	( mv -f build/build_tag.tmp build/build_tag.h && echo "  TAG   build/build_tag.h" )

build-cfg src/../build/build_cfg.h &:
	$(QUIET)mkdir -p build
	$(QUIET)for i in $(CFG); do echo "#define $$i"; done > build/build_cfg.tmp
	$(QUIET)echo "#define BUILTIN_LOGO $(BUILTIN_LOGO)" >> build/build_cfg.tmp
	$(QUIET)cmp -s build/build_cfg.h build/build_cfg.tmp 2>/dev/null || \
	( mv -f build/build_cfg.tmp build/build_cfg.h && echo "  CFG   build/build_cfg.h" )

# The built-in boot logos are linked gzip-compressed; fb.c inflates them on first use.
# They are only read, so they go in .rodata: its 64 KiB alignment padding has room for them,
# while .data counts towards the file part limit in aura-raw.ld.
LOGO_OBJCOPY_FLAGS := --rename-section .data=.rodata,alloc,load,readonly,data,contents
build/$(BUILTIN_LOGO)_%.bin.gz: $(LOGO_DIR)/$(BUILTIN_LOGO)_%.bin
	$(QUIET)echo "  GZIP  $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)gzip -9 -n -c $< > $@

build/$(BUILTIN_LOGO)_%.o: build/$(BUILTIN_LOGO)_%.bin.gz
	$(QUIET)echo "  BIN   $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(OBJCOPY) -I binary -B aarch64 -O elf64-littleaarch64 $(LOGO_OBJCOPY_FLAGS) $< $@

build/%.bin: data/%.bin
	$(QUIET)echo "  IMG   $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cp $< $@

build/%.o: build/%.bin
	$(QUIET)echo "  BIN   $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(OBJCOPY) -I binary -B aarch64 -O elf64-littleaarch64 $< $@

build/%.bin: font/%.bin
	$(QUIET)echo "  CP    $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)cp $< $@

build/%.rgba: data/%.png
	$(eval SIZE := $(lastword $(subst _, ,$*)))
	$(QUIET)echo "  MAGIC $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)magick $< -background black -flatten -depth 8 -crop $(SIZE)x$(SIZE) -resize $(SIZE)x$(SIZE) rgba:$@

build/%.logo: build/%_256.rgba build/%_128.rgba
	$(QUIET)echo "  PAYLOAD $@"
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)echo -n "aura_logo_256128" > $@
	$(QUIET)cat $^ >> $@

-include $(DEPDIR)/*
