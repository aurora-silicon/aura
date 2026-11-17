#!/usr/bin/env python3
"""Compile the actual EL2 accounting, report, FIQ, WFI and timer functions; model only clocks,
registers, locks and costs. Every reported number is checked against modeled time."""
import argparse, hashlib, json, pathlib, re, shutil, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HV = 'src/hv/windows/hv.c'
EXC = 'src/hv/windows/hv_exc.c'
FUNCTIONS = {
    HV: ['hv_host_counter', 'hv_mask_pending_tick', 'hv_arm_wfi_wake', 'hv_arm_tick'],
    EXC: ['hv_el2_add', 'hv_el2_enter', 'hv_el2_exit', 'hv_el2_cause', 'hv_el2_site_cause',
          'hv_el2_lock_acquired', 'hv_el2_lock_released', 'hv_el2_us', 'hv_report_t8142_el2_acct',
          'hv_exc_entry', 'hv_exc_exit', 'hv_handle_wfx', 'hv_exc_fiq'],
}
HEADERS = ['src/hv/windows/tick_deadline.h', 'src/hv/windows/hv_el2_acct.h']
MUTATIONS = [
    ('lock-wait-double-counted', 'hv_el2_add(&a->ticks[cause], stay - wait);', 'hv_el2_add(&a->ticks[cause], stay);'),
    ('lock-wait-leaks-between-stays', '    a->lock_wait = 0;\n', ''),
    ('guest-time-counted-as-el2', '    a->entered = hv_host_counter();\n', ''),
    ('tick-unclassified', '        hv_el2_cause(HV_EL2_TICK);\n', ''),
    ('wfi-unclassified', '    hv_el2_cause(HV_EL2_WFI);\n', ''),
    ('icc-counted-as-absent-register', 'bool icc = (key & 0x7f80) == 0x4600', 'bool icc = false && (key & 0x7f80) == 0x4600'),
    ('no-baseline', '        if (baseline)\n            continue;\n', ''),
    ('max-never-reset', '__atomic_exchange_n(&a->longest, 0, __ATOMIC_RELAXED)', '__atomic_load_n(&a->longest, __ATOMIC_RELAXED)'),
    ('lock-wait-unmeasured', "    hv_el2_lock_acquired(lock_started);\n    hv_wdt_breadcrumb('X');", "    (void)lock_started;\n    hv_wdt_breadcrumb('X');"),
    ('hold-never-released', '    hv_el2_lock_released();\n    spin_unlock(&bhl);', '    spin_unlock(&bhl);'),
    ('no-flood-guard', 'if (!baseline && now - last_report < freq / 2)', 'if (false)'),
    ('boot-cpu-omitted', 'if (cpu != boot_cpu_idx && !smp_is_alive(cpu))', 'if (!smp_is_alive(cpu))'),
    ('microseconds-assume-1ghz', 'return (ticks / freq) * 1000000 + ((ticks % freq) * 1000000) / freq;', 'return ticks / 1000 + 0 * freq;'),
]


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


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


def record(text):
    m = re.search(r'^struct hv_el2_acct \{.*?^\} ALIGNED\(64\);\n\nstatic struct hv_el2_acct hv_el2_acct\[MAX_CPUS\];\n', text, re.M | re.S)
    if not m:
        raise ValueError('accounting record not found')
    return m.group(0)


