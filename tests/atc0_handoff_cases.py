#!/usr/bin/env python3
"""E2E failure cases for the production handoff using an external ADT and DTB."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

exe, adt, dtb, directory = map(Path, sys.argv[1:])
prop = 'apple,atc0-handoff'
phy = '/soc@200000000/phy@2a3000000'
results = []


def get(file, node, key, kind='x'):
    return subprocess.check_output(['fdtget', '-t', kind, str(file), node, key], text=True).split()


def put(file, node, key, values, kind='x'):
    subprocess.run(['fdtput', '-t', kind, str(file), node, key, *values], check=True)


def tree(file):
    data = Path(file).read_bytes()
    magic, _, offset, strings, _, version, _, _, strings_size, _ = struct.unpack_from('>10I', data)
    assert magic == 0xd00dfeed and version >= 17
    names = data[strings:strings + strings_size]
    nodes, stack = {}, []
    while True:
        tag, = struct.unpack_from('>I', data, offset)
        offset += 4
        if tag == 1:
            end = data.index(0, offset)
            stack.append(data[offset:end].decode())
            offset = (end + 4) & ~3
            nodes['/' + '/'.join(stack[1:])] = {}
        elif tag == 2:
            stack.pop()
        elif tag == 3:
            size, name = struct.unpack_from('>II', data, offset)
            offset += 8
            name = names[name:names.index(0, name)].decode()
            nodes['/' + '/'.join(stack[1:])][name] = data[offset:offset + size]
            offset = (offset + size + 3) & ~3
        elif tag == 9:
            return nodes
        else:
            assert tag == 4


def cells(raw):
    return struct.unpack('>' + 'I' * (len(raw) // 4), raw)


def assert_activation(out, input_adt):
    before, after = tree(str(out) + '.before'), tree(out)
    assert before.keys() == after.keys()
    handles = cells(before['/chosen'][prop])
    paths = {p for p, n in before.items() if 'phandle' in n and cells(n['phandle'])[0] in handles}
    assert len(paths) == 27
    assert after[phy]['apple,guest-port-reserved'] == b''
    expected_properties = {'apple,tunable-' + name for name in (
        'axi2af fabric common-cfg lioa lane0-rx-clock lane1-rx-clock '
        'lane0-rx-eq lane1-rx-eq auspll aus40cmn-shm cio4pll '
        'usb2-default usb2-host pipe-default').split()}
    assert all(name in after[phy] for name in expected_properties)
    for path in paths:
        assert before[path]['status'] == b'disabled\0' and after[path]['status'] == b'okay\0'
    router = next(n for p, n in before.items() if p in paths and b'apple,t6050-usb4-acio\0' in n.get('compatible', b''))
    gpio_handle, pin, _ = cells(router['lstx-gpios'])
    assert pin == 77
    gpio = next(p for p, n in before.items() if 'phandle' in n and cells(n['phandle'])[0] == gpio_handle)
    def reserved(raw, pin):
        values = cells(raw)
        return any(start <= pin < start + size for start, size in zip(values[::2], values[1::2]))
    for pin in range(85):
        a = reserved(before[gpio]['gpio-reserved-ranges'], pin)
        b = reserved(after[gpio]['gpio-reserved-ranges'], pin)
        assert (a and not b) if pin == 77 else a == b
    for path, props in before.items():
        for key in props.keys() | after[path].keys():
            if path in paths and key == 'status':
                continue
            if path == phy and (key in expected_properties or key == 'apple,guest-port-reserved'):
                continue
            if path == gpio and key == 'gpio-reserved-ranges':
                continue
            assert props.get(key) == after[path].get(key), (path, key)
    expected = adt_property(Path(input_adt).read_bytes(), '/arm-io/atc-phy0', 'tunable_CIO4PLL_CORE')[1]
    records = [(packed & 0xffffff, mask, value) for packed, mask, value in struct.iter_unpack('<III', expected)]
    values = cells(after[phy]['apple,tunable-cio4pll'])
    assert list(zip(values[::3], values[1::3], values[2::3])) == records


def adt_property(data, path, name):
    def walk(offset):
        nprops, children = struct.unpack_from('<II', data, offset)
        props, offset = {}, offset + 8
        for _ in range(nprops):
            key = data[offset:offset + 32].split(b'\0')[0].decode()
            size, = struct.unpack_from('<I', data, offset + 32)
            size &= 0x7fffffff
            props[key] = (offset, data[offset + 36:offset + 36 + size])
            offset += 36 + ((size + 3) & ~3)
        kids = {}
        for _ in range(children):
            start = offset
            child, _, offset = walk(offset)
            kids[child['name'][1].split(b'\0')[0].decode()] = start
        return props, kids, offset
    offset = 0
    for part in path.strip('/').split('/'):
        offset = walk(offset)[1][part]
    return walk(offset)[0][name]


def mutate_adt(name, node, prop, update):
    data = bytearray(adt.read_bytes())
    offset, raw = adt_property(data, node, prop)
    update(data, offset, raw)
    file = directory / (name + '.adt')
    file.write_bytes(data)
    return file


def case(name, mode, expected=0, mutate=None, slack=65536, chip='6050', input_adt=adt, base=dtb):
    inp, out = directory / (name + '.in.dtb'), directory / (name + '.out.dtb')
    shutil.copyfile(base, inp)
    if mutate:
        mutate(inp)
    command = [str(exe), str(input_adt), str(inp), str(out), chip, str(slack), mode, str(expected)]
    run = subprocess.run(command, text=True, capture_output=True)
    (directory / (name + '.log')).write_text(run.stdout + run.stderr)
    assert run.returncode == 0, (name, run.stdout, run.stderr)
    results.append({'case': name, 'result': 'PASS', 'handoff_result': expected})
    return out


members = get(dtb, '/chosen', prop)
assert len(members) == 27 and len(set(members)) == 27
enabled = case('allow', 'allow')
assert_activation(enabled, adt)
assert get(enabled, phy, 'status', 's') == ['okay']
assert get(enabled, phy, 'apple,tunable-cio4pll')
case('deny', 'deny')
case('allocation-failure', 'alloc-fail', 1)
case('no-space', 'allow', 1, slack=0)
case('old-dt', 'allow', mutate=lambda f: subprocess.run(
    ['fdtput', '-d', str(f), '/chosen', prop], check=True))
case('other-soc', 'allow', chip='8103')
case('other-board', 'allow', mutate=lambda f: put(f, '/', 'compatible', ['apple,j716c'], 's'))
case('empty-list', 'allow', -1, mutate=lambda f: put(f, '/chosen', prop, []))
case('bad-phandle', 'allow', -1, mutate=lambda f: put(f, '/chosen', prop, ['0', *members[1:]]))
case('duplicate-phandle', 'allow', -1, mutate=lambda f: put(f, '/chosen', prop, [members[1], *members[1:]]))
case('already-enabled', 'allow', -1, mutate=lambda f: put(f, phy, 'status', ['okay'], 's'))
case('missing-phy', 'allow', -1, mutate=lambda f: put(f, phy, 'apple,port-index', ['1']))
case('dangling-phandle', 'allow', -1, mutate=lambda f: put(f, '/chosen', prop, ['deadbeef', *members[1:]]))
case('malformed-list', 'allow', -1, mutate=lambda f: put(f, '/chosen', prop, ['1'], 'bx'))
case('reused-tree', 'allow', -1, base=enabled)
node = '/arm-io/usb-drd0'
missing = mutate_adt('missing-final-table', node, 'tunable_PIPE_HANDLER_DEFAULT',
                     lambda d, o, r: d.__setitem__(o, ord('x')))
case('missing-final-table', 'allow', 1, input_adt=missing)
invalid = mutate_adt('invalid-final-table', node, 'tunable_PIPE_HANDLER_DEFAULT',
                     lambda d, o, r: d.__setitem__(o + 39, 16))
case('invalid-final-table', 'allow', 1, input_adt=invalid)
def different_pll(data, offset, raw):
    for i in range(0, len(raw), 12):
        packed, _, value = struct.unpack_from('<III', raw, i)
        if packed & 0xffffff == 0x4c:
            struct.pack_into('<I', data, offset + 36 + i + 8, value ^ 1)
            return
    raise AssertionError('PLL register absent')
other_adt = mutate_adt('different-pll', '/arm-io/atc-phy0', 'tunable_CIO4PLL_CORE', different_pll)
other = case('different-pll', 'allow', input_adt=other_adt)
assert_activation(other, other_adt)
(directory / 'RESULTS.json').write_text(json.dumps(results, indent=2) + '\n')
print('PASS:', len(results), 'end-to-end ATC0 handoff scenarios; repeat activation refused')
