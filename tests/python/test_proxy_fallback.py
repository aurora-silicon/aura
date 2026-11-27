# SPDX-License-Identifier: GPL-2.0-only
"""proxy_fallback(): an aura-try trial that would stop in the proxy with nobody at the Mac waits a
bounded time for a host and reboots to the configured image; any other stage keeps the proxy.

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
6. A stage that is not a trial waits or reboots (it must stop in the proxy as before), or a trial
   with a host in the window reboots under it.
8. A plain boot that stops in the proxy stays there for good: it must arm the boot watchdog first,
   but never under a host that has already attached, on a debug boot, or over a boot watchdog
   that is already running (Stage 1's).
7. A handoff failure leaves the panel dark (FB_SILENT_MODE), or a CDC transition still scheduled
   runs from the window's usb_cdc_poll() after USB was shut down for the handoff.
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
        call = body.index(f'proxy_fallback("{why}")')
        proxy = body.index("uartproxy_run(NULL);", call)
        between = body[call:proxy]
        assert "flush_and_reboot" not in between and "panic(" not in between, why
        if body is handoff:
            assert "if (" not in between, why
    # run_actions() decides before it brings USB up for the proxy (a port controller reset and a
    # watchdog of the USB path's own must not come before a reboot).
    call = run_actions.index('bool host_attached = proxy_fallback("no payload could be started");')
    usb = run_actions.index("usb_init();", run_actions.index("No valid payload found"))
    assert call < usb
    # A host that attached in the window keeps its carrier: USB is not brought up under it.
    assert re.search(r"if \(!host_attached && ", run_actions[call:usb])
    # The handoff stops light the panel before their first line, as run_actions() does.
    for why in ("NVMe handoff failed", "Display handoff failed"):
        line = handoff.index(f'printf("{why}; returning to proxy')
        assert "fb_set_active(true);" in handoff[handoff.rindex("{", 0, line):line], why
        assert f'proxy_fallback("{why}");' in handoff
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


FALLBACK = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;
#define WDT_TRIAL_SECONDS 178
#define WDT_BOOT_SECONDS 178
static bool trial, host, boot_wdt, arm_fails, host_seen, debug;
static unsigned waited, reboots, trial_arms, boot_arms;
static bool aura_trial_enabled(void) { return trial; }
static bool aura_debug_enabled(void) { return debug; }
static bool wdt_boot_watchdog_armed(void) { return boot_wdt; }
static bool wdt_host_was_attached(void) { return host_seen; }
static int wdt_arm_boot(void)
{
    boot_arms++;
    if (arm_fails)
        return -1;
    boot_wdt = true;
    return 0;
}
static int wdt_arm_trial(void)
{
    trial_arms++;
    boot_wdt = false;
    return arm_fails ? -1 : 0;
}
static bool uartproxy_wait_host(unsigned int ms)
{
    waited = ms;
    return host;
}
__attribute__((noreturn)) static void flush_and_reboot(void)
{
    reboots++;
    printf("ret=reboot waited=%u reboots=%u arms=%u boot=%u\n", waited, reboots, trial_arms,
           boot_arms);
    exit(0);
}
@HEADER@
@FALLBACK@
int main(int argc, char **argv)
{
    trial = atoi(argv[1]);
    host = atoi(argv[2]);
    boot_wdt = atoi(argv[3]);
    arm_fails = atoi(argv[4]);
    host_seen = atoi(argv[5]);
    debug = atoi(argv[6]);
    bool r = proxy_fallback("NVMe handoff failed");
    printf("ret=%d waited=%u reboots=%u arms=%u boot=%u\n", r, waited, reboots, trial_arms,
           boot_arms);
    return 0;
}
'''


def _fallback(tmp_path, trial, host, boot_wdt=False, arm_fails=False, host_seen=False,
              debug=False):
    header = (ROOT / "src/proxy_fallback.h").read_text()
    header = header[header.index("#define PROXY_FALLBACK_HOST_MS"):]
    header = header[:header.index("\n")]
    source = (ROOT / "src/proxy_fallback.c").read_text()
    fn = _body(source, "bool proxy_fallback(const char *why)")
    src = tmp_path / "f.c"
    src.write_text(FALLBACK.replace("@HEADER@", header).replace("@FALLBACK@", fn))
    exe = tmp_path / "f"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-parameter", str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe), str(int(trial)), str(int(host)), str(int(boot_wdt)),
                          str(int(arm_fails)), str(int(host_seen)), str(int(debug))], check=True,
                         capture_output=True, text=True).stdout
    return dict(item.split("=") for item in out.strip().splitlines()[-1].split())


def test_plain_boot_arms_the_boot_watchdog_and_keeps_the_proxy(tmp_path):
    """A Stage 2 disarmed Stage 1's boot watchdog at its start: a stop in the proxy arms it
    again, without waiting or rebooting, so the Mac restarts unless a host attaches."""
    for host in (False, True):
        assert _fallback(tmp_path, False, host) == {
            "ret": "0", "waited": "0", "reboots": "0", "arms": "0", "boot": "1"}


def test_plain_boot_leaves_a_running_boot_watchdog(tmp_path):
    """Stage 1's own boot watchdog still runs: nothing to arm (and no fresh period)."""
    assert _fallback(tmp_path, False, False, boot_wdt=True) == {
        "ret": "0", "waited": "0", "reboots": "0", "arms": "0", "boot": "0"}


def test_plain_boot_never_arms_under_a_host_or_on_a_debug_boot(tmp_path):
    for kw in ({"host_seen": True}, {"debug": True}):
        assert _fallback(tmp_path, False, False, **kw) == {
            "ret": "0", "waited": "0", "reboots": "0", "arms": "0", "boot": "0"}, kw


