#!/usr/bin/env python3
"""Compile the actual shadow-framebuffer functions from hv.c, start-up self-test included (and the
previous converter from c470c540), against a per-CPU cache/DRAM model; run every hardware variant;
check output, freshness, pacing, the cost line and the self-test verdict; reject mutations."""
import argparse, hashlib, json, pathlib, re, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HV = ROOT / 'src/hv/windows/hv.c'
HV_H = ROOT / 'src/hv/windows/hv.h'
BASE = 'c470c5404126ea95a81087d1e50fb5a27a7db088'
SELFTEST = ['hv_fb_selftest_step', 'hv_fb_selftest_mmu_on', 'hv_fb_selftest_remote', 'hv_fb_selftest_on',
            'hv_fb_selftest_mpidr', 'hv_fb_selftest_class', 'hv_fb_selftest_cross', 'hv_fb_selftest_cpus',
            'hv_fb_selftest']
FUNCTIONS = SELFTEST + ['hv_fb_init', 'hv_fb_convert_slice', 'hv_fb_idle_due', 'hv_fb_convert_idle', 'hv_fb_report']
DEFINES = ['HV_FB_SLICE_IDLE', 'HV_FB_SLICE_TICK', 'HV_FB_SLICE_STEP', 'HV_FB_IDLE_FPS']
VARIANTS = ['normal', 'maintenance-noop', 'maintenance-not-broadcast', 'maintenance-cluster-only', 'lines-32-bytes',
            'too-few-cpus', 'guest-owned-cpus', 'secondary-mmu-on', 'secondary-fault', 'secondary-hangs-writing']
CIVAC = 'dc_civac_range((void *)(hv_fb_source + (size_t)start'
CROSS = 'if (!failed && !hv_fb_selftest_cross(&t, '
MUTATIONS = [  # (name, [(old, new, occurrence: 1 = first, -1 = last, 0 = every)])
    ('no-clean-invalidate', [(CIVAC, 'if (0) ' + CIVAC, 1)]),
    ('invalidate-only', [(CIVAC, CIVAC.replace('dc_civac_range', 'dc_ivac_range'), 1)]),
    ('maintenance-through-nc-identity', [(CIVAC, CIVAC.replace('hv_fb_source +', 'hv_fb_shadow +'), 1)]),
    ('idle-conversion-unpaced', [('if (mrs(ISR_EL1) || !hv_fb_idle_due())', 'if (mrs(ISR_EL1))', 1)]),
    ('late-schedule-not-rebased', [('    if (now - due > period)\n        __atomic_store_n(&hv_fb_epoch, now - pass * period, __ATOMIC_RELAXED);\n', '', 1)]),
    ('pending-interrupt-ignored', [('if (mrs(ISR_EL1) || !hv_fb_idle_due())', 'if (!hv_fb_idle_due())', 1)]),
    ('conversion-changed', [('(((p >> 8) & 0xff) << 12)', '(((p >> 8) & 0xff) << 13)', 2)]),  # 1 = old, 2 = new, 3 = e2e
    ('cost-line-dropped', [('printf("HV: fb cost:', 'if (0) printf("HV: fb cost:', 1)]),
    # The start-up self-test.
    ('selftest-skipped', [('if (hv_fb_source_cached && !hv_fb_selftest()) {', 'if (0 && !hv_fb_selftest()) {', 1)]),
    ('selftest-verdict-ignored', [('    if (failed) {\n        hv_fb_source = hv_fb_shadow;\n        hv_fb_source_cached = false;\n    }\n', '', 1)]),
    ('selftest-check-unmaintained', [('    if (op == HV_FB_ST_CHECK) {\n', '    if (0) {\n', 1)]),
    ('selftest-same-core-unchecked', [('        failed = "same core";\n', '        (void)0;\n', 1)]),
    ('selftest-cross-cases-skipped', [(CROSS, 'if (0 && !hv_fb_selftest_cross(&t, ', 0)]),
    ('selftest-cross-cluster-cases-skipped', [(CROSS + 'other[0], same[0], boot,', 'if (0 && !hv_fb_selftest_cross(&t, other[0], same[0], boot,', 1),
                                              (CROSS + 'same[0], other[0], other[1],', 'if (0 && !hv_fb_selftest_cross(&t, same[0], other[0], other[1],', 1)]),
    ('selftest-region-smaller', [('#define HV_FB_SELFTEST_BYTES SZ_1M', '#define HV_FB_SELFTEST_BYTES SZ_16K', 1)]),
    ('selftest-region-left-dirty', [('        t.nc[i] = 0;\n', '        (void)0;\n', 1)]),
    ('selftest-zero-not-maintained', [('    dc_civac_range((void *)t.wb, HV_FB_SELFTEST_BYTES);\n', '', 1)]),
    ('selftest-boot-not-flushed-on-failure', [('        dcsw_op_all(DCSW_OP_DCCISW);', '        (void)0;', 1)]),
    ('selftest-secondary-mmu-left-on', [('hv_fb_selftest_remote(&t, parts[i], mmu_disable, 0, 0, 0);', '(void)0;', 1)]),
    ('selftest-mmu-restore-ignores-prior-state', [('        if (was_on[i])\n            continue;\n', '', 1)]),
    ('selftest-unguarded', [('    exc_guard = GUARD_MARK | GUARD_SILENT;\n', '', 1)]),
    ('selftest-exception-ignored', [('if (!failed && exc_count != before)', 'if (!failed && exc_count < before)', 1)]),
    ('selftest-guest-cpus-used', [(' || hv_started_cpus[cpu] || hv_setup_pending[cpu]', '', 1)]),
    ('selftest-unbounded-wait', [('rc = smp_wait_timed(cpu, &ret, 2000);', 'rc = smp_wait_timed(cpu, &ret, 300000);', 1)]),
    ('selftest-calls-after-failure', [('    if (!t->ok)\n        return ~0UL;\n    int rc', '    int rc', 1)]),
    ('selftest-hang-not-refused', [('if (hv_fb_source_cached && !hv_fb_selftest()) {', 'if (hv_fb_source_cached && !hv_fb_selftest() && !hv_fb_shadow) {', 1)]),
]


