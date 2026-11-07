#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the no-CRT ARM64 Windows diagnostic reader and bind its inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True, help="Windows SDK package/c directory")
    parser.add_argument("--wdk", type=Path, required=True, help="WDK package/c directory for no-CRT declarations")
    parser.add_argument("--sdk-version", default="10.0.26100.0")
    parser.add_argument("--arm64-sdk", type=Path, required=True, help="Microsoft ARM64 SDK nupkg")
    parser.add_argument("--clang-cl", type=Path, required=True)
    parser.add_argument("--linker", type=Path, required=True)
    parser.add_argument("--readobj", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().with_name("windows_g17_report_reader.c")
    receipt = {"result": "failed", "hardware_accessed": False,
               "scope": "native ARM64 Windows build; execution not claimed", "commands": [],
               "sources": {str(source): sha(source), str(Path(__file__).resolve()): sha(Path(__file__).resolve())}}
    try:
        kernel = output / "kernel32.Lib"
        with zipfile.ZipFile(args.arm64_sdk) as archive:
            kernel.write_bytes(archive.read("c/um/arm64/kernel32.Lib"))
        obj, exe, dependencies = output/"reader.obj", output/"aurora-g17-report.exe", output/"reader.d"
        includes = args.sdk.resolve()/"Include"/args.sdk_version
        compile_command = [str(args.clang_cl), "--target=aarch64-pc-windows-msvc", "/nologo", "/TC",
                           "/W4", "/WX", "/O2", "/GS-", "/Zl", "/std:c11",
                           "/clang:-fno-builtin", "/D_WIN32_WINNT=0x0A00",
                           "/imsvc", str(args.wdk.resolve()/"Include"/args.sdk_version/"km/crt"),
                           *[item for part in ("um", "shared", "ucrt") for item in ("/imsvc", str(includes/part))],
                           "/clang:-MD", "/clang:-MF"+str(dependencies), "/c", "/Fo"+str(obj), "--", str(source)]
        commands = [compile_command,
                    [str(args.linker), "/entry:mainCRTStartup", "/subsystem:console,6.2", "/osversion:10.0",
                     "/machine:arm64", "/nodefaultlib", "/timestamp:0", "/out:"+str(exe), str(obj), str(kernel)],
                    [str(args.readobj), "--file-headers", "--coff-imports", str(exe)]]
        for index, command in enumerate(commands):
            result = subprocess.run(command, text=True, capture_output=True)
            (output/f"command-{index}.log").write_text(result.stdout+result.stderr)
            receipt["commands"].append({"argv": command, "returncode": result.returncode})
            if result.returncode != 0:
                raise RuntimeError(f"command {index} failed; see its log")
        headers = (output/"command-2.log").read_text()
        required = ("IMAGE_FILE_MACHINE_ARM64", "MajorOperatingSystemVersion: 10",
                    "MinorOperatingSystemVersion: 0", "MajorSubsystemVersion: 6", "MinorSubsystemVersion: 2")
        if not all(value in headers for value in required):
            raise RuntimeError("native PE identity or version mismatch")
        imports = re.findall(r"Symbol: ([A-Za-z0-9_]+)", headers)
        if set(imports) != {"EnumSystemFirmwareTables", "GetSystemFirmwareTable", "GetStdHandle",
                            "WriteFile", "ExitProcess"} or len(imports) != 5:
            raise RuntimeError("unexpected native import set: " + repr(imports))
        receipt["imports"] = imports
        dep_text = dependencies.read_text().replace("\\\n", "")
        dep_paths = shlex.split(dep_text.split(":", 1)[1])
        receipt["compiler_inputs"] = {str(Path(p).resolve()): sha(Path(p)) for p in dep_paths}
        receipt["toolchain"] = {str(p.resolve()): sha(p) for p in (args.clang_cl, args.linker, args.readobj)}
        receipt["sdk_archive"] = {"path": str(args.arm64_sdk.resolve()), "sha256": sha(args.arm64_sdk)}
        receipt["result"] = "passed"
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
    finally:
        receipt["artifacts"] = {p.name: sha(p) for p in output.iterdir() if p.is_file()}
        (output/"receipt.json").write_text(json.dumps(receipt, indent=2)+"\n")
    print(receipt["result"], output/"receipt.json")
    return receipt["result"] != "passed"


if __name__ == "__main__":
    raise SystemExit(main())