def test_plain_boot_keeps_the_proxy_when_the_watchdog_cannot_be_armed(tmp_path):
    assert _fallback(tmp_path, False, False, arm_fails=True) == {
        "ret": "0", "waited": "0", "reboots": "0", "arms": "0", "boot": "1"}


def test_a_host_marks_itself_before_the_boot_watchdog_check():
    """wdt_host_attached() records the host even when no boot watchdog runs, so a stage whose
    host synced (a proxy window, a proxy request) never arms one under it later."""
    fn = _body((ROOT / "src/wdt.c").read_text(), "void wdt_host_attached(void)")
    assert fn.index("wdt_host_seen = true;") < fn.index("if (!wdt_boot_armed)")


def test_trial_without_a_host_reboots_after_the_window(tmp_path):
    assert _fallback(tmp_path, True, False) == {"ret": "reboot", "waited": "15000",
                                                 "reboots": "1", "arms": "0", "boot": "0"}


def test_trial_with_a_host_keeps_the_proxy(tmp_path):
    assert _fallback(tmp_path, True, True) == {"ret": "1", "waited": "15000", "reboots": "0",
                                                "arms": "0", "boot": "0"}


def test_stage1_trial_turns_its_boot_watchdog_into_the_trial_watchdog(tmp_path):
    """A Stage 1 that took the trial marker still runs its boot watchdog, which the first host
    turns off. The window must run under the trial watchdog, armed before a host can attach."""
    assert _fallback(tmp_path, True, True, boot_wdt=True) == {
        "ret": "1", "waited": "15000", "reboots": "0", "arms": "1", "boot": "0"}
    assert _fallback(tmp_path, True, False, boot_wdt=True) == {
        "ret": "reboot", "waited": "15000", "reboots": "1", "arms": "1", "boot": "0"}


def test_stage1_trial_without_a_trial_watchdog_reboots_without_a_window(tmp_path):
    assert _fallback(tmp_path, True, True, boot_wdt=True, arm_fails=True) == {
        "ret": "reboot", "waited": "0", "reboots": "1", "arms": "1", "boot": "0"}


def test_the_trial_watchdog_is_armed_before_the_host_window():
    fn = _body((ROOT / "src/proxy_fallback.c").read_text(), "bool proxy_fallback(const char *why)")
    assert fn.index("wdt_arm_trial()") < fn.index("uartproxy_wait_host(")
    # wdt_host_attached() only turns off the boot watchdog, which wdt_arm_trial() clears.
    wdt = (ROOT / "src/wdt.c").read_text()
    assert "wdt_boot_armed = false;" in _body(wdt, "int wdt_arm_trial(void)")
    assert "if (!wdt_boot_armed)\n        return;" in _body(wdt, "void wdt_host_attached(void)")


def test_cdc_cleanup_cancels_a_scheduled_transition():
    cdc = (ROOT / "src/usb_cdc.c").read_text()
    cleanup = _body(cdc, "void usb_cdc_cleanup(void)")
    assert "if (cdc.state == USB_CDC_SCHEDULED)\n        cdc.state = USB_CDC_IDLE;" in cleanup
    # usb_cdc_poll() only runs a transition that is still scheduled.
    poll = _body(cdc, "void usb_cdc_poll(void)")
    assert poll.count("usb_cdc_state_due(&cdc,") == 2


def test_bringup_builds_keep_the_proxy():
    """A BRINGUP build never hands off and stays in the proxy by design: no boot watchdog."""
    fn = _body((ROOT / "src/proxy_fallback.c").read_text(), "bool proxy_fallback(const char *why)")
    plain = fn[:fn.index("\n    }\n")]
    guard = plain.index("#ifndef BRINGUP")
    assert guard < plain.index("wdt_arm_boot()") < plain.index("#endif")


WDT_HANDOVER = r'''
#include <stdbool.h>
#include <stdio.h>
#define WDT_BOOT_SECONDS 178
static int disables, arms;
static int wdt_arm(unsigned s) { (void)s; arms++; return 0; }
static void wdt_disable(void) { disables++; }
@WDT@
int main(void)
{
    /* plain Stage 2 stop: boot watchdog, then usb_cdc's own (T8140), then a KIS host request */
    wdt_arm_boot();
    wdt_arm_seconds(170);
    wdt_host_attached();
    printf("%d %d %d %d\n", arms, disables, wdt_boot_watchdog_armed(), wdt_host_was_attached());
    /* a boot watchdog that nothing replaced is turned off by the first host */
    wdt_arm_boot();
    wdt_host_attached();
    printf("%d %d %d\n", arms, disables, wdt_boot_watchdog_armed());
    return 0;
}
'''


def test_another_watchdog_takes_over_from_the_boot_watchdog(tmp_path):
    """usb_cdc's CDC transition watchdog (and a host's P_WDT_ARM) reprogram the same hardware:
    a later host must not turn that one off as if it were the boot watchdog."""
    wdt = (ROOT / "src/wdt.c").read_text()
    parts = ["static bool wdt_boot_armed;", "static bool wdt_host_seen;"]
    for sig in ("int wdt_arm_seconds(unsigned seconds)", "int wdt_arm_boot(void)",
                "bool wdt_boot_watchdog_armed(void)", "bool wdt_host_was_attached(void)",
                "void wdt_host_attached(void)"):
        parts.append(_body(wdt, sig))
    src = tmp_path / "h.c"
    src.write_text(WDT_HANDOVER.replace("@WDT@", "\n".join(parts)).replace("printf(\"WDT: host attached",
                                                                          "(void)0; //"))
    exe = tmp_path / "h"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    assert out[0] == "2 0 0 1"
    assert out[1] == "3 1 0"
