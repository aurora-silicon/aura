/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

// #define DEBUG_IODEV

#include "iodev.h"
#include "memory.h"
#include "string.h"

#ifdef DEBUG_IODEV
#define dprintf printf
#else
#define dprintf(...)                                                                               \
    do {                                                                                           \
    } while (0)
#endif

#define CONSOLE_BUFFER_SIZE 8192

extern struct iodev iodev_uart;
extern struct iodev iodev_dockchannel_uart;
extern struct iodev iodev_fb;
extern struct iodev iodev_log;
extern struct iodev iodev_usb_vuart;

/* clang-format off */
struct iodev *iodevs[IODEV_NUM] = {
    [IODEV_UART] = &iodev_uart,
    [IODEV_DOCKCHANNEL_UART] = &iodev_dockchannel_uart,
    [IODEV_FB] = &iodev_fb,
    [IODEV_USB_VUART] = &iodev_usb_vuart,
    [IODEV_LOG] = &iodev_log,
};
/* clang-format on */

char con_buf[CONSOLE_BUFFER_SIZE];
size_t con_wp;
size_t con_rp[IODEV_NUM];

/*
 * G5b C-3: what a console output that cannot keep up costs, per device.  A device whose
 * write_nonblocking takes fewer bytes than offered keeps the rest in con_buf (its backlog); once
 * con_buf wraps past it, the oldest bytes are dropped and counted here instead of the writer
 * waiting for the host.  A stall is a period with a backlog; longest_us its longest duration.
 */
static u64 con_dropped[IODEV_NUM], con_stalls[IODEV_NUM], con_stall_since[IODEV_NUM];
static u64 con_stall_longest[IODEV_NUM], con_call_longest[IODEV_NUM];
/*
 * Must-deliver (iodev_console_must_deliver()): until this tick, a device that takes nothing is
 * polled (its handle_events) and offered the rest again, instead of being left with a backlog.
 * Only the fatal report and the guest-exit lines set it, with a bounded deadline.
 */
static u64 con_must_until;

void iodev_register_device(iodev_id_t id, struct iodev *dev)
{
    if (id >= IODEV_MAX)
        return;
    sysop("dmb ish"); /* a re-take registers it with other cores running (sptm_virt_s2r.c) */
    iodevs[id] = dev;
}

struct iodev *iodev_unregister_device(iodev_id_t id)
{
    if (id < IODEV_USB0 || id >= IODEV_MAX)
        return NULL;

    struct iodev *dev = iodevs[id];
    iodevs[id] = NULL;
    return dev;
}

