# SPDX-License-Identifier: MIT
"""Host tests for the byte ring buffer (src/ringbuffer.c).

The USB CDC console (src/usb_dwc3_impl.inc) starts a bulk OUT transfer only
when ringbuffer_get_free() covers a whole transfer, and closes the pipe if the
completed transfer then does not fit. So get_free() must be exactly what a
write accepts. The production file is compiled unchanged against small header
stand-ins and checked against a reference FIFO, including the one-byte-at-a-time
drain that the console's getchar path produces.
"""
from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

STUBS = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
''',
    "malloc.h": "#pragma once\n#include <stdlib.h>\n",
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ringbuffer.h"

static void test_capacity(void)
{
    assert(!ringbuffer_alloc(0));
    assert(!ringbuffer_alloc(1));

    ringbuffer_t *r = ringbuffer_alloc(8);
    assert(r);
    u8 in[16], out[16];
    for (unsigned i = 0; i < sizeof(in); i++)
        in[i] = (u8)(0x40 + i);

    /* empty: nothing to read, len - 1 bytes free */
    assert(ringbuffer_read(out, sizeof(out), r) == 0);
    assert(ringbuffer_get_used(r) == 0 && ringbuffer_get_free(r) == 7);
    /* a write stops one byte short of the read index */
    assert(ringbuffer_write(in, sizeof(in), r) == 7);
    assert(ringbuffer_get_used(r) == 7 && ringbuffer_get_free(r) == 0);
    assert(ringbuffer_write(in, 1, r) == 0);
    assert(ringbuffer_read(out, 3, r) == 3 && !memcmp(out, in, 3));
    assert(ringbuffer_get_free(r) == 3);
    /* across the wrap */
    assert(ringbuffer_write(in + 7, 3, r) == 3);
    assert(ringbuffer_get_free(r) == 0);
    assert(ringbuffer_read(out, sizeof(out), r) == 7 && !memcmp(out, in + 3, 7));
    assert(ringbuffer_get_used(r) == 0 && ringbuffer_get_free(r) == 7);
    ringbuffer_free(r);
    ringbuffer_free(NULL);
}

/* The CDC console reads the host-to-device ring one byte at a time and starts
 * the next OUT transfer once get_free() covers a whole one: the first time it
 * does, the transfer must fit. */
static void test_transfer_after_bytewise_drain(void)
{
    enum { LEN = 64, XFER = 16 };
    ringbuffer_t *r = ringbuffer_alloc(LEN);
    u8 data[LEN] = {0}, c;
    while (ringbuffer_write(data, sizeof(data), r))
        ;
    unsigned drained = 0;
    while (ringbuffer_get_free(r) < XFER) {
        assert(ringbuffer_read(&c, 1, r) == 1);
        drained++;
    }
    assert(drained == XFER);
    assert(ringbuffer_write(data, XFER, r) == XFER);
    assert(ringbuffer_get_free(r) == 0);
    ringbuffer_free(r);
}

/* Seeded random reads and writes against a reference FIFO. */
static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

static void test_random(size_t len)
{
    ringbuffer_t *r = ringbuffer_alloc(len);
    static u8 model[4096], in[4096], out[4096];
    size_t head = 0, count = 0; /* model[head .. head + count) */
    u8 next = 0;
    unsigned writes_full = 0, reads_empty = 0;

    for (int step = 0; step < 100000; step++) {
        size_t used = ringbuffer_get_used(r), free = ringbuffer_get_free(r);
        assert(used == count && used + free == len - 1);
        size_t n = rnd(4) ? rnd(len + 2) : free;
        if (rnd(2)) {
            for (size_t i = 0; i < n; i++)
                in[i] = next++;
            size_t w = ringbuffer_write(in, n, r);
            assert(w == (n < free ? n : free));
            for (size_t i = 0; i < w; i++)
                model[(head + count + i) % sizeof(model)] = in[i];
            next = (u8)(next - (n - w)); /* bytes not taken are offered again */
            count += w;
            writes_full += w < n;
        } else {
            size_t got = ringbuffer_read(out, n, r);
            assert(got == (n < count ? n : count));
            for (size_t i = 0; i < got; i++)
                assert(out[i] == model[(head + i) % sizeof(model)]);
            head = (head + got) % sizeof(model);
            count -= got;
            reads_empty += got < n;
        }
    }
    assert(writes_full > 100 && reads_empty > 100);
    ringbuffer_free(r);
}

int main(void)
{
    test_capacity();
    test_transfer_after_bytewise_drain();
    test_random(2);
    test_random(7);
    test_random(64);
    test_random(1000);
    puts("ringbuffer: ok");
    return 0;
}
'''


def build(tmp_path, source=None):
    inc = tmp_path / "inc"
    inc.mkdir(exist_ok=True)
    for name, text in STUBS.items():
        (inc / name).write_text(text)
    (inc / "ringbuffer.h").write_text((ROOT / "src/ringbuffer.h").read_text())
    (tmp_path / "ringbuffer.c").write_text(source or (ROOT / "src/ringbuffer.c").read_text())
    (tmp_path / "harness.c").write_text(HARNESS)
    binary = tmp_path / "ringbuffer-host"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(inc),
         str(tmp_path / "ringbuffer.c"), str(tmp_path / "harness.c"), "-o", str(binary)],
        check=True,
    )
    return binary


def test_ringbuffer(tmp_path):
    result = subprocess.run([str(build(tmp_path))], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ringbuffer: ok" in result.stdout
