# SPDX-License-Identifier: GPL-2.0-only
from types import SimpleNamespace
import struct

import pytest

from proxyclient.aura import cdc_reconnect as cdc


def port(path, vid, pid, interface=None):
    return SimpleNamespace(device=path, vid=vid, pid=pid, interface=interface)


def test_discovery_uses_usb_metadata_then_primary_nop():
    ports = [port("/dev/old-kis", None, None),
             port("/dev/second-acm", cdc.CDC_VID, cdc.CDC_PID, "secondary"),
             port("/dev/primary-acm", cdc.CDC_VID, cdc.CDC_PID, "primary"),
             port("/dev/other", 0x1234, 0x5678)]
    seen = []

    def probe(candidate, identity):
        seen.append(candidate.device)
        return "verified" if candidate.device == "/dev/primary-acm" else None

    result = cdc.discover_cdc(object(), timeout=1, ports=lambda: ports, probe=probe,
                              now=lambda: 0, sleep=lambda _: None)
    assert result == "verified"
    assert seen == ["/dev/primary-acm"]


def test_missing_primary_is_bounded_and_does_not_use_old_pty():
    times = iter([0, 0, 0.5, 1.1])
    with pytest.raises(cdc.CdcDiscoveryTimeout, match="acknowledged KIS"):
        cdc.discover_cdc(object(), timeout=1, ports=lambda: [port("/dev/old-kis", None, None)],
                         now=lambda: next(times), sleep=lambda _: None)


def test_identity_mismatch_is_distinct_from_discovery_timeout():
    candidate = port("/dev/acm", cdc.CDC_VID, cdc.CDC_PID)

    def wrong_identity(candidate, identity):
        raise cdc.CdcIdentityMismatch("wrong ADT")

    with pytest.raises(cdc.CdcIdentityMismatch, match="wrong ADT"):
        cdc.discover_cdc(object(), timeout=1, ports=lambda: [candidate],
                         probe=wrong_identity, now=lambda: 0, sleep=lambda _: None)


def candidate_artifact(folder, image):
    elf = bytearray(0x100 + len(image))
    elf[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", elf, 32, 64)
    struct.pack_into("<HH", elf, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", elf, 64, 1, 4, 0x100, 0, 0,
                     len(image), len(image), 0x100)
    elf[0x100:] = image
    (folder / "aura-raw.elf").write_bytes(elf)
    (folder / "aura.bin").write_bytes(image)


def test_capture_and_verify_full_adt_and_immutable_image(tmp_path):
    tag = b"##aura_ver##candidate-v1"
    image = tag + b"\0" * 17
    adt = b"full ADT bytes with private IDs"
    interface = SimpleNamespace(readmem=lambda addr, size: image[:size])
    utils = SimpleNamespace(iface=interface, base=0x1000, get_adt=lambda: adt)
    candidate_artifact(tmp_path, image)
    identity = cdc.capture_identity(utils, "candidate-v1", [(0, len(image))], tmp_path)
    cdc.verify_identity(utils, identity)
    interface.readmem = lambda addr, size: image[:size] + b"!"
    with pytest.raises(cdc.CdcIdentityMismatch, match="image"):
        cdc.verify_identity(utils, identity)
    interface.readmem = lambda addr, size: image[:size]
    utils.get_adt = lambda: adt + b"!"
    with pytest.raises(cdc.CdcIdentityMismatch, match="ADT"):
        cdc.verify_identity(utils, identity)


def test_legacy_version_marker_is_still_recognised(tmp_path):
    # LEGACY-ASAHI(2027-02-28): the pre-rename marker
    image = b"##m1n1_ver##candidate-v1" + b"\0" * 17
    interface = SimpleNamespace(readmem=lambda addr, size: image[:size])
    utils = SimpleNamespace(iface=interface, base=0x1000, get_adt=lambda: b"adt")
    candidate_artifact(tmp_path, image)
    identity = cdc.capture_identity(utils, "candidate-v1", [(0, len(image))], tmp_path)
    cdc.verify_identity(utils, identity)


def test_matching_tag_with_altered_kis_image_fails_before_schedule(tmp_path):
    image = b"##aura_ver##candidate-v1" + b"stable bytes"
    candidate_artifact(tmp_path, image)
    altered = image[:-1] + b"!"
    utils = SimpleNamespace(
        iface=SimpleNamespace(readmem=lambda addr, size: altered[:size]),
        base=0x1000, get_adt=lambda: b"adt")
    with pytest.raises(cdc.CdcIdentityMismatch, match="candidate artifact"):
        cdc.capture_identity(utils, "candidate-v1", [(0, len(image))], tmp_path)


def test_kis_ack_precedes_close_and_rebootstrap(monkeypatch):
    actions = []
    old_device = SimpleNamespace(close=lambda: actions.append("close-kis"))
    proxy = SimpleNamespace(
        iface=SimpleNamespace(dev=old_device),
        cdc_schedule=lambda delay, flags: actions.append(("schedule", delay, flags)),
    )
    identity = cdc.CdcIdentity("candidate", "a" * 64, (cdc.ImmutableRegion(0, 10, "b" * 64),))
    monkeypatch.setattr(cdc, "capture_identity", lambda utils, tag, ranges, artifact: identity)
    monkeypatch.setattr(cdc, "discover_cdc", lambda identity, **kwargs: actions.append("discover"))
    cdc.transition_to_cdc(proxy, object(), build_tag="candidate",
                          immutable_ranges=[(0, 10)], artifact_dir="/candidate")
    assert actions == [("schedule", 1000, 6), "close-kis", "discover"]


def test_old_kis_timeout_is_not_reported_as_target_failure(monkeypatch):
    old_device = SimpleNamespace(close=lambda: pytest.fail("KIS closed without ACK"))
    proxy = SimpleNamespace(
        iface=SimpleNamespace(dev=old_device),
        cdc_schedule=lambda delay, flags: (_ for _ in ()).throw(cdc.UartTimeout()),
    )
    identity = cdc.CdcIdentity("candidate", "a" * 64, (cdc.ImmutableRegion(0, 10, "b" * 64),))
    monkeypatch.setattr(cdc, "capture_identity", lambda utils, tag, ranges, artifact: identity)
    with pytest.raises(cdc.CdcScheduleUncertain, match="state unknown"):
        cdc.transition_to_cdc(proxy, object(), build_tag="candidate",
                              immutable_ranges=[(0, 10)], artifact_dir="/candidate")


def test_reopen_discards_old_port_and_revalidates_identity(monkeypatch):
    actions = []
    identity = cdc.CdcIdentity("candidate", "a" * 64, (cdc.ImmutableRegion(0, 10, "b" * 64),))
    old = cdc.CdcSession("/dev/old", "primary", SimpleNamespace(
        dev=SimpleNamespace(close=lambda: actions.append("close-old"))),
        object(), object(), identity)
    fresh = cdc.CdcSession("/dev/new", "primary", object(), object(), object(), identity)
    monkeypatch.setattr(cdc, "discover_cdc", lambda identity, **kwargs: fresh)
    result = old.reconnect(log=actions.append)
    assert result is fresh and result.reconnect_count == 1
    assert actions[0] == "close-old"
    assert actions[1]["event"] == "cdc_reconnected"
