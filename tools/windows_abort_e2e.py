#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay production Windows exception exit, peer drain, and first-reason handling."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import textwrap
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
FILES = ["src/hv/windows/hv.c", "src/hv/windows/hv.h", "src/hv/windows/hv_exc.c",
         "proxyclient/aura/hv/__init__.py", "tests/e2e/windows_abort_host.c",
         "tools/windows_abort_e2e.py", "docs/windows-abort-drain.md"]
CASES = ["ap-fault", "bsp-fault", "poweroff", "reset", "abort-after-poweroff",
         "poweroff-after-abort", "join-failure", "watchdog-failure", "mtp-stop-failure", "handled"]

def extract(source, name):
    match = re.search(r"(?m)^(?:static )?(?:void|int) " + name + r"\([^;]*?\)\n\{", source)
    if not match:
        raise ValueError("Missing production function: " + name)
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-unfixed", action="store_true")
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "hardware_qualified": False,
              "sources": {}, "commands": [], "cases": [],
              "boundary": "Production C lifecycle and Python abort method; modeled scheduling, architecture and transport"}
    def run(command, log):
        command = [str(x) for x in command]; report["commands"].append(command)
        result = subprocess.run(command, cwd=out, text=True, capture_output=True, timeout=30)
        (out/log).write_text(result.stdout+result.stderr); result.check_returncode()
    try:
        for name in FILES:
            p=out/"source"/name; p.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(ROOT/name,p)
            report["sources"][name]=sha(p.read_bytes())
        source=(out/"source/src/hv/windows/hv.c").read_text()
        names=["hv_windows_request_exit", "hv_windows_abort_guest"] if not args.expect_unfixed else []
        names += ["hv_join_cpus", "hv_windows_system_exit", "hv_maybe_exit"]
        fragments=[extract(source,name) for name in names]
        fragments.append(extract((out/"source/src/hv/windows/hv_exc.c").read_text(),"_hv_exc_proxy"))
        start=source.index("    __atomic_and_fetch(&hv_cpus_in_guest",source.index("    hv_enter_guest(regs[0]"))
        end=source.index("\n}",start)
        fragments.append("static int finish_boot_cpu(void) {\n"+source[start:end]+"\n}\n")
        (out/"production_abort.inc").write_text("\n".join(fragments))
        run([args.cc,"-std=gnu11","-O1","-g","-Wall","-Wextra","-Werror","-Wno-unused-function",
             "-fsanitize=address,undefined","-I",out,out/"source/tests/e2e/windows_abort_host.c","-o",out/"abort-host"],"compile.log")
        for case in ["unfixed"] if args.expect_unfixed else CASES:
            run([out/"abort-host",case],case+".log"); report["cases"].append(case)
        host=(out/"source/proxyclient/aura/hv/__init__.py").read_text()
        method=next(n for n in ast.walk(ast.parse(host)) if isinstance(n,ast.FunctionDef) and n.name=="_abort_windows_guest")
        body=textwrap.dedent("\n".join(host.splitlines()[method.lineno-1:method.end_lineno]))
        namespace={"EXC_RET":SimpleNamespace(EXIT_GUEST=1)};exec(compile(body,"production-host-abort","exec"),namespace)
        events=[];obj=SimpleNamespace(windows_abort_reason=None,p=SimpleNamespace(exit=lambda code:events.append(code)),log=lambda line:None)
        abort=namespace[method.name];abort(obj,2,0,0x1000);abort(obj,3,3,0x2000)
        assert events==[1,1]
        expected={"reason":3,"code":3,"info":"0x2000"} if args.expect_unfixed else {"reason":2,"code":0,"info":"0x1000"}
        assert obj.windows_abort_reason==expected
        report["host_first_reason"] = obj.windows_abort_reason
        report["cases"].append("host-reason-overwrite-reproduced" if args.expect_unfixed else "host-first-reason-retained")
        report["result"]="unfixed-reproduced" if args.expect_unfixed else "passed"
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        report["error"]=str(error)
    finally:
        report["artifacts"]={p.name:sha(p.read_bytes()) for p in out.iterdir() if p.is_file()}
        (out/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(report["result"]+": "+str(out/"result.json"))
    return 0 if report["result"] != "failed" else 1
if __name__=="__main__":raise SystemExit(main())
