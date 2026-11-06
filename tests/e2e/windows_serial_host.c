/* SPDX-License-Identifier: MIT */
/* Production capture -> event encoder -> atomic transport. See failure model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define AURA_WINDOWS
#define TYPES_H
#define UTILS_H
#define MEMORY_H
#define HV_H
#define __EXCEPTION_H__
#define USB_H
#define AIC_H
#define UART_H
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef int64_t s64;
#define UNUSED(x) ((void)(x))
#define PACKED __attribute__((packed))
#define BIT(x) (1ULL << (x))
#define GENMASK(h,l) (((1ULL << ((h)-(l)+1))-1) << (l))
#define FIELD_GET(f,v) (((v)&(f))/((f)&-(f)))
#define FIELD_PREP(f,v) (((v)*((f)&-(f)))&(f))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DECLARE_SPINLOCK(n) int n = 0
#define SPINLOCK_INIT 0
typedef int spinlock_t;
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
static bool mmu_active(void) { return true; }
static bool is_boot_cpu(void) { return true; }
static u32 chip_id;
#define T8140 0x8140
static void usb_init(void) {}
static void usb_iodev_init(void) {}
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_msecs(u64 ticks) { return ticks; }
static void write8(u64 address, u8 value) { (void)address; (void)value; }
static bool model_enabled = true, model_ready = true;
static bool hv_windows_serial_capture_enabled(void) { return model_enabled; }
static void udelay(unsigned x) { (void)x; }
#define mdelay(x) udelay((x)*1000)
#define sysop(x) ((void)(x))
enum exc_guard_t { GUARD_OFF, GUARD_RETURN, GUARD_SKIP };
int exc_count; enum exc_guard_t exc_guard;
static void hexdump(const void *a, size_t b) { (void)a; (void)b; }
bool monitor_active;
static unsigned atomic_calls;
static size_t maximum_frame;
static FILE *frames;
#include "../../src/iodev.h"
static ssize_t model_atomic(void *opaque, const void *buffer, size_t count) {
    (void)opaque; atomic_calls++;
    if (count > maximum_frame) maximum_frame = count;
    if (!model_ready) return 0;
    assert(count <= 160);
    assert(fwrite(buffer, 1, count, frames) == count);
    return count;
}
static bool no_write(void *opaque) { (void)opaque; return false; }
static ssize_t no_read(void *opaque) { (void)opaque; return 0; }
static ssize_t forbidden_write(void *opaque, const void *b, size_t n) {
    (void)opaque; (void)b; (void)n; assert(!"blocking write reached"); return -1;
}
static const struct iodev_ops atomic_ops = {.write=forbidden_write, .write_atomic=model_atomic};
static const struct iodev_ops empty_ops = {.can_read=no_read, .can_write=no_write};
struct iodev iodev_uart = {.ops=&empty_ops}, iodev_fb = {.ops=&empty_ops},
    iodev_log = {.ops=&empty_ops}, iodev_usb_vuart = {.ops=&empty_ops},
    iodev_dockchannel_uart = {.ops=&atomic_ops};
#include "../../src/iodev.c"
#include "../../src/uartproxy.c"
#include "../../src/hv/windows/serial_capture.c"
struct exc_info;
static void uart_clear_irqs(void) { assert(!"physical UART should be unused in replay"); }
static void aic_set_sw(int irq, bool on) { (void)irq; (void)on; }
static void usb_iodev_vuart_setup(iodev_id_t id) { (void)id; }
static int hv_map_hook(u64 base, bool (*fn)(struct exc_info *,u64,u64*,bool,int),u64 size) {
    (void)base; (void)fn; (void)size; return 0;
}
#include "../../src/hv/hv_vuart.c"

int main(int argc, char **argv) {
    assert(argc == 4);
    const char *mode = argv[1];
    FILE *input = fopen(argv[2], "rb"); assert(input);
    frames = fopen(argv[3], "wb"); assert(frames);
    uartproxy_iodev = IODEV_DOCKCHANNEL_UART;
    assert(!iodev_has_atomic_write((iodev_id_t)-1));
    assert(!iodev_has_atomic_write(IODEV_NUM));
    assert(hv_windows_serial_available());
    if (!strcmp(mode, "disabled")) model_enabled = false;
    if (!strcmp(mode, "unsupported")) {
        uartproxy_iodev = IODEV_UART;
        assert(!hv_windows_serial_available());
    }
    if (!strcmp(mode, "overflow") || !strcmp(mode, "no-host")) model_ready = false;
    if (!strcmp(mode, "sentinel")) disable_data_csums = true;
    int byte;
    while ((byte = fgetc(input)) != EOF) {
        u64 value = byte;
        assert(handle_vuart(NULL, UTXH, &value, true, 2));
    }
    u64 status = 0;
    assert(handle_vuart(NULL, UTRSTAT, &status, false, 2));
    assert((status & (UTRSTAT_TXBE|UTRSTAT_TXE)) == (UTRSTAT_TXBE|UTRSTAT_TXE));
    if (!strcmp(mode, "overflow")) model_ready = true;
    if (!strcmp(mode, "final-blocked")) model_ready = false;
    hv_windows_serial_flush();
    hv_windows_serial_finish();
    unsigned before = atomic_calls;
    hv_windows_serial_finish(); hv_windows_serial_putc('!'); hv_windows_serial_flush();
    assert(atomic_calls == before);
    if (!model_enabled || !strcmp(mode, "unsupported")) assert(atomic_calls == 0);
    fclose(input); fclose(frames);
    printf("{\"atomic_calls\":%u,\"max_frame\":%zu,\"dropped\":%llu}\n",
           atomic_calls, maximum_frame, (unsigned long long)serial_dropped);
    return 0;
}
