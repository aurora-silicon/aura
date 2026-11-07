/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"
#include "chainload.h"
#include "smp_asm.h"
#include "soc.h"

#define UTRSTAT 0x010
#define UTXH 0x020

/* struct boot_args (xnuboot.h); startup.c checks these offsets */
#define BOOTARGS_PHYS_BASE     16
#define BOOTARGS_MEM_SIZE      24
#define BOOTARGS_TOP_OF_KDATA  32

/* MIDR_EL1 implementer and the part range of the M3 cores (midr.h) */
#define MIDR_IMPL_APPLE        0x61
#define MIDR_PART_M3_FIRST     0x42 /* MIDR_PART_T8122_SAWTOOTH */
#define MIDR_PART_M3_LAST      0x49 /* MIDR_PART_T6031_EVEREST */

.extern _start_c
.extern _stack_bot
.extern _v_sp0_sync
.extern _v_sp0_irq
.extern _v_sp0_fiq
.extern _v_sp0_serr
.extern _reset_stack
.extern _cpu_reset_c
.extern smp_reset_stacks
.extern wdt_reboot

.section .init, "ax"

.align 11
.globl _vectors_start
_vectors_start:

    mov x9, '0'
    b cpu_reset
    .align 7
    mov x9, '1'
    b exc_unk
    .align 7
    mov x9, '2'
    b exc_unk
    .align 7
    mov x9, '3'
    b exc_unk
    .align 7
    b _v_sp0_sync
    .align 7
    b _v_sp0_irq
    .align 7
    b _v_sp0_fiq
    .align 7
    b _v_sp0_serr
    .align 7
    b _v_sp0_sync
    .align 7
    b _v_sp0_irq
    .align 7
    b _v_sp0_fiq
    .align 7
    b _v_sp0_serr
    .align 7
    mov x9, 'p'
    b exc_unk
    .align 7
    mov x9, 'q'
    b exc_unk
    .align 7
    mov x9, 'r'
    b exc_unk
    .align 7
    mov x9, 's'
    b exc_unk
    .align 7

.globl _start
.type _start, @function
_start:
    b .Lstart_entry
    .long 0
    /* Load header at CHAINLOAD_HDR_OFFSET, read by chainload.c */
.Lload_header:
    .ascii "AURALOAD"
    .long CHAINLOAD_HDR_VERSION
    .long CHAINLOAD_HDR_F_RUN_BASE
    .quad _image_footprint
.Lrun_base:
    .quad 0
.Lload_header_end:
    .if .Lload_header - _start != CHAINLOAD_HDR_OFFSET - CHAINLOAD_ENTRY_OFFSET
    .error "load header is not at CHAINLOAD_HDR_OFFSET"
    .endif
    .if .Lrun_base - .Lload_header != CHAINLOAD_HDR_RUN_BASE_OFF
    .error "run_base is not at CHAINLOAD_HDR_RUN_BASE_OFF"
    .endif
    .if .Lload_header_end - .Lload_header != CHAINLOAD_HDR_SIZE
    .error "load header size differs from CHAINLOAD_HDR_SIZE"
    .endif

.Lstart_entry:
    mov x19, x0

    /*
     * Run below the address stage 1 staged this image at (chainload.c). On
     * T8140, stage 1 copies a chainloaded image to its own base, where the
     * locked RVBAR of the secondary cores points, but iBoot ends its own
     * allocations there with boot_args and the firmware RO range, about
     * 10 MiB above that base on a J700. An image too large to fit below it
     * is staged after stage 1's heap instead, with run_base in the header
     * above set to stage 1's base. Copy this image from _base to _end there,
     * before any stack or .bss is touched, and continue in the copy. The
     * payload, SEPFW and boot_args stay where stage 1 staged them
     * (relocated_from records that base) and top_of_kernel_data already
     * lies above them, so the copy runs, resets and reserves memory exactly
     * as a smaller image placed at that base does.
     *
     * run_base is zero in every image file, so nothing moves unless a stage
     * 1 asked for it. Stage 1 checked that the copy ends below this image;
     * if it does not, park the CPU rather than run from here, where the
     * memory below would be left unreserved.
     */
    adr x1, .Lrun_base
    ldr x21, [x1]
    cbz x21, .Lrun_base_done
    adrp x20, _base
    adrp x22, _end
    sub x22, x22, x20
    tst x21, #0x3fff
    b.ne .Lrun_base_refused
    adds x23, x21, x22
    b.cs .Lrun_base_refused
    cmp x23, x20
    b.hi .Lrun_base_refused
    mov x1, x20
    mov x2, x21
    mov x3, x22
.Lrun_base_copy:
    ldp x4, x5, [x1], #16
    stp x4, x5, [x2], #16
    subs x3, x3, #16
    b.ne .Lrun_base_copy
    adrp x1, relocated_from
    add x1, x1, :lo12:relocated_from
    sub x1, x1, x20
    str x20, [x1, x21]
    /* Publish the copy as the chainload stub publishes an image */
    mov x1, x21
