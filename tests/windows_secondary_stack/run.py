#!/usr/bin/env python3
"""Assemble the production hv_enter_guest_on_stack / hv_enter_guest / hv_exit_guest for the arm64
host (only EL2-only instructions modeled) and run them natively; reject trampoline mutations."""
import argparse, hashlib, json, pathlib, re, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ASM = ROOT / 'src/hv/windows/hv_asm.S'
NAMES = ['hv_enter_guest', 'hv_enter_guest_on_stack', 'hv_exit_guest']
SWITCH = ('    adrp x16, _model_daif@PAGE\n    ldr x17, [x16, _model_daif@PAGEOFF]\n'
          '    adrp x16, _switch_count@PAGE\n    ldr x15, [x16, _switch_count@PAGEOFF]\n'
          '    add x14, x15, #1\n    str x14, [x16, _switch_count@PAGEOFF]\n'
          '    adrp x16, _switch_daif@PAGE\n    add x16, x16, _switch_daif@PAGEOFF\n'
          '    str x17, [x16, x15, lsl #3]\n')
MUTATIONS = [  # (name, old, new, occurrence) applied to the trampoline block only
    ('guest-entered-on-old-stack', '    mov sp, x5\n', '', 1),
    ('sp-not-restored-after-exit', '    mov sp, x19\n', '', 1),
    ('old-sp-kept-in-caller-saved-register', 'mov x19, sp', 'mov x9, sp', 1),
    ('entry-switch-unmasked', '    msr daifset, #0xf\n', '', 1),
    ('return-switch-unmasked', '    msr daifset, #0xf\n', '', 2),
    ('guest-gets-the-mask-as-pstate', '    msr daif, x6\n', '', 1),
    ('caller-daif-not-restored', '    msr daif, x6\n', '', 2),
]


def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()


def block(text, name):
    m = re.search(r'^\.globl ' + re.escape(name) + r'\n(.*?)(?=^\.globl |^/\*|\Z)', text, re.M | re.S)
    if not m:
        raise ValueError('asm block not found: ' + name)
    return m.group(0)


def replace_nth(text, old, new, n):
    pos = -1
    for _ in range(n):
        pos = text.find(old, pos + 1)
        if pos < 0:
            raise ValueError('mutation anchor missing: ' + old)
    return text[:pos] + new + text[pos + len(old):]


def to_host(src, name):
    out = []
    for line in src.splitlines():
        s = line.strip()
        if s.startswith('.type'):
            continue
        m = re.fullmatch(r'mrs (x\d+), (tpidr_el2|daif)', s)
        if m:
            var = '_model_cpu' if m.group(2) == 'tpidr_el2' else '_model_daif'
            out += [f'    adrp {m.group(1)}, {var}@PAGE', f'    ldr {m.group(1)}, [{m.group(1)}, {var}@PAGEOFF]']
            continue
        m = re.fullmatch(r'msr (daif|spsr_el2|elr_el2), (x\d+)', s)
        if m:
            var = {'daif': '_model_daif', 'spsr_el2': '_model_spsr', 'elr_el2': '_model_elr'}[m.group(1)]
            out += [f'    adrp x16, {var}@PAGE', f'    str {m.group(2)}, [x16, {var}@PAGEOFF]']
            continue
        if s == 'msr daifset, #0xf':
            out += ['    adrp x16, _model_daif@PAGE', '    ldr x17, [x16, _model_daif@PAGEOFF]',
                    '    orr x17, x17, #0x3c0', '    str x17, [x16, _model_daif@PAGEOFF]']
            continue
        if re.fullmatch(r'msr sp_el[01], x\d+', s):
            continue
        if s == 'eret':
            out.append('    br x4')
            continue
        if s == 'ldr x6, =hv_saved_sp':
            out += ['    adrp x6, _hv_saved_sp@PAGE', '    add x6, x6, _hv_saved_sp@PAGEOFF']
            continue
        line = re.sub(r'^\.globl (\w+)', r'.globl _\1', line)
        line = re.sub(r'^(\w+):', r'_\1:', line)
        line = re.sub(r'\bbl (hv_\w+)', r'bl _\1', line)
        if re.search(r'\b(mrs|msr|eret|tpidr|daif|_el[0-3])\b', line.split('//')[0]) and not line.lstrip().startswith(('/*', '*')):
            raise ValueError('unmodeled EL2 instruction in ' + name + ': ' + line)
        out.append(line)
        if name == 'hv_enter_guest_on_stack' and re.fullmatch(r'mov sp, x\d+', s):
            out.append(SWITCH.rstrip('\n'))
    return '\n'.join(out) + '\n'


