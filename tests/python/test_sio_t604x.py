# SPDX-License-Identifier: MIT
"""Exercise the actual SIO table producer with synthetic ADT records."""
from pathlib import Path
import subprocess


def test_sio_t604x_tables(tmp_path):
    root = Path(__file__).resolve().parents[2]
    source = "\n".join(x for x in (root / "src/sio.c").read_text().splitlines()
                       if not x.startswith("#include"))
    structs = "\n".join(x for x in (root / "src/sio.h").read_text().splitlines()
                        if not x.startswith("#include"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define T6030 0x6030
#define T6040 0x6040
#define T6041 0x6041
#define SZ_4K 0x1000UL
#define SZ_16K 0x4000UL
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ALIGN_UP(x,a) (((x) + (a) - 1) & ~((a) - 1))
static int chip_id, adt;
static u8 arena[0x80000] __attribute__((aligned(16384)));
static size_t used;
static u8 maps[100], shims[300], devices[200], tunables[16];
static u32 maplen, shimlen, devlen;
static bool missing, fail_alloc;
static u64 top_of_memory_alloc(size_t n)
{
    if (fail_alloc) return 0;
    n = ALIGN_UP(n, SZ_16K);
    assert(n <= sizeof(arena) - used);
    u64 p = (uintptr_t)(arena + used);
    used += n;
    return p;
}
static void memset64(void *p, u64 v, size_t n) { assert(!v); memset(p, 0, n); }
static void memcpy8(void *p, const void *q, size_t n) { memcpy(p, q, n); }
static int adt_path_offset(int tree, const char *p) { (void)tree; (void)p; return 1; }
static const void *adt_getprop(int tree, int node, const char *p, u32 *len)
{
    (void)tree; (void)node;
    if (!strcmp(p, "map-range")) { *len = maplen; return maps; }
    if (!strcmp(p, "dmashim")) { *len = shimlen; return missing ? NULL : shims; }
    if (!strcmp(p, "device-type")) { *len = devlen; return devices; }
    assert(!strcmp(p, "asio-ascwrap-tunables"));
    *len = sizeof(tunables); return tunables;
}
''' + structs + source + r'''
static void record(u8 *p, const char *key, size_t n, u8 value)
{
    u32 k = (u32)(u8)key[0] << 24 | (u32)(u8)key[1] << 16 |
            (u32)(u8)key[2] << 8 | (u8)key[3];
    memcpy(p, &k, 4); memset(p + 4, value, n);
}
static void reset(void)
{
    used = 0; missing = false; fail_alloc = false;
    memset(arena, 0xa5, sizeof(arena));
    maplen = 20; shimlen = 3 * 36; devlen = 3 * 12;
    record(maps, "MISC", 16, 0x11);
    record(shims, "SSPI", 32, 0x22);
    record(shims + 36, "SUAR", 32, 0x33);
    record(shims + 72, "SAUD", 32, 0x44);
    record(devices, "dSPI", 8, 0x55);
    record(devices + 12, "dUAR", 8, 0x66);
    record(devices + 24, "dDPA", 8, 0x77);
}
static u8 *table(struct sio_data *d, u32 key, u32 size)
{
    for (int i = 0; i < d->num_fwparams; i += 2) {
        if (d->fwparams[i].key != key) continue;
        assert(d->fwparams[i+1].key == key + 1 && d->fwparams[i+1].value == size);
        u64 start = (u64)d->fwparams[i].value << 12;
        for (int j = 0; j < d->num_fwdata; j++) {
            struct sio_mapping *m = &d->fwdata[j];
            assert(!(m->iova % SZ_16K) && !(m->size % SZ_16K));
            assert(m->iova + m->size <= (1ULL << 42));
            if (start >= m->iova && start + size <= m->iova + m->size)
                return (u8 *)(uintptr_t)(m->phys + start - m->iova);
        }
        assert(0);
    }
    assert(0); return NULL;
}
static void bytes(const u8 *p, size_t n, u8 value)
{ for (size_t i = 0; i < n; i++) assert(p[i] == value); }
int main(void)
{
    const int chips[] = {T6040, T6041};
    for (size_t c = 0; c < ARRAY_SIZE(chips); c++) {
        chip_id = chips[c]; reset();
        struct sio_data *d = sio_setup_fwdata("/arm-io/sio"); assert(d);
        u8 *p = table(d, 0x1a, 0x40); bytes(p, 48, 0); bytes(p+48, 16, 0x11);
        p = table(d, 0x22, 0xd8);
        for (int k = 0; k < 6; k++) {
            bytes(p + k*36, 32, k < 3 ? 0x22 + k*0x11 : 0);
            bytes(p + k*36 + 32, 4, 0xff);
        }
        p = table(d, 0x1c, 0x48);
        bytes(p, 8, 0x55); bytes(p+8, 8, 0x66); bytes(p+16, 8, 0);
        bytes(p+24, 8, 0x77); bytes(p+32, 40, 0); free(d);
        /* Named slots must not inherit T6030's anonymous spacer positions. */
        reset(); record(shims + shimlen, "SQSP", 32, 0x88); shimlen += 36;
        record(shims + shimlen, "ADMA", 32, 0x99); shimlen += 36;
        record(devices + devlen, "dQSP", 8, 0xaa); devlen += 12;
        d = sio_setup_fwdata("/arm-io/sio"); assert(d);
        p = table(d, 0x22, 0xd8); bytes(p+3*36, 32, 0x88); bytes(p+4*36, 32, 0x99);
        p = table(d, 0x1c, 0x48); bytes(p+8*8, 8, 0xaa); free(d);
        reset(); record(shims, "NOPE", 32, 1); assert(!sio_setup_fwdata("/arm-io/sio"));
        reset(); record(shims+36, "SSPI", 32, 2); assert(!sio_setup_fwdata("/arm-io/sio"));
        reset(); shimlen--; assert(!sio_setup_fwdata("/arm-io/sio"));
        reset(); shimlen = 0; assert(!sio_setup_fwdata("/arm-io/sio"));
        reset(); missing = true; assert(!sio_setup_fwdata("/arm-io/sio"));
        reset(); fail_alloc = true; assert(!sio_setup_fwdata("/arm-io/sio"));
    }
    chip_id = T6030; reset();
    struct sio_data *d = sio_setup_fwdata("/arm-io/sio"); assert(d);
    table(d, 0x22, 0xd8); table(d, 0x1c, 0x48); free(d);
    chip_id = 0x8103; reset(); d = sio_setup_fwdata("/arm-io/sio"); assert(d);
    table(d, 0x22, 5*32); table(d, 0x1c, 8*8); free(d);
    reset(); struct sio_data edge = {.iova_base = (1ULL << 42) - SZ_16K}; u64 iova;
    assert(alloc_mapped_data(&edge, SZ_16K, &iova));
    assert(!alloc_mapped_data(&edge, SZ_16K, &iova));
    reset(); memset(&edge, 0, sizeof(edge)); edge.iova_base = 1ULL << 42;
    assert(!alloc_mapped_data(&edge, 1, &iova));
    assert(!alloc_mapped_data(&edge, 0, &iova));
    puts("PASS T604x sparse, canonical slots, legacy and refusal tables");
    return 0;
}
'''
    binary = tmp_path / "sio-tables"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                    "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
