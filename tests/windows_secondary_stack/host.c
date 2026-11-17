/*
 * Runs the production hv_enter_guest_on_stack / hv_enter_guest / hv_exit_guest from hv_asm.S
 * natively on an arm64 host. run.py replaces only the EL2-only instructions: TPIDR_EL2 reads
 * the modeled CPU number, DAIF/SPSR/ELR live in model_* variables, and eret branches to the
 * modeled guest, which runs on SP_EL2 exactly like an exception handler would and leaves
 * through hv_exit_guest. The checks are the failure modes of docs/windows-secondary-el2-stack.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
u64 hv_saved_sp[32];
u64 model_cpu, model_daif, model_spsr, model_elr, switch_daif[4], switch_count;

void hv_enter_guest(u64, u64, u64, u64, void *);
void hv_enter_guest_on_stack(u64, u64, u64, u64, void *, void *);
void hv_exit_guest(void) __attribute__((noreturn));
/* glue.S: run fn(a0..a5) with x19-x28 set to known values; returns 0 if they and SP survived */
u64 call_checked(void *fn, u64 a0, u64 a1, u64 a2, u64 a3, void *a4, void *a5);

static unsigned checks;
static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
    checks++;
    printf("PASS %s\n", name);
}

static u64 guest_sp, guest_args[4], guest_entries;

/* The "guest": what EL2 runs on SP_EL2 after the next guest exception, ending in hv_exit_guest. */
void model_guest(u64 x0, u64 x1, u64 x2, u64 x3)
{
    u64 sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    volatile unsigned char pad[512];
    memset((void *)pad, 0xa5, sizeof(pad)); /* a handler's frame lands below the saved one */
    guest_sp = sp;
    guest_args[0] = x0; guest_args[1] = x1; guest_args[2] = x2; guest_args[3] = x3;
    guest_entries++;
    hv_exit_guest();
}

int main(void)
{
    enum { SIZE = 0x10000 };
    unsigned char *stack = aligned_alloc(0x4000, SIZE);
    u64 top = (u64)stack + SIZE, caller_sp;

    model_cpu = 7;
    for (int pass = 0; pass < 2; pass++) {
        u64 daif = pass ? 0 : 0x3c0;
        model_daif = daif;
        switch_count = 0;
        memset(switch_daif, 0, sizeof(switch_daif));
        memset(hv_saved_sp, 0, sizeof(hv_saved_sp));
        __asm__ volatile("mov %0, sp" : "=r"(caller_sp));
        u64 bad = call_checked((void *)hv_enter_guest_on_stack, 11, 22, 33, 44, (void *)model_guest, (void *)top);
        check(bad == 0, "x19-x28, x29 and SP survive guest entry and exit through the trampoline");
        check(guest_sp >= (u64)stack && guest_sp < top, "guest exceptions run on the new stack");
        check(hv_saved_sp[7] >= (u64)stack && hv_saved_sp[7] < top && hv_saved_sp[7] + 112 <= top,
              "hv_saved_sp of this CPU points into the new stack, frame inside it");
        check(caller_sp < (u64)stack || caller_sp >= top, "caller ran on a different stack");
        check(guest_args[0] == 11 && guest_args[1] == 22 && guest_args[2] == 33 && guest_args[3] == 44,
              "guest receives x0-x3 unchanged");
        check(model_elr == (u64)model_guest, "ELR is the guest entry");
        check(switch_count == 2 && switch_daif[0] == 0x3c0 && switch_daif[1] == 0x3c0, "SP moves twice, each time with DAIF fully masked");
        check(model_spsr == (daif | 5), "guest's first PSTATE carries the caller's DAIF, not the mask");
        check(model_daif == daif, "caller's DAIF is restored after exit");
    }

    u64 first = 0;
    for (int i = 0; i < 1000; i++) {
        switch_count = 0;
        call_checked((void *)hv_enter_guest_on_stack, i, 0, 0, 0, (void *)model_guest, (void *)top);
        if (!i)
            first = guest_sp;
        if (guest_sp != first || guest_args[0] != (u64)i) {
            check(0, "repeated entries reuse the same frame position");
        }
    }
    check(guest_entries == 1002, "repeated entries reuse the same frame position");

    /* The fallback path (no heap stack) still runs on the caller's stack. */
    switch_count = 0;
    u64 bad = call_checked((void *)hv_enter_guest, 1, 2, 3, 4, (void *)model_guest, NULL);
    __asm__ volatile("mov %0, sp" : "=r"(caller_sp));
    check(bad == 0 && (guest_sp < (u64)stack || guest_sp >= top), "fallback keeps today's stack");
    printf("CHECKS %u\n", checks);
    return 0;
}
