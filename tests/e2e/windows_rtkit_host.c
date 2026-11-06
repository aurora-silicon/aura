/* SPDX-License-Identifier: MIT */
/* Failure-first RTKit -> fixed DART pool -> mailbox protocol replay. */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AURA_WINDOWS
#define CONFIG_H
#define TYPES_H
#define UTILS_H
#define SOC_H
#define XDT_H
#define MALLOC_H
#define MEMORY_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define BIT(n)           (1ULL << (n))
#define GENMASK(h, l)    (((~0ULL) << (l)) & ((~0ULL) >> (63 - (h))))
#define FIELD_GET(m, v)  (((v) & (m)) / ((m) & -(m)))
#define FIELD_PREP(m, v) (((v) * ((m) & -(m))) & (m))
#define ALIGN_UP(n, a)   (((n) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(n, a) ((n) & ~((a) - 1))
#define SZ_16K           0x4000UL
#define USEC_PER_SEC     1000000
#define min(a, b)        ((a) < (b) ? (a) : (b))
#define T8152            0x8152
static u32 chip_id = 0x8142;
static void *adt;
#define ADT_GETPROP(a, n, p, d) ((void)(a), (void)(n), (void)(p), (void)(d), -1)
static int adt_path_offset(void *a, const char *p)
{
    (void)a;
    (void)p;
    return -1;
}
static bool adt_is_compatible(void *a, int n, const char *p)
{
    (void)a;
    (void)n;
    (void)p;
    return false;
}
static bool quiet_boundary, tx_ready = true, fail_map, omit_ap_ack;
static u64 now;
static unsigned maps, unmaps, cleans, heap_allocations, barriers;
#define sysop(op)                                                                                  \
    do {                                                                                           \
        assert(strcmp(op, "dsb sy") == 0);                                                         \
        barriers++;                                                                                \
    } while (0)
