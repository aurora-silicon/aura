#!/usr/bin/env python3
"""Verify and clear J873's PMU error byte before continuing a boot session.

Requires an identified Aura proxy, not a Linux console. No reset or retry.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

import fcntl
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from aura.adt import load_adt
from aura.hw.pmu import PMU
from aura.hw import spmi
from aura.hw.spmi4 import SPMI4Regs, R_STATUS
from aura.proxy import UartInterface, AuraProxy
from aura.tgtypes import BootArgs_r3
def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def serial(adt):
    value = adt._properties['serial-number']
    return value.rstrip(b'\0').decode('ascii') if isinstance(value, bytes) else value


def topology(adt):
    require(adt['/chosen'].chip_id == 0x8152 and adt['/chosen'].board_id == 0x24,
            'Wrong chip/board')
    require(adt.target_type == 'J873g' and adt.model == 'Mac18,5', 'Wrong target/model')
    path, bus = PMU.find_primary_pmu(adt)
    require(path == '/arm-io/nub-spmi0/pmu-main' and bus == 'spmi', 'Unexpected primary PMU')
    pmu = adt[path]
    controller = adt[path.rpartition('/')[0]]
    require(controller.gen == 4 and controller.compatible == ['aapl,spmi'], 'Wrong SPMI generation')
    require(pmu.compatible == ['pmu,spmi', 'pmu,woodcote'] and pmu.is_primary == 1,
            'Wrong PMU compatibility/ownership')
    require(isinstance(pmu.reg, bytes) and len(pmu.reg) == 24, 'Unexpected PMU reg representation')
    require(pmu.reg[0] == 14 and int.from_bytes(pmu.reg[:4], 'little') == 14, 'Unexpected PMU slave')
    require(pmu.info_leg__scrpad[0] == 0xf800, 'Unexpected scratchpad base')
    return dict(chip_id='0x8152', board_id='0x24', target=adt.target_type, model=adt.model,
                serial_sha256=hashlib.sha256(serial(adt).encode()).hexdigest(),
                controller_path=controller._path, generation=controller.gen,
                controller_ranges=[list(controller.get_reg(i)) for i in range(len(controller.reg))],
                pmu_path=path, pmu_reg_raw=pmu.reg.hex(), slave=14, error_byte_address=0xf802)


class CounterIO:
    """Allow only the selected counter's read/zero commands on this controller."""
    def __init__(self, adt, proxy=None):
        self.adt = adt
        self.proxy = proxy
        self.iface = proxy.iface if proxy is not None else None
        self.base = adt['/arm-io/nub-spmi0'].get_reg(0)[0]
        self.expected = []

    def read(self, address, width):
        require(self.proxy is not None, 'Offline admission attempted hardware access')
        require(width == 32 and address in (self.base + 0x200, self.base + 0x220),
                'Unexpected controller read')
        value = self.proxy.read32(address)
        require(self.proxy.get_exc_count() == 0, 'Exception during controller read')
        return value

    def write(self, address, value, width):
        require(self.proxy is not None, 'Offline admission attempted hardware access')
        require(width == 32 and address == self.base + 0x210 and self.expected,
                'Unexpected controller write')
        require(value == self.expected[0], 'SPMI command differs from admitted counter operation')
        self.expected.pop(0)
        self.proxy.write32(address, value)
        require(self.proxy.get_exc_count() == 0, 'Exception during controller write')

    def expect(self, operation):
        require(not self.expected, 'Incomplete previous controller operation')
        # Same EXT_READL / EXT_WRITEL protocol as PMU.reset_panic_counter().
        header = (0xf802 << 16) | (1 << 15) | (14 << 8)
        self.expected = [header | 0x38] if operation == 'read' else [header | 0x30, 0]

    def complete(self):
        require(not self.expected, 'SPMI operation did not issue all admitted words')


def construct(io):
    pmu = PMU(io)
    require(pmu.reg == 14 and pmu.primary and pmu.bus_type == 'spmi', 'PMU selection mismatch')
    require(type(pmu.spmi.regs) is SPMI4Regs, 'Generation-4 register map was not selected')
    require(pmu.spmi.regs.STATUS.addr == io.base + 0x200 and
            pmu.spmi.regs.CMD.addr == io.base + 0x210 and
            pmu.spmi.regs.REPLY.addr == io.base + 0x220, 'Register layout mismatch')
    require(R_STATUS(1 << 30).RX_EMPTY and not R_STATUS(1 << 24).RX_EMPTY, 'RX-empty bit mismatch')
    return pmu


