#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Step the J813 E cluster up one command state at a time and prove each step.

Runs against the resident Aura proxy with no guest. Failure model:
docs/t8142-ecpu-stepup.md. The resident's own T8142 setter cannot reach states
above 2 (P_CPUFREQ_INIT only replays the fixed E2/P10 defaults and
P_CPUFREQ_SET_CLUSTER_PSTATE is T8152-only), so this tool performs the same
command-register protocol as cpufreq.c set_pstate() from a small routine on the
boot core. Only the E command register is ever written.

Stops, without retries, on: BUSY before or after a write, a wrong echo, a
transition slower than the resident setter's 400 us bound, any exception, a
wrong loop result, an out-of-tolerance frequency or a silent proxy. After a stop
that follows a write it makes no further frequency requests on that boot.
Secondaries are left running: reset the machine before any chainload.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import secrets
import struct
import sys
import time

E_CMD = 0x210e20020
P_CMD = 0x211e20020
E_HANDOFF = 0x400102  # docs/t8142-cpufreq.md: J813 firmware handoff, E state 2
P_HANDOFF = 0x40010a  # P state 10
STATE_MASK = 0x1f
SET = 1 << 25
BUSY = 1 << 31
REQUEST_BITS = STATE_MASK | SET | BUSY
# voltage-states1-sram kHz; array index 0 is command state 2 (state 1 is a 300 MHz floor).
E_LADDER_KHZ = (972000, 1152000, 1584000, 1992000, 2352000, 2700000, 2964000, 3048000)
P10_KHZ = 3516000
HANDOFF_STATE, LAST_STATE = 2, 9
E_CPUS = tuple(range(6))
BOOT_CPU = 6
CPU_COUNT = 10
COUNTER_HZ = 1_000_000_000  # src/utils.c timer_prepare_windows(): T8142 CNTFRQ_EL0
TOLERANCE = 0.02
SETTER_BOUND_TICKS = 400 * COUNTER_HZ // 1_000_000  # cpufreq.c CLUSTER_SWITCH_TIMEOUT, 400 us
BUSY_LIMIT_TICKS = 20 * COUNTER_HZ // 1000  # routine's own hard bound, 20 ms
PROBE_LOOPS = 50_000_000
PROBE_SAMPLES = 3
SOAK_CALIBRATION_COUNT = 20_000_000
SOAK_CALL_SECONDS = 1.0
MASK64 = (1 << 64) - 1

# Register-only routines. dvfs_step runs on the boot core only: it is the one
# frequency request, and it stores its result to proxy-heap memory.
ROUTINES = """
read_cntfrq:
    mrs x0, CNTFRQ_EL0
    ret
read_mpidr:
    mrs x0, mpidr_el1
    ret
freq_probe:
    isb
    mrs x1, CNTPCT_EL0
1:  sub x0, x0, #1
    cbnz x0, 1b
    isb
    mrs x2, CNTPCT_EL0
    sub x0, x2, x1
    ret
soak:
    isb
    mrs x9, CNTPCT_EL0
    mov x2, #0
    mov x3, #0
2:  add x2, x2, x1
    madd x3, x1, x1, x3
    subs x1, x1, #1
    b.ne 2b
    isb
    mrs x10, CNTPCT_EL0
    sub x10, x10, x9
    eor x0, x0, x2
    eor x0, x0, x3, ror #29
    eor x0, x0, x0, lsr #32
    lsl x0, x0, #32
    mov w10, w10
    orr x0, x0, x10
    ret
dvfs_step:
    str xzr, [x4, #16]
    str xzr, [x4, #24]
    str xzr, [x4, #32]
    ldr x5, [x0]
    str x5, [x4, #8]
    mov x9, #1
    tbnz x5, #31, 4f
    mov x9, #2
    cmp x5, x1
    b.ne 4f
    isb
    mrs x6, CNTPCT_EL0
    dsb sy
    str x2, [x0]
    dsb sy
    mov x8, #0
3:  ldr x7, [x0]
    add x8, x8, #1
    isb
    mrs x10, CNTPCT_EL0
    sub x10, x10, x6
    str x7, [x4, #16]
    str x10, [x4, #24]
    str x8, [x4, #32]
    mov x9, #0
    tbz x7, #31, 4f
    mov x9, #3
    cmp x10, x3
    b.lo 3b
4:  str x9, [x4]
    mov x0, x9
    ret
"""
STEP_STATUS = {0: "done", 1: "busy-before-no-write", 2: "changed-before-no-write",
               3: "busy-timeout-after-write"}