def sha(data):
    return hashlib.sha256(data if isinstance(data, bytes) else pathlib.Path(data).read_bytes()).hexdigest()


def function(text, name):
    m = re.search(r'^(?:static )?(?:inline )?(?:void|bool|u64|u32|int|char) ' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
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


def mutate(text, edits):
    for old, new, n in edits:
        positions = [m.start() for m in re.finditer(re.escape(old), text)]
        if not positions:
            raise ValueError('mutation anchor missing: ' + old)
        for pos in sorted(positions if n == 0 else [positions[n - 1] if n > 0 else positions[n]], reverse=True):
            text = text[:pos] + new + text[pos + len(old):]
    return text


def build(directory, code, cc, sdk, receipt):
    directory.mkdir()
    (directory / 'production.c').write_text(code)
    exe = directory / 'replay'
    cmd = [cc, '-isysroot', sdk, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
           '-Wno-sign-compare', '-Wno-format', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           str(directory / 'production.c'), '-o', str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    (directory / 'build.log').write_text(result.stdout + result.stderr)
    receipt['commands'].append({'argv': cmd, 'returncode': result.returncode})
    return exe if not result.returncode else None


def run(directory, exe, variant, receipt):
    result = subprocess.run([str(exe), variant], capture_output=True, text=True, timeout=300)
    (directory / (variant + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'argv': [str(exe), variant], 'returncode': result.returncode})
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    out = pathlib.Path(ap.parse_args().output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    old_hv = subprocess.check_output(['git', '-C', str(ROOT), 'show', BASE + ':src/hv/windows/hv.c'])
    inputs = [HV, HV_H, HERE / 'host.h', HERE / 'e2e.c', pathlib.Path(__file__), ROOT / 'docs/windows-fb-cached-read.md']
    receipt = {'hardware_accessed': False, 'source_sha256': {str(p): sha(p) for p in inputs},
               'previous_converter': {'commit': BASE, 'hv_c_sha256': sha(old_hv)}, 'functions': FUNCTIONS,
               'variants': VARIANTS,
               'scope': 'verbatim production framebuffer functions and start-up self-test; DRAM, per-CPU caches of '
                        'the write-back alias (coherent fills), five maintenance behaviours, secondaries, their MMU, '
                        'faults, hangs, guest writes, clock and interrupts modeled; no hardware timing or panel claim',
               'commands': [], 'checks': {}, 'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun', '--find', 'clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip()
        receipt['compiler'] = {'path': cc, 'sha256': sha(cc), 'sdk': sdk}
        text, header = HV.read_text(), HV_H.read_text()
        code = (HERE / 'host.h').read_text() + '\n'
        code += ''.join(re.search(r'^#define ' + d + r' .*\n', header, re.M).group(0) for d in DEFINES) + '\n'
        statics = re.search(r'^static u32 \*hv_fb_shadow;\n.*?^static struct hv_fb_cost hv_fb_cost\[MAX_CPUS\];\n',
                            text, re.M | re.S)
        selftest = re.search(r'^#define HV_FB_SELFTEST_BYTES .*?^};\n', text, re.M | re.S)
        if not statics or not selftest:
            raise ValueError('framebuffer state or self-test declarations not found')
        code += statics.group(0) + '\n' + selftest.group(0) + '\n'
        old = function(old_hv.decode(), 'hv_fb_convert_slice').replace('void hv_fb_convert_slice(',
                                                                        'void old_hv_fb_convert_slice(')
        for name in ('shadow', 'scanout', 'stride_px', 'height', 'cursor', 'lines'):
            old = re.sub(r'\bhv_fb_' + name + r'\b', 'old_fb_' + name, old)
        code += old + '\n\n'
        code += '\n\n'.join(function(text, n) for n in FUNCTIONS) + '\n\n'
        code += (HERE / 'e2e.c').read_text()

        exe = build(out / 'actual', code, cc, sdk, receipt)
        if exe is None:
            raise RuntimeError('actual source failed to build: ' + (out / 'actual/build.log').read_text()[-600:])
        for variant in VARIANTS:
            result = run(out / 'actual', exe, variant, receipt)
            if result.returncode:
                raise RuntimeError('actual source failed variant %s: %s' % (variant, result.stderr[-600:]))
            receipt['checks'][variant] = [l[5:] for l in result.stdout.splitlines() if l.startswith('PASS ')]
        for name, edits in MUTATIONS:
            directory = out / ('mutant-' + name)
            exe = build(directory, mutate(code, edits), cc, sdk, receipt)
            if exe is None:
                raise RuntimeError(name + ' mutant did not build')
            rejected = {}
            for variant in VARIANTS:
                result = run(directory, exe, variant, receipt)
                if result.returncode:
                    rejected[variant] = next((l for l in result.stderr.splitlines() if l.startswith('FAIL ')),
                                             'exit %d' % result.returncode)
            if not rejected:
                raise RuntimeError(name + ' mutant was not rejected')
            receipt['mutations'].append({'name': name, 'rejected': True, 'rejected_by': rejected})
        assert all(sha(p) == h for p, h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict'] = 'PASS'
    except Exception as e:
        receipt['verdict'] = 'FAIL'
        receipt['error'] = str(e)
    receipt['artifacts_sha256'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*'))
                                   if p.is_file() and p.name != 'replay' and '.dSYM' not in str(p)}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'verdict': receipt['verdict'], 'variants': len(receipt['checks']),
                      'checks': sum(len(c) for c in receipt['checks'].values()),
                      'mutants_rejected': len(receipt['mutations'])} | ({'error': receipt['error']} if 'error' in receipt else {})))
    return receipt['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