ssize_t iodev_can_read(iodev_id_t id)
{
    if (!iodevs[id] || !iodevs[id]->ops->can_read)
        return 0;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->can_read(iodevs[id]->opaque);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

bool iodev_can_write(iodev_id_t id)
{
    if (!iodevs[id] || !iodevs[id]->ops->can_write)
        return false;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    bool ret = iodevs[id]->ops->can_write(iodevs[id]->opaque);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

ssize_t iodev_read(iodev_id_t id, void *buf, size_t length)
{
    if (!iodevs[id] || !iodevs[id]->ops->read)
        return -1;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->read(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

ssize_t iodev_write(iodev_id_t id, const void *buf, size_t length)
{
    if (!iodevs[id] || !iodevs[id]->ops->write)
        return -1;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->write(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

ssize_t iodev_write_nonblocking(iodev_id_t id, const void *buf, size_t length)
{
    if (!iodevs[id] || !iodevs[id]->ops->write)
        return -1;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->write_nonblocking
                      ? iodevs[id]->ops->write_nonblocking(iodevs[id]->opaque, buf, length)
                      : iodevs[id]->ops->write(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

ssize_t iodev_write_atomic(iodev_id_t id, const void *buf, size_t length)
{
    if (!iodevs[id] || !iodevs[id]->ops->write)
        return -1;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->write_atomic
                      ? iodevs[id]->ops->write_atomic(iodevs[id]->opaque, buf, length)
                      : iodevs[id]->ops->write(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

bool iodev_has_atomic_write(iodev_id_t id)
{
    return (unsigned)id < IODEV_NUM && iodevs[id] && iodevs[id]->ops &&
           iodevs[id]->ops->write_atomic;
}

/* Unlike iodev_write_atomic(), never fall back to a blocking write. */
ssize_t iodev_try_write_atomic(iodev_id_t id, const void *buf, size_t length)
{
    if (!iodev_has_atomic_write(id))
        return -1;
    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->write_atomic(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

ssize_t iodev_queue(iodev_id_t id, const void *buf, size_t length)
{
    if (!iodevs[id] || !iodevs[id]->ops->queue)
        return iodev_write(id, buf, length);

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    ssize_t ret = iodevs[id]->ops->queue(iodevs[id]->opaque, buf, length);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
    return ret;
}

void iodev_flush(iodev_id_t id)
{
    if (!iodevs[id] || !iodevs[id]->ops->flush)
        return;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
    iodevs[id]->ops->flush(iodevs[id]->opaque);
    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
}

void iodev_lock(iodev_id_t id)
{
    if (!iodevs[id])
        return;

    if (mmu_active())
        spin_lock(&iodevs[id]->lock);
}

void iodev_unlock(iodev_id_t id)
{
    if (!iodevs[id])
        return;

    if (mmu_active())
        spin_unlock(&iodevs[id]->lock);
}

int in_iodev = 0;

static DECLARE_SPINLOCK(console_lock);

void iodev_console_must_deliver(u32 ms)
{
    __atomic_store_n(&con_must_until, ms ? timeout_calculate(ms * 1000) : 0, __ATOMIC_RELEASE);
}

static bool con_must_deliver(void)
{
    u64 until = __atomic_load_n(&con_must_until, __ATOMIC_ACQUIRE);
    return until && !timeout_expired(until);
}

/* One device write; in must-deliver mode, poll the device and retry until the deadline. */
static ssize_t con_dev_write(iodev_id_t id, const void *buf, size_t length)
{
    ssize_t ret = iodev_write_nonblocking(id, buf, length);
    while (ret <= 0 && con_must_deliver() && iodevs[id]->ops->handle_events) {
        if (mmu_active())
            spin_lock(&iodevs[id]->lock);
        iodevs[id]->ops->handle_events(iodevs[id]->opaque);
        if (mmu_active())
            spin_unlock(&iodevs[id]->lock);
        ret = iodev_write_nonblocking(id, buf, length);
    }
    return ret;
}

static void con_backlog_note(iodev_id_t id, bool backlog, u64 now)
{
    if (backlog && !con_stall_since[id]) {
        con_stall_since[id] = now ? now : 1;
        con_stalls[id]++;
    } else if (!backlog && con_stall_since[id]) {
        u64 us = ticks_to_usecs(now - con_stall_since[id]);
        if (us > con_stall_longest[id])
            con_stall_longest[id] = us;
        con_stall_since[id] = 0;
    }
}

void iodev_console_stats(iodev_id_t id, struct iodev_console_stats *st)
{
    memset(st, 0, sizeof(*st));
    if ((unsigned)id >= IODEV_NUM)
        return;
    st->dropped = __atomic_load_n(&con_dropped[id], __ATOMIC_RELAXED);
    st->stalls = __atomic_load_n(&con_stalls[id], __ATOMIC_RELAXED);
    st->longest_stall_us = __atomic_load_n(&con_stall_longest[id], __ATOMIC_RELAXED);
    st->longest_call_us = __atomic_load_n(&con_call_longest[id], __ATOMIC_RELAXED);
    u64 since = __atomic_load_n(&con_stall_since[id], __ATOMIC_RELAXED);
    st->stalled_now_us = since ? ticks_to_usecs(get_ticks() - since) : 0;
}

static void _iodev_console_write_helper(iodev_id_t iod, char prefix, const void *buf, size_t length)
{
    if (!length)
        return;
    if (!(iodevs[iod]->usage & USAGE_CONSOLE))
        return;

    ssize_t (*write)(void *, const void *, size_t) = iodevs[iod]->ops->write_nonblocking
                                                         ? iodevs[iod]->ops->write_nonblocking
                                                         : iodevs[iod]->ops->write;
    if (write(iodevs[iod]->opaque, &prefix, 1) == 1)
        write(iodevs[iod]->opaque, buf, length);
}

void iodev_console_write(const void *buf, size_t length)
{
    bool do_lock = mmu_active();

    if (!do_lock && !is_boot_cpu()) {
        _iodev_console_write_helper(IODEV_UART, '*', buf, length);
        _iodev_console_write_helper(IODEV_DOCKCHANNEL_UART, '*', buf, length);
        return;
    }

    if (do_lock)
        spin_lock(&console_lock);

    if (in_iodev) {
        _iodev_console_write_helper(IODEV_UART, '+', buf, length);
        _iodev_console_write_helper(IODEV_DOCKCHANNEL_UART, '+', buf, length);

        if (do_lock)
            spin_unlock(&console_lock);
        return;
    }
    in_iodev++;

    dprintf("  iodev_console_write() wp=%d\n", con_wp);
    for (iodev_id_t id = 0; id < IODEV_NUM; id++) {
        if (!iodevs[id])
            continue;

        if (!(iodevs[id]->usage & USAGE_CONSOLE)) {
            /* Drop buffer */
            con_rp[id] = con_wp + length;
            continue;
        }

        if (!iodev_can_write(id))
            continue;

        if (con_wp > CONSOLE_BUFFER_SIZE && con_rp[id] < con_wp - CONSOLE_BUFFER_SIZE) {
            /* C-3: con_buf wrapped past this device's backlog: those bytes are gone. */
            con_dropped[id] += con_wp - CONSOLE_BUFFER_SIZE - con_rp[id];
            con_rp[id] = con_wp - CONSOLE_BUFFER_SIZE;
        }

        u64 t0 = get_ticks();
        dprintf("  rp=%d\n", con_rp[id]);
        // Flush existing buffer to device if possible
        while (con_rp[id] < con_wp) {
            size_t buf_rp = con_rp[id] % CONSOLE_BUFFER_SIZE;
            size_t block = min(con_wp - con_rp[id], CONSOLE_BUFFER_SIZE - buf_rp);

            dprintf("  write buf %d\n", block);
            ssize_t ret = con_dev_write(id, &con_buf[buf_rp], block);

            if (ret <= 0)
                goto next_dev;

            con_rp[id] += ret;
        }

        const u8 *p = buf;
        size_t wrote = 0;

        // Write the current buffer
        while (wrote < length) {
            ssize_t ret = con_dev_write(id, p, length - wrote);

            if (ret <= 0)
                goto next_dev;

            con_rp[id] += ret;
            wrote += ret;
            p += ret;
        }

    next_dev:;
        u64 t1 = get_ticks();
        u64 us = ticks_to_usecs(t1 - t0);
        if (us > con_call_longest[id])
            con_call_longest[id] = us;
        /* After the update below this device is level with con_wp + length, or behind it. */
        con_backlog_note(id, con_rp[id] < con_wp + length, t1);
    }

    // Update console buffer

    if (length > CONSOLE_BUFFER_SIZE) {
        buf += (length - CONSOLE_BUFFER_SIZE);
        con_wp += (length - CONSOLE_BUFFER_SIZE);
        length = CONSOLE_BUFFER_SIZE;
    }

    while (length) {
        size_t buf_wp = con_wp % CONSOLE_BUFFER_SIZE;
        size_t block = min(length, CONSOLE_BUFFER_SIZE - buf_wp);
        memcpy(&con_buf[buf_wp], buf, block);
        buf += block;
        con_wp += block;
        length -= block;
    }

    in_iodev--;
    if (do_lock)
        spin_unlock(&console_lock);
}

void iodev_handle_events(iodev_id_t id)
{
    bool do_lock = mmu_active();

    if (do_lock)
        spin_lock(&console_lock);

    if (in_iodev) {
        if (do_lock)
            spin_unlock(&console_lock);
        return;
    }

    in_iodev++;

    if (iodevs[id]->ops->handle_events)
        iodevs[id]->ops->handle_events(iodevs[id]->opaque);

    in_iodev--;

    if (iodev_can_write(id))
        iodev_console_write(NULL, 0);

    if (do_lock)
        spin_unlock(&console_lock);
}

/* Replay retained history on one output after its contents have been lost.
 * Other outputs keep their read positions, so serial logs are not duplicated. */
void iodev_console_rewind(iodev_id_t id)
{
    if ((unsigned)id >= IODEV_NUM || !iodevs[id])
        return;

    bool do_lock = mmu_active();
    if (do_lock)
        spin_lock(&console_lock);
    con_rp[id] = con_wp > CONSOLE_BUFFER_SIZE ? con_wp - CONSOLE_BUFFER_SIZE : 0;
    if (do_lock)
        spin_unlock(&console_lock);
}

void iodev_console_kick(void)
{
    iodev_console_write(NULL, 0);

    for (iodev_id_t id = 0; id < IODEV_NUM; id++) {
        if (!iodevs[id])
            continue;
        if (!(iodevs[id]->usage & USAGE_CONSOLE))
            continue;

        iodev_handle_events(id);
    }
}

void iodev_console_flush(void)
{
    for (iodev_id_t id = 0; id < IODEV_NUM; id++) {
        if (!iodevs[id])
            continue;
        if (!(iodevs[id]->usage & USAGE_CONSOLE))
            continue;

        iodev_flush(id);
    }
}

void iodev_set_usage(iodev_id_t id, iodev_usage_t usage)
{
    if (iodevs[id])
        iodevs[id]->usage = usage;
}

iodev_usage_t iodev_get_usage(iodev_id_t id)
{
    if (iodevs[id])
        return iodevs[id]->usage;
    return 0;
}

void *iodev_get_opaque(iodev_id_t id)
{
    if (id >= IODEV_MAX || !iodevs[id])
        return NULL;

    return iodevs[id]->opaque;
}
