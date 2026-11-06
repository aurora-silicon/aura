#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay production RTKit boot, fixed-pool grants and bounded quiet service."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {'result':'failed', 'hardware_accessed':False, 'sources':{}, 'commands':[],
              'limitations':['ASC, DART and cache operations modeled; no MTP hardware qualification']}
    def run(command, cwd, log):
        command = [str(x) for x in command]; report['commands'].append(command)
        result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=30)
        (out/log).write_text(result.stdout+result.stderr); result.check_returncode(); return result.stdout
    try:
        flags = ['-Isrc', '-idirafter', 'sysinc']
        deps = run([args.cc,'-MM',*flags,'tests/e2e/windows_rtkit_host.c'],ROOT,'deps.log')
        paths = [(ROOT/x).resolve() for x in deps.replace('\\\n',' ').split(':',1)[1].split()]
        names = {str(x.relative_to(ROOT)) for x in paths if x.is_relative_to(ROOT)}
        names.update(('sysinc/malloc.h','config.h','docs/windows-rtkit.md','tools/windows_rtkit_e2e.py'))
        source = out/'source'
        for name in sorted(names):
            target = source/name; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(ROOT/name,target)
            report['sources'][name]=sha(target)
        binary=out/'rtkit-host'
        run([args.cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
             '-Wno-unused-variable','-Wno-multichar','-ffunction-sections','-fdata-sections',
             '-fsanitize=address,undefined',*flags,'tests/e2e/windows_rtkit_host.c','-o',binary,
             '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'],source,'compile.log')
        report['observations']=json.loads(run([binary,out/'mailbox.bin'],source,'replay.log'))
        report.update(result='passed',binary_sha256=sha(binary),mailbox_sha256=sha(out/'mailbox.bin'))
    except (OSError,ValueError,AssertionError,subprocess.SubprocessError) as error:
        report['error']=str(error)
    finally:
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['result']+': '+str(out/'result.json'));return 0 if report['result']=='passed' else 1
if __name__=='__main__': raise SystemExit(main())
