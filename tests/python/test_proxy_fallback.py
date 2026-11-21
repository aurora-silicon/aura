# SPDX-License-Identifier: GPL-2.0-only
"""proxy_fallback(): a stage that would stop in the proxy with nobody at the Mac waits a bounded
time for a host and reboots, on an aura-try trial or while the chain's failed-boot count allows it
(the decision cases and the multi-boot chains are in test_nvme_start_reboot.py).

Ways it can fail, and the check for each:
1. A host-less proxy stop in main.c skips it (the NVMe and display handoff failures did, so a
   trial that failed there stopped in the proxy): every one calls it right before the proxy.
2. A second, older trial path stays next to it: run_actions() has no trial check of its own.
   It decides before run_actions() brings USB up for the proxy, which may reset a port controller
   and arm a watchdog of its own.
3. The host window never ends, or ends before a host that synced: uartproxy_wait_host() returns
   at the deadline without a host, and early with the iodev presynced when a host syncs.
4. The window takes the DockChannel from a CDC carrier, or a byte stream without the sync word
   counts as a host.
5. The window outlives the trial watchdog, which would then reset the Mac with no log line.
"""
import re
import subprocess
from pathlib import Path

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def _body(source, signature):
    start = source.index(signature)
    depth = 0
    for i in range(source.index("{", start), len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(signature)


def test_every_host_less_proxy_stop_falls_back():
    main = (ROOT / "src/main.c").read_text()
    run_actions = _body(main, "void run_actions(void)")
    aura_main = _body(main, "void aura_main(void)")
    handoff = aura_main[aura_main.index("next_stage:"):]
    stops = {
        "no payload could be started": run_actions,
        "NVMe handoff failed": handoff,
        "Display handoff failed": handoff,
    }
    for why, body in stops.items():
        call = body.index(f'proxy_fallback("{why}");')
        proxy = body.index("uartproxy_run(NULL);", call)
        between = body[call:proxy]
        assert "flush_and_reboot" not in between and "panic(" not in between, why
        if body is handoff:
            assert "if (" not in between, why
    # run_actions() decides before it brings USB up for the proxy (a port controller reset and a
    # watchdog of the USB path's own must not come before a reboot).
    call = run_actions.index('proxy_fallback("no payload could be started");')
    assert call < run_actions.index("usb_init();", run_actions.index("No valid payload found"))
    # The last proxy entry of run_actions() and both handoff ones, and no other host-less one.
    assert len(re.findall(r"\buartproxy_run\(NULL\);", handoff)) == 2
    tail = run_actions[run_actions.index("No valid payload found"):]
    assert len(re.findall(r"\buartproxy_run\(NULL\);", tail)) == 1


def test_no_second_trial_path():
    main = (ROOT / "src/main.c").read_text()
    run_actions = _body(main, "void run_actions(void)")
    assert "aura_trial_enabled()" not in run_actions
    fallback = (ROOT / "src/proxy_fallback.c").read_text()
    assert "aura_trial_enabled()" in fallback


def test_window_is_shorter_than_the_trial_watchdog():
    header = (ROOT / "src/proxy_fallback.h").read_text()
    wdt = (ROOT / "src/wdt.h").read_text()
    window_ms = int(re.search(r"#define PROXY_FALLBACK_HOST_MS (\d+)", header).group(1))
    trial_s = int(re.search(r"#define WDT_TRIAL_SECONDS (\d+)", wdt).group(1))
    assert 0 < window_ms < trial_s * 1000 // 2


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int iodev_id_t;
#define IODEV_UART 0
#define IODEV_DOCKCHANNEL_UART 1
#define IODEV_USB0 2
#define IODEV_MAX 4
#define USAGE_UARTPROXY 1
static u64 now_ms;
static u64 get_ticks(void) { return now_ms; }
static u64 ticks_to_msecs(u64 t) { return t; }
static void mdelay(int ms) { now_ms += ms; }
static int cdc_owns, polls, attached;
static void usb_cdc_poll(void) { polls++; }
static bool usb_cdc_owns_carrier(void) { return cdc_owns; }
static void wdt_host_attached(void) { attached++; }
/* Per iodev: the bytes a host sends, from the given ms on. */
static const u8 *stream[IODEV_MAX];
static size_t stream_len[IODEV_MAX], stream_pos[IODEV_MAX];
static u64 stream_at[IODEV_MAX];
static int iodev_get_usage(iodev_id_t i) { (void)i; return USAGE_UARTPROXY; }
static void iodev_handle_events(iodev_id_t i) { (void)i; }
static int iodev_can_read(iodev_id_t i)
{
    return stream[i] && now_ms >= stream_at[i] && stream_pos[i] < stream_len[i];
}
static int iodev_read(iodev_id_t i, u8 *b, int n)
{
    (void)n;
    *b = stream[i][stream_pos[i]++];
    return 1;
}
static u32 iodev_proxy_buffer[IODEV_MAX];
static iodev_id_t presynced_iodev = IODEV_MAX;
@SYNC@
@WAIT@
static const u8 sync[] = {0x00, 0xff, 0x55, 0xaa, 0x01};
static const u8 noise[] = {0x12, 0x34, 0xff, 0x55, 0x12, 0xaa, 0x55};
int main(int argc, char **argv)
{
    const char *c = argv[1];
    iodev_id_t on = IODEV_USB0;
    if (!strcmp(c, "dockchannel") || !strcmp(c, "dockchannel-cdc"))
        on = IODEV_DOCKCHANNEL_UART;
    cdc_owns = !strcmp(c, "dockchannel-cdc");
    if (strcmp(c, "none")) {
        stream[on] = !strcmp(c, "noise") ? noise : sync;
        stream_len[on] = !strcmp(c, "noise") ? sizeof(noise) : sizeof(sync);
        stream_at[on] = !strcmp(c, "late") ? 20000 : 3000;
    }
    bool host = uartproxy_wait_host(15000);
    printf("%d %d %llu %d\n", host, host ? presynced_iodev : -1, (unsigned long long)now_ms,
           attached);
    return 0;
}
'''


def _wait_host(tmp_path, case):
    source = (ROOT / "src/uartproxy.c").read_text()
    sync_fn = _body(source, "static bool uartproxy_sync_iodev(iodev_id_t iodev)")
    wait_fn = _body(source, "bool uartproxy_wait_host(unsigned int timeout_ms)")
    src = tmp_path / "w.c"
    src.write_text(HARNESS.replace("@SYNC@", sync_fn).replace("@WAIT@", "static " + wait_fn))
    exe = tmp_path / "w"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe), case], check=True, capture_output=True, text=True).stdout
    host, iodev, ms, attached = (int(x) for x in out.split())
    return host, iodev, ms, attached


def test_no_host_ends_at_the_deadline(tmp_path):
    host, _, ms, attached = _wait_host(tmp_path, "none")
    assert (host, attached) == (0, 0) and 15000 <= ms <= 15001


def test_host_on_usb_ends_early_presynced(tmp_path):
    assert _wait_host(tmp_path, "usb") == (1, 2, 3000, 1)


def test_host_on_dockchannel(tmp_path):
    assert _wait_host(tmp_path, "dockchannel") == (1, 1, 3000, 1)


def test_dockchannel_skipped_while_cdc_owns_the_carrier(tmp_path):
    host, _, ms, _ = _wait_host(tmp_path, "dockchannel-cdc")
    assert host == 0 and ms >= 15000


def test_bytes_without_the_sync_word_are_no_host(tmp_path):
    host, _, ms, _ = _wait_host(tmp_path, "noise")
    assert host == 0 and ms >= 15000


def test_host_after_the_deadline_is_too_late(tmp_path):
    host, _, ms, _ = _wait_host(tmp_path, "late")
    assert host == 0 and ms < 20000
