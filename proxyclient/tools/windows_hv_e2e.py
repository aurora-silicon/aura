#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the real Windows HV host lifecycle against a bounded transport model.

Failure model: configuration rejection must precede HV initialization; host
setup must never overwrite C-owned system registers; a locked target must not
write RVBAR; raw payload/ADT/bootargs must retain their ABI and 64-KiB alignment;
out-of-RAM images must not upload; requested conversion must not spoof depth.
This does not execute the C backend, Mu or Windows, or qualify any hardware.
"""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import signal
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from construct import Array, Container, Int64ul
from aura.adt import ADTNode, load_adt
from aura.hv import HV
from aura.proxy import AuraProxy, EXC, EXC_RET, IODEV, ProxyCommandError, START
from aura.tgtypes import BootArgs_r2, BootArgs_r3
from aura.windows import (CAPS_MAGIC, CONFIGURATION, FRAMEBUFFER_SHADOW, REQUIRED_CAPS,
                          HardwareSession, N1_HANDOFF, N1_PROPERTY, N1_RESOURCES,
                          MEMORY_PREFLIGHT, SERIAL_CAPTURE, memory_reservation_preflight, prepare_n1_handoff, source_manifest)


def fixture():
    root = ADTNode()
    root.name = "device-tree"
    root.model = "Mac17,3"
    root.address_cells = root.size_cells = 2
    product = root.create_node("product")
    product.product_name = product.product_description = "Synthetic Mac"
    product.product_soc_name = "T8142"
    root.create_node("defaults")
    chosen = root.create_node("chosen")
    chosen.chip_id = 0x8142
    chosen._properties[N1_PROPERTY] = b"stale"
    chosen.stdout_path = "usb-console:115200"
    aliases = root.create_node("aliases")
    aliases.usb_console = "/arm-io/usb-drd0"
    aliases.usb_chain = "usb-console"
    aliases.usb_child = "/arm-io/usb-drd1/device"
    aliases.display = "/arm-io/disp0"
    memory = chosen.create_node("memory-map")
    for name, value in (("SEPFW", (0x810000000, 0x1000)), ("TrustCache", (0x810004000, 0x1000)),
                        ("DeviceTree", (0x800100000, 0x10000)), ("BootArgs", (0x800110000, 0x4000))):
        setattr(memory, name, value)
        memory._types[name] = (Array(2, Int64ul), False)
    cpus = root.create_node("cpus")
    for i in range(2):
        cpu = cpus.create_node("cpu" + str(i))
        cpu.cpu_id, cpu.cluster_id = i, 0
        cpu.state = "running" if i == 0 else "halted"
        cpu.cpu_impl_reg = (0x210000000 + i * 0x10000, 0x4000)
        cpu._types["cpu-impl-reg"] = (Array(2, Int64ul), False)
    arm = root.create_node("arm-io")
    arm.address_cells = arm.size_cells = 2
    arm.ranges = struct.pack("<QQQ", 0, 0, 1 << 40)
    for name in ("usb-drd0", "usb-drd1", "dart-usb0", "dart-usb1", "disp0", "dart-disp0"):
        arm.create_node(name)
    pmgr = arm.create_node("pmgr")
    pmgr.compatible = b"pmgr,synthetic\0"
    pmgr.devices = bytes(48 * 2)  # Two fixed-width synthetic PMGR records.
    uart = arm.create_node("uart0")
    uart.reg = [Container(addr=0x235200000, size=0x4000)]
    uart.interrupts = (10,)
    setattr(uart, "AAPL,phandle", 1)
    blob = root.build()
    args = Container(revision=2, version=2, virt_base=0xfffffe0010000000,
                     phys_base=0x800000000, mem_size=0x80000000,
                     mem_size_actual=0x80000000, top_of_kernel_data=0x820000000,
                     video=Container(base=0x810100000, display=1, stride=4096,
                                     width=1024, height=768, depth=30),
                     machine_type=0, devtree=0xfffffe0010100000,
                     devtree_size=0x10000, cmdline="", boot_flags=0)
    return load_adt(blob), blob, args


def wire_namespace():
    """Exercise real request serialization, including an old resident rejection."""
    observed = []
    class Wire:
        legacy = False
        def proxyreq(self, payload, **kwargs):
            opcode, *args = struct.unpack("<7Q", payload)
            observed.append(opcode)
            assert opcode not in range(0xc11, 0xc19), "unsafe legacy PSCI opcode"
            assert opcode in (0xc80, 0xc81, 0xc82, 0xc83)
            if opcode == 0xc82: assert args[:2] == [0x1000, 128]
            if opcode == 0xc83: assert args == [0] * 6
            if self.legacy:
                return struct.pack("<QqQ", opcode, AuraProxy.S_BADCMD, 0)
            value = CAPS_MAGIC | 1 << 16 | REQUIRED_CAPS | CONFIGURATION if opcode == 0xc80 else 0
            return struct.pack("<QqQ", opcode, AuraProxy.S_OK, value)
    wire = Wire()
    proxy = AuraProxy(wire)
    assert proxy.hv_windows_capabilities() >> 32 == CAPS_MAGIC >> 32
    assert proxy.hv_windows_configure(0) == 0
    assert proxy.hv_windows_prepare_n1(0x1000, 128) == 0
    assert proxy.hv_windows_heap_high_water() == 0
    wire.legacy = True
    try:
        proxy.hv_windows_capabilities()
    except ProxyCommandError:
        pass
    else:
        raise AssertionError("legacy resident accepted capability probe")
    assert observed == [0xc80, 0xc81, 0xc82, 0xc83, 0xc80]
    return {"case": "serialized-safe-opcode-namespace", "opcodes": [hex(op) for op in observed]}


def identity_translation(case):
    _, blob, args = fixture()
    layout, bootargs_address = BootArgs_r2, 0x800008000
    args.virt_base = 0xffffffffffff0000
    physical = args.phys_base + 0x20000
    if case == "identity-outside-ram":
        physical = args.phys_base + args.mem_size - 0x8000
    args.devtree = (physical - args.phys_base + args.virt_base) & ((1 << 64) - 1)
    if case == "identity-j813-recorded-addresses":
        # Exact address fields from the read-only hardware receipt; ADT content
        # remains synthetic so this case can be shared/replayed without a Mac.
        args.update(dict(revision=3, phys_base=0x10003084000, mem_size=0x3d957c000,
                         mem_size_actual=0x400000000, virt_base=0xffffffffff084000,
                         devtree=0xfa8000, devtree_size=0x7c000, top_of_kernel_data=0x10007ac0000))
        physical = 0x10004fa8000
        layout, bootargs_address = BootArgs_r3, 0x10007abc000
    reads = []
    class Proxy:
        def get_bootargs_rev(self): return (bootargs_address, args.revision)
        def get_base(self): return 0x800000000
    class Interface:
        def readmem(self, address, size):
            reads.append([hex(address), size])
            if address == bootargs_address:
                assert size == layout.sizeof()
                return layout.build(args)
            assert case != "identity-outside-ram" and address == physical
            assert size == args.devtree_size
            return blob.ljust(size, b"\0")
    session = HardwareSession.__new__(HardwareSession)
    session.proxy, session.iface = Proxy(), Interface()
    try:
        identity = session.identity()
        assert case != "identity-outside-ram"
        assert identity["chip_id"] == 0x8142 and identity["model"] == "Mac17,3"
        assert len(reads) == 2
    except ValueError:
        assert case == "identity-outside-ram"
        assert len(reads) == 1
    return {"case": case, "reads": reads}


def reservation_prepare_flow(case):
    """Use real HardwareSession, ProxyUtils and HV admission; model only I/O."""
    adt, _, args = fixture()
    ranges = [{"name": "handoff", "base": 0x850000000, "size": 0x404000},
              {"name": "uefi-workspace", "from_ram_top": 0x8000000, "size": 0x8000000}]
    if case == "reservation-prepare-default": ranges = []
    if case == "reservation-prepare-payload": ranges[0]["base"] = 0x821020000
    if case == "reservation-prepare-heap": ranges[0]["base"] = 0x819000000
    if case == "reservation-prepare-outside": ranges[0]["base"] = 0x880000000
    if case == "reservation-prepare-overlap": ranges[1] = {"name": "overlap", "base": 0x850001000, "size": 0x1000}
    if case == "reservation-prepare-source": adt["/chosen/memory-map"].SEPFW = (0x850000000, 0x1000)
    if case == "reservation-prepare-framebuffer": args.video.base = 0x850000000
    blob = adt.build()
    trace, heap_queries = [], 0
    plan = SimpleNamespace(chip_id=0x8142, model="Mac17,3", framebuffer="native",
                           guest_compatibility="none", usb="disabled", n1="disabled", serial="disabled",
                           image=bytes(range(256)) * 32, reserved_memory=tuple(ranges))
    class Interface:
        dev = SimpleNamespace(timeout=3)
        tty_log = None
        def nop(self): trace.append("nop")
        def readmem(self, address, size):
            if address == 0x800008000:
                return BootArgs_r2.build(args)
            assert address == 0x800100000 and size == args.devtree_size
            return blob.ljust(size, b"\0")
        def readstruct(self, address, layout): return layout.parse(self.readmem(address, layout.sizeof()))
        def set_handler(self, *args): pass
        def set_event_handler(self, *args):
            if int(args[0]) == 0x80: trace.append("serial-handler")
    class Proxy:
        def hv_windows_capabilities(self):
            trace.append("caps")
            flags = REQUIRED_CAPS | CONFIGURATION
            if case != "reservation-prepare-missing-capability": flags |= MEMORY_PREFLIGHT
            return CAPS_MAGIC | 1 << 16 | flags
        def hv_windows_heap_high_water(self):
            nonlocal heap_queries
            heap_queries += 1
            trace.append("heap-query")
            return 0x810004000 + (0x4000 if case == "reservation-prepare-heap-drift" and heap_queries > 1 else 0)
        def get_bootargs_rev(self): return (0x800008000, 2)
        def get_base(self): return 0x800000000
        def get_cpu_features(self): return SimpleNamespace(apple_sysregs_unlocked=False)
        def iodev_whoami(self): return IODEV.USB0
        def heapblock_alloc(self, size):
            assert size == 0
            trace.append("mutate-heap-alignment")
            return 0x810004000
        def heapblock_set_limit(self, limit):
            assert limit == 0x818004000
            trace.append("mutate-heap-limit")
        def hv_windows_configure(self, flags):
            assert flags == 0
            trace.append("mutate-config")
            return 0
        def iodev_set_usage(self, *args): trace.append("mutate-iodev")
        def hv_init(self): trace.append("mutate-hv-init"); return 0
        def hv_map_vuart(self, *args): trace.append("mutate-vuart"); return 0
        def unexpected(self, *args): raise AssertionError("unexpected modeled proxy operation")
        call = el0_call = el1_call = gl1_call = gl2_call = unexpected
        read8 = read16 = read32 = write8 = write16 = write32 = unexpected
    session = HardwareSession.__new__(HardwareSession)
    session.iface, session.proxy, session.hv = Interface(), Proxy(), None
    session.serial_capture = None
    session.proxy.iface = session.iface
    identity = session.identity()
    assert identity["model"] == "Mac17,3"
    expected_success = case in ("reservation-prepare-success", "reservation-prepare-default")
    try:
        session.prepare(plan)
        assert expected_success
        assert "mutate-hv-init" in trace
        if case == "reservation-prepare-default": assert heap_queries == 0
        else:
            assert heap_queries == 2
            assert session.hv.windows_memory_guard["layout"]["guest_base"] == 0x821020000
    except ValueError:
        assert not expected_success
        assert not any(event.startswith("mutate-") for event in trace), trace
    return {"case": case, "trace": trace, "declared_ranges": ranges}


def lifecycle(case, output):
    adt, blob, args = fixture()
    if case == "adt-overflow-init":
        adt.overflow = "x" * 0x20000
        blob = adt.build()
    trace, writes = [], {}
    n1_descriptor = struct.pack("<IHHIIII4H12Q", 0x4831574e, 1, 128, 7, 0x8142, 0x24,
                                0x32354776, 0x100, 0x101, 0x102, 0,
                                *(value for pair in N1_RESOURCES for value in pair))
    class Proxy:
        def hv_windows_capabilities(self):
            trace.append("caps")
            serial_cap = 0 if case == "serial-missing-capability" else SERIAL_CAPTURE
            return CAPS_MAGIC | 1 << 16 | REQUIRED_CAPS | CONFIGURATION | FRAMEBUFFER_SHADOW | N1_HANDOFF | MEMORY_PREFLIGHT | serial_cap | 0x200
        def hv_windows_configure(self, flags):
            trace.append(["config", flags])
            return -1 if case in ("config-rejected", "serial-carrier-rejected") else 0
        def iodev_whoami(self): return IODEV.UART
        def iodev_set_usage(self, *args): trace.append("iodev")
        def hv_init(self): trace.append("hv_init"); return 0
        def hv_windows_prepare_n1(self, address, size):
            assert "hv_init" in trace and size == 128
            trace.append("n1-prepare")
            if case == "n1-producer-failure": return -1
            value = bytearray(n1_descriptor)
            if case == "n1-bad-header": value[8] ^= 1
            if case == "n1-bad-resource": value[32] ^= 1
            writes[address] = bytes(value)
            return 0
        def hv_map_vuart(self, *args): trace.append("vuart"); return 0
        def mcc_get_carveouts(self): return 0x1000
        def read64(self, address): return 0
        def dc_cvau(self, *args): pass
        def ic_ivau(self, *args): pass
        def memcpy8(self, *args): trace.append("copy")
        def hv_map(self, *args): trace.append("map"); return 0
        def fb_improve_logo(self): pass
        def fb_shutdown(self, *args): pass
        def hv_wdt_start(self, cpu): trace.append("watchdog"); return -1
        def exit(self, result):
            assert result == EXC_RET.EXIT_GUEST
            trace.append("exit-guest")
        def hv_start(self, entry, bootargs):
            trace.append("start")
            if case == "transport-error": raise OSError("injected transport failure")
            if case == "forwarded-exception":
                hv.handle_exception(START.EXCEPTION_LOWER, EXC.SYNC, 0xdead0000)
                return 0
            if case == "forwarded-panic":
                hv.handle_bark(START.HV, 0, 0xdead1000)
                return 0
            assert entry % 0x10000 == 0x800
            parsed = BootArgs_r2.parse(writes[bootargs])
            assert parsed.video.depth == 30, "host spoofed framebuffer depth"
            assert parsed.devtree - parsed.virt_base + parsed.phys_base == hv.adt_base
            chunks = [data for address, data in sorted(writes.items())
                      if hv.guest_base <= address < hv.guest_base + len(payload)]
            assert b"".join(chunks) == payload
            if case == "chunked-upload": assert len(chunks) == 2
            guest_adt = load_adt(writes[hv.adt_base])
            for name in ("usb-drd0", "usb-drd1", "dart-usb0", "dart-usb1"):
                assert "/arm-io/" + name not in guest_adt
            assert "/arm-io/disp0" in guest_adt and "/arm-io/dart-disp0" in guest_adt
            alias_properties = guest_adt["/aliases"]._properties
            assert set(alias_properties) == {"name", "display"}
            assert "stdout-path" not in guest_adt["/chosen"]._properties
            marker = next(p for p in guest_adt["/chosen"].tostruct()["properties"]
                          if p["name"] == "aura,windows-usb-disabled")
            assert marker["value"] == b"\x01\x00\x00\x00"
            if case == "n1-success":
                assert guest_adt["/chosen"].getprop(N1_PROPERTY) == n1_descriptor
            else:
                assert N1_PROPERTY not in guest_adt["/chosen"]._properties
            return 0
        def write64(self, *args): raise AssertionError("RVBAR write on locked target")
    class Interface:
        dev = SimpleNamespace(timeout=3)
        def set_handler(self, *args): pass
        def set_event_handler(self, *args):
            if int(args[0]) == 0x80: trace.append("serial-handler")
        def writemem(self, address, data): writes[address] = bytes(data)
        def readmem(self, address, size):
            if case == "n1-read-failure": raise OSError("injected N1 read failure")
            return writes[address][:size]
    class Utils:
        def __init__(self):
            self.adt, self.ba = adt, args
            self.heap_top = 0x820004000
            self.cpu_features = SimpleNamespace(apple_sysregs_unlocked=False)
            if case == "out-of-ram": self.ba.mem_size = 0x10000
        def get_adt(self): return blob
        def memalign(self, alignment, size):
            assert alignment == 16 and size == 128
            return 0x820001000
        def free(self, address):
            assert address == 0x820001000
            trace.append("n1-free")
        def compressed_writemem(self, address, data, *args):
            if case == "chunked-upload": assert len(data) <= 16 * 1024 * 1024
            trace.append("upload-image"); writes[address] = bytes(data)
        def inst(self, instruction):
            assert instruction == 0xd50c83df
        def msr(self, *args): raise AssertionError("host overwrote C-owned system register")
        def mrs(self, *args): raise AssertionError("host requested generic register setup")
    hv = HV(Interface(), Proxy(), Utils(), windows=True)
    hv.windows_guest_compatibility = "t8142-experimental"
    hv.windows_framebuffer = "bgra-shadow"
    serial_recorder = None
    if case == "cdc-success": hv.windows_serial = "cdc"
    if case.startswith("serial-"):
        from aura.windows_serial import WindowsSerialRecorder
        folder = output / case
        folder.mkdir()
        serial_recorder = WindowsSerialRecorder(folder)
        hv.windows_serial = "capture"
        hv.windows_serial_recorder = serial_recorder
    if case.startswith("n1-"): hv.windows_n1 = "j813-v1"
    if case == "watchdog-failure": hv.wdt_cpu = 1
    payload = bytes(range(256)) * 32
    if case == "chunked-upload": payload = bytes(range(256)) * ((16 * 1024 * 1024 + 8192) // 256)
    if case.startswith("reservation-"):
        plan = SimpleNamespace(image=payload, reserved_memory=(
            {"name": "handoff", "base": 0x850000000, "size": 0x404000},
            {"name": "uefi-workspace", "from_ram_top": 0x8000000, "size": 0x8000000}))
        hv.windows_memory_guard = memory_reservation_preflight(
            plan, args, adt, hv.u.heap_top - 0x10000000, 0x800000000,
            0x800008000, BootArgs_r2.sizeof(), 0x800100000)
        if case == "reservation-layout-drift": hv.u.heap_top += 0x4000
    previous = {signum: signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGQUIT)}
    try:
        hv.init()
        if case != "n1-unprepared": prepare_n1_handoff(hv)
        hv.load_raw(payload, 0x800)
        assert hv.guest_base % 0x10000 == 0
        assert hv.emulate_sprr is False
        if case == "adt-overflow-start": hv.adt.overflow = "x" * 0x20000
        start_trace_index = len(trace)
        hv.start()
        assert case in ("success", "n1-success", "chunked-upload", "reservation-success", "serial-success", "cdc-success")
        config = 11 if case == "cdc-success" else 7 if case.startswith("serial-") else 3
        assert trace.index(["config", config]) < trace.index("hv_init") < trace.index("upload-image") < trace.index("start")
        if case == "serial-success": assert trace.index("serial-handler") < trace.index("start")
    except (RuntimeError, ValueError, OSError):
        assert case not in ("success", "n1-success", "chunked-upload", "reservation-success", "serial-success", "cdc-success")
        if case in ("serial-missing-capability", "serial-carrier-rejected"):
            assert "hv_init" not in trace and "upload-image" not in trace and "start" not in trace
        if case == "serial-missing-capability": assert trace == ["caps"]
        if case == "reservation-layout-drift": assert "upload-image" not in trace and "start" not in trace
        if case.startswith("n1-"):
            assert "upload-image" not in trace and "start" not in trace
            if case != "n1-unprepared":
                assert trace.count("n1-prepare") == 1 and "n1-free" in trace
                hv.windows_n1 = "disabled"
                try:
                    hv.load_raw(payload, 0x800)
                except RuntimeError:
                    pass
                else:
                    raise AssertionError("disabled profile bypassed failed N1 handoff")
        if case in ("config-rejected", "out-of-ram", "adt-overflow-init"):
            assert "upload-image" not in trace and "start" not in trace
        if case in ("config-rejected", "adt-overflow-init"): assert "hv_init" not in trace
        if case == "adt-overflow-init": assert trace == ["caps"]
        if case == "adt-overflow-start": assert trace[start_trace_index:] == []
        if case == "watchdog-failure": assert "watchdog" in trace and "start" not in trace
        if case.startswith("forwarded-"):
            assert "exit-guest" in trace and hv.windows_abort_reason is not None
    finally:
        if serial_recorder is not None: serial_recorder.close()
        assert hv.iface.dev.timeout == 3
        assert all(signal.getsignal(signum) == handler for signum, handler in previous.items())
        for signum, handler in previous.items(): signal.signal(signum, handler)
    for address, data in writes.items():
        (output / f"{case}-{address:x}.bin").write_bytes(data)
    return {"case": case, "trace": trace, "writes": {
        hex(address): {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for address, data in writes.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    output = parser.parse_args().out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    with (output / "host.log").open("w") as log, contextlib.redirect_stdout(log):
        cases = [wire_namespace(), identity_translation("identity-modular-wrap"),
                 identity_translation("identity-outside-ram"),
                 identity_translation("identity-j813-recorded-addresses")] + [lifecycle(case, output) for case in (
            "success", "cdc-success", "serial-success", "serial-missing-capability", "serial-carrier-rejected",
            "chunked-upload", "config-rejected", "out-of-ram", "adt-overflow-init", "adt-overflow-start",
            "watchdog-failure", "forwarded-exception", "forwarded-panic", "transport-error",
            "n1-success", "n1-producer-failure", "n1-bad-header", "n1-bad-resource", "n1-read-failure",
            "n1-unprepared", "reservation-success", "reservation-layout-drift")]
        cases += [reservation_prepare_flow(case) for case in (
            "reservation-prepare-success", "reservation-prepare-default", "reservation-prepare-payload",
            "reservation-prepare-heap", "reservation-prepare-outside", "reservation-prepare-overlap",
            "reservation-prepare-source", "reservation-prepare-framebuffer",
            "reservation-prepare-missing-capability", "reservation-prepare-heap-drift")]
    report = {"result": "passed", "hardware_qualified": False,
              "scope": "Real Python HV init/load/start, modeled transport; no C backend or guest execution",
              "cases": cases, "host_sources": source_manifest(),
              "e2e_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "replay": [sys.executable, str(Path(__file__).resolve()), "--out", "NEW_EMPTY_DIRECTORY"]}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS real HV host lifecycle: {output / 'result.json'}")


if __name__ == "__main__":
    main()
