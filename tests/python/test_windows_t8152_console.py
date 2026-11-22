# SPDX-License-Identifier: GPL-2.0-only
"""G5b C-3: the console never waits on the host link (G5B-BUILD 3.2).

d5c: cpu6 froze 705 s inside the host tick's printout, holding the big lock, while the Mac slept
and the proxy CDC's 1 MiB ring stayed full (the HS engine's queue loop has no deadline).

Failure modes written down before the code:
  N-1 a console write blocks while the device takes nothing (the d5c freeze);
  N-2 bytes the device could not take are lost silently instead of counted, or counted wrong;
  N-3 after the device recovers, the backlog comes out of order, duplicated, or not at all;
  N-4 with nothing dropped, the byte stream is not exactly what was printed;
  N-5 the must-deliver mode (the fatal report, "All CPUs exited") waits without a bound, or does
      not wait at all;
  N-6 the stall statistics (count, longest, current) do not match what happened;
  N-7 the proxy protocol (iodev_write) becomes non-blocking too, or the console's USB write keeps
      a host wait (handle_events, a loop) on the Windows build.
These run src/iodev.c itself on the host against a model CDC device.
"""
import pathlib
import re
import shutil
import subprocess

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "src"

STUBS = {
    "types.h": "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n"
               "#include <sys/types.h>\n"
               "typedef unsigned long u64; typedef uint32_t u32; typedef uint8_t u8;\n"
               "#define BIT(x) (1UL << (x))\n",
    "utils.h": "#pragma once\n#include \"types.h\"\n"
               "typedef struct { int lock; int count; } spinlock_t;\n"
               "#define SPINLOCK_INIT {-1, 0}\n#define DECLARE_SPINLOCK(n) spinlock_t n = SPINLOCK_INIT;\n"
               "static inline void spin_lock(spinlock_t *l) { l->count++; }\n"
               "static inline void spin_unlock(spinlock_t *l) { l->count--; }\n"
               "#define min(a, b) ((a) < (b) ? (a) : (b))\n#define max(a, b) ((a) > (b) ? (a) : (b))\n"
               "extern u64 fake_now;\n"
               "static inline u64 get_ticks(void) { return fake_now; }\n"
               "static inline u64 ticks_to_usecs(u64 t) { return t; }\n"
               "static inline u64 timeout_calculate(u32 us) { return fake_now + us; }\n"
               "static inline bool timeout_expired(u64 t) { return fake_now > t; }\n"
               "static inline bool is_boot_cpu(void) { return true; }\n"
               "#define sysop(op) __asm__ volatile(\"\" ::: \"memory\")\n",
    "memory.h": "#pragma once\nstatic inline bool mmu_active(void) { return true; }\n",
    "string.h": "#pragma once\n#include <string.h>\n",
}

HOST = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iodev.h"

u64 fake_now = 1000;
static const struct iodev_ops none_ops;
struct iodev iodev_uart = {.ops = &none_ops}, iodev_dockchannel_uart = {.ops = &none_ops},
             iodev_fb = {.ops = &none_ops}, iodev_log = {.ops = &none_ops},
             iodev_usb_vuart = {.ops = &none_ops};

/* The model CDC: room bytes free in its ring now; each poll frees poll_room and costs 1 ms. */
static u8 out[1 << 20];
static size_t out_len, room, poll_room;
static long writes, polls, blocking_writes;
static ssize_t dev_write_nb(void *o, const void *b, size_t n)
{
    (void)o;
    writes++;
    size_t take = n < room ? n : room;
    memcpy(out + out_len, b, take);
    out_len += take;
    room -= take;
    return take;
}
static ssize_t dev_write_blocking(void *o, const void *b, size_t n)
{
    (void)o; (void)b;
    blocking_writes++;
    return n;
}
static bool dev_can_write(void *o) { (void)o; return true; }
static void dev_events(void *o) { (void)o; polls++; room += poll_room; fake_now += 1000; }
static const struct iodev_ops dev_ops = {.can_write = dev_can_write, .write = dev_write_blocking,
                                         .write_nonblocking = dev_write_nb,
                                         .handle_events = dev_events};
static struct iodev dev = {.ops = &dev_ops, .usage = USAGE_CONSOLE};

static u8 in[1 << 20];
static size_t in_len;
static void line(size_t i)
{
    char s[64];
    int n = snprintf(s, sizeof(s), "HV: line %06zu of the tick printout ........\n", i);
    memcpy(in + in_len, s, n);
    in_len += n;
    fake_now += 10;
    iodev_console_write(s, n);
}

