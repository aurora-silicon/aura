# SPDX-License-Identifier: GPL-2.0-only
"""A low-window SIO gets the earlier tables plus a protocol-10 list; a failed protocol-10 list
leaves the earlier tables as they were."""

from pathlib import Path
import re
import subprocess

import pytest


def build(tmp_path, fail_at=0):
    repo = Path(__file__).resolve().parents[2]
    header = (repo / "src/sio.h").read_text()
    header = re.sub(r'#include "[^"]*"\n', "", header)
    source = (repo / "src/sio.c").read_text()
    body = source[source.index("// Reuse pages"):]
    # Fault injection: the fail_at'th allocation fails (0: none).
    check = "if (siodata->num_fwdata >= MAX_FWDATA)"
    assert check in body
    body = body.replace(check, "if (siodata->num_fwdata >= MAX_FWDATA || ++allocs == FAIL_AT)")
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
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SZ_4K (1 << 12)
#define SZ_16K (1 << 14)
#define SZ_1M (1 << 20)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define T6030 0x6030
#define memset64 memset
#define memcpy8 memcpy
static int chip_id = 0x8112, allocs;
static void *adt;
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; } PACKED;
static u64 top_of_memory_alloc(size_t size) { return (u64)(uintptr_t)calloc(1, ALIGN_UP(size, SZ_16K)); }
static int adt_path_offset(void *a, const char *p) { (void)a; return strcmp(p, "/arm-io/sio") ? -1 : 1; }
#define K(s) (u32)((s)[0] << 24 | (s)[1] << 16 | (s)[2] << 8 | (s)[3])
static u8 tunables[0x30], maprange[4 * 20], dmashim[5 * 36], devtype[8 * 12];
static struct adt_segment_ranges seg = {0, 0, 0x200000, 0x10000, 0};
static void fill(u8 *p, const char *const *keys, int n, int blob)
{
    for (int i = 0; i < n; i++) {
        u32 k = K(keys[i]);
        memcpy(p + i * (blob + 4), &k, 4);
        memset(p + i * (blob + 4) + 4, 0x10 + i, blob);
    }
}
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a; (void)node;
#define P(n, v) if (!strcmp(name, n)) { *len = sizeof(v); return &(v); }
    P("segment-ranges", seg) P("asio-ascwrap-tunables", tunables) P("map-range", maprange)
    P("dmashim", dmashim) P("device-type", devtype)
    return NULL;
}
''' + header + body + r'''
static u32 value(struct sio_fwparam *p, int n, u32 key)
{
    for (int i = 0; i < n; i++)
        if (p[i].key == key)
            return p[i].value;
    return 0xdeadbeef;
}
int main(int argc, char **argv)
{
    static const char *const mr[] = {"\xff\xff\xff\xff", "\xff\xff\xff\xff", "\xff\xff\xff\xff", "MISC"};
    static const char *const ds[] = {"SSPI", "SUAR", "SAUD", "ADMA", "AAUD"};
    static const char *const dt[] = {"dSPI", "dUAR", "dMCA", "dDPA", "dPDM", "dALE", "dAMC", "dAPD"};
    fill(maprange, mr, 4, 16);
    fill(dmashim, ds, 5, 32);
    fill(devtype, dt, 8, 8);
    struct sio_data *d = sio_setup_fwdata("/arm-io/sio");
    assert(d);
    /* The earlier list is the same either way. */
    assert(d->num_fwparams == 14);
    assert(value(d->fwparams, d->num_fwparams, 0x23) == 5 * 32);
    assert(value(d->fwparams, d->num_fwparams, 0x1d) == 8 * 8);
    if (argv[1][0] == 'f') {
        /* The protocol-10 list did not fit: undone, the mappings as the earlier list left them. */
        assert(d->num_fwparams_v10 == 0);
        assert(d->num_fwdata == 4);
        for (int i = d->num_fwdata; i < MAX_FWDATA; i++)
            assert(!d->fwdata[i].size && !d->fwdata[i].phys);
        for (int i = 0; i < d->num_fwdata; i++)
            assert(d->fwdata[i].size == ALIGN_UP(d->fwdata[i].size, SZ_16K));
        return 0;
    }
    assert(d->num_fwdata == 5);
    assert(d->num_fwparams_v10 == 14);
    assert(value(d->fwparams_v10, 14, 0x23) == 6 * 36);
    assert(value(d->fwparams_v10, 14, 0x1d) == 9 * 8);
    assert(value(d->fwparams_v10, 14, 0x22) != value(d->fwparams, 14, 0x22));
    for (u32 k = 0x0b; k <= 0x30e; k++)
        if (k != 0x1c && k != 0x1d && k != 0x22 && k != 0x23)
            assert(value(d->fwparams_v10, 14, k) == value(d->fwparams, 14, k));
    /* The shim records end in the all-ones selector, as T6030's do. */
    for (int i = 0; i < d->num_fwdata; i++) {
        struct sio_mapping *m = &d->fwdata[i];
        u64 iova = (u64)value(d->fwparams_v10, 14, 0x22) << 12;
        if (iova >= m->iova && iova < m->iova + m->size) {
            u8 *p = (u8 *)(uintptr_t)(m->phys + iova - m->iova);
            for (int r = 0; r < 6; r++)
                assert(*(u32 *)(p + r * 36 + 32) == 0xffffffffu);
        }
    }
    (void)argc;
    return 0;
}
'''
    binary = tmp_path / f"sio-v10-{fail_at}"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    f"-DFAIL_AT={fail_at}", "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    return binary


def test_sio_v10_list(tmp_path):
    subprocess.run([str(build(tmp_path)), "ok"], check=True)


# The earlier tables take seven allocations in four mappings. The protocol-10 shim records
# (allocation 8) merge into the last mapping, resizing it; its device types (9) take a new one.
@pytest.mark.parametrize("fail_at", [8, 9])
def test_sio_v10_list_rollback(tmp_path, fail_at):
    subprocess.run([str(build(tmp_path, fail_at)), "fail"], check=True)
