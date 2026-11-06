# SPDX-License-Identifier: MIT
"""Explicit, fail-closed CDC transitions for the guarded J813/J873 boot tools."""
import hashlib
import json
from pathlib import Path
import subprocess
import time

from serial.tools import list_ports

from .proxy import IODEV


def require(value, message):
    if not value:
        raise RuntimeError(message)


def identity(session, image_sha256):
    return dict(base=session.base, image_sha256=image_sha256,
                adt_sha256=hashlib.sha256(session.adt_bytes).hexdigest(),
                serial=session.serial, chip=session.p.get_chipid())


def require_upload_transport(args, session, image_sha256):
    if args.transport == "kis":
        require(session.p.iodev_whoami() == IODEV.DOCKCHANNEL_UART, "KIS preparation requires the KIS proxy")
        return
    record = json.loads((args.output / "cdc.json").read_text())
    require(record.get("passed") is True and record.get("device") == args.device,
            "missing or mismatched successful CDC receipt")
    require(record["identity"] == identity(session, image_sha256), "CDC boot identity changed")
    require(session.p.iodev_whoami() == IODEV.USB0, "not the CDC primary")
    require(session.p.cdc_status() & 0xff == 2, "CDC is not ready")


def admit_port(device, serial, *, required=False):
    if not Path(device).exists() and not required:
        return
    port = next((p for p in list_ports.comports() if p.device == device), None)
    require(port is not None and (port.vid, port.pid) == (0x1209, 0x316d),
            "selected port is not an Aura CDC device")
    require(port.serial_number == serial, "CDC USB serial does not match the target")
    owners = subprocess.run(["lsof", "-n", device], capture_output=True, timeout=10)
    require(owners.returncode == 1 and not owners.stdout, "CDC endpoint is already owned")


def start_cdc(args, image_sha256, open_session, guard):
    """The caller holds the target lock and has just passed its physical PMU guard."""
    record = dict(passed=False, device=args.cdc_device, phase="source-identity")
    receipt_path = args.output / "cdc.json"
    require(not receipt_path.exists(), "CDC phase has already been attempted; do not retry")
    try:
        admit_port(args.cdc_device, args.serial)
        source = open_session(args.device)
        try:
            require(guard.get("passed") is True and guard.get("offline_only") is False and
                    guard.get("after_raw") == 0 and guard.get("device") == args.device,
                    "fresh physical PMU guard required")
            require(int(guard["aura_base"], 16) == source.base, "source changed after PMU guard")
            require(source.p.iodev_whoami() == IODEV.DOCKCHANNEL_UART, "CDC transition must start on KIS")
            record["identity"] = identity(source, image_sha256)
            require(record["identity"]["serial"] == args.serial, "wrong source serial")
            record["phase"] = "schedule"
            # No HPM reset, lane-swap flag, implicit retry or fallback.
            require(source.p.cdc_schedule(2000, flags=0) == 0, "CDC schedule refused")
            record["schedule_acknowledged"] = True
        finally:
            source.close()
        record["phase"] = "enumeration"
        deadline = time.monotonic() + 30
        while not Path(args.cdc_device).exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        require(Path(args.cdc_device).exists(), "CDC did not enumerate; stop without reset/retry")
        admit_port(args.cdc_device, args.serial, required=True)
        record["phase"] = "cdc-identity"
        session = open_session(args.cdc_device)
        try:
            require(session.p.iodev_whoami() == IODEV.USB0, "not the CDC primary interface")
            require(identity(session, image_sha256) == record["identity"],
                    "physical target or live image changed across CDC transition")
            require(session.p.cdc_status() & 0xff == 2, "CDC transition failed")
            record["phase"] = "roundtrip"
            data = bytes(range(256)) * 4096
            start = time.monotonic()
            address = session.alloc(data, 16384)  # Full readback is mandatory.
            record.update(roundtrip_bytes=len(data),
                          roundtrip_sha256=hashlib.sha256(data).hexdigest(),
                          roundtrip_seconds=time.monotonic() - start)
            session.p.free(address)
            require(session.p.get_exc_count() == 0, "proxy exception during roundtrip")
            record.update(passed=True, phase="complete", status=session.p.cdc_status())
        finally:
            session.close()
    except Exception as error:
        record["error"] = repr(error)
        raise
    finally:
        with receipt_path.open("x") as stream:
            json.dump(record, stream, indent=2)
            stream.write("\n")