int main(void)
{
    iodev_register_device(IODEV_USB0, &dev);
    struct iodev_console_stats st;

    /* N-4: room for everything: the stream is exact, no stall, nothing dropped. */
    room = 1 << 20;
    for (size_t i = 0; i < 100; i++)
        line(i);
    assert(out_len == in_len && !memcmp(out, in, in_len));
    iodev_console_stats(IODEV_USB0, &st);
    assert(st.dropped == 0 && st.stalls == 0 && st.stalled_now_us == 0);

    /* N-1: the host stops reading: every write returns at once, nothing reaches the device. */
    room = 0;
    size_t mark_in = in_len, mark_out = out_len;
    long w0 = writes;
    u64 t_stall = fake_now;
    for (size_t i = 100; i < 600; i++)
        line(i);
    assert(out_len == mark_out && polls == 0 && blocking_writes == 0);
    assert(writes - w0 <= 2 * 500);                    /* at most backlog + line, each once */
    iodev_console_stats(IODEV_USB0, &st);
    assert(st.stalls == 1 && st.stalled_now_us > 0);
    size_t printed = in_len - mark_in;
    assert(printed > 8192);

    /* N-2, N-3: the host reads again: the newest 8 KiB come out in order, the rest is counted. */
    room = 1 << 20;
    fake_now += 705000000;                             /* the d5c gap */
    iodev_console_write(NULL, 0);
    iodev_console_stats(IODEV_USB0, &st);
    size_t got = out_len - mark_out;
    assert(got == 8192);
    assert(st.dropped == printed - 8192);
    assert(!memcmp(out + mark_out, in + in_len - 8192, 8192));
    assert(st.stalls == 1 && st.stalled_now_us == 0);
    assert(st.longest_stall_us >= 705000000 && st.longest_stall_us < 705000000 + 100000 + (fake_now - t_stall));

    /* Recovering partially: a trickle; still in order, no duplicate, no new drop. */
    mark_out = out_len;
    mark_in = in_len;
    for (size_t i = 600; i < 700; i++) {
        room = 17;
        line(i);
    }
    room = 1 << 20;
    iodev_console_write(NULL, 0);
    assert(out_len - mark_out == in_len - mark_in && !memcmp(out + mark_out, in + mark_in, in_len - mark_in));
    iodev_console_stats(IODEV_USB0, &st);
    assert(st.dropped == printed - 8192 && st.stalls == 2);

    /* N-5: must-deliver: a device that frees 50 bytes per poll gets the whole report. */
    room = 0;
    poll_room = 50;
    mark_out = out_len;
    mark_in = in_len;
    iodev_console_must_deliver(2000);
    for (size_t i = 700; i < 720; i++)
        line(i);
    iodev_console_must_deliver(0);
    assert(out_len - mark_out == in_len - mark_in && !memcmp(out + mark_out, in + mark_in, in_len - mark_in));
    assert(polls > 0 && blocking_writes == 0);
    /* ... and a device that never frees anything costs at most the deadline (2 s, 1 ms a poll). */
    poll_room = 0;
    room = 0;
    long p0 = polls;
    u64 t0 = fake_now;
    iodev_console_must_deliver(2000);
    line(720);
    line(721);
    iodev_console_must_deliver(0);
    assert(fake_now - t0 <= 2000000 + 2000 && polls - p0 <= 2001);
    /* Off again: no poll at all. */
    p0 = polls;
    line(722);
    assert(polls == p0);
    puts("CONSOLE OK");
    return 0;
}
"""


def test_console_never_waits_and_counts_drops(tmp_path):
    """N-1 .. N-6 on src/iodev.c itself (ASan + UBSan, -Werror)."""
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    for name, text in STUBS.items():
        (tmp_path / name).write_text(text)
    # Copied next to the stubs so its quoted includes resolve to them, not to src/.
    shutil.copy(SRC / "iodev.c", tmp_path / "iodev.c")
    shutil.copy(SRC / "iodev.h", tmp_path / "iodev.h")
    (tmp_path / "host.c").write_text(HOST)
    exe = tmp_path / "console"
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wno-unused-function", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    str(tmp_path / "iodev.c"), str(tmp_path / "host.c"), "-o", str(exe)],
                   check=True, cwd=tmp_path)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert "CONSOLE OK" in out


def test_console_usb_write_has_no_host_wait():
    """N-7: the Windows console write on the proxy CDC takes what fits and returns; the proxy
    protocol keeps the blocking usb_dwc3_write()."""
    inc = (SRC / "usb_dwc3_vuart.inc").read_text()
    fn = inc.split("size_t usb_dwc3_console_write(", 1)[1].split("\n}\n", 1)[0]
    for banned in ("while", "for (", "handle_events", "usb_dwc3_queue", "get_ticks"):
        assert banned not in fn, banned
    assert "ringbuffer_write(buf, take, device2host)" in fn
    usb = (SRC / "usb.c").read_text()
    ops = re.split(r"(?:static )?struct iodev_ops iodev_usb_ops = \{", usb, maxsplit=1)[1]
    ops = ops.split("};", 1)[0]
    assert ".write = usb_0_write," in ops
    assert "#ifdef AURA_WINDOWS\n    .write_nonblocking = usb_0_write_nonblocking,\n#endif" in ops
    wrapper = usb.split("static ssize_t usb_0_write_nonblocking(", 1)[1].split("\n}\n", 1)[0]
    assert "usb_dwc3_console_write(dev, CDC_ACM_PIPE_0, buf, count)" in wrapper
    iodev = (SRC / "iodev.c").read_text()
    write = iodev.split("ssize_t iodev_write(iodev_id_t id", 1)[1].split("\n}\n", 1)[0]
    assert "ops->write(" in write and "nonblocking" not in write


def test_must_deliver_callers_are_bounded():
    """N-5: only the fatal report and the guest-exit line wait, each with 2 s and turned off."""
    win = SRC / "hv/windows"
    callers = {}
    for p in sorted(win.glob("*.c")):
        n = p.read_text().count("iodev_console_must_deliver(")
        if n:
            callers[p.name] = n
    assert callers == {"hv.c": 2, "hv_exc.c": 4}, callers
    for name in callers:
        text = (win / name).read_text()
        assert text.count("iodev_console_must_deliver(2000);") == \
            text.count("iodev_console_must_deliver(0);")
    exc = (win / "hv_exc.c").read_text()
    serr = exc.split("void hv_exc_serr(struct exc_info *ctx)\n{", 1)[1].split("\n}\n", 1)[0]
    assert serr.index("iodev_console_must_deliver(2000);") < \
        serr.index("hv_windows_guest_serror_report(") < \
        serr.index("iodev_console_must_deliver(0);") < serr.index("hv_exc_proxy(")
