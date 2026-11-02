# SPDX-License-Identifier: GPL-2.0-only
B ?= $(CURDIR)/build
CFLAGS = --target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -fPIE -mgeneral-regs-only -O2 -Wall \
	-mbranch-protection=bti -fno-stack-protector $(EXTRA)
OBJS = $(B)/start.o $(B)/vectors.o $(B)/main.o $(B)/ladder.o $(B)/mm.o $(B)/con.o $(B)/guest.o $(B)/fb.o $(B)/wdt.o $(B)/linux.o $(B)/apif.o $(B)/smp.o $(B)/adt.o $(B)/iommu.o $(B)/uat_selftest.o $(B)/gpu.o $(B)/mhv.o $(B)/vgic_cpuif.o
GCFLAGS = --target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -fPIE -mgeneral-regs-only -O2 -Wall \
	-mstrict-align -fno-zero-initialized-in-bss -fno-stack-protector -mbranch-protection=none -DGSTUB_C $(EXTRA)
all: $(B)/mon.elf
$(B)/gstub.bin: gstub.S gstub.c gstub.h gstub.ld font8x16.h
	@mkdir -p $(B)
	clang $(GCFLAGS) -c gstub.S -o $(B)/gstub-s.o
	clang $(GCFLAGS) -c gstub.c -o $(B)/gstub-c.o
	ld.lld -T gstub.ld -static --no-pie -o $(B)/gstub.elf $(B)/gstub-s.o $(B)/gstub-c.o
	llvm-objcopy -O binary $(B)/gstub.elf $@
$(B)/start.o: start.S $(B)/gstub.bin ; clang $(CFLAGS) -DGSTUB_BIN='"$(B)/gstub.bin"' -c $< -o $@
$(B)/%.o: %.S ; @mkdir -p $(B); clang $(CFLAGS) -c $< -o $@
$(B)/%.o: %.c mon.h mhv.h platform-io.h ; @mkdir -p $(B); clang $(CFLAGS) -c $< -o $@
$(B)/mon.elf: $(OBJS) link.ld ; ld.lld -T link.ld -static --no-pie -o $@ $(OBJS)
clean: ; rm -rf $(B)
