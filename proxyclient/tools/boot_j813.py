#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guarded single-CPU RAM boot on J813, with optional verified CDC uploads.

Start with a matching public Aura image already running on the KIS proxy.
The legacy resident's first chainload must use its own compatible client.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import struct
import time

import boot_j873 as common
from aura.boot_cdc import start_cdc, require_upload_transport


def session(args, raw, symbols, device=None):
    return common.Session(device or args.device, raw, symbols,
                          target="j813", serial_number=args.serial)


def audit(args, s, raw):
    state = json.loads((args.output / "linux.json").read_text())
    common.require(state["stage"] == "prepared" and state["base"] == s.base and
                   state["image_sha256"] == common.sha(raw), "prepared boot identity changed")
    dt = s.read(state["dt"], state["dt_size"])
    common.require(common.sha(dt) == state["dt_sha256"], "prepared DT changed")
    common.require(not any(s.p.smp_is_alive(i) for i in range(10)), "secondary CPUs are active")
    common.require(b"M5-RTKit-3255.160.4.release\0" in dt, "missing M5 firmware profile")
    common.save(args.output / "prepare-audit.json",
                dict(passed=True, dt_sha256=state["dt_sha256"], base=s.base))
    return state


def prepare(args, s, raw):
    require_upload_transport(args, s, common.sha(raw))
    common.require(args.artifacts and args.artifacts.is_dir(), "prepare requires --artifacts")
    common.require(not any(s.p.smp_is_alive(i) for i in range(10)), "secondary CPUs are active")
    hashes = {}
    for line in (args.artifacts / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split()
        name = name.lstrip("*")
        common.require(Path(name).name == name, "artifact path is not local")
        common.require(common.sha((args.artifacts / name).read_bytes()) == digest,
                       "artifact changed: " + name)
        hashes[name] = digest
    names = ["Image", "initramfs.cpio.gz", "t8142-j813.dtb"]
    common.require(all(name in hashes for name in names), "incomplete kernel artifacts")
    image = (args.artifacts / "Image").read_bytes()
    common.require(image[56:60] == b"ARMd" and 0 < struct.unpack_from("<Q", image, 16)[0] < 64 << 20,
                   "unsupported arm64 Image")
    state = dict(stage="uploading", base=s.base, image_sha256=common.sha(raw),
                 hashes=hashes, transport=args.transport, payloads={})
    common.save(args.output / "linux.json", state)
    for name in names:
        data = (args.artifacts / name).read_bytes()
        reserve = 64 << 20 if name == "Image" else len(data)
        alignment = 2 << 20 if name == "Image" else 65536
        address = s.p.memalign(alignment, reserve)
        common.require(address and s.ba.phys_base <= address and
                       address + reserve <= s.ba.phys_base + s.ba.mem_size,
                       "allocation outside usable RAM")
        common.require(all(address + reserve <= item["address"] or
                           item["address"] + item["reserved"] <= address
                           for item in state["payloads"].values()), "payload allocations overlap")
        start = time.monotonic()
        s.write(address, data)
        state["payloads"][name] = dict(address=address, size=len(data), reserved=reserve,
                                      sha256=hashes[name], readback_verified=True)
        common.save(args.output / "linux.json", state)
        print("RAM_UPLOAD_VERIFIED", name, len(data), hashes[name], time.monotonic() - start, flush=True)
    payloads = state["payloads"]
    p = s.p
    # kboot_prepare_dt reserves the MTE tag store, or appends arm64.nomte.
    bootargs = ("earlycon=dockchannel,0x38812c000 console=tty0 console=ttyDC0 keep_bootcon "
                "maxcpus=1 nokaslr rdinit=/init panic=0")
    # Success is the chosen-table slot index, not necessarily zero.
    common.require(0 <= s.call("kboot_set_chosen", s.cstring("bootargs"), s.cstring(bootargs)) < 16,
                   "bootargs failed")
    common.require(0 <= s.call("kboot_set_chosen", s.cstring("stdout-path"),
                          s.cstring("serial0:1500000n8")) < 16, "stdout-path failed")
    p.kboot_set_initrd(payloads["initramfs.cpio.gz"]["address"], payloads["initramfs.cpio.gz"]["size"])
    common.require(p.kboot_prepare_dt(payloads["t8142-j813.dtb"]["address"]) == 0,
                   "device-tree preparation failed; do not boot")
    dt_addr = p.read64(s.base + s.symbols["dt"])
    magic, total = struct.unpack_from(">II", s.read(dt_addr, 40))
    common.require(magic == 0xd00dfeed and 40 < total < 1024 * 1024, "invalid prepared DT")
    dt = s.read(dt_addr, total)
    common.require(b"M5-RTKit-3255.160.4.release\0" in dt, "missing M5 firmware profile")
    (args.output / "prepared.dtb").write_bytes(dt)
    state.update(stage="prepared", dt=dt_addr, dt_size=total, dt_sha256=common.sha(dt), bootargs=bootargs)
    common.require(p.get_exc_count() == 0, "proxy exception during preparation")
    common.save(args.output / "linux.json", state)
    audit(args, s, raw)


def boot(args, s, raw):
    common.require(s.p.iodev_whoami() == common.IODEV.DOCKCHANNEL_UART,
                   "boot requires restored Aura KIS")
    state = audit(args, s, raw)
    kernel = state["payloads"]["Image"]
    s.p.dc_cvau(kernel["address"], kernel["size"])
    s.p.ic_ivau(kernel["address"], kernel["size"])
    state["stage"] = "entering"
    common.save(args.output / "linux.json", state)
    common.require(s.p.request(s.p.P_KBOOT_BOOT, kernel["address"]) == 0, "kernel entry refused")
    common.capture_consoles(args, s, 300, b"J813_ARTIFACT_BASE64_END")
    print("Console captured; inspect linux-console.bin for the kernel result.", flush=True)


def main():
    common.require(__debug__ and not os.environ.get("PYTHONOPTIMIZE"), "run normal Python without -O")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=["cdc", "prepare", "audit", "boot"])
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--image-sha256", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--device", required=True)
    ap.add_argument("--console", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--artifacts", type=Path)
    ap.add_argument("--transport", choices=["kis", "cdc"], default="kis")
    ap.add_argument("--cdc-device")
    ap.add_argument("--lock-file", type=Path)
    ap.add_argument("--pmu-guard", type=Path, default=Path(__file__).with_name("j813_pmu_guard.py"))
    args = ap.parse_args()
    common.require(args.serial.isalnum(), "invalid serial number")
    common.require(args.device != args.console, "Aura proxy and Linux console must be distinct")
    if args.action == "cdc" and (not args.cdc_device or args.cdc_device in (args.device, args.console)):
        ap.error("cdc requires a separate --cdc-device primary port")
    raw, symbols = common.image_pair(args.image, args.image_sha256, macho=True)
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / (args.action + "-started.json")).open("x") as stream:
        json.dump(dict(image_sha256=common.sha(raw), device=args.device, started=time.time()), stream)
    guard = None if args.action == "audit" else common.guard(args, args.device)
    lock_path = args.lock_file or Path("/tmp/aura-j813-" + args.serial + ".lock")
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.action == "cdc":
            start_cdc(args, common.sha(raw), lambda device: session(args, raw, symbols, device), guard)
        else:
            s = session(args, raw, symbols)
            try:
                if guard:
                    common.require(int(guard["aura_base"], 16) == s.base, "source changed after PMU guard")
                {"prepare": prepare, "audit": audit, "boot": boot}[args.action](args, s, raw)
            finally:
                s.close()
    print("AURA_J813_" + args.action.upper() + "_COMPLETE", flush=True)


if __name__ == "__main__":
    main()