.Lrun_base_clean:
    dc cvac, x1
    add x1, x1, #64
    cmp x1, x23
    b.lo .Lrun_base_clean
    dsb sy
    mov x1, x21
.Lrun_base_invalidate:
    ic ivau, x1
    add x1, x1, #64
    cmp x1, x23
    b.lo .Lrun_base_invalidate
    dsb sy
    isb
    adr x1, 3f
    sub x1, x1, x20
    add x1, x1, x21
    br x1
.Lrun_base_refused:
    wfe
    b .Lrun_base_refused
.Lrun_base_done:

    /*
     * Run from above the image we were chainloaded in. Stage 1 copies this
     * stage, its payload and the boot_args to its own base and sets
     * top_of_kernel_data to the end of that image. Its own footprint ended
     * where iBoot placed live buffers of its own (on T8122 and T6030 the DCP
     * __OS_LOG buffers start at that base + 0x120000), so a stage whose .bss
     * and stacks reach past that line would share memory with a running
     * coprocessor. Copy the file part of this image to top_of_kernel_data,
     * before any stack or .bss is touched, and continue there. The payload
     * stays where stage 1 put it (relocated_from records the old base), and
     * the heap starts after the moved image. Cores whose RVBAR iBoot locked
     * to the old base still reset there, so the old reset vector is then
     * rewritten to forward them to the new one (kboot keeps that page
     * reserved). Stay put if top_of_kernel_data is not above us or the moved
     * image would not fit in RAM.
     *
     * Only an ESP stage 2 (stage2_relocates) moves, since a stage 1 runs
     * where iBoot placed it, and only on M3 SoCs (T8122, T6030, T6031: Apple
     * MIDR parts 0x42-0x49), where iBoot puts those buffers there. Every
     * other SoC runs where stage 1 put it, as before this move existed.
     */

    adrp x1, stage2_relocates
    ldr w1, [x1, :lo12:stage2_relocates]
    cbz w1, 3f
    adrp x20, _base
    mrs x1, midr_el1
    ubfx x2, x1, #24, #8
    cmp x2, #MIDR_IMPL_APPLE
    b.ne 3f
    ubfx x2, x1, #4, #12
    sub x2, x2, #MIDR_PART_M3_FIRST
    cmp x2, #(MIDR_PART_M3_LAST - MIDR_PART_M3_FIRST)
    b.hi 3f
    ldr x21, [x19, #BOOTARGS_TOP_OF_KDATA]
    mov x1, #0xffff
    add x21, x21, x1
    and x21, x21, #0xffffffffffff0000
    cmp x21, x20
    b.ls 3f
    ldr x22, [x19, #BOOTARGS_PHYS_BASE]
    ldr x23, [x19, #BOOTARGS_MEM_SIZE]
    add x22, x22, x23
    adrp x23, _end
    sub x23, x23, x20
    add x24, x21, x23
    cmp x24, x22
    b.hs 3f
    adrp x25, _file_end
    sub x25, x25, x20
    mov x1, x20
    mov x2, x21
    mov x3, x25
1:
    ldp x4, x5, [x1], #16
    stp x4, x5, [x2], #16
    subs x3, x3, #16
    b.ne 1b
    str x24, [x19, #BOOTARGS_TOP_OF_KDATA]
    adrp x1, relocated_from
    add x1, x1, :lo12:relocated_from
    sub x1, x1, x20
    str x20, [x1, x21]
    /* old _vectors_start: ldr x16, 1f; br x16; 1: .quad new _vectors_start */
    adrp x1, _vectors_start
    add x1, x1, :lo12:_vectors_start
    sub x2, x1, x20
    add x2, x2, x21
    movz w3, #0x0050
    movk w3, #0x5800, lsl #16
    str w3, [x1]
    movz w3, #0x0200
    movk w3, #0xd61f, lsl #16
    str w3, [x1, #4]
    str x2, [x1, #8]
    dc cvau, x1
    mov x1, x21
    add x2, x21, x25
2:
    dc cvau, x1
    add x1, x1, #64
    cmp x1, x2
    b.lo 2b
    dsb ish
    ic ialluis
    dsb ish
    isb
    adr x1, 3f
    sub x1, x1, x20
    add x1, x1, x21
    br x1
3:
    mov w0, 'm'
    bl debug_putc

    adrp x1, _stack_bot
    mov sp, x1

    mov w0, '1'
    bl debug_putc

    ldr x2, [sp, #-8]

    mov w0, 'n'
    bl debug_putc

    adrp x0, _base
    mov x20, x0
    adrp x1, _rela_start
    add x1, x1, :lo12:_rela_start
    adrp x2, _rela_end
    add x2, x2, :lo12:_rela_end
    bl apply_rela

    mov w0, '1'
    bl debug_putc
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    bl pan_fixup

    mrs x8, CurrentEL
    cmp x8, #0xc
    beq start_el3

    mov x0, x19
    mov x1, x20
    bl _start_c
    b .

start_el3:
    bl exception_initialize
    adrp x8, _stack_bot_el3
    mov sp, x8
    mov x0, x19
    mov x1, x20
    msr tpidr_el3, xzr
    adrp x6, _stack_bot
    adr x7, _start_c
    b el3_eret_to_el1

.globl exc_unk
.type exc_unk, @function
exc_unk:
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    mov w0, '!'
    bl debug_putc
    mov w0, 'E'
    bl debug_putc
    mov w0, 'x'
    bl debug_putc
    mov w0, 'C'
    bl debug_putc
    mov w0, ':'
    bl debug_putc
    mov w0, w9
    bl debug_putc
    mov w0, '!'
    bl debug_putc
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    b reboot

.globl cpu_reset
.type cpu_reset, @function
cpu_reset:
    mov w0, 'O'
    bl debug_putc

    /* T8152 has two six-core banks; Aff2 describes the core class. Pick a
     * private stack before C so a late reset never uses the next CPU's stack. */
    adrp x1, chip_id
    ldr w1, [x1, :lo12:chip_id]
    mov w2, #T8152
    cmp w1, w2
    b.ne 4f
    mrs x2, MPIDR_EL1
    and x3, x2, #0xff
    ubfx x4, x2, #8, #8
    cmp x3, #6
    b.hs 5f
    cmp x4, #1
    b.hi 5f
    add x5, x4, x4, lsl #1
    add x3, x3, x5, lsl #1
    adrp x1, secondary_stacks
    add x1, x1, :lo12:secondary_stacks
#ifdef SMP_HEAP_STACKS
    /* Stage 1 holds a pointer per CPU (smp_asm.h); stay parked if smp_init() gave it none. */
    ldr x1, [x1, x3, lsl #3]
    cbz x1, 5f
    mov x5, #SECONDARY_STACK_SIZE
    add x1, x1, x5
#else
    add x3, x3, #1
    mov x5, #SECONDARY_STACK_SIZE
    madd x1, x3, x5, x1
#endif
    b 3f
5:
    wfe
    b 5b
4:

    // for EL3 just keep the shared reset stack for now
    mrs x8, CurrentEL
    cmp x8, #0xc
    beq 2f

    // check if there's already an entry in the stack table for our MPIDR
    mrs x3, mpidr_el1
    and x3, x3, #0xffffff
    adrp x4, smp_reset_stacks
    add x4, x4, :lo12:smp_reset_stacks
    ldr x5, =MAX_CPUS
1:
    cbz x5, 2f // end if table, MPIDR not found -> use shared reset stack
    sub x5, x5, #1

    // x6 = MPIDR, x7 = stack
    ldp x6, x7, [x4], #16

    // check if the current entry's MPIDR matches ours
    cmp x6, x3
    bne 1b

    // entry is valid
    cbz x7, 2f // entry has no stack -> use shared reset stack
    mov x1, x7 // entry has a valid stack -> use it
    b 3f

2:
    adrp x1, _reset_stack
    add x1, x1, :lo12:_reset_stack
    ldr x1, [x1]
3:
    mov sp, x1

    ldr x2, [sp, #-8]

    mov w0, 'K'
    bl debug_putc

    mov x0, sp

    mrs x8, CurrentEL
    cmp x8, #0xc
    beq cpu_reset_el3

    bl _cpu_reset_c
    b .

cpu_reset_el3:
    bl _cpu_reset_c
    adrp x6, _reset_stack_el1
    add x6, x6, :lo12:_reset_stack_el1
    ldr x6, [x6]
    mov x0, x6
    adr x7, _cpu_reset_c

el3_eret_to_el1:
    adr x8, _vectors_start
    msr sctlr_el1, xzr

    mrs x8, scr_el3
    orr x8, x8, #(1<<10) // EL1 execution state is AArch64
    orr x8, x8, #(1<<0)  // EL1 is non-secure
    msr scr_el3, x8
    msr elr_el3, x7 // Set EL1 entry point
    mov x8, #0x5 // EL1h
    msr spsr_el3, x8

    msr sp_el1, x6 // Set EL1 stack pointer

    eret

.globl debug_putc
.type debug_putc, @function
debug_putc:
#ifdef EARLY_UART_BASE
    ldr x1, =EARLY_UART_BASE

1:
    ldr w2, [x1, UTRSTAT]
    tst w2, #2
    beq 1b
    str w0, [x1, UTXH]
#endif
    ret

.globl reboot
.type reboot, @function
reboot:
    mrs x8, id_aa64pfr0_el1
    tst w8, 0xf00
    beq 1f
    mrs x0, CurrentEL
    cmp x0, #8
    beq 1f
    hvc #0
1:
    bl wdt_reboot
    b .

.pool
