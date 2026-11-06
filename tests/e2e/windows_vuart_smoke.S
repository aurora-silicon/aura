/* SPDX-License-Identifier: MIT */
/* Position-independent EL1 guest; only the host-validated virtual UART is touched. */
.section .text, "ax"
.global _start
.global vuart_results
_start:
    msr daifset, #0xf
    adr x19, vuart_results
    adr x1, guest_stack_end
    mov sp, x1
    str x0, [x19, #120]
    mrs x0, CurrentEL
    str x0, [x19, #128]
    cmp x0, #4
    b.ne bad_input
    ldr x20, [x19, #48]
    ldr x22, [x19, #32]
    cbz x20, bad_input
    cbz x22, bad_input
    mrs x21, cntfrq_el0
    str x21, [x19, #56]
    cbz x21, bad_input
    adr x0, magic
    ldr x0, [x0]
    str x0, [x19]
    mov x0, #1
    str x0, [x19, #8]
    str x0, [x19, #16]
    mrs x24, cntpct_el0
    mov x0, #220
    madd x24, x21, x0, x24     /* Absolute guest deadline, independent of host. */
    adr x0, ready
    bl marker
    mov w13, #'S'
    bl command
    bl pattern
    bl isolation_payload
    adr x0, pause_ready
    bl marker
    mov w13, #'P'
    bl command
    adr x0, pause_ack
    bl marker
    mrs x25, cntpct_el0
    mov x0, #45
    madd x25, x21, x0, x25    /* Must be full before host resumes at >=60s. */
    mov x23, #0
    mov x0, #2
    str x0, [x19, #16]
fill:
    mrs x10, cntpct_el0
    cmp x10, x25
    b.hs no_pressure
    ldr w9, [x20, #0x10]
    tbz w9, #1, first_full
    mov w0, #0xa5
    str w0, [x20, #0x20]
    add x23, x23, #1
    str x23, [x19, #64]
    movz x0, #0x80, lsl #16   /* Eight MiB maximum. */
    cmp x23, x0
    b.lo fill
no_pressure:
    mov x0, #2
    b fail
first_full:
    mrs x27, cntpct_el0
    mov x0, #5
    udiv x28, x21, x0
    cbnz x28, prove_full
    mov x28, #1
prove_full:
    mrs x10, cntpct_el0
    cmp x10, x25
    b.hs no_pressure
    ldr w9, [x20, #0x10]
    tbnz w9, #1, fill        /* Host progressed; refill and restart the dwell. */
    mrs x10, cntpct_el0
    sub x10, x10, x27
    cmp x10, x28
    b.lo prove_full
    str x10, [x19, #160]
full:
    mov x0, #3
    str x0, [x19, #16]
    mrs x26, cntpct_el0
    mov w0, #0xee
    str w0, [x20, #0x20]
    str w0, [x20, #0x20]
    str w0, [x20, #0x20]
    str w0, [x20, #0x20]
    mrs x0, cntpct_el0
    sub x0, x0, x26
    str x0, [x19, #80]
    cmp x0, x21              /* Four ignored-full writes must take <1 second. */
    b.hs slow_full_write
    mov x0, #4
    str x0, [x19, #144]
    mov x23, #0
wait_resume:
    bl deadline
    ldr w9, [x20, #0x10]
    tbnz w9, #1, resumed
    add x23, x23, #1
    str x23, [x19, #72]
    b wait_resume
resumed:
    adr x0, full_done
    bl marker
    mov x0, #4
    str x0, [x19, #16]
    mov w13, #'C'
    bl command
    mrs x26, cntpct_el0
    str x26, [x19, #112]
    adr x0, close_ack
    bl marker
    add x25, x26, x21, lsl #1
wait_close:
    bl deadline
    ldr w9, [x20, #0x10]     /* Service the host's DTR-close control request. */
    mrs x10, cntpct_el0
    cmp x10, x25
    b.lo wait_close
    sub x0, x10, x26
    str x0, [x19, #96]
    mov x23, #1024
closed_write:
    ldr w9, [x20, #0x10]
    tbz w9, #1, closed_not_ready
    mov w0, #0xcc
    str w0, [x20, #0x20]
    subs x23, x23, #1
    b.ne closed_write
    mrs x0, cntpct_el0
    sub x0, x0, x26
    str x0, [x19, #104]
    ldr x1, [x19, #96]
    sub x1, x0, x1
    str x1, [x19, #88]
    add x1, x21, x21, lsl #1
    cmp x0, x1              /* All discard writes end <3s after close ACK. */
    b.hs slow_closed_write
    mov x0, #5
    str x0, [x19, #16]
    mov w13, #'R'
    bl command
    adr x0, reopened
    bl marker
    bl pattern
    adr x0, done
    bl marker
    mov w13, #'Q'            /* Host has received all bytes before SYSTEM_OFF. */
    bl command
    str x22, [x19, #40]
    adr x0, done
    ldr x0, [x0]
    str x0, [x19, #152]
    mov x0, #6
    str x0, [x19, #16]
    b poweroff
bad_input:
    mov x0, #1
    b fail
slow_full_write:
    mov x0, #3
    b fail
closed_not_ready:
    mov x0, #4
    b fail
slow_closed_write:
    mov x0, #5
    b fail
bad_command:
    mov x0, #6
    b fail
timed_out:
    mov x0, #7
fail:
    str x0, [x19, #24]
    mov x0, #255
    str x0, [x19, #16]
poweroff:
    dsb sy
    movz w0, #8
    movk w0, #0x8400, lsl #16
    smc #0
    mov x0, #254
    str x0, [x19, #16]
1:  wfe
    b 1b

/* Helpers clobber x0, x9, x10; caller state uses x19..x26. */
deadline:
    mrs x10, cntpct_el0
    cmp x10, x24
    b.hs timed_out
    ret
put:
    stp x29, x30, [sp, #-16]!
1:  bl deadline
    ldr w9, [x20, #0x10]
    tbz w9, #1, 1b
    str w0, [x20, #0x20]
    ldp x29, x30, [sp], #16
    ret
get:
    stp x29, x30, [sp, #-16]!
1:  bl deadline
    ldr w9, [x20, #0x10]
    tbz w9, #0, 1b
    ldr w0, [x20, #0x24]
    ldp x29, x30, [sp], #16
    ret
marker:
    stp x29, x30, [sp, #-16]!
    ldr x12, [x0]
    mov x11, #8
1:  and w0, w12, #0xff
    bl put
    lsr x12, x12, #8
    subs x11, x11, #1
    b.ne 1b
    mov x12, x22
    mov x11, #8
2:  and w0, w12, #0xff
    bl put
    lsr x12, x12, #8
    subs x11, x11, #1
    b.ne 2b
    ldp x29, x30, [sp], #16
    ret
command:
    stp x29, x30, [sp, #-16]!
    bl get
    cmp w0, w13
    b.ne bad_command
    mov x11, #0
    mov x12, #0
1:  bl get
    lsl x0, x0, x11
    orr x12, x12, x0
    add x11, x11, #8
    cmp x11, #64
    b.lo 1b
    cmp x12, x22
    b.ne bad_command
    ldp x29, x30, [sp], #16
    ret
pattern:
    stp x29, x30, [sp, #-16]!
    mov x23, #0
1:  mov w0, w23
    bl put
    add x23, x23, #1
    cmp x23, #256
    b.lo 1b
    ldp x29, x30, [sp], #16
    ret
isolation_payload:
    stp x29, x30, [sp, #-16]!
    adr x12, isolation_prefix
    mov x11, #21
1:  ldrb w0, [x12], #1
    bl put
    subs x11, x11, #1
    b.ne 1b
    mov x12, x22
    mov x11, #60
2:  lsr x0, x12, x11
    and w0, w0, #15
    cmp w0, #10
    b.lo 3f
    add w0, w0, #39
3:  add w0, w0, #'0'
    bl put
    subs x11, x11, #4
    b.pl 2b
    adr x12, isolation_suffix
    mov x11, #11
4:  ldrb w0, [x12], #1
    bl put
    subs x11, x11, #1
    b.ne 4b
    ldp x29, x30, [sp], #16
    ret
.balign 8
isolation_prefix: .ascii "HVLOG: VUART-ISOLATE-"
isolation_suffix: .byte 0xff,0x55,0xaa,0x05,0,13,10
                  .ascii "END\n"
.balign 8
magic:       .ascii "AVUART01"
ready:       .ascii "VURDY001"
pause_ready: .ascii "VUPAUSE1"
pause_ack:   .ascii "VUPACK01"
full_done:   .ascii "VUFULL01"
close_ack:   .ascii "VUCLOSE1"
reopened:    .ascii "VUREOPEN"
done:        .ascii "VUDONE01"
.org 0x1000
vuart_results:
    .space 256
.org 0x3000
    .space 0x1000
guest_stack_end:
