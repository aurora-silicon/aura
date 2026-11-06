#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay production delays, deadline conversion and USB command polling."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
def extract(source, name):
    m = re.search(r'(?m)^(?:static (?:inline )?)?(?:int|bool|u64|void) '+name+r'\([^;]*?\)\n\{', source)
    if not m:
        raise ValueError('Missing production function: '+name)
    return source[m.start():source.index('\n}',m.end())+2]+'\n'
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--expect-unfixed',action='store_true')
    p.add_argument('--cc',default='clang')
    a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=False)
    report={'result':'failed','hardware_accessed':False,'commands':[],'sources':{},'cases':{},
            'limitations':['Clock registers and MMIO are modeled; physical counter calibration is separate.']}
    def run(cmd,log):
        cmd=list(map(str,cmd));report['commands'].append(cmd)
        r=subprocess.run(cmd,cwd=o,capture_output=True,text=True,timeout=30)
        (o/log).write_text(r.stdout+r.stderr);r.check_returncode();return r.stdout
    try:
        names=['src/utils.h','src/utils.c','src/hv/windows/hv.c','src/hv/windows/hv_exc.c','src/usb_dwc3_impl.inc','tests/e2e/windows_counter_host.c','docs/windows-counter-timeouts.md','tools/windows_counter_e2e.py']
        for n in names:
            t=o/'source'/n;t.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/n,t);report['sources'][n]=sha(t)
        h=(o/'source/src/utils.h').read_text();c=(o/'source/src/utils.c').read_text()
        prepared='bool timer_prepare_windows(' in c
        parts=[]
        if prepared:parts.append(extract(h,'timer_scale_alias'))
        parts.append(extract(h,'get_ticks'))
        if prepared:parts.append(extract(c,'timer_prepare_windows'))
        parts += [extract(c,n) for n in ['udelay','timeout_calculate','timeout_expired']]
        parts += [extract(h,'poll32'),extract((o/'source/src/usb_dwc3_impl.inc').read_text(),'usb_dwc3_ep_command')]
        if prepared:
            parts.append(extract((o/'source/src/hv/windows/hv.c').read_text(),'hv_host_counter'))
            exc=(o/'source/src/hv/windows/hv_exc.c').read_text()
            if 'static u64 hv_guest_timer_counter(' in exc:
                parts.append(extract(exc,'hv_guest_timer_counter'))
            begin=exc.index('        case SYSREG_ISS(sys_reg(3, 3, 14, 0, 1)):')
            end=exc.index('            return true;',begin)+len('            return true;')
            parts.append('static bool guest_counter_read(bool is_read, u64 regs[32]) { unsigned rt=4; switch(1) {\n'+exc[begin:end]+'\n default:return false; } }\n')
        (o/'production_counter.inc').write_text('\n'.join(parts))
        report['production_sha256']=sha(o/'production_counter.inc')
        for mode in ['windows','linux']:
            # The optional Windows initializer is omitted from the default build.
            flags=['-DAURA_WINDOWS'] if mode=='windows' else []
            if prepared and mode=='windows':flags+=['-DTIMER_PREPARED']
            run([a.cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined',*flags,'-I.',o/'source/tests/e2e/windows_counter_host.c','-o',o/mode],mode+'-compile.log')
            cases=['frozen','other'] if mode=='windows' else ['default']
            if prepared and mode=='windows':cases+=['bad-hz','bad-rate','offset','first-backward','first-wide','poll-backward','last-backward','last-wide','alias-backward','frozen-init','short-window','fast-reads']
            for case in cases:
                value=json.loads(run([o/mode,case],mode+'-'+case+'.log'))
                if case=='frozen':assert value['stalled'] is a.expect_unfixed
                else:assert not value.get('stalled',False)
                report['cases'][mode+'-'+case]=value
        report['result']='unfixed-reproduced' if a.expect_unfixed else 'passed'
    except (OSError,ValueError,AssertionError,subprocess.SubprocessError) as e:report['error']=str(e)
    (o/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(report['result']+': '+str(o/'result.json'))
    return 0 if report['result'] in ['passed','unfixed-reproduced'] else 1
if __name__=='__main__':raise SystemExit(main())
