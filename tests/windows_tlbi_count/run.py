#!/usr/bin/env python3
"""Replay the guest TLBI trap count from the actual source; no guest, proxy or hardware.

Compiles, verbatim, the production ISS macros, the per-CPU record, the counter
hv_count_tlbi_trap() and the exit report hv_report_tlbi_traps() from
src/hv/windows/hv_exc.c, with the rewrite's own case labels and the handler's
own ISS mask statement read out of hv_handle_msr_unlocked(). Models only the
CPU number and the console. Checks the call sites in the source text, runs the
replay under ASan/UBSan, and requires every listed mutant to be rejected.
Failure modes: docs/windows-tlbi-trap-count.md.
"""
import argparse, hashlib, json, pathlib, re, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXC = 'src/hv/windows/hv_exc.c'
HV = 'src/hv/windows/hv.c'
NAME_RULE = re.compile(r'wifi|wlan|wpa|password|passwd|psk|credential|secret', re.I)
MUTATIONS = [
    ('os-form-dropped', '        case SYSREG_ISS(sys_reg(1, 0, 8, 5, 1)): /* RVAE1OS */\n', ''),
    ('is-range-forms-as-other', '(crm == 2 || crm == 3)', '(crm == 3)'),
    ('nxs-not-counted', '(crn != 8 && crn != 9)', '(crn != 8)'),
    ('op0-ignored', 'FIELD_GET(ESR_ISS_MSR_OP0, reg) != 1 || ', ''),
    ('one-shared-slot', '&hv_tlbi_traps[cpu];', '&hv_tlbi_traps[0];'),
    ('no-bounds-check', '    if (cpu >= MAX_CPUS)\n        return;\n', ''),
    ('printed-twice', '    if (reported)\n', '    if (false && reported)\n'),
    ('sum-skips-cpu0', 'for (int cpu = 0; cpu < MAX_CPUS; cpu++) {', 'for (int cpu = 1; cpu < MAX_CPUS; cpu++) {'),
    ('other-printed-as-is', 'os, is, other);', 'os, is, other - other + is);'),
]


def sha(data):
    return hashlib.sha256(data if isinstance(data, bytes) else data.encode()).hexdigest()


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


def macro(text, name, contains=''):
    found = [m.group(0) for m in re.finditer(r'^#define ' + re.escape(name) + r'\b(?:[^\n]*\\\n)*[^\n]*\n', text, re.M)
             if contains in m.group(0)]
    if len(found) != 1:
        raise ValueError('macro %s: %d definitions' % (name, len(found)))
    return found[0]


def record(text):
    m = re.search(r'^struct hv_tlbi_traps \{.*?^\} ALIGNED\(64\);\n\nstatic struct hv_tlbi_traps hv_tlbi_traps\[MAX_CPUS\];\n',
                  text, re.M | re.S)
    if not m:
        raise ValueError('record not found')
    return m.group(0)


def rewrite_block(handler):
    start = handler.rindex('/*', 0, handler.index('Outer-shareable TLBI operations trap'))
    return handler[start:handler.index('case SYSREG_ISS(SYS_ACTLR_EL1)', start)]