def assemble():
    from aura.asm import ARMAsm
    code = ARMAsm(ROUTINES)
    offsets = {name: getattr(code, name) for name in
               ("read_cntfrq", "read_mpidr", "freq_probe", "soak", "dvfs_step")}
    return code.data, offsets


def command_value(before, state):
    """The exact value cpufreq.c set_pstate() writes on T8142."""
    return (before & ~STATE_MASK) | SET | state


def soak_expected(nonce, count):
    s1 = (count * (count + 1) // 2) & MASK64
    s2 = (count * (count + 1) * (2 * count + 1) // 6) & MASK64
    x = nonce ^ s1 ^ (((s2 >> 29) | (s2 << 35)) & MASK64)
    return (x ^ (x >> 32)) & 0xffffffff


class Stop(Exception):
    def __init__(self, reason, **detail):
        super().__init__(reason)
        self.reason, self.detail = reason, detail


class TargetSilent(Exception):
    """The proxy stopped answering; no further requests of any kind."""


class TargetRefused(Exception):
    """The proxy answered with an error; it is still responsive."""


class LiveTarget:
    """The resident proxy. Opening this is the first device access."""
    kind = "live"

    def __init__(self, device, proxyclient):
        os.environ["AURADEVICE"] = device
        os.environ["AURATIMEOUT"] = "15"  # longest single request: one ~1 s soak wait
        sys.path.insert(0, str(proxyclient))
        from aura import proxy as proxy_module
        from aura.proxyutils import ProxyUtils, bootstrap_port
        self.module = proxy_module
        self.iface = proxy_module.UartInterface()
        self.p = proxy_module.AuraProxy(self.iface)
        self._guard(lambda: bootstrap_port(self.iface, self.p))
        self.u = self._guard(lambda: ProxyUtils(self.p))

    def _guard(self, fn):
        try:
            return fn()
        except self.module.ProxyRemoteError as error:
            raise TargetRefused(f"{type(error).__name__}: {error}") from error
        except (self.module.UartError, self.module.ProxyError, OSError) as error:
            # Timeouts, I/O and protocol errors: the transport is no longer trusted.
            raise TargetSilent(f"{type(error).__name__}: {error}") from error

    def identity(self):
        def gather():
            adt = self.u.adt
            cpus = [{"id": c.cpu_id, "mpidr": c.reg | (0x10000 if c.reg & 0x100 else 0),
                     "cluster": c.cluster_id, "running": c.state == "running"} for c in adt["/cpus"]]
            ladder = list(adt["/arm-io/pmgr"].getprop("voltage-states1-sram"))
            return {"chip_id": self.p.get_chipid(), "board_id": getattr(adt["/chosen"], "board_id", None),
                    "model": adt.model, "iodev": int(self.p.iodev_whoami()), "cpus": cpus,
                    "e_ladder_words": ladder}
        return self._guard(gather)

    def exc_count(self): return self._guard(self.p.get_exc_count)
    def read64(self, address): return self._guard(lambda: self.p.read64(address))

    def load_code(self, data):
        def load():
            base = self.u.memalign(0x4000, 0x4000)
            self.iface.writemem(base, data)
            self.p.dc_cvac(base, len(data))
            self.p.ic_ivau(base, len(data))
            return base
        return self._guard(load)

    def alloc_result(self): return self._guard(lambda: self.u.memalign(64, 64))
    def clear_result(self, address): self._guard(lambda: self.iface.writemem(address, bytes(40)))
    def read_result(self, address):
        return struct.unpack("<5Q", self._guard(lambda: self.iface.readmem(address, 40)))
    def call(self, address, *args): return self._guard(lambda: self.p.call(address, *args))
    def start_secondaries(self): self._guard(self.p.smp_start_secondaries)
    def is_alive(self, cpu): return bool(self._guard(lambda: self.p.smp_is_alive(cpu)))
    def smp_call_sync(self, cpu, address, *args):
        return self._guard(lambda: self.p.smp_call_sync(cpu, address, *args))
    def smp_call(self, cpu, address, *args): self._guard(lambda: self.p.smp_call(cpu, address, *args))
    def smp_wait(self, cpu): return self._guard(lambda: self.p.smp_wait(cpu))

    def close(self):
        try:
            self.iface.dev.close()
        except Exception:
            pass


class ModelTarget:
    """A modeled J813 for --dry-run and offline replays. No device is opened.

    `faults` injects one failure: see proxyclient/tools/qualify_t8142_estates_e2e.py.
    """
    kind = "model"
    CODE_BASE = 0x900000000
    RESULT = 0x900010000
    CYCLES_PER_SOAK_ITERATION = 3

    def __init__(self, offsets, faults=None):
        self.offsets, self.faults = offsets, dict(faults or {})
        self.e, self.p_reg, self.exceptions = self.faults.get("handoff", E_HANDOFF), P_HANDOFF, 0
        self.result = (0, 0, 0, 0, 0)
        self.alive = set()
        self.pending = {}
        self.writes = []

    def _fault(self, name, state=None):
        value = self.faults.get(name)
        return value is not None and (state is None or value == state)

    def _state(self):
        return self.e & STATE_MASK

    def _khz(self, cpu):
        if cpu not in E_CPUS:
            return P10_KHZ * (1.05 if self._fault("boot_shift", self._state()) else 1.0)
        khz = 300000 if self._state() == 1 else E_LADDER_KHZ[self._state() - HANDOFF_STATE]
        return khz * (0.9 if self._fault("e_slow", self._state()) else 1.0)

    def identity(self):
        cpus = [{"id": i, "mpidr": i if i < 6 else 0x10100 + i - 6, "cluster": 0 if i < 6 else 1,
                 "running": i == BOOT_CPU} for i in range(CPU_COUNT)]
        words = [word for khz in E_LADDER_KHZ for word in (khz, 800)]
        if self.faults.get("ladder"):
            words[2] += 1000
        return {"chip_id": self.faults.get("chip", 0x8142), "board_id": 0x24, "model": "Mac17,3",
                "iodev": 4, "cpus": cpus, "e_ladder_words": words}

    def exc_count(self):
        count, self.exceptions = self.exceptions, 0
        return count

    def read64(self, address):
        if self._fault("silent_read", self._state()) and self.writes:
            raise TargetSilent("modeled CDC drop")
        if address == E_CMD:
            return self.e
        assert address == P_CMD, hex(address)
        return self.p_reg

    def load_code(self, data): return self.CODE_BASE
    def alloc_result(self): return self.RESULT
    def clear_result(self, address): self.result = (0, 0, 0, 0, 0)
    def read_result(self, address): return self.result

    def call(self, address, *args):
        name = self._name(address)
        if name == "dvfs_step":
            return self._step(*args)
        return self._run(BOOT_CPU, name, args, False)

    def _name(self, address):
        names = {self.CODE_BASE + offset: name for name, offset in self.offsets.items()}
        return names[address]

    def _step(self, register, expected, value, limit, result):
        assert register == E_CMD and result == self.RESULT, "write outside the E command register"
        state = value & STATE_MASK
        before = self.e | (BUSY if self._fault("busy_race", state) else 0)
        if self._fault("changed_race", state):
            before ^= 1 << 42
        if before & BUSY:
            self.result = (1, before, 0, 0, 0)
            return 1
        if before != expected:
            self.result = (2, before, 0, 0, 0)
            return 2
        self.writes.append(value)
        if self._fault("silent_step", state):
            raise TargetSilent("modeled proxy silence during the step")
        if self._fault("busy_timeout", state):
            self.e = value | BUSY
            self.result = (3, before, self.e, limit, 1000)
            return 3
        settled = (value & ~SET) if not self._fault("wrong_echo", state) else (value & ~SET & ~STATE_MASK) | (state - 1)
        if self._fault("p_changes", state):
            self.p_reg ^= 1
        if self._fault("bit_changes", state):
            settled ^= 1 << 22
        self.e = settled
        ticks = 600_000 if self._fault("slow_busy", state) else 18_000
        self.result = (0, before, self.e, ticks, 3)
        return 0

    def _run(self, cpu, name, args, loaded):
        if name == "read_cntfrq":
            return COUNTER_HZ
        if name == "read_mpidr":
            return next(c["mpidr"] for c in self.identity()["cpus"] if c["id"] == cpu)
        if name == "freq_probe":
            return round(args[0] * COUNTER_HZ / (self._khz(cpu) * 1000))
        assert name == "soak"
        nonce, count = args
        if self._fault("exception", self._state()) and cpu == 3:
            self.exceptions += 1
        check = soak_expected(nonce, count)
        if self._fault("wrong_result", self._state()) and cpu == 4:
            check ^= 1
        khz = self._khz(cpu)
        if self._fault("throttle", self._state()) and loaded:
            khz *= 0.8
        ticks = round(count * self.CYCLES_PER_SOAK_ITERATION * COUNTER_HZ / (khz * 1000))
        return check << 32 | ticks & 0xffffffff

    def start_secondaries(self):
        if self.faults.get("start_fails"):
            raise TargetRefused("ProxyRemoteError: Reply error: Remote operation failed")
        self.alive = set(range(CPU_COUNT)) - {BOOT_CPU}

    def is_alive(self, cpu): return cpu in self.alive
    def smp_call_sync(self, cpu, address, *args): return self._run(cpu, self._name(address), args, False)

    def smp_call(self, cpu, address, *args):
        assert cpu not in self.pending
        self.pending[cpu] = (self._name(address), args)

    def smp_wait(self, cpu):
        if self._fault("hang", self._state()) and cpu == 5:
            raise TargetSilent("UartTimeout: modeled hung secondary")
        name, args = self.pending[cpu]
        value = self._run(cpu, name, args, True)
        del self.pending[cpu]
        return value

    def close(self): pass


class Qualifier:
    def __init__(self, open_target, kind, code, offsets, max_state, return_to_handoff, soak_seconds,
                 nonce, output=None):
        self.open_target, self.t, self.code, self.offsets = open_target, None, code, offsets
        self.max_state, self.return_to_handoff = max_state, return_to_handoff
        self.soak_rounds = max(1, round(soak_seconds / SOAK_CALL_SECONDS))
        self.nonce, self.output = nonce, output
        self.started = time.monotonic()
        self.events, self.steps = [], []
        self.written = False  # any frequency write issued (or possibly issued) on this boot
        self.requests_closed = False
        self.proxy_dead = False
        self.base = self.result = None
        self.report = {
            "schema": "aura.t8142.estate-qualification.v1", "target": kind,
            "dry_run": kind != "live", "measurements_are_modeled": kind != "live",
            "plan": {"states": list(range(HANDOFF_STATE + 1, max_state + 1)),
                     "return_to_handoff": return_to_handoff, "e_register": hex(E_CMD),
                     "p_register": hex(P_CMD), "planned_writes": self.planned_writes(),
                     "tolerance": TOLERANCE, "setter_bound_ticks": SETTER_BOUND_TICKS,
                     "busy_limit_ticks": BUSY_LIMIT_TICKS, "probe_loops": PROBE_LOOPS,
                     "probe_samples": PROBE_SAMPLES, "soak_rounds": self.soak_rounds,
                     "soak_call_seconds": SOAK_CALL_SECONDS},
            "code": {"sha256": hashlib.sha256(code).hexdigest(), "size": len(code),
                     "offsets": offsets, "hex": code.hex()},
            "events": self.events, "steps": self.steps, "result": "running"}

    def planned_writes(self):
        writes = [{"state": s, "address": hex(E_CMD), "value": hex(command_value(E_HANDOFF, s))}
                  for s in range(HANDOFF_STATE + 1, self.max_state + 1)]
        if self.return_to_handoff:
            writes.append({"state": HANDOFF_STATE, "address": hex(E_CMD),
                           "value": hex(command_value(E_HANDOFF, HANDOFF_STATE))})
        return writes

    # -- recording -------------------------------------------------------
    def log(self, kind, **fields):
        event = {"seq": len(self.events), "t": round(time.monotonic() - self.started, 6), "kind": kind}
        event.update(fields)
        self.events.append(event)
        return event

    def op(self, kind, fn, *args, **fields):
        if self.proxy_dead:
            raise Stop("silent proxy", op=kind)
        event = self.log(kind, args=[hex(a) if isinstance(a, int) else a for a in args], **fields)
        begin = time.monotonic()
        try:
            value = fn(*args)
        except TargetSilent as error:
            self.proxy_dead = True
            event.update(error=str(error))
            raise Stop("silent proxy", op=kind, error=str(error)) from error
        except TargetRefused as error:
            event.update(error=str(error))
            raise Stop("proxy refused", op=kind, error=str(error)) from error
        event["host_seconds"] = round(time.monotonic() - begin, 6)
        event["value"] = hex(value) if isinstance(value, int) else value
        return value

    def decide(self, ok, reason, **detail):
        self.log("check", passed=bool(ok), what=reason, **detail)
        if not ok:
            raise Stop(reason, **detail)

    def save(self):
        if self.output is None:
            return
        tmp = self.output.with_name(self.output.name + ".tmp")
        tmp.write_text(json.dumps(self.report, indent=2, default=str) + "\n")
        tmp.replace(self.output)

    # -- target operations ---------------------------------------------------
    def read(self, address): return self.op("read64", self.t.read64, address)
    def fn(self, name): return self.base + self.offsets[name]

    def exc_zero(self, where):
        count = self.op("exc_count", self.t.exc_count, where=where)
        self.decide(count == 0, "exception", where=where, count=count)

    def registers(self, where, e_state):
        e, p = self.read(E_CMD), self.read(P_CMD)
        self.decide(p == P_HANDOFF, "P command register changed", where=where, value=hex(p))
        self.decide(not e & BUSY and e & STATE_MASK == e_state and
                    (e ^ E_HANDOFF) & ~(STATE_MASK | SET) == 0,
                    "E command register not as expected", where=where, value=hex(e), state=e_state)
        return e

    def probe(self, cpu, expected_khz, where):
        samples = []
        for _ in range(PROBE_SAMPLES):
            if cpu == BOOT_CPU:
                ticks = self.op("call:freq_probe", self.t.call, self.fn("freq_probe"), PROBE_LOOPS)
            else:
                ticks = self.op("smp_call_sync:freq_probe", self.t.smp_call_sync, cpu,
                                self.fn("freq_probe"), PROBE_LOOPS)
            self.decide(0 < ticks < 1 << 40, "counter did not advance", cpu=cpu, ticks=ticks)
            samples.append(round(PROBE_LOOPS * COUNTER_HZ / ticks / 1e6, 3))
        target = expected_khz / 1000
        ok = all(abs(mhz - target) <= target * TOLERANCE for mhz in samples)
        self.decide(ok, "out-of-tolerance frequency", where=where, cpu=cpu,
                    expected_mhz=target, samples_mhz=samples)
        return samples

    def soak_one(self, cpu, packed, nonce, count, where):
        check, ticks = packed >> 32, packed & 0xffffffff
        expected = soak_expected(nonce, count)
        self.decide(check == expected, "wrong loop result", where=where, cpu=cpu,
                    check=hex(check), expected=hex(expected))
        self.decide(ticks > 0, "counter did not advance", where=where, cpu=cpu)
        return count * COUNTER_HZ / ticks

    def soak(self, khz, where):
        nonce = self.nonce()
        packed = self.op("smp_call_sync:soak", self.t.smp_call_sync, 0, self.fn("soak"), nonce,
                         SOAK_CALIBRATION_COUNT)
        solo = self.soak_one(0, packed, nonce, SOAK_CALIBRATION_COUNT, where + "/calibration")
        # The loop carries a madd chain: at most one iteration per cycle. A rate outside
        # 1/16..1 iteration per table cycle means the timing is wrong; do not size from it.
        self.decide(khz * 1000 / 16 <= solo <= khz * 1000 * (1 + TOLERANCE),
                    "implausible soak calibration", where=where, iterations_per_second=round(solo))
        count = max(SOAK_CALIBRATION_COUNT, int(solo * SOAK_CALL_SECONDS))
        rounds = []
        for index in range(self.soak_rounds):
            nonces = {cpu: self.nonce() for cpu in E_CPUS}
            for cpu in E_CPUS:
                self.op("smp_call:soak", self.t.smp_call, cpu, self.fn("soak"), nonces[cpu], count)
            rates = {}
            for cpu in E_CPUS:
                packed = self.op("smp_wait:soak", self.t.smp_wait, cpu)
                rates[cpu] = self.soak_one(cpu, packed, nonces[cpu], count, f"{where}/round{index}")
            low = {cpu: rate for cpu, rate in rates.items() if abs(rate - solo) > solo * TOLERANCE}
            rounds.append({"count": count, "nonces": {c: hex(n) for c, n in nonces.items()},
                           "iterations_per_second": {c: round(r) for c, r in rates.items()}})
            self.decide(not low, "out-of-tolerance frequency under load", where=where,
                        solo=round(solo), deviating={c: round(r) for c, r in low.items()})
            self.exc_zero(f"{where}/round{index}")
        return {"solo_iterations_per_second": round(solo), "rounds": rounds}

    def command(self, state, current, where):
        """One frequency request: current -> state, a single write to the E register."""
        if self.requests_closed or self.proxy_dead:
            raise Stop("frequency request after a stop", state=state)
        self.decide(HANDOFF_STATE <= state <= LAST_STATE, "state outside the J813 E ladder",
                    state=state)
        before = self.read(E_CMD)
        self.decide(not before & BUSY and before & STATE_MASK == current and
                    (before ^ E_HANDOFF) & ~(STATE_MASK | SET) == 0,
                    "E command register not as expected", where=where, value=hex(before),
                    state=current)
        value = command_value(before, state)
        self.decide(value == command_value(E_HANDOFF, state), "write differs from the plan",
                    value=hex(value))
        self.op("clear_result", self.t.clear_result, self.result)
        previously, self.written = self.written, True  # from here the write may have happened
        status = self.op("call:dvfs_step", self.t.call, self.fn("dvfs_step"), E_CMD, before, value,
                         BUSY_LIMIT_TICKS, self.result, state=state, write=hex(value))
        st, seen, after, ticks, polls = self.op("read_result", self.t.read_result, self.result)
        record = {"state": state, "before": hex(before), "write": hex(value),
                  "status": STEP_STATUS.get(status, status), "seen": hex(seen), "after": hex(after),
                  "busy_ticks": ticks, "busy_us": round(ticks * 1e6 / COUNTER_HZ, 3), "polls": polls}
        if status in (1, 2) and st == status:
            self.written = previously  # the routine returned before its store
        self.decide(status == st and status in STEP_STATUS, "step routine result unreadable",
                    record=record)
        self.decide(status != 1, "busy before write", record=record)
        self.decide(status != 2, "register changed before write", record=record)
        self.decide(status != 3, "busy timeout", record=record)
        echo = self.read(E_CMD)
        record["echo"] = hex(echo)
        self.decide(not echo & BUSY and echo & STATE_MASK == state and after & STATE_MASK == state,
                    "wrong echo", record=record)
        self.decide((echo ^ before) & ~REQUEST_BITS == 0, "unrelated E bits changed", record=record)
        self.decide(ticks <= SETTER_BOUND_TICKS, "slower than the resident setter bound",
                    record=record)
        p = self.read(P_CMD)
        self.decide(p == P_HANDOFF, "P command register changed", value=hex(p))
        self.exc_zero(where + "/command")
        return record

    # -- phases ------------------------------------------------------------
    def preflight(self):
        identity = self.op("identity", self.t.identity)
        self.report["identity"] = identity
        self.decide(identity["chip_id"] == 0x8142, "wrong chip", chip=identity["chip_id"])
        self.decide(identity["board_id"] == 0x24, "wrong board", board=identity["board_id"])
        self.decide(identity["model"] == "Mac17,3", "wrong model", model=identity["model"])
        words = identity["e_ladder_words"]
        self.decide(len(words) == 2 * len(E_LADDER_KHZ) and tuple(words[::2]) == E_LADDER_KHZ,
                    "E ladder differs from J813", ladder=words[::2])
        cpus = sorted(identity["cpus"], key=lambda c: c["id"])
        running = [c["id"] for c in cpus if c["running"]]
        e_clusters = {c["cluster"] for c in cpus if c["id"] in E_CPUS}
        p_clusters = {c["cluster"] for c in cpus if c["id"] not in E_CPUS}
        self.decide([c["id"] for c in cpus] == list(range(CPU_COUNT)) and running == [BOOT_CPU] and
                    len(e_clusters) == 1 and len(p_clusters) == 1 and e_clusters != p_clusters,
                    "CPU topology is not cpu0-5 E, cpu6 boot P", cpus=cpus)
        self.exc_zero("preflight")
        e, p = self.read(E_CMD), self.read(P_CMD)
        self.decide(e == E_HANDOFF and p == P_HANDOFF, "command registers are not the J813 handoff",
                    e=hex(e), p=hex(p))
        self.base = self.op("load_code", self.t.load_code, self.code)
        self.result = self.op("alloc_result", self.t.alloc_result)
        frequency = self.op("call:read_cntfrq", self.t.call, self.fn("read_cntfrq"))
        self.decide(frequency == COUNTER_HZ, "counter frequency is not 1 GHz", value=frequency)

    def start_cores(self):
        self.op("smp_start_secondaries", self.t.start_secondaries)
        mpidrs = {c["id"]: c["mpidr"] for c in self.report["identity"]["cpus"]}
        for cpu in E_CPUS:
            self.decide(self.op("smp_is_alive", self.t.is_alive, cpu), "secondary missing", cpu=cpu)
            mpidr = self.op("smp_call_sync:read_mpidr", self.t.smp_call_sync, cpu, self.fn("read_mpidr"))
            self.decide(mpidr & 0xffffff == mpidrs[cpu] & 0xffffff, "wrong MPIDR", cpu=cpu,
                        mpidr=hex(mpidr))
        self.exc_zero("start")

    def measure(self, state, where):
        khz = E_LADDER_KHZ[state - HANDOFF_STATE]
        result = {"e_probe_mhz": self.probe(0, khz, where), "boot_probe_mhz": self.probe(BOOT_CPU, P10_KHZ, where)}
        self.exc_zero(where + "/probe")
        return result

    def run(self):
        try:
            self.t = self.op("open", self.open_target)
            self.preflight()
            self.save()
            self.start_cores()
            baseline = {"state": HANDOFF_STATE, "measure": self.measure(HANDOFF_STATE, "baseline")}
            baseline["soak"] = self.soak(E_LADDER_KHZ[0], "baseline")
            self.registers("baseline", HANDOFF_STATE)
            self.report["baseline"] = baseline
            self.save()
            for state in range(HANDOFF_STATE + 1, self.max_state + 1):
                step = {"state": state, "khz": E_LADDER_KHZ[state - HANDOFF_STATE]}
                self.steps.append(step)
                step["command"] = self.command(state, state - 1, f"E{state}")
                step["measure"] = self.measure(state, f"E{state}")
                step["soak"] = self.soak(step["khz"], f"E{state}")
                step["post_soak_e_probe_mhz"] = self.probe(0, step["khz"], f"E{state}/post-soak")
                self.registers(f"E{state}/end", state)
                self.exc_zero(f"E{state}/end")
                step["accepted"] = True
                self.log("accept", state=state)
                self.save()
            if self.return_to_handoff:
                back = {"state": HANDOFF_STATE, "return": True}
                self.steps.append(back)
                back["command"] = self.command(HANDOFF_STATE, self.max_state, "return")
                back["measure"] = self.measure(HANDOFF_STATE, "return")
                self.registers("return/end", HANDOFF_STATE)
                back["accepted"] = True
            self.report.update(result="passed", stop=None)
        except Stop as stop:
            self.stopped(stop.reason, stop.detail)
        except BaseException as error:  # host error or Ctrl-C: the state may be unknown
            self.stopped("host error", {"error": f"{type(error).__name__}: {error}"})
            if not isinstance(error, Exception):
                raise
        finally:
            self.finish()
        return self.report

    def stopped(self, reason, detail):
        self.requests_closed = self.written
        self.report.update(result="stopped", stop={"reason": reason, "detail": detail,
                                                   "after_write": self.written,
                                                   "proxy_silent": self.proxy_dead})
        self.log("stop", reason=reason, after_write=self.written)

    def finish(self):
        accepted = [s["state"] for s in self.steps if s.get("accepted") and not s.get("return")]
        self.report["accepted_states"] = accepted
        self.report["frequency_requests_closed"] = self.requests_closed
        self.report["frequency_writes_issued"] = sum(
            1 for e in self.events if e["kind"] == "call:dvfs_step")
        if not self.proxy_dead and self.base is not None:
            try:  # read-only snapshot; never a frequency request
                self.report["final"] = {"e": hex(self.read(E_CMD)), "p": hex(self.read(P_CMD)),
                                        "exc_count": self.op("exc_count", self.t.exc_count)}
            except Stop:
                pass
        self.report["secondaries_started"] = any(
            e["kind"] == "smp_start_secondaries" and "error" not in e for e in self.events)
        self.save()


def summary_lines(report):
    yield f"RESULT {report['result']} target={report['target']} accepted={report['accepted_states']}"
    if report.get("stop"):
        stop = report["stop"]
        yield f"STOP {stop['reason']}: {json.dumps(stop['detail'], default=str)}"
        if stop["after_write"]:
            yield ("NO FURTHER FREQUENCY REQUESTS ON THIS BOOT: the E cluster state is as last "
                   "read above; recover with a VDM reset.")
        else:
            yield "Stopped before any frequency write; the E cluster was not commanded."
    if report.get("final"):
        yield f"FINAL E={report['final']['e']} P={report['final']['p']}"
    if report["target"] == "live" and report["secondaries_started"]:
        yield "Secondaries were started: reset the machine before any chainload."


def compress(events):
    """Operation kinds in order, repeats folded; checks omitted."""
    out = []
    for event in events:
        if event["kind"] in ("check", "accept", "stop"):
            continue
        label = event["kind"]
        if label == "read64":
            label += " " + event["args"][0]
        if out and out[-1][0] == label:
            out[-1][1] += 1
        else:
            out.append([label, 1])
    return " ; ".join(f"{label} x{n}" if n > 1 else label for label, n in out)


def plan_lines(report):
    events = report["events"]
    steps = [i for i, e in enumerate(events) if e["kind"] == "call:dvfs_step"]
    accepts = [i for i, e in enumerate(events) if e["kind"] == "accept"]
    if not steps:
        return
    first = steps[0] - 3  # read64 E, clear_result, then the step
    yield "PLAN preflight/start/baseline (no frequency writes): " + compress(events[:first])
    if accepts:
        yield (f"PLAN per step (E{events[steps[0]]['state']}; every step identical): "
               + compress(events[first:accepts[0] + 1]))
    for write in report["plan"]["planned_writes"]:
        yield f"PLAN write64 {write['address']} <- {write['value']} (E{write['state']})"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", help="resident proxy device (required unless --dry-run)")
    parser.add_argument("--proxyclient", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--max-state", type=int, default=LAST_STATE,
                        help=f"highest E command state to try, {HANDOFF_STATE + 1}..{LAST_STATE}")
    parser.add_argument("--soak-seconds", type=float, default=3.0,
                        help="six-core soak per state, 1..10 s in 1 s rounds")
    parser.add_argument("--no-return", action="store_true",
                        help=f"leave the E cluster at the last accepted state, not {HANDOFF_STATE}")
    parser.add_argument("--dry-run", action="store_true",
                        help="open no device; run the plan against a nominal model and print it")
    parser.add_argument("--output", type=Path, help="JSON artifact (must not exist)")
    args = parser.parse_args(argv)
    if not HANDOFF_STATE < args.max_state <= LAST_STATE:
        parser.error(f"--max-state must be {HANDOFF_STATE + 1}..{LAST_STATE}")
    if not 1.0 <= args.soak_seconds <= 10.0:
        parser.error("--soak-seconds must be 1..10")
    if args.dry_run == bool(args.device):
        parser.error("give exactly one of --device or --dry-run")
    if args.output is not None:
        args.output = args.output.resolve()
        if args.output.exists():
            parser.error("--output exists; give a new path")
    sys.path.insert(0, str(args.proxyclient.resolve()))
    code, offsets = assemble()
    if args.dry_run:
        kind, open_target = "model", lambda: ModelTarget(offsets)
        generator = random.Random(0x8142)
        nonce = lambda: generator.getrandbits(64)
    else:
        kind, open_target = "live", lambda: LiveTarget(args.device, args.proxyclient.resolve())
        nonce = lambda: secrets.randbits(64)
    qualifier = Qualifier(open_target, kind, code, offsets, args.max_state, not args.no_return,
                          args.soak_seconds, nonce, args.output)
    try:
        report = qualifier.run()
    finally:
        if qualifier.t is not None:
            qualifier.t.close()
    if args.dry_run:
        print("DRY RUN: no device opened; frequencies below are modeled, not measured.")
        print(f"code sha256={report['code']['sha256']} size={report['code']['size']}")
        for line in plan_lines(report):
            print(line)
        for event in report["events"]:
            if event["kind"] == "stop" or (event["kind"] == "check" and not event["passed"]):
                print("EVENT", json.dumps(event, default=str))
    for line in summary_lines(report):
        print(line)
    if args.output:
        print(f"ARTIFACT {args.output}")
    return 0 if report["result"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
