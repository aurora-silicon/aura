#!/usr/bin/env python3
"""Compile the actual shadow-framebuffer functions from hv.c (and the previous converter from
c470c540) against a cache/DRAM model; check output, freshness, pacing and the cost line; reject
mutations."""
import argparse, hashlib, json, pathlib, re, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HV = ROOT / 'src/hv/windows/hv.c'
HV_H = ROOT / 'src/hv/windows/hv.h'
BASE = 'c470c5404126ea95a81087d1e50fb5a27a7db088'
FUNCTIONS = ['hv_fb_init', 'hv_fb_convert_slice', 'hv_fb_idle_due', 'hv_fb_convert_idle', 'hv_fb_report']
DEFINES = ['HV_FB_SLICE_IDLE', 'HV_FB_SLICE_TICK', 'HV_FB_SLICE_STEP', 'HV_FB_IDLE_FPS']
CIVAC = 'dc_civac_range((void *)(hv_fb_source + (size_t)start'
MUTATIONS = [  # (name, old, new, occurrence: 1 = first, -1 = last)
    ('no-clean-invalidate', CIVAC, 'if (0) ' + CIVAC, 1),
    ('invalidate-only', CIVAC, CIVAC.replace('dc_civac_range', 'dc_ivac_range'), 1),
    ('maintenance-through-nc-identity', CIVAC, CIVAC.replace('hv_fb_source +', 'hv_fb_shadow +'), 1),
    ('idle-conversion-unpaced', 'if (mrs(ISR_EL1) || !hv_fb_idle_due())', 'if (mrs(ISR_EL1))', 1),
    ('late-schedule-not-rebased', '    if (now - due > period)\n        __atomic_store_n(&hv_fb_epoch, now - pass * period, __ATOMIC_RELAXED);\n', '', 1),
    ('pending-interrupt-ignored', 'if (mrs(ISR_EL1) || !hv_fb_idle_due())', 'if (!hv_fb_idle_due())', 1),
    ('conversion-changed', '(((p >> 8) & 0xff) << 12)', '(((p >> 8) & 0xff) << 13)', 2),  # 1 = old, 2 = new, 3 = e2e
    ('cost-line-dropped', 'printf("HV: fb cost:', 'if (0) printf("HV: fb cost:', 1),
]


def sha(data):
    return hashlib.sha256(data if isinstance(data, bytes) else pathlib.Path(data).read_bytes()).hexdigest()


def function(text, name):
    m = re.search(r'^(?:static )?(?:inline )?(?:void|bool|u64|u32|int) ' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
    if not m:
        raise ValueError('function not found: ' + name)
    start, pos, depth, state = m.start(), text.index('{', m.start()), 0, 'code'
    while pos < len(text):
        c, n = text[pos], text[pos:pos + 2]
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
            if not depth: return text[start:pos + 1]
        pos += 1
    raise ValueError('unterminated function: ' + name)


def replace_nth(text, old, new, n):
    positions = [m.start() for m in re.finditer(re.escape(old), text)]
    if not positions:
        raise ValueError('mutation anchor missing: ' + old)
    pos = positions[n - 1] if n > 0 else positions[n]
    return text[:pos] + new + text[pos + len(old):]


def build_and_run(directory, code, cc, sdk, receipt):
    directory.mkdir()
    (directory / 'production.c').write_text(code)
    exe = directory / 'replay'
    cmd = [cc, '-isysroot', sdk, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
           '-Wno-sign-compare', '-Wno-format', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           str(directory / 'production.c'), '-o', str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (directory / 'build.log').write_text(build.stdout + build.stderr)
    receipt['commands'].append({'argv': cmd, 'returncode': build.returncode})
    if build.returncode:
        return None
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    (directory / 'run.log').write_text(run.stdout + run.stderr)
    receipt['commands'].append({'argv': [str(exe)], 'returncode': run.returncode})
    return run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    out = pathlib.Path(ap.parse_args().output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    old_hv = subprocess.check_output(['git', '-C', str(ROOT), 'show', BASE + ':src/hv/windows/hv.c'])
    inputs = [HV, HV_H, HERE / 'host.h', HERE / 'e2e.c', pathlib.Path(__file__), ROOT / 'docs/windows-fb-cached-read.md']
    receipt = {'hardware_accessed': False, 'source_sha256': {str(p): sha(p) for p in inputs},
               'previous_converter': {'commit': BASE, 'hv_c_sha256': sha(old_hv)}, 'functions': FUNCTIONS,
               'scope': 'verbatim production framebuffer functions; DRAM, the write-back alias, guest writes, '
                        'cache maintenance, clock and interrupts modeled; no hardware timing or panel claim',
               'commands': [], 'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun', '--find', 'clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip()
        receipt['compiler'] = {'path': cc, 'sha256': sha(cc), 'sdk': sdk}
        text, header = HV.read_text(), HV_H.read_text()
        code = (HERE / 'host.h').read_text() + '\n'
        code += ''.join(re.search(r'^#define ' + d + r' .*\n', header, re.M).group(0) for d in DEFINES) + '\n'
        statics = re.search(r'^static u32 \*hv_fb_shadow;\n.*?^static struct hv_fb_cost hv_fb_cost\[MAX_CPUS\];\n',
                            text, re.M | re.S)
        if not statics:
            raise ValueError('framebuffer state block not found')
        code += statics.group(0) + '\n'
        old = function(old_hv.decode(), 'hv_fb_convert_slice').replace('void hv_fb_convert_slice(',
                                                                        'void old_hv_fb_convert_slice(')
        for name in ('shadow', 'scanout', 'stride_px', 'height', 'cursor', 'lines'):
            old = re.sub(r'\bhv_fb_' + name + r'\b', 'old_fb_' + name, old)
        code += old + '\n\n'
        code += '\n\n'.join(function(text, n) for n in FUNCTIONS) + '\n\n'
        code += (HERE / 'e2e.c').read_text()

        run = build_and_run(out / 'actual', code, cc, sdk, receipt)
        if run is None or run.returncode:
            detail = (out / 'actual/build.log').read_text()[-600:] if run is None else run.stderr[-600:]
            raise RuntimeError('actual source failed: ' + detail)
        receipt['checks'] = [l[5:] for l in run.stdout.splitlines() if l.startswith('PASS ')]
        for name, old_text, new_text, n in MUTATIONS:
            run = build_and_run(out / ('mutant-' + name), replace_nth(code, old_text, new_text, n), cc, sdk, receipt)
            if run is None:
                raise RuntimeError(name + ' mutant did not build')
            if run.returncode == 0:
                raise RuntimeError(name + ' mutant was not rejected')
            reason = next((l for l in run.stderr.splitlines() if l.startswith('FAIL ')), 'exit %d' % run.returncode)
            receipt['mutations'].append({'name': name, 'rejected': True, 'reason': reason})
        assert all(sha(p) == h for p, h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict'] = 'PASS'
    except Exception as e:
        receipt['verdict'] = 'FAIL'
        receipt['error'] = str(e)
    receipt['artifacts_sha256'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*'))
                                   if p.is_file() and p.name != 'replay' and '.dSYM' not in str(p)}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'verdict': receipt['verdict'], 'checks': len(receipt.get('checks', [])),
                      'mutants_rejected': len(receipt['mutations'])} | ({'error': receipt['error']} if 'error' in receipt else {})))
    return receipt['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