def source_checks(exc, hv, hv_h, sources):
    """Call sites and side effects, read from the source text (F2, F3, F8, F9, F12, F13)."""
    checks = []

    def need(ok, name):
        if not ok:
            raise RuntimeError('source check failed: ' + name)
        checks.append(name)

    handler = function(exc, 'hv_handle_msr_unlocked')
    call = '    hv_count_tlbi_trap(reg);\n'
    need(handler.count(call) == 1, 'the unlocked MSR handler calls the counter once')
    head = handler[:handler.index(call)]
    need('regs[31] = 0;' in head and 'return' not in head and 'if (' not in head and
         handler.index(call) < handler.index('if (hv_handle_software_pmu('),
         'the counter runs first, before any dispatch or return')
    counter = function(exc, 'hv_count_tlbi_trap')
    need(counter.startswith('static inline void hv_count_tlbi_trap(u64 reg)'), 'the counter takes the encoding by value and returns nothing')
    body = counter[counter.index('{'):]
    need(not re.search(r'\b(msr|mrs|_msr|_mrs|sysop|printf|spin_lock|spin_unlock|regs|dsb|isb|tlbi)\b', body),
         'the counter writes no system register, prints nothing, takes no lock, touches no guest register')
    need('hv_count_tlbi_trap' not in function(exc, 'hv_handle_msr') and
         'hv_count_tlbi_trap' not in function(exc, 'hv_t8142_sysreg_assist_impl'),
         'neither the locked handler nor the T8142 assist counts')
    uses = {p: t.count('hv_count_tlbi_trap(') for p, t in sources.items() if 'hv_count_tlbi_trap(' in t}
    need(uses == {EXC: 2}, 'the counter exists only in hv_exc.c: its definition and that one call')
    reports = {p: t.count('hv_report_tlbi_traps(') for p, t in sources.items() if 'hv_report_tlbi_traps(' in t}
    need(reports == {EXC: 1, HV: 1, 'src/hv/windows/hv.h': 1}, 'one definition, one declaration, one call')
    start = function(hv, 'hv_start')
    site = re.search(r'\n    \}\n(?:    /\*[^\n]*\*/\n)?    hv_report_tlbi_traps\(\);\n    return result \? result :', start)
    need(start.count('hv_report_tlbi_traps();') == 1 and site is not None and
         start.index('int result = hv_join_cpus();') < start.index('printf("HV: All CPUs exited\\n");') < site.start(),
         'printed by the boot CPU in hv_start, after the join and after "HV: All CPUs exited", drained or not')
    report = function(exc, 'hv_report_tlbi_traps')
    need('spin_lock' not in report and report.count('printf(') == 1, 'the report takes no lock and prints one line')
    return checks