static void *pool;
static size_t pool_size;
static int debug_printf(const char *fmt, ...)
{
    (void)fmt;
    assert(!quiet_boundary);
    return 0;
}
static u64 timeout_calculate(u32 us)
{
    return now + us;
}
static bool timeout_expired(u64 deadline)
{
    now += 10;
    return now >= deadline;
}
static void udelay(u32 us)
{
    now += us;
}
static void dc_cvac_range(void *p, size_t n)
{
    assert((uintptr_t)p >= (uintptr_t)pool && n <= pool_size &&
           (uintptr_t)p - (uintptr_t)pool <= pool_size - n);
    cleans++;
}
static void *memalign(size_t alignment, size_t n)
{
    assert(!quiet_boundary);
    void *p = NULL;
    heap_allocations++;
    return posix_memalign(&p, alignment, n) == 0 ? p : NULL;
}
static void real_free(void *p)
{
    free(p);
}
static void model_free(void *p)
{
    assert(p != pool);
    real_free(p);
}
#define free model_free
#include "../../src/rtkit.h"
struct asc_dev {
    bool running;
};
struct dart_dev {
    int unused;
};
struct iova_domain {
    u64 next;
};
static struct asc_message rx[128], tx[128];
static unsigned read_at, write_at, sent;
static void queue(u32 ep, u64 value)
{
    assert(write_at < 128);
    rx[write_at++] = (struct asc_message){value, ep};
}
int asc_get_iop_node(asc_dev_t *a)
{
    (void)a;
    return 0;
}
bool asc_is_v8(asc_dev_t *a)
{
    (void)a;
    return false;
}
void asc_cpu_start(asc_dev_t *a)
{
    a->running = true;
}
void asc_cpu_stop(asc_dev_t *a)
{
    a->running = false;
}
bool asc_cpu_running(asc_dev_t *a)
{
    return a->running;
}
bool asc_can_recv(asc_dev_t *a)
{
    (void)a;
    return read_at < write_at;
}
bool asc_can_send(asc_dev_t *a)
{
    (void)a;
    return tx_ready;
}
bool asc_recv(asc_dev_t *a, struct asc_message *m)
{
    (void)a;
    if (read_at == write_at)
        return false;
    *m = rx[read_at++];
    return true;
}
bool asc_recv_timeout(asc_dev_t *a, struct asc_message *m, u32 us)
{
    u64 deadline = timeout_calculate(us);
    while (!timeout_expired(deadline))
        if (asc_recv(a, m))
            return true;
    return false;
}
bool asc_send(asc_dev_t *a, const struct asc_message *m)
{
    (void)a;
    assert(barriers == cleans);
    assert(tx_ready && sent < 128);
    tx[sent++] = *m;
    if (m->msg1 == 0 && FIELD_GET(GENMASK(59, 52), m->msg0) == 0xb && (m->msg0 & 0xffff) == 0x20) {
        queue(2, (1ULL << 52) | (1ULL << 44));
        queue(2, (8ULL << 52) | (64ULL << 24) | 4);
        queue(8, (1ULL << 56) | (0x4000ULL << 36));
        queue(4, (1ULL << 52) | (1ULL << 44) | 0x390000000ULL);
        queue(0, (7ULL << 52) | 0x20);
        if (!omit_ap_ack)
            queue(0, (0xbULL << 52) | 0x20);
    }
    return true;
}
u64 iova_alloc(iova_domain_t *d, size_t n)
{
    assert(!quiet_boundary);
    u64 v = d->next;
    d->next += n;
    return v;
}
void iova_free(iova_domain_t *d, u64 v, size_t n)
{
    (void)d;
    (void)v;
    (void)n;
    assert(!quiet_boundary);
}
int dart_map(dart_dev_t *d, uintptr_t v, void *p, size_t n)
{
    (void)d;
    (void)v;
    (void)p;
    (void)n;
    assert(!quiet_boundary);
    maps++;
    return fail_map ? -1 : 0;
}
void dart_unmap(dart_dev_t *d, uintptr_t v, size_t n)
{
    (void)d;
    (void)v;
    (void)n;
    assert(!quiet_boundary);
    unmaps++;
}
void *dart_translate(dart_dev_t *d, uintptr_t v)
{
    (void)d;
    (void)v;
    assert(!quiet_boundary);
    return NULL;
}
bool sart_add_allowed_region(sart_dev_t *s, void *p, size_t n)
{
    (void)s;
    (void)p;
    (void)n;
    return false;
}
bool sart_remove_allowed_region(sart_dev_t *s, void *p, size_t n)
{
    (void)s;
    (void)p;
    (void)n;
    return false;
}
#include "../../src/rtkit.c"
static void reset_bus(void)
{
    read_at = write_at = sent = 0;
    tx_ready = true;
    omit_ap_ack = false;
    now = 0;
}
static void boot_messages(void)
{
    queue(0, (1ULL << 52) | 11 | (12ULL << 16));
    queue(0, (8ULL << 52) | BIT(51) | BIT(1) | BIT(2) | BIT(4) | BIT(8) | BIT(10));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    pool_size = 2 * SZ_16K;
    assert(posix_memalign(&pool, SZ_16K, pool_size) == 0);
    memset(pool, 0xa5, pool_size);
    struct asc_dev asc = {0};
    struct dart_dev dart = {0};
    struct iova_domain iova = {0x2000000};
    rtkit_dev_t *r = rtkit_init("test", &asc, &dart, &iova, NULL, false);
    assert(r);
    assert(!rtkit_set_phys_window(r, UINT64_MAX - 4, 8));
    assert(rtkit_set_phys_window(r, 0x390000000ULL, 0x100000));
    assert(!rtkit_set_buffer_pool(r, (u64)pool + 1, pool_size));
    assert(rtkit_set_buffer_pool(r, (u64)pool, pool_size));
    assert(maps == 1 && rtkit_has_owned_buffers(r));
    assert(!rtkit_set_buffer_pool(r, (u64)pool, pool_size));
    assert(rtkit_set_early_ap_power(r, true));
    boot_messages();
    assert(rtkit_boot_timed(r, 10000));
    assert(!rtkit_set_phys_window(r, 0x390000000ULL, 0x100000));
    unsigned ep10 = 128, ap_on = 128;
    for (unsigned i = 0; i < sent; i++)
        if (tx[i].msg1 == 0) {
            if (FIELD_GET(MGMT_TYPE, tx[i].msg0) == 5 &&
                FIELD_GET(MGMT_MSG_START_EP_IDX, tx[i].msg0) == 10)
                ep10 = i;
            if (FIELD_GET(MGMT_TYPE, tx[i].msg0) == 0xb)
                ap_on = i;
        }
    assert(ep10 < ap_on && r->iop_power == RTKIT_POWER_ON && r->ap_power == RTKIT_POWER_ON);
    assert(r->syslog_bfr.owned && r->oslog_bfr.owned && !r->ioreport_bfr.owned);
    assert(r->syslog_bfr.bfr == pool && r->oslog_bfr.bfr == (u8 *)pool + SZ_16K);
    assert(r->ioreport_bfr.bfr == (void *)0x390000000ULL && maps == 1 && cleans >= 2);
    assert(heap_allocations == 0);
    for (size_t i = 0; i < pool_size; i++)
        assert(((u8 *)pool)[i] == 0);
    queue(2, 5ULL << 52);
    queue(4, 8ULL << 52);
    unsigned before = read_at, old_sent = sent;
    quiet_boundary = true;
    tx_ready = false;
    assert(rtkit_service_quiet(r, 4) == 0);
    assert(read_at == before && sent == old_sent);
    tx_ready = true;
    assert(rtkit_service_quiet(r, 1) == 1);
    assert(read_at == before + 1 && sent == old_sent + 1);
    assert(rtkit_service_quiet(r, 1) == 1);
    assert(rtkit_service_quiet(r, 0) < 0);
    assert(rtkit_service_quiet(r, 65) < 0);
    r->service_busy = true;
    assert(rtkit_service_quiet(r, 1) < 0);
    r->service_busy = false;
    assert(maps == 1 && heap_allocations == 0);
    FILE *f = fopen(argv[1], "wb");
    assert(f);
    assert(sent > 0);
    assert(fwrite(tx, sizeof(tx[0]), sent, f) == sent);
    fclose(f);
    /* A third grant exhausts the fixed pool; no fallback or message skipping. */
    struct rtkit_buffer extra = {0};
    assert(!rtkit_alloc_buffer(r, &extra, SZ_16K));
    queue(2, (1ULL << 52) | (1ULL << 44) | 0x390100000ULL);
    assert(rtkit_service_quiet(r, 1) < 0);
    assert(rtkit_service_quiet(r, 1) < 0);
    quiet_boundary = false;
    asc_cpu_stop(&asc);
    rtkit_free(r);
    assert(unmaps == 1);
    /* Whole-handshake timeout, with no HELLO; setters cannot reset the attempt. */
    reset_bus();
    r = rtkit_init("timeout", &asc, &dart, &iova, NULL, false);
    assert(r);
    assert(rtkit_set_phys_window(r, 0x390000000ULL, 0x100000));
    assert(rtkit_set_buffer_pool(r, (u64)pool, pool_size));
    assert(rtkit_set_early_ap_power(r, true));
    assert(!rtkit_boot_timed(r, 100));
    assert(now <= 120 && !rtkit_set_early_ap_power(r, false));
    asc_cpu_stop(&asc);
    rtkit_free(r);
    /* A failed pool map is not published and cannot trigger heap fallback. */
    reset_bus();
    r = rtkit_init("mapfail", &asc, &dart, &iova, NULL, false);
    assert(r);
    assert(rtkit_set_phys_window(r, 0x390000000ULL, 0x100000));
    fail_map = true;
    assert(!rtkit_set_buffer_pool(r, (u64)pool, pool_size));
    assert(!rtkit_alloc_buffer(r, &extra, SZ_16K));
    fail_map = false;
    rtkit_free(r);
    /* Each malformed transcript starts a real production handshake. */
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        reset_bus();
        r = rtkit_init("transcript", &asc, &dart, &iova, NULL, false);
        assert(r);
        assert(rtkit_set_phys_window(r, 0x390000000ULL, 0x100000));
        assert(rtkit_set_buffer_pool(r, (u64)pool, pool_size));
        assert(rtkit_set_early_ap_power(r, true));
        if (scenario == 0) {
            queue(0, (1ULL << 52) | 12 | (11ULL << 16)); /* Reversed version range. */
        } else if (scenario == 1) {
            queue(0, (1ULL << 52) | 11 | (12ULL << 16));
            queue(0, (8ULL << 52) | BIT(1));
            queue(0, (8ULL << 52) | BIT(51) | BIT(1));
        } else {
            boot_messages();
            omit_ap_ack = scenario == 2;
        }
        if (scenario < 3) {
            assert(!rtkit_boot_timed(r, 1000));
            assert(now <= 1020 && r->windows_failed && rtkit_has_owned_buffers(r));
        } else {
            assert(rtkit_boot_timed(r, 1000));
            queue(0x20, 0x1234);
            quiet_boundary = true;
            assert(rtkit_service_quiet(r, 1) < 0 && r->windows_failed);
            quiet_boundary = false;
        }
        asc_cpu_stop(&asc);
        rtkit_free(r);
    }
    real_free(pool);
    puts("{\"passed\":true,\"heap_grants\":0,\"quiet_poll_bound\":64}");
}