def build_and_run(directory, code, cc, sdk, receipt):
    directory.mkdir()
    (directory / 'production.c').write_text(code)
    for header in HEADERS:
        shutil.copy2(ROOT / header, directory / pathlib.Path(header).name)
    exe = directory / 'replay'
    cmd = [cc, '-isysroot', sdk, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-function', '-Wno-sign-compare', '-Wno-format', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-I', str(directory), str(directory / 'production.c'), '-o', str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (directory / 'build.log').write_text(build.stdout + build.stderr)
    receipt['commands'].append({'argv': cmd, 'returncode': build.returncode})
    if build.returncode:
        return None
    runs = {}
    for scenario in ['1ghz', '24mhz']:
        run = subprocess.run([str(exe), scenario], capture_output=True, text=True, timeout=120)
        (directory / ('run-' + scenario + '.log')).write_text(run.stdout + run.stderr)
        receipt['commands'].append({'argv': [str(exe), scenario], 'returncode': run.returncode})
        runs[scenario] = run
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    out = pathlib.Path(ap.parse_args().output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in FUNCTIONS] + [ROOT / h for h in HEADERS] + \
             [HERE / 'host.h', HERE / 'e2e.c', pathlib.Path(__file__), ROOT / 'docs/windows-el2-accounting.md']
    receipt = {'hardware_accessed': False,
               'scope': 'verbatim production EL2 accounting, report, FIQ, WFI, bhl entry/exit and timer functions; '
                        'modeled per-CPU clocks, registers, lock contention and handler costs; no hardware timing claim',
               'source_sha256': {str(p): sha(p) for p in inputs}, 'functions': FUNCTIONS,
               'commands': [], 'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun', '--find', 'clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip()
        receipt['compiler'] = {'path': str(pathlib.Path(cc).resolve()), 'sha256': sha(pathlib.Path(cc).resolve()), 'sdk': sdk}
        code = (HERE / 'host.h').read_text() + '\n'
        hv_text = (ROOT / HV).read_text()
        exc_text = (ROOT / EXC).read_text()
        code += '\n\n'.join(function(hv_text, n) for n in FUNCTIONS[HV]) + '\n\n'
        code += record(exc_text) + '\n'
        code += '\n\n'.join(function(exc_text, n) for n in FUNCTIONS[EXC]) + '\n\n'
        code += (HERE / 'e2e.c').read_text()

        runs = build_and_run(out / 'actual', code, cc, sdk, receipt)
        if runs is None:
            raise RuntimeError('actual source did not compile')
        for scenario, run in runs.items():
            if run.returncode:
                raise RuntimeError('actual source replay failed (%s): %s' % (scenario, run.stderr.strip()[-400:]))
        checks = [s[5:] for s in runs['1ghz'].stdout.splitlines() if s.startswith('PASS ')]
        console = runs['1ghz'].stdout.split('CONSOLE\n', 1)[1].rsplit('CHECKS ', 1)[0]
        (out / 'console-1ghz.txt').write_text(console)
        receipt['checks_1ghz'] = len(checks)
        receipt['checks_24mhz'] = sum(1 for s in runs['24mhz'].stdout.splitlines() if s.startswith('PASS '))
        receipt['distinct_checks'] = sorted(set(checks))
        receipt['report_lines'] = console.count('\n')
        receipt['example_lines'] = [l for l in console.splitlines() if l.startswith('HV: el2 cpu7 ') or l.startswith('HV: el2 cpu3 ')][:4]

        for name, old, new in MUTATIONS:
            assert code.count(old) == 1, name + ': mutation anchor not unique in assembled source'
            runs = build_and_run(out / ('mutant-' + name), code.replace(old, new, 1), cc, sdk, receipt)
            if runs is None:
                raise RuntimeError(name + ' mutant did not compile')
            failed = {s: r for s, r in runs.items() if r.returncode}
            if not failed:
                raise RuntimeError(name + ' mutant was not rejected')
            scenario, run = next(iter(failed.items()))
            reason = next((l for l in run.stderr.splitlines() if l.startswith('FAIL ')), run.stderr.strip()[-200:])
            receipt['mutations'].append({'name': name, 'rejected': True, 'scenario': scenario, 'reason': reason})

        assert all(sha(p) == h for p, h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict'] = 'PASS'
    except Exception as e:
        receipt['verdict'] = 'FAIL'
        receipt['error'] = str(e)
    receipt['artifacts_sha256'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*'))
                                   if p.is_file() and p.name != 'replay' and '.dSYM' not in str(p)}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ['verdict', 'checks_1ghz', 'checks_24mhz', 'report_lines', 'error']
                      if k in receipt} | {'mutants_rejected': len(receipt['mutations'])}))
    return receipt['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
