#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build by default; explicitly verify an AP fault drains a Windows Aura guest."""

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
OFFSET, SIZE, FAULT = 0x1000, 256, 0x1000


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def tool(name, formula):
    p = Path("/opt/homebrew/opt") / formula / "bin" / name
    return str(p) if p.exists() else shutil.which(name) or name


def build(args, out, report):
    for name in [
        "tests/e2e/windows_abort_smoke.S",
        "tools/windows_abort_smoke.py",
        "docs/windows-abort-drain.md",
    ]:
        shutil.copy2(ROOT / name, out / Path(name).name)
    report["build_commands"] = []

    def run(command, log):
        command = [str(x) for x in command]
        report["build_commands"].append(command)
        r = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (out / log).write_text(r.stdout + r.stderr)
        r.check_returncode()
        return r.stdout

    for suffix in ["", "-repeat"]:
        obj = out / ("guest" + suffix + ".o")
        elf = out / ("guest" + suffix + ".elf")
        raw = out / ("template" + suffix + ".bin")
        run(
            [
                args.clang,
                "--target=aarch64-none-elf",
                "-march=armv8-a",
                "-c",
                out / "windows_abort_smoke.S",
                "-o",
                obj,
            ],
            "compile" + suffix + ".log",
        )
        run(
            [
                args.ld,
                "-Ttext=0",
                "--image-base=0",
                "--entry=_start",
                "--build-id=none",
                obj,
                "-o",
                elf,
            ],
            "link" + suffix + ".log",
        )
        run(
            [args.objcopy, "-O", "binary", "-j", ".text", elf, raw],
            "objcopy" + suffix + ".log",
        )
    symbols = run([args.objdump, "-t", out / "guest.elf"], "symbols.log")
    run([args.objdump, "-d", out / "guest.elf"], "disassembly.log")
    positions = {
        line.split()[-1]: int(line.split()[0], 16)
        for line in symbols.splitlines()
        if line.split() and line.split()[-1] in ["abort_results", "abort_fault"]
    }
    require(positions["abort_results"] == OFFSET, "result block moved")
    template = (out / "template.bin").read_bytes()
    require(
        template == (out / "template-repeat.bin").read_bytes(),
        "guest build does not reproduce",
    )
    require(
        len(template) == 0x2000 and template[OFFSET : OFFSET + SIZE] == bytes(SIZE),
        "invalid result reservation",
    )
    report["symbols"] = positions
    report["template_sha256"] = sha(out / "template.bin")
    return int.from_bytes(os.urandom(8), "little") or 1


def profile(args, out, nonce, target):
    from aura.windows import plan_profile

    image = bytearray((out / "template.bin").read_bytes())
    for offset, value in [(32, nonce), (56, target), (96, FAULT)]:
        struct.pack_into("<Q", image, OFFSET + offset, value)
    (out / "guest.bin").write_bytes(image)
    data = {
        "schema": "aura.windows.boot.v1",
        "name": "AP abort drain smoke",
        "target": {"chip_id": args.chip_id, "model": args.model},
        "mu": {
            "path": "guest.bin",
            "sha256": sha(out / "guest.bin"),
            "entry_offset": 0,
        },
        "framebuffer": "native",
        "guest_compatibility": "none",
        "usb": "disabled",
        "n1": "disabled",
        "mtp": "disabled",
    }
    (out / "profile.json").write_text(json.dumps(data, indent=2) + "\n")
    return plan_profile(out / "profile.json")