def build_and_run(directory, blocks, cc, sdk, receipt):
    directory.mkdir()
    prod = '.text\n.p2align 2\n' + ''.join(to_host(blocks[n], n) for n in NAMES)
    (directory / 'production-host.S').write_text(prod)
    exe = directory / 'replay'
    cmd = [cc, '-isysroot', sdk, '-arch', 'arm64', '-O1', '-g', '-Wall', '-Wextra', '-Werror', str(HERE / 'host.c'),
           str(HERE / 'glue.S'), str(directory / 'production-host.S'), '-o', str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (directory / 'build.log').write_text(build.stdout + build.stderr)
    receipt['commands'].append({'argv': cmd, 'returncode': build.returncode})
    if build.returncode:
        return None
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    (directory / 'run.log').write_text(run.stdout + run.stderr)
    receipt['commands'].append({'argv': [str(exe)], 'returncode': run.returncode})
    return run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    out = pathlib.Path(ap.parse_args().output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = [ASM, HERE / 'host.c', HERE / 'glue.S', pathlib.Path(__file__), ROOT / 'docs/windows-secondary-el2-stack.md']
    receipt = {'hardware_accessed': False, 'source_sha256': {str(p): sha(p) for p in inputs}, 'commands': [],
               'scope': 'production hv_asm.S enter/exit/trampoline run natively on the arm64 host; EL2 system '
                        'registers and eret modeled; no claim about device-memory cost or EL2 behaviour',
               'mutations': []}
    try:
        cc = subprocess.check_output(['/usr/bin/xcrun', '--find', 'clang'], text=True).strip()
        sdk = subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip()
        receipt['compiler'] = {'path': cc, 'sha256': sha(cc), 'sdk': sdk}
        text = ASM.read_text()
        blocks = {n: block(text, n) for n in NAMES}
        run = build_and_run(out / 'actual', blocks, cc, sdk, receipt)
        if run is None or run.returncode:
            raise RuntimeError('actual source failed: ' + ((out / 'actual/build.log').read_text()[-400:] if run is None else run.stderr[-400:]))
        receipt['checks'] = sorted({l[5:] for l in run.stdout.splitlines() if l.startswith('PASS ')})
        receipt['check_count'] = sum(1 for l in run.stdout.splitlines() if l.startswith('PASS '))
        for name, old, new, n in MUTATIONS:
            mutated = dict(blocks, hv_enter_guest_on_stack=replace_nth(blocks['hv_enter_guest_on_stack'], old, new, n))
            run = build_and_run(out / ('mutant-' + name), mutated, cc, sdk, receipt)
            if run is None:
                raise RuntimeError(name + ' mutant did not build')
            if run.returncode == 0:
                raise RuntimeError(name + ' mutant was not rejected')
            reason = next((l for l in run.stderr.splitlines() if l.startswith('FAIL ')), 'exit %d (crash)' % run.returncode)
            receipt['mutations'].append({'name': name, 'rejected': True, 'reason': reason})
        assert all(sha(p) == h for p, h in receipt['source_sha256'].items()), 'source changed during replay'
        receipt['verdict'] = 'PASS'
    except Exception as e:
        receipt['verdict'] = 'FAIL'
        receipt['error'] = str(e)
    receipt['artifacts_sha256'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*'))
                                   if p.is_file() and p.name != 'replay' and '.dSYM' not in str(p)}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ('verdict', 'check_count', 'error') if k in receipt}
                     | {'mutants_rejected': len(receipt['mutations'])}))
    return receipt['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
