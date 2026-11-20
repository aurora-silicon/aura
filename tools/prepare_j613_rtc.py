#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Opt-in offline RTC-only J613 DTB preparation for Stage 2 composition."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

SPMI = '/soc/spmi@2e4714000'
RTC = '/soc/smc@2ec400000/rtc'
LAYOUT = SPMI + '/pmic@e/nvmem-layout'
AIC = '/soc/interrupt-controller@2d1000000'


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def prepare(source, output):
    receipt = output.with_suffix(output.suffix + '.json')
    if source.resolve() == output.resolve() or output.exists() or receipt.exists():
        raise ValueError('refusing existing output/receipt or in-place modification')
    original = source.read_bytes()
    with tempfile.TemporaryDirectory(prefix='j613-rtc-') as scratch:
        work = Path(scratch) / 'work.dtb'
        work.write_bytes(original)
        def get(node, prop, kind='x'):
            return run('fdtget', '-t', kind, str(work), node, prop).split()
        compatible = get('/', 'compatible', 's')
        if compatible[0] not in ('apple,j613', 'apple,j613-25g83') or 'apple,t8122' not in compatible:
            raise ValueError('expected J613/T8122 DTB')
        if get(SPMI, 'compatible', 's') != ['apple,t8122-spmi', 'apple,t8103-spmi']:
            raise ValueError('unexpected SPMI controller')
        if get(AIC, 'compatible', 's') != ['apple,t8122-aic3']:
            raise ValueError('unexpected AIC')
        if get(RTC, 'nvmem-cell-names', 's') != ['rtc_offset']:
            raise ValueError('alarm wiring already present or partial')
        rtc_props = run('fdtget', '-p', str(work), RTC).split()
        if any(x in rtc_props for x in ('interrupts', 'interrupts-extended', 'wakeup-source')):
            raise ValueError('partial RTC alarm wiring')
        for resource in (SPMI, RTC, AIC):
            node = resource
            while node:
                status = run('fdtget', '-t', 's', '-d', 'okay', str(work), node, 'status')
                if status not in ('ok', 'okay'):
                    raise ValueError('disabled resource: ' + node)
                node = node.rsplit('/', 1)[0]
        offset_node = LAYOUT + '/rtc-offset@f900'
        if get(offset_node, 'reg') != ['f900', '6']:
            raise ValueError('unexpected RTC offset cell')
        offset = get(RTC, 'nvmem-cells')
        if len(offset) != 1 or offset != get(offset_node, 'phandle'):
            raise ValueError('expected one RTC offset cell')
        # Discover all existing phandles to allocate without collisions.
        def walk(node):
            yield node
            for child in run('fdtget', '-l', str(work), node).split():
                yield from walk(node.rstrip('/') + '/' + child)
        nodes = list(walk('/'))
        handles = []
        for node in nodes:
            props = run('fdtget', '-p', str(work), node).split()
            for name in ('phandle', 'linux,phandle'):
                if name in props:
                    handles.append(int(get(node, name)[0], 16))
        handle = max(handles) + 1
        if handle + 4 >= 0xffffffff:
            raise ValueError('phandle space exhausted')
        def put(node, prop, *values, kind='x'):
            subprocess.run(['fdtput', '-t', kind, str(work), node, prop, *values], check=True)
        aic = get(AIC, 'phandle')[0]
        props = run('fdtget', '-p', str(work), SPMI).split()
        if any(x in props for x in ('interrupt-controller', '#interrupt-cells', 'interrupts', 'interrupt-parent')):
            raise ValueError('SPMI interrupt wiring already present or partial')
        put(SPMI, 'interrupt-parent', aic)
        put(SPMI, 'interrupt-controller')
        put(SPMI, '#interrupt-cells', '2')
        put(SPMI, 'interrupts', '0', '1b5', '4')
        if 'phandle' in props:
            spref = get(SPMI, 'phandle')[0]
        else:
            spref = f'{handle:x}'
            put(SPMI, 'phandle', spref)
            handle += 1
        cells = []
        for name, address, size in (('rtc-ctrl', 'f800', '1'),
                                    ('rtc-alarm', 'f808', '4'),
                                    ('rtc-alarm-mask', 'f80e', '1')):
            node = LAYOUT + '/' + name + '@' + address
            if node in nodes:
                raise ValueError('alarm cell already exists')
            subprocess.run(['fdtput', '-c', str(work), node], check=True)
            put(node, 'reg', address, size)
            cells.append(f'{handle:x}')
            put(node, 'phandle', cells[-1])
            handle += 1
        put(RTC, 'nvmem-cells', *offset, *cells)
        put(RTC, 'nvmem-cell-names', 'rtc_offset', 'rtc_ctrl', 'rtc_alarm', 'rtc_alarm_mask', kind='s')
        put(RTC, 'interrupts-extended', spref, '2', '1')
        put(RTC, 'wakeup-source')
        result = work.read_bytes()
    data = dict(schema=1, input_sha256=hashlib.sha256(original).hexdigest(),
                output_sha256=hashlib.sha256(result).hexdigest(),
                compatible=compatible, purpose='RTC-only offline trial DTB')
    with output.open('xb') as stream:
        stream.write(result)
    with receipt.open('x') as stream:
        json.dump(data, stream, indent=2)
        stream.write('\n')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.input, args.output), indent=2))
