#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build/run a bounded PSCI smoke guest on an explicit public Windows Aura resident."""
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
RESULT_OFFSET = 0x1000
RESULT_SIZE = 128


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tool(name, formula):
    brew = Path("/opt/homebrew/opt") / formula / "bin" / name
    return str(brew) if brew.is_file() else shutil.which(name) or name


def build_guest(args, output, report):
    source = ROOT / "tests/e2e/windows_hv_smoke.S"
    shutil.copy2(source, output / source.name)
    shutil.copy2(Path(__file__), output / Path(__file__).name)
    shutil.copy2(ROOT / "docs/windows-hv-smoke.md", output / "windows-hv-smoke.md")
    commands = report["build_commands"] = []

    def run(argv, log):
        commands.append([str(x) for x in argv])
        result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (output / log).write_text(result.stdout)
        require(result.returncode == 0, "build command failed; see " + str(output / log))
        return result.stdout

    for suffix in ("", "-repeat"):
        obj = output / ("guest" + suffix + ".o")
        elf = output / ("guest" + suffix + ".elf")
        raw = output / ("guest-template" + suffix + ".bin")
        run([args.clang, "--target=aarch64-none-elf", "-march=armv8-a", "-c",
             str(source), "-o", str(obj)], "compile" + suffix + ".log")
        run([args.ld, "-Ttext=0", "--image-base=0", "--entry=_start", "--build-id=none",
             str(obj), "-o", str(elf)], "link" + suffix + ".log")
        run([args.objcopy, "-O", "binary", "-j", ".text", str(elf), str(raw)],
            "objcopy" + suffix + ".log")
    run([args.objdump, "-d", str(output / "guest.elf")], "guest.disassembly.txt")
    symbols = run([args.objdump, "-t", str(output / "guest.elf")], "guest.symbols.txt")
    require(any(line.startswith("0000000000001000") and line.endswith("smoke_results")
                for line in symbols.splitlines()), "guest result address is not 0x1000")
    elf = (output / "guest.elf").read_bytes()
    require(elf[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", elf, 18)[0] == 183
            and struct.unpack_from("<Q", elf, 24)[0] == 0,
            "guest must be little-endian AArch64 ELF with entry zero")
    template = (output / "guest-template.bin").read_bytes()
    require(template == (output / "guest-template-repeat.bin").read_bytes(),
            "guest template did not reproduce")
    require(len(template) == 0x2000 and template[RESULT_OFFSET:RESULT_OFFSET + RESULT_SIZE]
            == bytes(RESULT_SIZE), "guest result reservation is not zero-initialized")
    nonce = int.from_bytes(os.urandom(8), "little") or 1
    image = bytearray(template)
    struct.pack_into("<Q", image, RESULT_OFFSET + 64, nonce)
    (output / "guest.bin").write_bytes(image)
    report["nonce"] = hex(nonce)
    report["template_sha256"] = hashlib.sha256(template).hexdigest()
    profile = {"schema": "aura.windows.boot.v1", "name": "PSCI smoke guest",
               "target": {"chip_id": args.chip_id, "model": args.model},
               "mu": {"path": "guest.bin", "sha256": digest(output / "guest.bin"),
                      "entry_offset": 0},
               "framebuffer": "native", "guest_compatibility": "none"}
    (output / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    return nonce


def check_results(raw, nonce, bootargs):
    require(len(raw) == RESULT_SIZE, "short guest result readback")
    words = struct.unpack("<16Q", raw)
    require(raw[:8] == b"AWINSMK1" and words[1] == 1, "missing guest result magic/ABI")
    require(words[2] == 4, "guest did not reach the non-returning SYSTEM_OFF call")
    require(words[3] == 0x10001 and words[5] == 0x10001, "PSCI_VERSION must return 1.1 on SMC and HVC")
    require(words[4] == 0xffffffff and words[6] == 0xffffffff,
            "unknown PSCI function must return NOT_SUPPORTED on SMC and HVC")
    require(words[8] == nonce and words[9] == nonce, "stale or corrupted guest nonce")
    require(words[10] == 4, "guest did not enter at EL1")
    require(words[11] == bootargs, "guest entry x0 does not match the loader's boot arguments")
    require(raw[96:104] == b"DONEPSCI", "guest result completion marker is missing")
    require(words[7] == words[13] == words[14] == words[15] == 0,
            "guest result guard bytes were overwritten")
    return {"phase": words[2], "psci_version_smc": hex(words[3]),
            "unknown_smc": hex(words[4]), "psci_version_hvc": hex(words[5]),
            "unknown_hvc": hex(words[6]), "current_el": words[10] >> 2,
            "nonce": hex(words[9]), "bootargs": hex(words[11])}


def execute(args, output, plan, nonce, report):
    from aura.windows import HardwareSession, validate_capabilities
    session = None
    saved_handlers = {s: signal.getsignal(s) for s in (signal.SIGALRM, signal.SIGINT, signal.SIGQUIT)}

    def timeout(signum, frame):
        raise TimeoutError("hardware phase exceeded deadline; guest state is unknown")

    signal.signal(signal.SIGALRM, timeout)
    signal.setitimer(signal.ITIMER_REAL, args.timeout)
    try:
        with (output / "console.log").open("w") as console, (output / "host.log").open("w") as host:
            with contextlib.redirect_stdout(host), contextlib.redirect_stderr(host):
                session = HardwareSession(args.device, console)
                caps = session.capabilities()
                validate_capabilities(caps, plan.framebuffer, plan.guest_compatibility)
                identity = session.identity()
                report.update(capabilities=hex(caps), identity=identity)
                require(identity["chip_id"] == plan.chip_id and identity["model"] == plan.model,
                        "target identity does not match the explicit chip/model")
                report["target_mutated"] = True
                session.prepare(plan)
                loaded = session.load(plan)
                report["loaded"] = loaded
                address = int(loaded["guest_base"], 0) + RESULT_OFFSET
                before = session.iface.readmem(address, RESULT_SIZE)
                require(before == plan.image[RESULT_OFFSET:RESULT_OFFSET + RESULT_SIZE],
                        "uploaded result block does not match the immutable image")
                (output / "result-before.bin").write_bytes(before)
                report["result"] = "guest-running"
                report["guest_state"] = "unknown-until-verified-return"
                (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
                status = session.start()
                report["host_return_status"] = status
                require(status == 1, "expected SYSTEM_OFF host return status 1 after guest drain")
                report["guest_state"] = "stopped"
                session.iface.dev.timeout = min(args.timeout, 5.0)
                raw = session.iface.readmem(address, RESULT_SIZE)
                (output / "result-after.bin").write_bytes(raw)
                report["observations"] = check_results(raw, nonce, int(loaded["bootargs"], 0))
                require(session.capabilities() == caps, "resident proxy did not remain responsive")
                report["result"] = "passed"
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for signum, handler in saved_handlers.items():
            signal.signal(signum, handler)
        if session is not None:
            session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true", help="compile without opening hardware")
    parser.add_argument("--device", help="explicit serial/USB proxy device for a hardware run")
    parser.add_argument("--chip-id", required=True, type=lambda v: int(v, 0))
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--timeout", type=float, default=30.0, help="hardware phase deadline in seconds")
    parser.add_argument("--clang", default=tool("clang", "llvm"))
    parser.add_argument("--ld", default=tool("ld.lld", "lld"))
    parser.add_argument("--objcopy", default=tool("llvm-objcopy", "llvm"))
    parser.add_argument("--objdump", default=tool("llvm-objdump", "llvm"))
    args = parser.parse_args()
    if args.build_only and args.device:
        parser.error("--build-only must not specify a device")
    if not args.build_only and not args.device:
        parser.error("a hardware run requires --device")
    if not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300 seconds")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": "aura.windows.hv-smoke.v1", "result": "preflight",
              "target_mutated": False, "hardware_qualified": False, "windows_ready": False,
              "device": args.device, "guest_state": "not-started",
              "command": [sys.executable, *sys.argv], "started_unix": time.time()}
    code = 0
    try:
        nonce = build_guest(args, output, report)
        sys.path.insert(0, str(ROOT / "proxyclient"))
        from aura.windows import plan_profile, source_manifest
        plan = plan_profile(output / "profile.json")
        report.update(profile_sha256=plan.profile_sha256, host_sources=source_manifest())
        names = ("src/hv/windows/hv.c", "src/hv/windows/hv_exc.c", "src/hv/windows/hv_psci.c")
        report["local_backend_sources"] = {n: digest(ROOT / n) for n in names}
        report["local_backend_sources_note"] = "Local hashes do not attest the resident binary."
        if args.build_only:
            report["result"] = "build-only"
        else:
            execute(args, output, plan, nonce, report)
    except BaseException as error:
        report.update(result="failed", error=type(error).__name__ + ": " + str(error))
        code = 1
    finally:
        report["ended_unix"] = time.time()
        report["artifacts"] = {p.name: digest(p) for p in sorted(output.iterdir())
                               if p.is_file() and p.name != "receipt.json"}
        (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "receipt": str(output / "receipt.json")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