def durable(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    descriptor = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--serial', required=True, help='Expected physical J873 serial number')
    parser.add_argument('--output', type=Path, required=True, help='New receipt directory')
    args = parser.parse_args()
    require(__debug__ and not os.environ.get('PYTHONOPTIMIZE'), 'Run Python without -O')
    require(args.serial.isalnum(), 'Invalid serial number')
    device = args.device
    lock = Path('/tmp/aura-j873-' + args.serial + '.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    receipt = dict(passed=False, offline_only=False, pmu_write_attempted=False,
                   phase='ownership', device=device,
                   started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    args.output.mkdir(exist_ok=False)
    durable(args.output / 'inputs.json', receipt)
    iface = None
    previous_handler = signal.getsignal(signal.SIGALRM)
    def deadline(signum, frame):
        raise TimeoutError('PMU guard deadline exceeded; no automatic retry')
    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(60)
    try:
        receipt['phase'] = 'ownership'
        owners = subprocess.run(['lsof', '-n', device], capture_output=True, text=True, timeout=10)
        require(Path(device).exists() and owners.returncode == 1 and not owners.stdout,
                'Target endpoint unavailable or already owned')
        receipt['phase'] = 'proxy-nop'
        iface = UartInterface(device)
        iface.dev.timeout = 3
        iface.dev.write_timeout = 3
        iface.nop()
        proxy = AuraProxy(iface)
        receipt['phase'] = 'fresh-identity'
        address, revision = proxy.get_bootargs_rev()
        require(revision == 3, 'Unexpected bootargs revision')
        bootargs = BootArgs_r3.parse(iface.readmem(address, BootArgs_r3.sizeof()))
        require(0 < bootargs.devtree_size < 4 * 1024 * 1024, 'Invalid ADT size')
        address = (bootargs.devtree - bootargs.virt_base + bootargs.phys_base) & ((1 << 64) - 1)
        data = b''.join(iface.readmem(address + n, min(16384, bootargs.devtree_size - n))
                        for n in range(0, bootargs.devtree_size, 16384))
        adt = load_adt(data)
        require(serial(adt) == args.serial, 'Wrong physical target')
        receipt['target'] = topology(adt)
        receipt['live_adt_sha256'] = hashlib.sha256(data).hexdigest()
        receipt['aura_base'] = hex(proxy.get_base())
        receipt['iodev'] = proxy.iodev_whoami()
        require(proxy.get_exc_count() == 0, 'Existing proxy exception state')
        io = CounterIO(adt, proxy)
        pmu = construct(io)
        spmi.SPMI_POLL_LIMIT = 64
        receipt['phase'] = 'counter-read'
        require(pmu.spmi.regs.STATUS.reg.RX_EMPTY, 'SPMI RX FIFO is occupied; do not consume it')
        io.expect('read')
        before = pmu.spmi.read8(pmu.reg, 0xf802)
        io.complete()
        receipt['before_raw'] = before
        durable(args.output / 'before.json', receipt)
        if before:
            receipt['phase'] = 'counter-clear'
            receipt['pmu_write_attempted'] = True
            durable(args.output / 'write-intent.json', receipt)
            io.expect('clear')
            pmu.reset_panic_counter()
            io.complete()
        receipt['phase'] = 'counter-readback'
        io.expect('read')
        receipt['after_raw'] = pmu.spmi.read8(pmu.reg, 0xf802)
        io.complete()
        require(receipt['after_raw'] == 0, 'PMU error byte did not read back as zero')
        receipt['phase'] = 'final-proxy-check'
        proxy.nop()
        receipt.update(passed=True, phase='complete')
    except Exception as error:
        receipt['error'] = repr(error)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous_handler)
        if iface is not None:
            iface.dev.close()
        durable(args.output / 'receipt.json', receipt)
    print('J873_PMU_GUARD_PASS' if receipt['passed'] else 'J873_PMU_GUARD_FAILED',
          json.dumps({k: receipt.get(k) for k in ('phase', 'before_raw', 'after_raw', 'pmu_write_attempted', 'error')}),
          flush=True)
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