def build_and_run(directory, code, cc, sdk, receipt):
    directory.mkdir()
    (directory / 'production.c').write_text(code)
    exe = directory / 'replay'
    cmd = [cc, '-isysroot', sdk, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-pthread',
           str(directory / 'production.c'), '-o', str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (directory / 'build.log').write_text(build.stdout + build.stderr)
    receipt['commands'].append({'argv': cmd, 'returncode': build.returncode})
    if build.returncode:
        return None
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    (directory / 'run.log').write_text(run.stdout + run.stderr)
    receipt['commands'].append({'argv': [str(exe)], 'returncode': run.returncode})
    return run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    out = pathlib.Path(ap.parse_args().output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / EXC, ROOT / HV, ROOT / 'src/hv/windows/hv.h', ROOT / 'src/types.h', ROOT / 'src/arm_cpu_regs.h',
              ROOT / 'src/smp_asm.h', HERE / 'host.h', HERE / 'e2e.c', pathlib.Path(__file__),
              ROOT / 'docs/windows-tlbi-trap-count.md']
    receipt = {'hardware_accessed': False,
               'scope': 'verbatim production TLBI trap record, counter, exit report and ISS macros; rewrite labels '
                        'and the handler ISS mask read from hv_handle_msr_unlocked(); modeled CPU number and console',
               'source_sha256': {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in inputs},
               'commands': [], 'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun', '--find', 'clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip()
        receipt['compiler'] = {'path': str(pathlib.Path(cc).resolve()), 'sha256': sha(pathlib.Path(cc).resolve().read_bytes()), 'sdk': sdk}
        exc = (ROOT / EXC).read_text()
        hv = (ROOT / HV).read_text()
        hv_h = (ROOT / 'src/hv/windows/hv.h').read_text()
        types = (ROOT / 'src/types.h').read_text()
        regs = (ROOT / 'src/arm_cpu_regs.h').read_text()
        sources = {}
        for p in sorted((ROOT / 'src').rglob('*')):
            rel = p.relative_to(ROOT)
            if p.is_file() and p.suffix in ('.c', '.h', '.S') and not any(NAME_RULE.search(part) for part in rel.parts):
                sources[str(rel)] = p.read_text(errors='replace')
        receipt['source_checks'] = source_checks(exc, hv, hv_h, sources)

        handler = function(exc, 'hv_handle_msr_unlocked')
        block = rewrite_block(handler)
        forms = re.findall(r'case SYSREG_ISS\(sys_reg\((\d+), (\d+), (\d+), (\d+), (\d+)\)\)', block) + \
                re.findall(r'SYSREG_MAP\(sys_reg\((\d+), (\d+), (\d+), (\d+), (\d+)\),', block)
        if not forms:
            raise ValueError('no rewrite labels found')
        receipt['rewrite_block_sha256'] = sha(block)
        receipt['rewrite_forms'] = [list(map(int, f)) for f in forms]
        mask = re.search(r'^    u64 reg = iss & \(.*?\);\n', handler, re.M | re.S).group(0)

        code = (HERE / 'host.h').read_text() + '\n'
        code += ''.join(macro(types, n) for n in ('ALIGNED', 'BIT', 'GENMASK', '_FIELD_LSB', 'FIELD_GET'))
        code += macro(types, 'sys_reg', ', _S,')
        code += ''.join(m.group(0) for m in re.finditer(r'^#define ESR_ISS_MSR_\w+ [^\n]*\n', regs, re.M))
        code += macro(regs, '_SYSREG_ISS') + macro(regs, 'SYSREG_ISS')
        code += macro((ROOT / 'src/smp_asm.h').read_text(), 'MAX_CPUS') + '\n'
        code += 'static const unsigned rewrite_forms[][5] = {%s};\n#define REWRITE_FORMS %du\n\n' % (
            ', '.join('{%s}' % ', '.join(f) for f in forms), len(forms))
        code += 'static u64 handler_reg(u64 iss)\n{\n' + mask + '    return reg;\n}\n\n'
        code += record(exc) + '\n'
        code += function(exc, 'hv_count_tlbi_trap') + '\n\n' + function(exc, 'hv_report_tlbi_traps') + '\n\n'
        code += (HERE / 'e2e.c').read_text()

        run = build_and_run(out / 'actual', code, cc, sdk, receipt)
        if run is None:
            raise RuntimeError('actual source did not compile: ' + (out / 'actual' / 'build.log').read_text()[-600:])
        if run.returncode:
            raise RuntimeError('actual source replay failed: ' + run.stderr.strip()[-400:])
        receipt['checks'] = [s[5:] for s in run.stdout.splitlines() if s.startswith('PASS ')]
        receipt['console'] = run.stdout.split('CONSOLE\n', 1)[1].rsplit('CHECKS ', 1)[0]

        for name, old, new in MUTATIONS:
            assert code.count(old) == 1, name + ': mutation anchor not unique in assembled source'
            run = build_and_run(out / ('mutant-' + name), code.replace(old, new, 1), cc, sdk, receipt)
            if run is None:
                raise RuntimeError(name + ' mutant did not compile')
            if not run.returncode:
                raise RuntimeError(name + ' mutant was not rejected')
            reason = next((l for l in run.stderr.splitlines() if l.startswith('FAIL ') or 'ERROR: AddressSanitizer' in l),
                          run.stderr.strip()[-200:])
            receipt['mutations'].append({'name': name, 'rejected': True, 'reason': reason})

        assert all(sha((ROOT / p).read_bytes()) == h for p, h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict'] = 'PASS'
    except Exception as e:
        receipt['verdict'] = 'FAIL'
        receipt['error'] = str(e)
    receipt['artifacts_sha256'] = {str(p.relative_to(out)): sha(p.read_bytes()) for p in sorted(out.rglob('*'))
                                   if p.is_file() and p.name != 'replay' and '.dSYM' not in str(p)}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ['verdict', 'error'] if k in receipt} |
                     {'source_checks': len(receipt.get('source_checks', [])), 'checks': len(receipt.get('checks', [])),
                      'mutants_rejected': len(receipt['mutations'])}))
    return receipt['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