def execute(args, out, nonce, report):
    from aura.adt import load_adt
    from aura.proxy import ExcInfo, ProxyRemoteError
    from aura.windows import HardwareSession

    session = None
    events = []
    replies = []
    saved = {
        s: signal.getsignal(s) for s in [signal.SIGALRM, signal.SIGINT, signal.SIGQUIT]
    }

    def timeout(signum, frame):
        raise TimeoutError("AP drain deadline; guest state unqualified")

    signal.signal(signal.SIGALRM, timeout)
    signal.setitimer(signal.ITIMER_REAL, args.timeout)
    try:
        with (out / "console.log").open("w") as console, (out / "host.log").open(
            "w"
        ) as host:
            with contextlib.redirect_stdout(host), contextlib.redirect_stderr(host):
                session = HardwareSession(args.device, console)
                ident = session.identity()
                report["identity"] = ident
                require(
                    ident["chip_id"] == args.chip_id and ident["model"] == args.model,
                    "target identity mismatch",
                )
                caps = session.capabilities()
                report["capabilities"] = hex(caps)
                adt = load_adt(session._identity_adt_blob)
                cpus = list(adt["cpus"])
                bsps = [c for c in cpus if c.state == "running"]
                aps = [c for c in cpus if c.state == "waiting"]
                require(
                    len(bsps) == 1 and aps, "no unambiguous boot CPU and waiting AP"
                )
                bsp, ap = bsps[0], aps[0]
                target = int(ap.reg)
                bsp_affinity = int(bsp.reg)
                require(
                    target != bsp_affinity and ap.cpu_id != bsp.cpu_id, "AP equals BSP"
                )
                report["cpu_selection"] = {
                    "bsp": bsp.cpu_id,
                    "bsp_affinity": bsp_affinity,
                    "ap": ap.cpu_id,
                    "ap_affinity": target,
                }
                plan = profile(args, out, nonce, target)
                report["profile_sha256"] = plan.profile_sha256
                report["target_mutated"] = True
                session.prepare(plan)
                loaded = session.load(plan)
                report["loaded"] = loaded
                address = int(loaded["guest_base"], 0) + OFFSET
                require(
                    session.proxy.hv_pt_walk(FAULT) == 0,
                    "deliberate fault IPA is mapped",
                )
                before = session.iface.readmem(address, SIZE)
                (out / "result-before.bin").write_bytes(before)
                require(
                    before == plan.image[OFFSET : OFFSET + SIZE],
                    "result upload mismatch",
                )
                original_abort = session.hv._abort_windows_guest

                def capture(reason, code, info):
                    event = {
                        "reason": int(reason),
                        "code": int(code),
                        "info": hex(info),
                        "monotonic": time.monotonic(),
                    }
                    try:
                        if int(reason) == 2 and int(code) == 0:
                            raw = session.iface.readmem(info, ExcInfo.sizeof())
                            ctx = ExcInfo.parse(raw)
                            event.update(
                                cpu=ctx.cpu_id,
                                elr=hex(ctx.elr),
                                far=hex(ctx.far),
                                esr=int(ctx.esr),
                            )
                            (out / f"exception-{len(events):02d}.bin").write_bytes(raw)
                        events.append(event)
                        (out / "exceptions.json").write_text(
                            json.dumps(events, indent=2) + "\n"
                        )
                    except Exception as error:
                        event["capture_error"] = (
                            type(error).__name__ + ": " + str(error)
                        )
                        if not events or events[-1] is not event:
                            events.append(event)
                    finally:
                        # Diagnostic failure must not leave the faulting CPU
                        # waiting for an acknowledgement in the proxy.
                        original_abort(reason, code, info)

                session.hv._abort_windows_guest = capture
                original_proxyreq = session.iface.proxyreq

                def proxyreq(request, *a, **kw):
                    reply = original_proxyreq(request, *a, **kw)
                    if (
                        struct.unpack_from("<Q", request)[0] == session.proxy.P_HV_START
                        and reply is not None
                    ):
                        opcode, status, value = struct.unpack("<Qqq", reply)
                        replies.append(
                            {"opcode": opcode, "status": status, "value": value}
                        )
                        (out / "hv-start-reply.bin").write_bytes(reply)
                    return reply

                session.iface.proxyreq = proxyreq
                report["guest_state"] = "running-until-return"
                started = time.monotonic()
                try:
                    session.start()
                except (ProxyRemoteError, RuntimeError) as error:
                    report["expected_start_error"] = str(error)
                report["elapsed_seconds"] = time.monotonic() - started
                report["hv_start_replies"] = replies
                report["events"] = events
                require(
                    len(replies) == 1
                    and replies[0]["status"] == session.proxy.S_ERROR
                    and replies[0]["value"] == -1,
                    "abort did not return explicit failure after drain",
                )
                report["guest_state"] = "returned-failure"
                require(
                    len(events) == 1
                    and events[0]["reason"] == 2
                    and events[0]["code"] == 0,
                    "unexpected or repeated exception",
                )
                require(
                    "capture_error" not in events[0],
                    "first exception context was not preserved",
                )
                event = events[0]
                fault_pc = (
                    int(loaded["guest_base"], 0) + report["symbols"]["abort_fault"]
                )
                require(
                    event["cpu"] == ap.cpu_id
                    and int(event["elr"], 0) == fault_pc
                    and int(event["far"], 0) == FAULT,
                    "fault CPU/PC/address mismatched",
                )
                require(
                    (event["esr"] >> 26) == 0x24 and not (event["esr"] & 0x40),
                    "expected lower-EL read data abort",
                )
                raw = session.iface.readmem(address, SIZE)
                (out / "result-after.bin").write_bytes(raw)
                w = struct.unpack("<32Q", raw)
                require(
                    raw[:8] == b"AWABORT1" and w[1] == 1 and w[2] == 4 and w[3] == 0,
                    "guest did not arm AP fault",
                )
                require(
                    w[4] == w[5] == w[6] == nonce and w[7] == target,
                    "nonce or AP target mismatch",
                )
                mask = 0xFF00FFFFFF
                require(
                    w[8] & mask == bsp_affinity and w[9] & mask == target,
                    "guest CPU identity mismatch",
                )
                require(
                    w[10] > 0
                    and w[11] >= 100
                    and w[12] == FAULT
                    and w[13] == fault_pc
                    and w[14] == 1,
                    "AP did not observe running BSP before fault",
                )
                require(
                    w[15] == w[16] == 0
                    and w[17] == int(loaded["bootargs"], 0)
                    and w[18] == 1
                    and w[19] > 0
                    and not any(w[20:]),
                    "guest continued after fault, hit timeout, or corrupted result",
                )
                console.flush()
                host.flush()
                require(
                    "HV: All CPUs exited" in (out / "console.log").read_text(),
                    "missing completed CPU drain",
                )
                require(
                    session.capabilities() == caps, "proxy not responsive after abort"
                )
                report.update(
                    result="passed",
                    hardware_qualified=True,
                    proxy_responsive=True,
                    observations={
                        "bsp_heartbeat": w[10],
                        "ap_observed_heartbeat": w[11],
                    },
                )
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for sig, handler in saved.items():
            signal.signal(sig, handler)
        if session is not None:
            session.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", action="store_true")
    p.add_argument("--device")
    p.add_argument("--chip-id", required=True, type=lambda x: int(x, 0))
    p.add_argument("--model", required=True)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--timeout", type=float, default=30)
    for name, binary, formula in [
        ("clang", "clang", "llvm"),
        ("ld", "ld.lld", "lld"),
        ("objcopy", "llvm-objcopy", "llvm"),
        ("objdump", "llvm-objdump", "llvm"),
    ]:
        p.add_argument("--" + name, default=tool(binary, formula))
    args = p.parse_args()
    if args.run != bool(args.device) or not 15 <= args.timeout <= 120:
        p.error("--run requires --device; timeout must be15..120")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": "aura.windows.ap-abort-smoke.v1",
        "result": "preflight",
        "target_mutated": False,
        "hardware_qualified": False,
        "guest_state": "not-started",
        "command": [sys.executable, *sys.argv],
    }
    code = 0
    try:
        nonce = build(args, out, report)
        report["nonce"] = hex(nonce)
        sys.path.insert(0, str(ROOT / "proxyclient"))
        from aura.windows import source_manifest

        report["host_sources"] = source_manifest()
        report["backend_sources"] = {
            n: sha(ROOT / n)
            for n in [
                "src/hv/windows/hv.c",
                "src/hv/windows/hv_exc.c",
                "src/hv/windows/hv_psci.c",
            ]
        }
        report["backend_note"] = (
            "Local hashes do not attest the running resident binary."
        )
        if args.run:
            execute(args, out, nonce, report)
        else:
            profile(args, out, nonce, 0)
            report["result"] = "build-only"
    except BaseException as error:
        report.update(result="failed", error=type(error).__name__ + ": " + str(error))
        code = 1
    finally:
        report["artifacts"] = {
            p.name: sha(p)
            for p in out.iterdir()
            if p.is_file() and p.name != "receipt.json"
        }
        (out / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps({"result": report["result"], "receipt": str(out / "receipt.json")})
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
