#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build by default; explicitly run the J813 N1 preparation/census microguest."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
RESULT_OFFSET = RESULT_SIZE = 0x1000
READ_ACK = 0x4e3141434b31
READS = (
    ("port_link", 0x488028208), ("root_id", 0x1cb0000000),
    ("control_id", 0x1cb0100000), ("control_command", 0x1cb0100004),
    ("bar_low", 0x1cb0100010), ("bar_high", 0x1cb0100014),
    ("radio1_id", 0x1cb0101000), ("radio1_command", 0x1cb0101004),
    ("radio2_id", 0x1cb0102000), ("radio2_command", 0x1cb0102004),
    ("rom_stage", 0xb80008000), ("dart_p1", 0x488000000),
    ("dart_p3", 0x488000008), ("dart_p4", 0x48800000c),
    ("dart_protect", 0x488000200), ("dart_tcr0", 0x488001000),
    ("dart_ttbr0", 0x488001400),
)
REQUESTS = tuple((name, address, 1, 0, 0) for name, address in READS) + (
    ("wrong_version", 0x488028208, 2, 0, -2),
    ("unaligned_address", 0x488028209, 1, 0, -2),
    ("forbidden_address", 0x488000080, 1, 0, -2),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tool(name, formula):
    brew = Path("/opt/homebrew/opt") / formula / "bin" / name
    return str(brew) if brew.is_file() else shutil.which(name) or name


def build_guest(args, output, report):
    for source in (ROOT / "tests/e2e/windows_n1_smoke.S", Path(__file__),
                   ROOT / "docs/windows-n1-smoke.md"):
        shutil.copy2(source, output / source.name)
    source = output / "windows_n1_smoke.S"
    report["build_commands"] = []

    def run(argv, log):
        command = [str(arg) for arg in argv]
        report["build_commands"].append(command)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False, timeout=120)
        (output / log).write_text(result.stdout)
        require(result.returncode == 0, "build command failed; see " + str(output / log))
        return result.stdout

    for name in ("clang", "ld", "objcopy", "objdump"):
        run([getattr(args, name), "--version"], name + "-version.txt")
    for suffix in ("", "-repeat"):
        obj, elf, raw = (output / (stem + suffix + extension) for stem, extension in
                         (("guest", ".o"), ("guest", ".elf"), ("guest-template", ".bin")))
        run([args.clang, "--target=aarch64-none-elf", "-march=armv8-a", "-c",
             source, "-o", obj], "compile" + suffix + ".log")
        run([args.ld, "-Ttext=0", "--image-base=0", "--entry=_start", "--build-id=none",
             obj, "-o", elf], "link" + suffix + ".log")
        run([args.objcopy, "-O", "binary", "-j", ".text", elf, raw],
            "objcopy" + suffix + ".log")
    run([args.objdump, "-d", output / "guest.elf"], "guest.disassembly.txt")
    symbols = run([args.objdump, "-t", output / "guest.elf"], "guest.symbols.txt")

    def symbol(name):
        matches = [int(line.split()[0], 16) for line in symbols.splitlines()
                   if line.split() and line.split()[-1] == name]
        require(len(matches) == 1, "missing or ambiguous guest symbol: " + name)
        return matches[0]

    require(symbol("_start") == 0 and symbol("n1_smoke_results") == RESULT_OFFSET,
            "guest entry or result location changed")
    request_start, request_end = symbol("n1_smoke_requests"), symbol("n1_smoke_requests_end")
    require(0 < request_start < request_end <= RESULT_OFFSET, "request table is outside guest code")
    elf = (output / "guest.elf").read_bytes()
    require(elf[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", elf, 18)[0] == 183
            and struct.unpack_from("<Q", elf, 24)[0] == 0,
            "guest must be little-endian AArch64 ELF with entry zero")
    template = (output / "guest-template.bin").read_bytes()
    require(template == (output / "guest-template-repeat.bin").read_bytes(),
            "guest template did not reproduce")
    require(len(template) == RESULT_OFFSET + RESULT_SIZE and
            template[RESULT_OFFSET:] == bytes(RESULT_SIZE), "invalid guest result reservation")
    requests = b"".join(struct.pack("<3Q", address, version, reserved)
                        for _, address, version, reserved, _ in REQUESTS)
    require(template[request_start:request_end] == requests, "guest and host requests disagree")
    nonce = int.from_bytes(os.urandom(8), "little") or 1
    image = bytearray(template)
    struct.pack_into("<Q", image, RESULT_OFFSET + 32, nonce)
    (output / "guest.bin").write_bytes(image)
    report.update(nonce=hex(nonce), template_sha256=hashlib.sha256(template).hexdigest())
    profile = {"schema": "aura.windows.boot.v1", "name": "J813 N1 census microguest",
               "target": {"chip_id": args.chip_id, "model": args.model},
               "mu": {"path": "guest.bin", "sha256": digest(output / "guest.bin"),
                      "entry_offset": 0}, "framebuffer": "native",
               "guest_compatibility": "none", "usb": "disabled", "n1": "j813-v1"}
    (output / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    return nonce


def check_census(values):
    require(values["port_link"] != 0xffffffff and values["port_link"] & 1,
            "N1 link is not up")
    require(values["root_id"] == 0x100c106b, "N1 root identity mismatch")
    require(values["control_id"] in (0x1900106b, 0x1901106b), "N1 Control identity mismatch")
    require(values["control_command"] & 6 == 2, "Control decode/BME state mismatch")
    for index in (1, 2):
        device = values[f"radio{index}_id"]
        if device == 0xffffffff:
            continue
        require(device == ((0x1901 + index) << 16 | 0x106b), "radio identity mismatch")
        require(not values[f"radio{index}_command"] & 4, "radio bus mastering is enabled")
    require(values["bar_low"] in (0x80000000, 0x80000004), "Control BAR0 mismatch")
    require(values["bar_low"] == 0x80000000 or values["bar_high"] == 0,
            "Control 64-bit BAR0 high word is nonzero")
    require(values["rom_stage"] == 0, "N1 ROM is not cold")
    p1, p3, p4 = (values[key] for key in ("dart_p1", "dart_p3", "dart_p4"))
    require((p1 >> 24) & 15 == 14 and (p3 >> 24) & 63 == 42 and
            36 <= (p3 >> 16) & 63 <= 42 and 0x100 <= p3 & 0xffff < 0x300 and
            1 <= p4 & 511 <= 256, "DART geometry mismatch")
    require(not values["dart_protect"] & 1 and values["dart_tcr0"] == 0 and
            values["dart_ttbr0"] == 0, "DART SID0 is not disabled, unlocked and empty")


def check_results(raw, nonce, bootargs):
    require(len(raw) == RESULT_SIZE, "short guest result page")
    header = struct.unpack_from("<16Q", raw)
    require(raw[:8] == b"AN1SMK01" and header[1] == 1, "guest result magic/ABI mismatch")
    require(header[2] == 3 and header[3] == len(REQUESTS), "guest did not complete all requests")
    require(header[4] == header[5] == nonce, "stale or corrupted guest nonce")
    require(header[6] == 4 and header[7] == bootargs, "guest EL or entry argument mismatch")
    require(raw[64:72] == b"DONEN1HV" and raw[72:128] == bytes(56),
            "completion marker or header guards corrupted")
    observations, values = [], {}
    for index, (name, address, version, reserved, expected) in enumerate(REQUESTS):
        record = struct.unpack_from("<8Q", raw, 128 + index * 64)
        require(record[:3] == (address, version, reserved), name + ": request mismatch")
        require(record[3] == READ_ACK and record[6] == 1, name + ": wrong ACK/response ABI")
        status = record[4] if record[4] < 1 << 63 else record[4] - (1 << 64)
        require(status == expected, name + f": expected status {expected}, got {status}")
        require(record[5] <= 0xffffffff and (status == 0 or record[5] == 0),
                name + ": invalid value width or nonzero error data")
        require(record[7] == index + 1, name + ": incomplete record")
        if status == 0:
            values[name] = record[5]
        observations.append({"name": name, "address": hex(address), "status": status,
                             "value": hex(record[5]), "ack": hex(record[3]), "version": record[6]})
    tail = 128 + len(REQUESTS) * 64
    require(raw[tail:] == bytes(RESULT_SIZE - tail), "result page guards overwritten")
    check_census(values)
    return observations


def execute(args, output, plan, nonce, report):
    from aura.windows import HardwareSession, validate_capabilities, validate_n1_descriptor
    session = None
    saved = {sig: signal.getsignal(sig) for sig in (signal.SIGALRM, signal.SIGINT, signal.SIGQUIT)}

    def timeout(signum, frame):
        raise TimeoutError("hardware deadline exceeded; guest/controller state may be unknown")

    def checkpoint():
        (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")

    signal.signal(signal.SIGALRM, timeout)
    signal.setitimer(signal.ITIMER_REAL, args.timeout)
    try:
        with (output / "console.log").open("w") as console, (output / "host.log").open("w") as host:
            with contextlib.redirect_stdout(host), contextlib.redirect_stderr(host):
                report["hardware_accessed"] = True
                session = HardwareSession(args.device, console)
                caps = session.capabilities()
                validate_capabilities(caps, plan.framebuffer, plan.guest_compatibility, plan.n1)
                identity = session.identity()
                report.update(capabilities=hex(caps), identity=identity)
                require(identity["chip_id"] == plan.chip_id and identity["model"] == plan.model,
                        "target identity does not match explicit chip/model")
                report.update(target_mutated=True, n1_state="unknown-after-prepare-request")
                checkpoint()
                session.prepare(plan)
                handoff = session.hv.windows_n1_handoff
                require(isinstance(handoff, dict), "no N1 preparation receipt")
                descriptor = bytes.fromhex(handoff["descriptor_hex"])
                (output / "n1-descriptor.bin").write_bytes(descriptor)
                report["n1_descriptor"] = validate_n1_descriptor(descriptor)
                report["n1_state"] = "prepared-no-firmware-or-DMA"
                loaded = session.load(plan)
                report["loaded"] = loaded
                require(loaded["n1"] == handoff and loaded["usb"] == "disabled",
                        "loaded handoff/policy differs from preparation")
                address = int(loaded["guest_base"], 0) + RESULT_OFFSET
                before = session.iface.readmem(address, RESULT_SIZE)
                (output / "result-before.bin").write_bytes(before)
                require(before == plan.image[RESULT_OFFSET:RESULT_OFFSET + RESULT_SIZE],
                        "uploaded result page differs from immutable image")
                report.update(result="guest-running", guest_state="unknown-until-verified-return")
                checkpoint()
                status = session.start()
                report["host_return_status"] = status
                require(status == 1, "expected SYSTEM_OFF return status 1 after guest drain")
                report["guest_state"] = "stopped"
                session.iface.dev.timeout = min(args.timeout, 5.0)
                raw = session.iface.readmem(address, RESULT_SIZE)
                (output / "result-after.bin").write_bytes(raw)
                report["observations"] = check_results(raw, nonce, int(loaded["bootargs"], 0))
                require(session.capabilities() == caps, "resident proxy did not remain responsive")
                report.update(proxy_responsive=True, result="passed")
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for sig, handler in saved.items():
            signal.signal(sig, handler)
        if session is not None:
            session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true", help="explicitly forbid device access")
    parser.add_argument("--device", help="explicit serial/USB proxy device; enables hardware execution")
    parser.add_argument("--chip-id", type=lambda value: int(value, 0), help="required for hardware")
    parser.add_argument("--model", help="required for hardware")
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--timeout", type=float, default=60.0, help="hardware deadline in seconds")
    for name, program, formula in (("clang", "clang", "llvm"), ("ld", "ld.lld", "lld"),
                                   ("objcopy", "llvm-objcopy", "llvm"),
                                   ("objdump", "llvm-objdump", "llvm")):
        parser.add_argument("--" + name, default=tool(program, formula))
    args = parser.parse_args()
    if args.build_only and args.device:
        parser.error("--build-only must not specify a device")
    if args.device and (args.chip_id is None or args.model is None):
        parser.error("hardware execution requires explicit --chip-id and --model")
    if args.chip_id not in (None, 0x8142) or args.model not in (None, "Mac17,3"):
        parser.error("the N1 v1 contract requires chip 0x8142 and model Mac17,3")
    if not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300 seconds")
    args.chip_id, args.model = 0x8142, "Mac17,3"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": "aura.windows.n1-smoke.v1", "result": "preflight",
              "hardware_accessed": False, "target_mutated": False, "hardware_qualified": False,
              "windows_ready": False, "device": args.device, "guest_state": "not-started",
              "n1_state": "not-attempted", "command": [sys.executable, *sys.argv],
              "started_unix": time.time()}
    code = 0
    try:
        nonce = build_guest(args, output, report)
        sys.path.insert(0, str(ROOT / "proxyclient"))
        from aura.windows import plan_profile, source_manifest
        plan = plan_profile(output / "profile.json")
        report.update(profile_sha256=plan.profile_sha256, host_sources=source_manifest())
        sources = ("src/hv/windows/windows_n1.c", "src/hv/windows/windows_n1.h",
                   "src/hv/windows/hv.c", "src/hv/windows/hv_exc.c", "src/hv/windows/hv_psci.c")
        report["local_backend_sources"] = {name: digest(ROOT / name) for name in sources}
        report["local_backend_sources_note"] = "Local hashes do not attest the resident binary."
        if args.device:
            execute(args, output, plan, nonce, report)
        else:
            report["result"] = "build-only"
    except BaseException as error:
        report.update(result="failed", error=type(error).__name__ + ": " + str(error))
        code = 1
    finally:
        report["ended_unix"] = time.time()
        report["artifacts"] = {path.name: digest(path) for path in sorted(output.iterdir())
                               if path.is_file() and path.name != "receipt.json"}
        (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "receipt": str(output / "receipt.json")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
