# SPDX-License-Identifier: MIT
"""Run the production DART proxy cases with the real wire structs and opcodes."""
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_dart_failures_are_protocol_errors(tmp_path):
    source = (ROOT / "src/proxy.c").read_text()
    cases = source[source.index("        case P_DART_INIT:"):
                   source.index("        case P_HV_INIT:")]
    # proxy.h without its includes: the host libc provides the base types.
    header = "\n".join(line for line in (ROOT / "src/proxy.h").read_text().splitlines()
                       if not line.startswith("#include"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64;
typedef long s64;
''' + header + r'''
typedef struct { bool failed; } dart_dev_t;
static dart_dev_t domain;
static bool null_init, shutdown_ok, unmap_ok;
static int map_result;
static unsigned calls;
static dart_dev_t *dart_init(u64 base, u8 sid, bool keep, unsigned type)
{
    assert(base == 0x382f00000 && sid == 1 && keep && type == 1);
    calls++;
    return null_init ? NULL : &domain;
}
static bool dart_has_failed(dart_dev_t *d) { return !d || d->failed; }
static bool dart_shutdown_checked(dart_dev_t *d)
{
    assert(d == &domain);
    calls++;
    return shutdown_ok;
}
static int dart_map(dart_dev_t *d, u64 iova, void *buffer, u64 size)
{
    assert(d == &domain && iova == 0x4000 && buffer == (void *)0x8000 && size == 0x4000);
    calls++;
    return map_result;
}
static bool dart_unmap_checked(dart_dev_t *d, u64 iova, u64 size)
{
    assert(d == &domain && iova == 0x4000 && size == 0x4000);
    calls++;
    return unmap_ok;
}
static void dispatch(ProxyRequest *request, ProxyReply *reply)
{
    memset(reply, 0, sizeof(*reply));
    reply->opcode = request->opcode;
    switch (request->opcode) {
''' + cases + r'''
    default:
        assert(0);
    }
}
int main(void)
{
    ProxyReply reply;
    ProxyRequest request = {.opcode = P_DART_INIT, .args = {0x382f00000, 1, 1, 1}};
    dispatch(&request, &reply);
    assert(reply.status == S_OK && reply.retval == (u64)&domain && calls == 1);
    domain.failed = true;
    dispatch(&request, &reply);
    assert(reply.status == S_ERROR && reply.retval == (u64)&domain);
    null_init = true;
    dispatch(&request, &reply);
    assert(reply.status == S_ERROR && reply.retval == 0);
    request = (ProxyRequest){.opcode = P_DART_MAP,
                            .args = {(u64)&domain, 0x4000, 0x8000, 0x4000}};
    const int results[] = {0, -1, -4, 1};
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        map_result = results[i];
        unsigned before = calls;
        dispatch(&request, &reply);
        assert(calls == before + 1 && reply.retval == (u64)(s64)map_result);
        assert(reply.status == (map_result ? S_ERROR : S_OK));
    }
    request = (ProxyRequest){.opcode = P_DART_UNMAP,
                            .args = {(u64)&domain, 0x4000, 0x4000}};
    for (unsigned good = 0; good < 2; good++) {
        unmap_ok = good;
        unsigned before = calls;
        dispatch(&request, &reply);
        assert(calls == before + 1 && reply.retval == 0);
        assert(reply.status == (good ? S_OK : S_ERROR));
        shutdown_ok = good;
        request.opcode = P_DART_SHUTDOWN;
        dispatch(&request, &reply);
        assert(calls == before + 2 && reply.retval == 0);
        assert(reply.status == (good ? S_OK : S_ERROR));
        request.opcode = P_DART_UNMAP;
    }
    return 0;
}
'''
    binary = tmp_path / "proxy-dart-status"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src"), "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
