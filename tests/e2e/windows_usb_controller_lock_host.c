/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * G6 r3: one DWC3 controller, two iodevs.  The proxy console (a USB iodev with USAGE_CONSOLE) and
 * the Windows vUART (IODEV_USB_VUART) are two CDC interfaces of the same controller.  A printf on
 * a CPU that does not hold the hypervisor lock (the PMU site emulation's fast path) reaches the
 * controller through iodev_console_write(); the vUART emulation reaches it through the vUART
 * iodev (update_irq's poll, the UTXH write, the input feed's reads).  Before r3 the two only
 * shared nothing: two CPUs could be inside the controller at once (G6 session 230552's EL2
 * SError, cpu 9 in the vUART path while three other CPUs printed).
 *
 * This compiles src/iodev.c unchanged with recursive per-thread spinlocks (as utils.c's) and
 * runs real threads: printers call iodev_console_write(), vUART threads take the same iodev
 * calls hv_vuart.c and hv_input.c make.  Every entry to the modelled controller counts how many
 * threads are inside it.  Build with -DUNFIXED to clear console_controller on the vUART iodev
 * (the pre-r3 wiring): the overlap must then be seen.  Nothing here touches hardware.
 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define TYPES_H
#define UTILS_H
#define MEMORY_H
#define STRING_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#define BIT(x)        (1ULL << (x))
#define min(a, b)     ((a) < (b) ? (a) : (b))
#define SPINLOCK_INIT {-1, 0}
typedef struct {
    s64 lock;
    int count;
} spinlock_t;
#define DECLARE_SPINLOCK(n) spinlock_t n = SPINLOCK_INIT

static _Thread_local s64 my_id = -1;
static s64 smp_id(void)
{
    return my_id;
}
/* utils.c's recursive spinlock, with a yield in the wait. */
static void spin_lock(spinlock_t *l)
{
    s64 me = smp_id();
    if (__atomic_load_n(&l->lock, __ATOMIC_ACQUIRE) == me) {
        l->count++;
        return;
    }
    s64 expected = -1;
    while (!__atomic_compare_exchange_n(&l->lock, &expected, me, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
        expected = -1;
        sched_yield();
    }
    assert(l->count == 0);
    l->count = 1;
}
static void spin_unlock(spinlock_t *l)
{
    assert(__atomic_load_n(&l->lock, __ATOMIC_RELAXED) == smp_id() && l->count > 0);
    if (--l->count == 0)
        __atomic_store_n(&l->lock, -1, __ATOMIC_RELEASE);
}
#define sysop(op) __asm__ volatile("" ::: "memory")
static bool mmu_active(void)
{
    return true;
}
static bool is_boot_cpu(void)
{
    return smp_id() == 0;
}
static u64 get_ticks(void)
{
    return 0;
}
static u64 ticks_to_usecs(u64 t)
{
    return t;
}
static u64 timeout_calculate(u64 us)
{
    return us;
}
static bool timeout_expired(u64 t)
{
    (void)t;
    return true;
}

#include "../../src/iodev.h"

/* The controller: how many threads are inside it now, the most ever, and the entries. */
static int inside, most_inside;
static u64 entries, overlaps;
static void controller_work(void)
{
    int now = __atomic_add_fetch(&inside, 1, __ATOMIC_ACQ_REL);
    if (now > 1)
        __atomic_add_fetch(&overlaps, 1, __ATOMIC_RELAXED);
    int seen = __atomic_load_n(&most_inside, __ATOMIC_RELAXED);
    while (now > seen &&
           !__atomic_compare_exchange_n(&most_inside, &seen, now, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        ;
    __atomic_add_fetch(&entries, 1, __ATOMIC_RELAXED);
    /* An event loop or endpoint command takes a while; let the other thread in if it can. */
    for (volatile int i = 0; i < 200; i++)
        ;
    sched_yield();
    __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
}

/* Console interface (pipe 0): what usb_0_* reach. */
static bool con_can_write(void *d)
{
    (void)d;
    controller_work(); /* usb_dwc3_can_write: pipe state */
    return true;
}
static ssize_t con_write(void *d, const void *b, size_t n)
{
    (void)d;
    (void)b;
    controller_work(); /* usb_dwc3_console_write: ring + bulk-in STARTTRANSFER */
    return (ssize_t)n;
}
static const struct iodev_ops con_ops = {
    .can_write = con_can_write, .write = con_write, .write_nonblocking = con_write};

/* vUART interface (pipe 1): what usb_vuart_* and usb_1_* reach. */
static ssize_t vu_can_read(void *d)
{
    (void)d;
    controller_work();
    return 0;
}
static bool vu_can_write(void *d)
{
    (void)d;
    controller_work();
    return true;
}
static ssize_t vu_write(void *d, const void *b, size_t n)
{
    (void)d;
    (void)b;
    controller_work();
    return (ssize_t)n;
}
static ssize_t vu_read(void *d, void *b, size_t n)
{
    (void)d;
    (void)b;
    (void)n;
    controller_work(); /* usb_dwc3_read: event loop + bulk-out STARTTRANSFER */
    return 0;
}
static const struct iodev_ops vu_ops = {
    .can_read = vu_can_read, .can_write = vu_can_write, .read = vu_read, .write = vu_write};

static ssize_t null_write(void *d, const void *b, size_t n)
{
    (void)d;
    (void)b;
    return (ssize_t)n;
}
static bool null_can_write(void *d)
{
    (void)d;
    return true;
}
static const struct iodev_ops null_ops = {.can_write = null_can_write, .write = null_write};

struct iodev iodev_uart = {.ops = &null_ops, .lock = SPINLOCK_INIT},
             iodev_dockchannel_uart = {.ops = &null_ops, .lock = SPINLOCK_INIT},
             iodev_fb = {.ops = &null_ops, .lock = SPINLOCK_INIT},
             iodev_log = {.ops = &null_ops, .lock = SPINLOCK_INIT};
struct iodev iodev_usb_vuart = {.ops = &vu_ops,
                                .lock = SPINLOCK_INIT,
#ifndef UNFIXED
                                .console_controller = true
#endif
};
static struct iodev iodev_usb_console = {
    .ops = &con_ops, .usage = USAGE_CONSOLE, .lock = SPINLOCK_INIT};
#include "../../src/iodev.c"

#define ROUNDS 4000
static void *printer(void *arg)
{
    my_id = (s64)(intptr_t)arg;
    for (int i = 0; i < ROUNDS; i++)
        iodev_console_write("HV: site emulated\n", 18); /* printf from an unlocked fast path */
    return NULL;
}
static void *vuart(void *arg)
{
    my_id = (s64)(intptr_t)arg;
    u8 b = 'x', c;
    for (int i = 0; i < ROUNDS; i++) {
        /* hv_vuart.c update_irq(): the poll under iodev_lock, then can_write. */
        iodev_lock(IODEV_USB_VUART);
        controller_work(); /* usb_dwc3_vuart_poll */
        iodev_unlock(IODEV_USB_VUART);
        (void)iodev_can_write(IODEV_USB_VUART);
        (void)iodev_can_read(IODEV_USB_VUART);
        (void)iodev_write(IODEV_USB_VUART, &b, 1); /* UTXH */
        (void)iodev_read(IODEV_USB_VUART, &c, 1);  /* URXH / the input feed */
    }
    return NULL;
}

int main(void)
{
    my_id = 0;
    iodev_register_device(IODEV_USB0, &iodev_usb_console);
    pthread_t t[4];
    assert(!pthread_create(&t[0], NULL, printer, (void *)(intptr_t)1));
    assert(!pthread_create(&t[1], NULL, printer, (void *)(intptr_t)2));
    assert(!pthread_create(&t[2], NULL, vuart, (void *)(intptr_t)3));
    assert(!pthread_create(&t[3], NULL, vuart, (void *)(intptr_t)4));
    for (int i = 0; i < 4; i++)
        assert(!pthread_join(t[i], NULL));
    assert(console_lock.lock == -1 && console_lock.count == 0 && in_iodev == 0);
    assert(iodev_usb_vuart.lock.lock == -1 && iodev_usb_console.lock.lock == -1);
    printf("{\"entries\":%llu,\"overlaps\":%llu,\"most_inside\":%d,\"console_controller\":%s}\n",
           (unsigned long long)entries, (unsigned long long)overlaps, most_inside,
           iodev_usb_vuart.console_controller ? "true" : "false");
    return 0;
}
