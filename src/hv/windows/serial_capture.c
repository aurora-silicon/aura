/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "serial_capture.h"
#include "hv.h"

#include "../../iodev.h"
#include "../../uartproxy.h"
#include "../../utils.h"

#define SERIAL_CAPACITY 4096
#define SERIAL_BATCH    128
#define SERIAL_FINAL    1

struct serial_event {
    u16 abi;
    u16 flags;
    u32 sequence;
    u64 dropped;
    u16 length;
    u16 reserved;
    u8 data[SERIAL_BATCH];
} PACKED;
static_assert(sizeof(struct serial_event) == 20 + SERIAL_BATCH, "serial event ABI");

static DECLARE_SPINLOCK(serial_lock);
static u8 serial_buffer[SERIAL_CAPACITY];
static size_t serial_head, serial_count;
static u64 serial_dropped, serial_reported_dropped;
static u32 serial_sequence;
static bool serial_finished;

bool hv_windows_serial_available(void)
{
    return iodev_has_atomic_write(uartproxy_iodev);
}

/* Called with serial_lock held. One bounded, all-or-none frame attempt. */
static bool serial_send(u16 flags)
{
    struct serial_event event = {
        .abi = 1,
        .flags = flags,
        .sequence = serial_sequence,
        .dropped = serial_dropped,
        .length = min(serial_count, SERIAL_BATCH),
    };
    for (size_t i = 0; i < event.length; i++)
        event.data[i] = serial_buffer[(serial_head + i) % SERIAL_CAPACITY];
    if (!uartproxy_try_send_event(EVT_WINDOWS_SERIAL, &event, 20 + event.length))
        return false;
    serial_head = (serial_head + event.length) % SERIAL_CAPACITY;
    serial_count -= event.length;
    serial_reported_dropped = serial_dropped;
    serial_sequence++;
    return true;
}

void hv_windows_serial_putc(u8 byte)
{
    if (!hv_windows_serial_capture_enabled())
        return;
    spin_lock(&serial_lock);
    if (!serial_finished) {
        if (serial_count == SERIAL_CAPACITY) {
            serial_dropped++;
        } else {
            serial_buffer[(serial_head + serial_count) % SERIAL_CAPACITY] = byte;
            serial_count++;
        }
        if (byte == '\n' || serial_count >= SERIAL_BATCH)
            serial_send(0);
    }
    spin_unlock(&serial_lock);
}

void hv_windows_serial_flush(void)
{
    if (!hv_windows_serial_capture_enabled())
        return;
    spin_lock(&serial_lock);
    if (!serial_finished && (serial_count || serial_dropped != serial_reported_dropped))
        serial_send(0);
    spin_unlock(&serial_lock);
}

void hv_windows_serial_finish(void)
{
    if (!hv_windows_serial_capture_enabled())
        return;
    spin_lock(&serial_lock);
    if (!serial_finished) {
        /* The caller has stopped all guest CPUs. Never wait for a host. */
        for (size_t i = 0; i < SERIAL_CAPACITY / SERIAL_BATCH && serial_count; i++) {
            if (!serial_send(0))
                break;
        }
        serial_dropped += serial_count;
        serial_count = 0;
        serial_send(SERIAL_FINAL);
        serial_finished = true;
    }
    spin_unlock(&serial_lock);
}
