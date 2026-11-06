#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay the production DART constructor with allocation/MMIO fault injection."""
import argparse
import hashlib
import json
import itertools
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source = (ROOT / 'src/dart.c').read_text()
start = source.index('dart_dev_t *dart_init(')
end = source.index('dart_dev_t *dart_init_adt(', start)
(out / 'dart_init_body.inc').write_text(source[start:end])
for name in ('src/dart.c', 'tests/e2e/windows_dart_init_host.c', 'tools/windows_dart_init_e2e.py',
             'docs/windows-mtp-handoff.md'):
    destination = out / 'source' / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / name, destination)
report = {'scope': 'actual constructor, modeled allocator/MMIO; not physical DART proof',
          'hardware_accessed': False, 'result': 'failed', 'cases': [],
          'replay': 'python3 source/tools/windows_dart_init_e2e.py --output /tmp/dart-replay-new'}
try:
    command = ['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-I', str(out), str(out / 'source/tests/e2e/windows_dart_init_host.c'),
               '-o', str(out / 'dart-init-host')]
    report['compile_command'] = command
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (out / 'compile.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    for kind, failure, mode in itertools.product(range(3), range(6), range(3)):
        result = subprocess.run([out / 'dart-init-host', str(kind), str(failure), str(mode)],
                                capture_output=True, text=True, timeout=10)
        (out / f'case-{kind}-{failure}-{mode}.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        report['cases'].append({'type': kind, 'fail_allocation': failure, 'mode': mode})
    report['result'] = 'passed'
finally:
    report['artifacts'] = {str(f.relative_to(out)): hashlib.sha256(f.read_bytes()).hexdigest()
                           for f in sorted(out.rglob('*')) if f.is_file()}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'result.json')
