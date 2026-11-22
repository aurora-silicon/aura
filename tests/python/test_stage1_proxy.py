# SPDX-License-Identifier: GPL-2.0-only
import ctypes
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class Window(ctypes.Structure):
    _fields_ = [("timeout_ms", ctypes.c_uint32), ("kis_only", ctypes.c_bool)]


def proxy_lib(tmp_path):
    library = tmp_path / "libstage1_proxy.dylib"
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-DSTAGE1_PROXY_HOST_TEST",
         str(ROOT / "src/stage1_proxy.c"), "-o", str(library)],
        check=True,
    )
    lib = ctypes.CDLL(str(library))
    lib.stage1_proxy_window_init.argtypes = [ctypes.POINTER(Window), ctypes.c_uint32]
    lib.stage1_proxy_window_step.argtypes = [ctypes.POINTER(Window), ctypes.c_uint32] + [ctypes.c_bool] * 5
    lib.stage1_proxy_window_step.restype = ctypes.c_int
    lib.stage1_proxy_payload_action.argtypes = [ctypes.c_int] + [ctypes.c_bool] * 3
    lib.stage1_proxy_payload_action.restype = ctypes.c_int
    return lib


def step(lib, window, elapsed, ready=False, failed=False, taken=False,
         cdc_sync=False, kis_sync=False):
    return lib.stage1_proxy_window_step(
        ctypes.byref(window), elapsed, ready, failed, taken, cdc_sync, kis_sync
    )


def test_no_host_expires_to_native(tmp_path):
    lib = proxy_lib(tmp_path)
    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 9_999, ready=True, taken=True) == 0
    assert step(lib, window, 10_000, ready=True, taken=True) == 3
    action = lib.stage1_proxy_payload_action(3, True, True, False)
    assert action == 0


def test_cdc_is_primary_when_both_syncs_are_observed(tmp_path):
    lib = proxy_lib(tmp_path)
    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 1_500, ready=True, taken=True,
                cdc_sync=True, kis_sync=True) == 1


def test_kis_can_claim_only_before_cdc_takes_carrier(tmp_path):
    lib = proxy_lib(tmp_path)
    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 500, kis_sync=True) == 2
    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 1_500, ready=True, taken=True, kis_sync=True) == 0


def test_failed_cdc_start_has_bounded_safe_ownership(tmp_path):
    lib = proxy_lib(tmp_path)
    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 1_000, failed=True, taken=False) == 0
    assert window.kis_only
    assert step(lib, window, 2_000, failed=True, taken=False, kis_sync=True) == 2

    window = Window()
    lib.stage1_proxy_window_init(ctypes.byref(window), 10_000)
    assert step(lib, window, 1_000, failed=True, taken=True, kis_sync=True) == 4
    assert lib.stage1_proxy_payload_action(4, False, False, False) == 3


def test_missing_path_keeps_live_cdc_as_safe_proxy_fallback(tmp_path):
    lib = proxy_lib(tmp_path)
    assert lib.stage1_proxy_payload_action(3, False, True, False) == 1
    assert lib.stage1_proxy_payload_action(3, False, False, True) == 2


def test_watchdog_and_cdc_cleanup_precede_native_handoff():
    main = (ROOT / "src/main.c").read_text()
    cdc = (ROOT / "src/usb_cdc.c").read_text()
    prepare = main.index("Preparing to run next stage")
    nvme = main.index("nvme_shutdown()", prepare)
    cleanup = main.index("usb_cdc_cleanup();", prepare)
    handoff = main.index("exception_shutdown()", cleanup)
    assert prepare < nvme < cleanup < handoff
    cleanup_body = cdc[cdc.index("void usb_cdc_cleanup"):]
    assert "if (watchdog_armed)" in cleanup_body
    assert "wdt_disable();" in cleanup_body


def test_recovery_window_follows_a_boot_that_stopped_before_the_os(tmp_path):
    lib = proxy_lib(tmp_path)
    early = lib.stage1_previous_boot_stopped_early
    early.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    early.restype = ctypes.c_bool
    # A clean record (the OS cleared the counts) opens nothing, whatever stage is left over.
    assert not early(0x00, 0x00)
    assert not early(0x00, 0x2f)
    # Panics alone (high nibble) are not failed boots.
    assert not early(0x10, 0x00)
    # A failed boot that stopped in a loader: iBoot done (0x2f) or nothing recorded (0x00, as
    # the M2 showed after a U-Boot stall on 2026-10-30).
    assert early(0x01, 0x2f)
    assert early(0x01, 0x00)
    assert early(0x13, 0x10)
    # A failed boot that had started the kernel reached the OS.
    assert not early(0x01, 0x30)
    assert not early(0x02, 0x31)
