#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build by default; explicitly qualify CDC vUART using a bounded EL1 microguest."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OFFSET, SIZE = 0x1000, 256

def require(condition, message):
    if not condition:
        raise ValueError(message)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def tool(name, formula):
    path = Path('/opt/homebrew/opt') / formula / 'bin' / name
    return str(path) if path.is_file() else shutil.which(name) or name

def build(args, out, report):
    for name in ('tests/e2e/windows_vuart_smoke.S', 'tools/windows_vuart_smoke.py',
                 'docs/windows-vuart-smoke.md'):
        shutil.copy2(ROOT / name, out / Path(name).name)
    report['build_commands'] = []
    def run(command, log):
        command = [str(x) for x in command]
        report['build_commands'].append(command)
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (out / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout
    for suffix in ('', '-repeat'):
        obj, elf, raw = [out / (name + suffix + ext) for name, ext in
                         [('guest', '.o'), ('guest', '.elf'), ('template', '.bin')]]
        run([args.clang, '--target=aarch64-none-elf', '-march=armv8-a', '-c',
             out / 'windows_vuart_smoke.S', '-o', obj], 'compile' + suffix + '.log')
        run([args.ld, '-Ttext=0', '--image-base=0', '--entry=_start', '--build-id=none',
             obj, '-o', elf], 'link' + suffix + '.log')
        run([args.objcopy, '-O', 'binary', '-j', '.text', elf, raw], 'objcopy' + suffix + '.log')
    run([args.objdump, '-d', out / 'guest.elf'], 'guest.disassembly.txt')
    symbols = run([args.objdump, '-t', out / 'guest.elf'], 'guest.symbols.txt')
    require(any(line.startswith('0000000000001000') and line.endswith('vuart_results')
                for line in symbols.splitlines()), 'result block moved')
    template = (out / 'template.bin').read_bytes()
    require(template == (out / 'template-repeat.bin').read_bytes(), 'non-reproducible guest')
    require(len(template) == 0x4000 and template[OFFSET:OFFSET + SIZE] == bytes(SIZE),
            'invalid guest image/result layout')
    nonce = int.from_bytes(os.urandom(8), 'little') or 1
    report.update(nonce=hex(nonce), template_sha256=hashlib.sha256(template).hexdigest())
    return nonce

def profile(args, out, nonce, uart):
    image = bytearray((out / 'template.bin').read_bytes())
    struct.pack_into('<Q', image, OFFSET + 32, nonce)
    struct.pack_into('<Q', image, OFFSET + 48, uart)
    (out / 'guest.bin').write_bytes(image)
    data = {'schema': 'aura.windows.boot.v1', 'name': 'CDC vUART smoke',
            'target': {'chip_id': args.chip_id, 'model': args.model},
            'mu': {'path': 'guest.bin', 'sha256': digest(out / 'guest.bin'), 'entry_offset': 0},
            'framebuffer': 'native', 'guest_compatibility': 'none', 'usb': 'disabled',
            'n1': 'disabled', 'serial': 'cdc'}
    (out / 'profile.json').write_text(json.dumps(data, indent=2) + '\n')
    from aura.windows import plan_profile
    return plan_profile(out / 'profile.json')

def bind_cdc(primary, guest, out):
    require(sys.platform == 'darwin', 'physical port binding currently requires macOS IORegistry')
    require(os.path.realpath(primary) != os.path.realpath(guest), 'CDC paths must differ')
    raw = subprocess.check_output(['ioreg', '-a', '-l', '-p', 'IOService',
                                   '-r', '-c', 'IOUSBHostDevice'], timeout=15)
    (out / 'ioreg-before.plist').write_bytes(raw)
    ports = {}
    def walk(node, device=None, interface=None):
        if node.get('IOObjectClass') == 'IOUSBHostDevice':
            device = {key: node.get(key) for key in
                      ('IORegistryEntryID', 'locationID', 'idVendor', 'idProduct')}
            interface = None
        if 'bInterfaceNumber' in node:
            interface = node['bInterfaceNumber']
        if device is not None:
            for key in ('IOCalloutDevice', 'IODialinDevice'):
                if key in node:
                    ports[os.path.realpath(node[key])] = dict(device, interface=interface)
        for child in node.get('IORegistryEntryChildren', []):
            walk(child, device, interface)
    tree = plistlib.loads(raw)
    for node in tree if isinstance(tree, list) else [tree]:
        walk(node)
    first, second = ports.get(os.path.realpath(primary)), ports.get(os.path.realpath(guest))
    require(first is not None and second is not None, 'explicit CDC paths missing from IORegistry')
    require(first['interface'] == 1 and second['interface'] == 3, 'expected proxy interface1 and guest interface3')
    require(first['IORegistryEntryID'] is not None and
            first['IORegistryEntryID'] == second['IORegistryEntryID'], 'CDC ports belong to different devices')
    require((first['idVendor'], first['idProduct']) == (0x1209, 0x316d), 'not public Aura CDC')
    return {'primary': first, 'guest': second}

class HostProtocol(threading.Thread):
    def __init__(self, port, nonce, out):
        super().__init__(daemon=True)
        self.port, self.nonce, self.out = port, nonce.to_bytes(8, 'little'), out
        self.stop = threading.Event()
        self.error = None
        self.events = []
        self.buffer = bytearray()
        self.filler = 0
        self.admitted_probes = 0

    def event(self, name):
        when = time.monotonic()
        self.events.append({'event': name, 'monotonic': when})
        self.rx.flush()
        self.tx.flush()
        (self.out / 'host-progress.json').write_text(json.dumps(
            {'events': self.events, 'filler_bytes': self.filler,
             'admitted_probe_bytes': self.admitted_probes}, indent=2) + '\n')
        return when

    def wait_until(self, deadline):
        while time.monotonic() < deadline:
            if self.stop.wait(min(.1, deadline - time.monotonic())):
                raise RuntimeError('guest ended before host protocol completed')

    def read(self, count):
        deadline = time.monotonic() + 100
        while len(self.buffer) < count:
            require(not self.stop.is_set(), 'guest ended during host receive')
            require(time.monotonic() < deadline, 'CDC receive deadline')
            chunk = self.port.read(min(65536, max(self.port.in_waiting, count - len(self.buffer), 1)))
            self.rx.write(chunk)
            self.buffer.extend(chunk)
        data = bytes(self.buffer[:count]); del self.buffer[:count]
        return data

    def marker(self, name):
        require(self.read(16) == name + self.nonce, 'bad marker/nonce: ' + name.decode())
        return self.event(name.decode())

    def command(self, command):
        data = command + self.nonce
        self.tx.write(data)
        when = self.event('send-' + command.decode())
        require(self.port.write(data) == len(data), 'partial CDC command')
        return when

    def run(self):
        try:
            with (self.out / 'guest-rx.bin').open('wb') as self.rx, (self.out / 'guest-tx.bin').open('wb') as self.tx:
                self.marker(b'VURDY001'); self.command(b'S')
                require(self.read(256) == bytes(range(256)), 'initial TX/RX payload mismatch')
                isolation = (b'HVLOG: VUART-ISOLATE-' +
                             f'{int.from_bytes(self.nonce, "little"):016x}'.encode() +
                             b'\xff\x55\xaa\x05\0\r\nEND\n')
                (self.out / 'isolation-payload.bin').write_bytes(isolation)
                require(self.read(len(isolation)) == isolation, 'HVLOG isolation payload mismatch')
                self.marker(b'VUPAUSE1'); self.command(b'P')
                pause = self.marker(b'VUPACK01')
                self.event('reader-paused')
                self.wait_until(pause + 60)
                self.event('reader-resumed')
                # Filler cannot contain the first byte of the following marker.
                while True:
                    byte = self.read(1)
                    if byte == b'\xee':
                        self.admitted_probes += 1
                        require(self.admitted_probes <= 4, 'unexpected excess probe bytes')
                        continue
                    if byte != b'\xa5':
                        self.buffer[:0] = byte
                        break
                    self.filler += 1
                    require(self.filler <= 8 * 1024 * 1024, 'filler exceeds guest limit')
                self.marker(b'VUFULL01')
                sent = self.command(b'C')
                ack = self.marker(b'VUCLOSE1')
                self.port.dtr = False
                self.port.close()
                closed = self.event('guest-port-closed')
                require(closed - sent < 1, 'host missed conservative DTR-close window')
                self.wait_until(ack + 4)
                self.port.open()
                self.port.dtr = True
                self.event('guest-port-reopened')
                self.command(b'R'); self.marker(b'VUREOPEN')
                require(self.read(256) == bytes(range(256)), 'reopened payload mismatch or leaked discard bytes')
                self.marker(b'VUDONE01'); self.command(b'Q')
                self.event('host-protocol-complete')
        except BaseException as error:
            self.error = type(error).__name__ + ': ' + str(error)
        finally:
            (self.out / 'host-protocol.json').write_text(json.dumps(
                {'events': self.events, 'filler_bytes': self.filler,
                 'admitted_probe_bytes': self.admitted_probes, 'error': self.error}, indent=2) + '\n')

def check_results(raw, nonce, uart, bootargs, worker):
    require(len(raw) == SIZE, 'short result readback')
    w = struct.unpack('<32Q', raw)
    require(raw[:8] == b'AVUART01' and w[1] == 1, 'missing guest ABI/magic')
    require(w[2] == 6 and w[3] == 0, f'guest failed at phase {w[2]} error {w[3]}')
    require(w[4] == w[5] == nonce and w[6] == uart, 'input/nonce mismatch')
    require(w[7] > 0 and 0 < w[8] <= 8 * 1024 * 1024 and w[8] == worker.filler,
            'filler count mismatch')
    require(w[10] < w[7] and w[18] == 4 and w[20] >= max(1, w[7] // 5),
            'sustained full queue/write latency proof missing')
    require(w[12] >= 2 * w[7] and w[12] <= w[13] < 3 * w[7] and w[11] == w[13] - w[12],
            'guest discard writes were outside proven closed-DTR window')
    require(w[15] == bootargs and w[16] == 4 and w[17] == 0, 'entry state/guard mismatch')
    require(raw[152:160] == b'VUDONE01' and not any(w[21:]), 'missing completion or damaged guards')
    return {'phase': w[2], 'frequency': w[7], 'filler_bytes': w[8], 'full_wait_iterations': w[9],
            'ignored_full_write_seconds': w[10] / w[7], 'closed_write_seconds': w[11] / w[7],
            'full_dwell_seconds': w[20] / w[7],
            'closed_start_seconds_after_ack': w[12] / w[7], 'closed_end_seconds_after_ack': w[13] / w[7]}

def check_trailing_bytes(worker, report):
    """Guest and reader are stopped; keep the port open for final USB completion."""
    tail = {'prefetched_bytes': len(worker.buffer), 'fresh_bytes': 0, 'scan_limit_bytes': 65536}
    report['trailing_bytes'] = tail
    previous_timeout = worker.port.timeout
    worker.port.timeout = .05
    start = time.monotonic()
    try:
        with (worker.out / 'guest-rx.bin').open('ab') as rx:
            while time.monotonic() - start < .5 and tail['fresh_bytes'] < 65536:
                chunk = worker.port.read(min(4096, 65536 - tail['fresh_bytes']))
                rx.write(chunk)
                tail['fresh_bytes'] += len(chunk)
    finally:
        worker.port.timeout = previous_timeout
        tail['scan_seconds'] = time.monotonic() - start
    require(tail['prefetched_bytes'] == tail['fresh_bytes'] == 0,
            'unexpected trailing guest bytes after final marker/SYSTEM_OFF')

def execute(args, out, nonce, report):
    import serial
    from aura.adt import load_adt
    from aura.windows import HardwareSession, validate_capabilities
    report['usb_binding'] = bind_cdc(args.device, args.guest_device, out)
    session = worker = port = None
    saved = {s: signal.getsignal(s) for s in (signal.SIGALRM, signal.SIGINT, signal.SIGQUIT)}
    def expired(signum, frame):
        raise TimeoutError('hardware deadline; guest state unknown unless return was recorded')
    signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, 250)
    try:
        with (out / 'console.log').open('w') as console, (out / 'host.log').open('w') as host:
            with contextlib.redirect_stdout(host), contextlib.redirect_stderr(host):
                session = HardwareSession(args.device, console)
                identity = session.identity(); report['identity'] = identity
                require(identity['chip_id'] == args.chip_id and identity['model'] == args.model,
                        'explicit target identity mismatch')
                adt = load_adt(session._identity_adt_blob)
                uart, length = adt['/arm-io/uart0'].get_reg(0)
                require(uart > 0 and uart % 0x1000 == 0 and length >= 0x1000, 'invalid ADT UART range')
                plan = profile(args, out, nonce, uart)
                report['profile_sha256'] = plan.profile_sha256
                report['uart'] = {'address': hex(uart), 'length': length, 'source': '/arm-io/uart0/reg[0]'}
                report['serial_transport'] = session.validate_serial_transport(plan)
                caps = session.capabilities(); report['capabilities'] = hex(caps)
                validate_capabilities(caps, plan.framebuffer, plan.guest_compatibility,
                                      plan.n1, bool(plan.reserved_memory), plan.serial)
                report['transport_mutated'] = True
                port = serial.Serial(args.guest_device, 115200, timeout=.25, write_timeout=3)
                require(port.in_waiting == 0, 'guest CDC contains stale bytes before preparation')
                report['target_mutated'] = True
                session.prepare(plan)
                require(session.hv.adt['/arm-io/uart0'].get_reg(0)[0] == uart, 'mapped UART differs from validated input')
                loaded = session.load(plan); report['loaded'] = loaded
                address = int(loaded['guest_base'], 0) + OFFSET
                before = session.iface.readmem(address, SIZE)
                (out / 'result-before.bin').write_bytes(before)
                require(before == plan.image[OFFSET:OFFSET + SIZE], 'uploaded input block mismatch')
                worker = HostProtocol(port, nonce, out); worker.start()
                report['guest_state'] = 'running-until-verified-return'
                (out / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n')
                status = session.start(); report['host_return_status'] = status
                report['guest_state'] = 'returned'
                require(status == 1, 'expected virtual SYSTEM_OFF and completed drain')
                raw = session.iface.readmem(address, SIZE)
                (out / 'result-after.bin').write_bytes(raw)
                report['proxy_responsive'] = session.capabilities() == caps
                worker.join(1)
                require(not worker.is_alive() and worker.error is None, 'host protocol failed: ' + str(worker.error))
                check_trailing_bytes(worker, report)
                report['observations'] = check_results(raw, nonce, uart, int(loaded['bootargs'], 0), worker)
                console.flush()
                primary = (out / 'console.log').read_bytes()
                marker = b'VUART-ISOLATE-' + f'{nonce:016x}'.encode()
                report['primary_isolation'] = {'nonce_marker_present': marker in primary,
                                                'prefix_present': b'VUART-ISOLATE-' in primary}
                require(not any(report['primary_isolation'].values()), 'guest isolation payload reached primary console')
                require(report['proxy_responsive'], 'final primary proxy check failed')
                if worker.admitted_probes or not report['observations']['full_wait_iterations']:
                    report['result'] = 'inconclusive'
                    report['reason'] = 'Host progress resumed during the deliberate full-write probe.'
                    report['admitted_probe_bytes'] = worker.admitted_probes
                else:
                    report['result'] = 'passed'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for sig, handler in saved.items(): signal.signal(sig, handler)
        if worker is not None:
            worker.stop.set(); worker.join(5)
        if port is not None: port.close()
        if worker is not None:
            worker.join(1)
            report['host_worker_stopped'] = not worker.is_alive()
        if session is not None: session.close()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true', help='explicitly open hardware; default builds only')
    p.add_argument('--device', help='explicit primary Aura CDC path')
    p.add_argument('--guest-device', help='explicit dedicated guest CDC path')
    p.add_argument('--chip-id', type=lambda x: int(x, 0), required=True)
    p.add_argument('--model', required=True)
    p.add_argument('--output', type=Path, required=True)
    for name, binary, formula in [('clang', 'clang', 'llvm'), ('ld', 'ld.lld', 'lld'),
                                  ('objcopy', 'llvm-objcopy', 'llvm'), ('objdump', 'llvm-objdump', 'llvm')]:
        p.add_argument('--' + name, default=tool(binary, formula))
    args = p.parse_args()
    if args.run != bool(args.device and args.guest_device) or (not args.run and (args.device or args.guest_device)):
        p.error('--run requires both explicit CDC paths; build-only mode accepts neither')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {'schema': 'aura.windows.vuart-smoke.v1', 'result': 'preflight',
              'target_mutated': False, 'transport_mutated': False, 'hardware_qualified': False,
              'windows_kd_qualified': False, 'guest_state': 'not-started',
              'command': [sys.executable, *sys.argv], 'started_unix': time.time()}
    code = 0
    try:
        nonce = build(args, out, report)
        sys.path.insert(0, str(ROOT / 'proxyclient'))
        from aura.windows import source_manifest
        report['host_sources'] = source_manifest()
        report['backend_sources'] = {name: digest(ROOT / name) for name in
            ('src/hv/hv_vuart.c', 'src/usb.c', 'src/usb_dwc3.c', 'src/usb_dwc3_vuart.inc', 'src/usb_dwc3_impl.inc')}
        report['backend_note'] = 'Local hashes do not attest the resident binary.'
        if args.run:
            execute(args, out, nonce, report)
            if report['result'] == 'inconclusive': code = 2
        else:
            profile(args, out, nonce, 0)
            report['result'] = 'build-only'
            report['note'] = 'UART input remains zero; hardware mode derives and hashes it from validated ADT.'
    except BaseException as error:
        report.update(result='failed', error=type(error).__name__ + ': ' + str(error)); code = 1
    finally:
        report['ended_unix'] = time.time()
        report['artifacts'] = {x.name: digest(x) for x in sorted(out.iterdir()) if x.is_file() and x.name != 'receipt.json'}
        (out / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': report['result'], 'receipt': str(out / 'receipt.json')}))
    return code

if __name__ == '__main__':
    raise SystemExit(main())
