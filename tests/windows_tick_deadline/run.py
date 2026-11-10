#!/usr/bin/env python3
"""Compile actual timer/WFI/FIQ/tick functions; model only the hardware/OS boundary."""
import argparse, hashlib, json, pathlib, re, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def function(text, name):
    m = re.search(r'^(?:static )?(?:void|bool|u64) ' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
    if not m: raise ValueError('function not found: ' + name)
    start = m.start(); pos = text.index('{', m.start()); depth = 0; state = 'code'
    while pos < len(text):
        c = text[pos]; n = text[pos:pos+2]
        if state == 'line':
            if c == '\n': state = 'code'
        elif state == 'block':
            if n == '*/': state = 'code'; pos += 1
        elif state in ('"', "'"):
            if c == '\\': pos += 1
            elif c == state: state = 'code'
        elif n == '//': state = 'line'; pos += 1
        elif n == '/*': state = 'block'; pos += 1
        elif c in ('"', "'"): state = c
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if not depth: return text[start:pos+1]
        pos += 1
    raise ValueError('unterminated function: ' + name)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--output', required=True); a = ap.parse_args()
    out = pathlib.Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=False)
    sources = {'src/hv/windows/hv.c': ['hv_host_counter','hv_arm_wfi_wake','hv_arm_tick','hv_mask_pending_tick','hv_tick_host_io','hv_tick'],
               'src/hv/windows/hv_exc.c': ['hv_handle_wfx','hv_exc_fiq']}
    files = [ROOT/p for p in sources] + [ROOT/'src/hv/windows/tick_deadline.h', HERE/'host.h', HERE/'e2e.c', pathlib.Path(__file__), ROOT/'docs/windows-wfi-tick-deadlines.md']
    receipt = {'hardware_accessed': False, 'scope': 'verbatim production timer/WFI/full FIQ/tick functions; modeled per-CPU clock, registers and I/O effects; no hardware timing claim',
               'source_sha256': {str(p):sha(p) for p in files}, 'functions': sources, 'commands': [], 'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun','--find','clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun','--show-sdk-path'], text=True).strip()
        ccpath = pathlib.Path(cc).resolve(); receipt['compiler']={'path':str(ccpath),'sha256':sha(ccpath),'sdk':sdk}
        src = (HERE/'host.h').read_text() + '\n'
        for path,names in sources.items():
            text = (ROOT/path).read_text()
            src += '\n\n'.join(function(text, n) for n in names) + '\n'
        src += (HERE/'e2e.c').read_text()
        header = (ROOT/'src/hv/windows/tick_deadline.h').read_text()
        mutations = [
          ('full-on-wake', src.replace('if (!hv_tick_deadline_due(state, now)) {','if (false) {',1), header),
          ('deadline-reset', src, header.replace('if (!state->valid) {','if (true) {',1)),
          ('no-wake-host-io', src.replace('hv_windows_serial_flush();\n            hv_tick_host_io(ctx);','hv_windows_serial_flush();',1), header),
          ('drop-ipi-retry', src.replace('hv_guest_ipi_retry_tick();','(void)0;',1), header),
          ('consume-primary-deadline', src.replace('if (is_secondary)\n            hv_tick_deadline_complete','if (true)\n            hv_tick_deadline_complete',1), header),
          ('ignore-pending-guest-irq', src.replace('if (mrs(ISR_EL1) == 0 && !(mrs(HCR_EL2) & (HCR_VI | HCR_VF))) {','if (true) {',1), header),
        ]
        for name,code,hdr in [('actual',src,header)] + mutations:
            if name != 'actual': assert code != src or hdr != header, name+' mutation was not applied'
            directory = out/name; directory.mkdir(); (directory/'production.c').write_text(code); (directory/'tick_deadline.h').write_text(hdr)
            exe = directory/'replay'
            cmd = [cc,'-isysroot',sdk,'-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare','-Wno-format','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(directory),str(directory/'production.c'),'-o',str(exe)]
            build = subprocess.run(cmd,capture_output=True,text=True,timeout=60)
            (directory/'build.log').write_text(build.stdout+build.stderr)
            receipt['commands'].append({'argv':cmd,'returncode':build.returncode})
            if build.returncode: raise RuntimeError(name+' did not compile')
            run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
            (directory/'run.log').write_text(run.stdout+run.stderr)
            receipt['commands'].append({'argv':[str(exe)],'returncode':run.returncode})
            if name == 'actual':
                if run.returncode: raise RuntimeError('actual source replay failed')
                checks = [s[5:] for s in run.stdout.splitlines() if s.startswith('PASS ')]
                timeline = [json.loads(s[6:]) for s in run.stdout.splitlines() if s.startswith('TRACE ')]
                assert len(checks)==25, len(checks)
                (out/'timeline.json').write_text(json.dumps(timeline,indent=2)+'\n')
                receipt['checks']=checks;receipt['timeline_events']=len(timeline)
            else:
                assert run.returncode != 0 and 'FAIL ' in run.stderr, name+' mutant not rejected'
                receipt['mutations'].append({'name':name,'rejected':True,'reason':next(s for s in run.stderr.splitlines() if s.startswith('FAIL '))})
        assert sha(ccpath)==receipt['compiler']['sha256']
        assert all(sha(pathlib.Path(p))==h for p,h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict']='PASS'
    except Exception as e:
        receipt['verdict']='FAIL';receipt['error']=str(e)
    receipt['artifacts_sha256']={str(p.relative_to(out)):sha(p) for p in out.rglob('*') if p.is_file()}
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:receipt[k] for k in ['verdict','checks','timeline_events','mutations','error'] if k in receipt}))
    return receipt['verdict'] != 'PASS'
if __name__=='__main__': raise SystemExit(main())
