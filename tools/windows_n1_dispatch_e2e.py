#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay actual N1 HVC dispatch/entry/exit and census without target access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['src/hv/windows/hv_exc.c', 'src/hv/windows/hv_asm.S',
           'src/hv/windows/windows_n1.c', 'src/hv/windows/windows_n1.h',
           'src/exception.h', 'src/types.h', 'src/arm_cpu_regs.h', 'src/cpu_regs.h',
           'tests/e2e/windows_n1/census_dispatch_host.c',
           'tests/e2e/windows_n1/failure-model.md', 'tools/windows_n1_dispatch_e2e.py']
CASES = ['relink-routed', 'success', 'pending', 'wrong-cpu', 'unprepared', 'guard-busy',
         'invalid-address', 'invalid-version', 'reserved-argument', 'read-fault',
         'unrelated-hvc', 'nonzero-immediate', 'fast-msr', 'slow-msr']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(text, name):
    match = re.search(r'(?m)^(?:static )?(?:inline )?(?:bool|void|int)\s+' + name +
                      r'\([^\n]*\)\n\{', text)
    assert match, name
    cursor, depth = match.end(), 1
    while depth:
        if text[cursor] == '{': depth += 1
        if text[cursor] == '}': depth -= 1
        cursor += 1
    return text[match.start():cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--simulate-incomplete-slow-move', action='store_true',
                        help='baseline-only counterexample: move N1 dispatch but leave ELR+4')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'result': 'failed', 'hardware_accessed': False, 'cases': [],
              'counterexample': args.simulate_incomplete_slow_move,
              'sources': {}, 'extracted_functions': {}, 'commands': [],
              'limits': ['Architectural entry registers and peripheral services are modeled.',
                         'Actual dispatcher, HV entry/exit and guarded N1 read bodies are compiled.',
                         'No physical exception delivery, guest firmware or Windows proof.']}
    try:
        for relative in SOURCES:
            target = out / 'source' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
            report['sources'][relative] = sha(target)
        hv = (out / 'source/src/hv/windows/hv_exc.c').read_text()
        n1 = (out / 'source/src/hv/windows/windows_n1.c').read_text()
        bodies = []
        for text, names in [(n1, ['n1_read', 'n1_census_address', 'hv_windows_n1_census_hvc']),
                            (hv, ['hv_get_context', 'hv_exc_entry', 'hv_exc_exit', 'hv_exc_sync'])]:
            for name in names:
                body = extract(text, name)
                report['extracted_functions'][name] = hashlib.sha256(body.encode()).hexdigest()
                if name == 'hv_exc_sync' and args.simulate_incomplete_slow_move:
                    before = ('            handled = (FIELD_GET(ESR_ISS, ctx->esr) == 0 && '
                              'hv_windows_n1_census_hvc(ctx->regs)) ||\n'
                              '                      hv_handle_t8142_undef_trampoline(ctx) || hv_handle_smc(ctx);')
                    after = ('            if (FIELD_GET(ESR_ISS, ctx->esr) == 0 &&\n'
                             '                ctx->regs[0] == HV_WINDOWS_N1_READ_CALL)\n'
                             '                break;\n'
                             '            handled = hv_handle_t8142_undef_trampoline(ctx) || hv_handle_smc(ctx);')
                    assert before in body, 'counterexample requires the original unlocked dispatcher'
                    body = body.replace(before, after)
                    before = '    hv_exc_entry();\n\n    switch (ec) {\n'
                    assert before in body
                    body = body.replace(before, before + '        case ESR_EC_HVC:\n'
                                        '            handled = FIELD_GET(ESR_ISS, ctx->esr) == 0 &&\n'
                                        '                      hv_windows_n1_census_hvc(ctx->regs);\n'
                                        '            break;\n')
                bodies.append(body)
        (out / 'dispatch-production.h').write_text('\n\n'.join(bodies) + '\n')
        wanted = {'ESR_EC', 'ESR_ISS', 'ESR_EC_MSR', 'ESR_EC_WFI', 'ESR_EC_HVC', 'ESR_EC_SMC',
                  'ESR_EC_IMPDEF', 'ESR_ISS_IMPDEF_MSR', 'ESR_EC_DABORT_LOWER', 'ESR_EC_BRK', 'ESR_EC_SERROR'}
        definitions = {}
        for relative in ['src/arm_cpu_regs.h', 'src/cpu_regs.h']:
            for line in (out / 'source' / relative).read_text().splitlines():
                match = re.match(r'#define\s+(\w+)\s+', line)
                if match and match[1] in wanted: definitions[match[1]] = line
        assert set(definitions) == wanted
        (out / 'dispatch-registers.h').write_text('\n'.join(definitions.values()) + '\n')
        compiler = shutil.which('clang')
        argv = [compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(out), '-I' + str(out / 'source/src'),
                str(out / 'source/tests/e2e/windows_n1/census_dispatch_host.c'), '-o', str(out / 'dispatch-host')]
        report['commands'].append(argv)
        result = subprocess.run(argv, text=True, capture_output=True, timeout=120)
        (out / 'compile.log').write_text(result.stdout + result.stderr)
        if result.returncode: raise RuntimeError('compile failed; see compile.log')
        env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for case in CASES:
            result = subprocess.run([str(out / 'dispatch-host'), case], cwd=out, env=env,
                                    text=True, capture_output=True, timeout=10)
            log = out / (case + '.log')
            log.write_text(result.stdout + result.stderr)
            report['cases'].append({'case': case, 'returncode': result.returncode,
                                    'transcript_sha256': sha(log)})
            if result.returncode: raise RuntimeError(case + ' rejected expected dispatch contract')
        report['result'] = 'passed'
    except (OSError, RuntimeError, AssertionError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
    finally:
        report['artifacts'] = {p.name: sha(p) for p in out.glob('*') if p.is_file()}
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': report['result'], 'cases': len(report['cases']),
                      'error': report.get('error'), 'receipt': str(out / 'result.json')}))
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
