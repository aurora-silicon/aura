#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real profile -> host lifecycle -> ADT/BootArgs -> Mu HOB/EFI artifact replay.

Only transport, physical memory and hardware calls are modeled. See the failure
model written before implementation in docs/windows-g17-failure-model.md.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import traceback
from types import SimpleNamespace
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "proxyclient"))
from construct import Bytes, Int32ul, Int64ul
from aura.adt import load_adt
from aura.proxy import (Feature, IODEV, UartInterface, UartError, UartTimeout,
                        UartCMDError, UartChecksumError)
from serial import SerialException
from aura.tgtypes import BootArgs_r2
from aura.windows import (CAPS_MAGIC, CONFIGURATION, REQUIRED_CAPS, MEMORY_PREFLIGHT,
                          HardwareSession, plan_profile, source_manifest)
from aura import windows_g17 as g17


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class AdmissionWire:
    """Actual UART codec over an owned byte stream; never opens a device."""
    errors = {"wire-feature-timeout": UartTimeout, "wire-read-timeout": UartTimeout,
              "wire-command-mismatch": UartCMDError, "wire-data-checksum": UartChecksumError,
              "wire-device-loss": SerialException, "wire-semantic-rejection": ValueError}

    def __init__(self, case, texts, trace):
        self.case, self.texts, self.trace = case, texts, trace
        self.incoming, self.tx, self.rx = bytearray(), bytearray(), bytearray()
        self.poisoned, self.device_lost = False, False
        self.iface = UartInterface(self)
        self.iface.enabled_features = Feature.DISABLE_DATA_CSUMS

    def flushOutput(self): pass
    def flushInput(self): pass

    def read(self, size):
        if self.device_lost: raise SerialException("injected TEXT device loss")
        data = bytes(self.incoming[:size]); del self.incoming[:size]
        return data

    def write(self, command):
        assert len(command) == 64
        assert self.iface.checksum(command[:-4]) == struct.unpack_from("<I", command, 60)[0]
        kind = struct.unpack_from("<I", command)[0]
        self.tx.extend(command)
        self.trace.append({"op": "wire-command", "kind": kind, "after_fault": self.poisoned})
        if self.poisoned:
            raise OSError("unexpected command after ambiguous transport failure")
        extra = b""
        if kind == UartInterface.REQ_NOP:
            features = struct.unpack_from("<Q", command, 4)[0]
            result = struct.pack("<Q", features).ljust(24, b"\0")
        else:
            assert kind == UartInterface.REQ_MEMREAD
            address, size = struct.unpack_from("<QQ", command, 4)
            base = next(base for base, data in self.texts.items() if base <= address < base + len(data))
            extra = self.texts[base][address - base:address - base + size]
            assert len(extra) == size
            result = struct.pack("<I", self.iface.checksum(extra)).ljust(24, b"\0")
        reply_kind = UartInterface.REQ_MEMWRITE if self.case == "wire-command-mismatch" and extra else kind
        header = struct.pack("<Ii24s", reply_kind, 0, result)
        reply = header + struct.pack("<I", self.iface.checksum(header)) + extra
        if self.case == "wire-feature-timeout" and kind == UartInterface.REQ_NOP:
            reply = reply[:12]; self.poisoned = True
        elif extra and self.case != "wire-semantic-rejection":
            self.poisoned = True
            if self.case == "wire-read-timeout": reply = reply[:36 + 16]
            if self.case == "wire-data-checksum": reply = reply[:-1] + bytes([reply[-1] ^ 1])
            if self.case == "wire-device-loss": self.device_lost = True
        self.rx.extend(reply); self.incoming.extend(reply)
        return len(command)

    def save(self, folder, error):
        (folder / "transport-tx.bin").write_bytes(self.tx)
        (folder / "transport-rx.bin").write_bytes(self.rx)
        (folder / "transport.json").write_text(json.dumps({
            "expected_exception": self.errors[self.case].__name__,
            "actual_exception": type(error).__name__, "message": str(error),
            "trace": self.trace, "physical_device_opened": False}, indent=2) + "\n")


def fixture():
    spec = importlib.util.spec_from_file_location("windows_hv_e2e", ROOT / "proxyclient/tools/windows_hv_e2e.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    adt, _, args = helper.fixture()
    # Real J813 Mu fixed metadata destinations lie in the 16-GiB DRAM envelope.
    delta = 0x10000000000 - args.phys_base
    args.phys_base += delta
    args.top_of_kernel_data += delta
    args.video.base += delta
    args.mem_size, args.mem_size_actual = 0x378000000, 0x400000000
    memory = adt["/chosen/memory-map"]
    for name in ("SEPFW", "TrustCache", "DeviceTree", "BootArgs"):
        base, size = memory._properties[name]
        memory._properties[name] = (base + delta, size)
    chosen = adt["/chosen"]
    chosen.board_id = 0x24
    chosen._types["board-id"] = (Int32ul, False)
    chosen._properties[g17.PROPERTY] = b"stale"
    chosen._types[g17.PROPERTY] = (Bytes(5), False)
    chosen._properties["aura,windows-g17-report-v1"] = b"stale"
    chosen._types["aura,windows-g17-report-v1"] = (Bytes(5), False)
    regions = [(0x10050000000, 0x4000), (0x10050004000, 0x4000),
               (0x10050008000, 0x80000), (0x103fc000000, 0x4000),
               (0x10004000000, 0x134000), (0x10004200000, 0x130000),
               (0x10002000000, 0x4c000), (0x10002080000, 0x48000)]
    sgx = adt["/arm-io"].create_node("sgx")
    for name, (base, size) in zip(g17.NAMES, regions):
        if name.endswith("-text"):
            continue
        for suffix, value in (("-base", base), ("-size", size)):
            sgx._properties[name + suffix] = value
            sgx._types[name + suffix] = (Int64ul, False)
    texts = {}
    for i, (role, size, gkts, ecap) in enumerate(g17.TEXT_PROFILES):
        node = adt["/arm-io"].create_node(role)
        node.segment_names = "__TEXT;__DATA"
        text_base, _ = regions[6 + i]
        data_base, data_size = regions[4 + i]
        node.segment_ranges = (struct.pack("<QQQII", text_base, 0xfffffc0000000000, text_base, size, 1) +
                               struct.pack("<QQQII", data_base, 0xfffffc0000000000 + size, data_base, data_size, 0))
        node._types["segment-ranges"] = (Bytes(64), False)
        image = bytearray(size)
        image[0x100:0x100 + len(g17.BUILD)] = g17.BUILD
        for off, tag in ((gkts, b"GKTS"), (ecap, b"ECAP")):
            image[off:off + 8] = tag + struct.pack("<I", 8)
        texts[text_base] = bytes(image)
    return load_adt(adt.build()), args, texts


def run_host(case, folder, image, policy=None, require_report=False):
    folder.mkdir()
    adt, args, texts = fixture()
    sgx, chosen = adt["/arm-io/sgx"], adt["/chosen"]
    if case == "wrong-board": chosen.board_id = 0x25
    if case == "wrong-chip": chosen.chip_id = 0x8152
    if case == "wrong-model": adt.model = "Mac17,4"
    if case == "wrong-width": sgx._types["gfx-handoff-base"] = (Int32ul, False); sgx.gfx_handoff_base = 0x1000
    if case == "missing-region": del sgx._properties["gfx-shared-region-size"]
    if case == "overlap": sgx.gfx_shared_l2_region_base = sgx.gfx_handoff_base
    if case == "outside-dram": sgx.gpu_region_base = 0x10400000000
    if case == "guest-boundary": sgx.gfx_shared_region_base = 0x10377fc0000
    if case == "heap-collision": sgx.gfx_handoff_base = 0x10019000000
    if case == "image-collision": sgx.gfx_handoff_base = 0x10021000000
    if case == "data-disagreement": sgx.gfx_data_base += 0x4000
    if case == "short-tables": sgx.gfx_shared_region_size = 0x40000
    if case == "unaligned": sgx.gpu_region_base += 1
    if case == "zero-span": sgx.gpu_region_size = 0
    if case == "wrong-segment-names": adt["/arm-io/gfx-asc"].segment_names = "__DATA;__TEXT"
    if case in ("unknown-build", "wire-semantic-rejection"):
        texts = {a: d.replace(g17.BUILD, b"x" * len(g17.BUILD)) for a, d in texts.items()}
    if case == "wrong-tlv":
        address = min(texts); value = bytearray(texts[address]); value[0x45d09] ^= 1; texts[address] = bytes(value)
    blob = adt.build()
    trace, writes, queries, adt_reads, bootarg_reads = [], {}, 0, 0, 0
    wire = AdmissionWire(case, texts, trace) if case.startswith("wire-") else None
    profile = {"schema": "aura.windows.boot.v1", "name": case,
               "target": {"chip_id": "0x8142", "model": "Mac17,3"},
               "mu": {"path": str(image), "sha256": sha(image), "entry_offset": "0x800"},
               "usb": "disabled"}
    if case != "disabled": profile["g17"] = policy or g17.POLICY
    if case == "unknown-policy": profile["g17"] = "auto"
    if case == "nonstring-policy": profile["g17"] = True
    if case == "reservation-collision":
        profile["reserved_memory"] = [{"name": "other-owner", "base": "0x10050000000", "size": "0x4000"}]
    profile_path = folder / "profile.json"
    profile_path.write_text(json.dumps(profile, indent=2) + "\n")

    class Interface:
        dev = SimpleNamespace(timeout=3)
        tty_log = None
        enabled_features = Feature.DISABLE_DATA_CSUMS

        def nop(self, features=None):
            if wire:
                wire.iface.nop(features)
                self.enabled_features = wire.iface.enabled_features
                trace.append({"op": "features", "value": int(self.enabled_features)})
                return
            self.enabled_features = Feature.get_all() if features is None else features
            if case == "checksum-refused": self.enabled_features = Feature.DISABLE_DATA_CSUMS
            trace.append({"op": "features", "value": int(self.enabled_features)})

        def readmem(self, address, size):
            nonlocal adt_reads, bootarg_reads
            trace.append({"op": "read", "address": address, "size": size})
            if address == 0x10000008000:
                bootarg_reads += 1
                value = bytearray(BootArgs_r2.build(args))
                if case == "bootargs-drift" and bootarg_reads > 1: value[-1] ^= 1
                return bytes(value)
            if address == 0x10000100000:
                adt_reads += 1
                value = bytearray(blob.ljust(size, b"\0"))
                if case == "source-drift" and adt_reads > 1: value[-1] ^= 1
                return bytes(value)
            if address in texts:
                assert not self.enabled_features & Feature.DISABLE_DATA_CSUMS
                if wire: return wire.iface.readmem(address, size)
                if case == "read-fault": raise OSError("injected guarded TEXT transfer fault")
                return texts[address][:size - (1 if case == "short-read" else 0)]
            raise AssertionError("unexpected read")

        def writemem(self, address, data):
            writes[address] = bytes(data)
            trace.append({"op": "write", "address": address, "size": len(data)})

        def readstruct(self, address, layout): return layout.parse(self.readmem(address, layout.sizeof()))
        def set_handler(self, *args): pass
        def set_event_handler(self, *args): pass

    class Proxy:
        def unexpected(self, *args): raise AssertionError("unexpected modeled proxy operation")
        call = el0_call = el1_call = gl1_call = gl2_call = unexpected
        read8 = read16 = read32 = write8 = write16 = write32 = write64 = unexpected
        def hv_windows_capabilities(self):
            flags = REQUIRED_CAPS | CONFIGURATION
            if case != "missing-capability": flags |= MEMORY_PREFLIGHT
            return CAPS_MAGIC | 1 << 16 | flags
        def hv_windows_heap_high_water(self):
            nonlocal queries
            queries += 1
            if case == "unaligned-guest": return 0x10010004040
            return 0x10010004000 + (0x4000 if case == "heap-drift" and queries > 1 else 0)
        def get_bootargs_rev(self): return 0x10000008000, 2
        def get_base(self): return 0x10008000000
        def get_cpu_features(self): return SimpleNamespace(apple_sysregs_unlocked=False)
        def iodev_whoami(self): return IODEV.USB0
        def heapblock_alloc(self, size):
            assert size == 0; trace.append({"op": "mutate-heap"}); return 0x10010004000
        def heapblock_set_limit(self, limit):
            assert limit == 0x10018004000; trace.append({"op": "mutate-limit"})
        def hv_windows_configure(self, flags):
            assert flags == 0; trace.append({"op": "mutate-config"}); return 0
        def iodev_set_usage(self, *args): pass
        def hv_init(self): trace.append({"op": "mutate-hv-init"}); return 0
        def hv_map_vuart(self, *args): return 0
        def mcc_get_carveouts(self): return 0x1000
        def read64(self, address): assert address in (0x1000, 0x1008); return 0
        def dc_cvau(self, *args): pass
        def ic_ivau(self, *args): pass
        def memcpy8(self, dest, source, size): trace.append({"op": "copy", "address": dest, "size": size})
        def hv_map(self, *args): return 0
        def fb_improve_logo(self): pass
        def fb_shutdown(self, *args): pass
        def hv_start(self, entry, bootargs):
            assert entry == session.hv.entry
            passed = BootArgs_r2.parse(writes[bootargs])
            assert passed.devtree - passed.virt_base + passed.phys_base == session.hv.adt_base
            assert passed.devtree_size == len(writes[session.hv.adt_base])
            (folder / "guest-bootargs.bin").write_bytes(writes[bootargs])
            (folder / "guest.adt").write_bytes(writes[session.hv.adt_base])
            trace.append({"op": "modeled-entry"})
            return 0

    session = HardwareSession.__new__(HardwareSession)
    session.iface, session.proxy, session.hv, session.serial_capture = Interface(), Proxy(), None, None
    session.proxy.iface = session.iface
    accepted = case in ("enabled", "disabled")
    late_failure = case in ("late-marker", "late-source", "late-tree-size", "late-report")
    before_start = None
    result = {"case": case, "expected_acceptance": accepted}
    try:
        plan = plan_profile(profile_path)
        session.identity()
        session.prepare(plan)
        session.hv.u.compressed_writemem = lambda address, data, *unused: session.iface.writemem(address, data)
        def instruction(value):
            assert value == 0xd50c83df
            trace.append({"op": "modeled-tlbi"})
        session.hv.u.inst = instruction
        result["loaded"] = session.load(plan)
        if case == "late-marker":
            value = bytearray(session.hv.adt["/chosen"]._properties[g17.PROPERTY])
            value[-1] ^= 1
            session.hv.adt["/chosen"]._properties[g17.PROPERTY] = bytes(value)
        if case == "late-source": session.hv.adt["/arm-io/sgx"].gfx_handoff_base += 0x4000
        if case == "late-tree-size": session.hv.adt["/product"].product_name += " changed"
        if case == "late-report":
            value = bytearray(session.hv.adt["/chosen"]._properties["aura,windows-g17-report-v1"])
            value[-1] ^= 1
            session.hv.adt["/chosen"]._properties["aura,windows-g17-report-v1"] = bytes(value)
        before_start = len(trace)
        session.start()
        assert accepted, "unexpected admission: " + case
        guest = load_adt((folder / "guest.adt").read_bytes())
        if require_report:
            report_record = g17.raw_property(guest["/chosen"], "aura,windows-g17-report-v1")
            assert len(report_record) == 688
            (folder / "report.bin").write_bytes(report_record)
        else:
            assert "aura,windows-g17-report-v1" not in guest["/chosen"]._properties
        if accepted and case != "disabled":
            record = g17.raw_property(guest["/chosen"], g17.PROPERTY)
            (folder / "record.bin").write_bytes(record)
            assert len(record) == g17.SIZE
            assert [r["classification"] for r in session.hv.windows_g17_receipt["regions"]] == [1, 1, 1, 3, 2, 2, 2, 2]
        else:
            assert g17.PROPERTY not in guest["/chosen"]._properties
            assert not any(x["op"] == "read" and x["address"] in texts for x in trace)
        result["accepted"] = True
    except (ValueError, OSError, UartError) as error:
        if wire:
            wire.save(folder, error)
            assert type(error) is wire.errors[case], "first transport/admission failure was masked"
            assert not any(item.get("after_fault") for item in trace), "command followed ambiguous failure"
            nops = [item for item in trace if item.get("kind") == UartInterface.REQ_NOP]
            assert len(nops) == (2 if case == "wire-semantic-rejection" else 1)
        assert not accepted, str(error)
        if late_failure:
            assert before_start is not None and len(trace) == before_start, trace
        else:
            assert not any(x["op"].startswith("mutate-") for x in trace), trace
        result.update(accepted=False, rejection=str(error))
    result["trace"] = trace
    (folder / "host.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def run_mu(mu, output, report):
    """Consume exactly the host's serialized artifacts through production Mu."""
    consumer_folder = output / "mu-consumer"

    def command(name, argv, expected):
        result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=60)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        report["commands"].append({"name": name, "argv": [str(x) for x in argv],
                                   "exit_code": result.returncode, "expected": expected})
        assert result.returncode == expected, name + ": " + result.stderr

    command("build-mu-consumer", [sys.executable, mu / "Tools/g17-preservation-e2e/build.py",
                                  "--output", consumer_folder], 0)
    binary = consumer_folder / "mu-g17-consumer"
    passed = BootArgs_r2.parse((output / "enabled/guest-bootargs.bin").read_bytes())
    bounds = [hex(passed.phys_base), hex(passed.mem_size), hex(passed.mem_size_actual)]

    fd_base = json.loads((output / "enabled/host.json").read_text())["loaded"]["guest_base"]

    def consume(name, adt, expected=19, extra=(), memory=bounds, boot_args=None):
        result = output / (name + ".json")
        if boot_args is None:
            boot_args = output / ("disabled" if name == "mu-disabled" else "enabled") / "guest-bootargs.bin"
        command(name, [binary, adt, *memory, result, "--fd-base", fd_base,
                       "--bootargs", boot_args, "--bootargs-sha256", sha(boot_args), *extra], expected)
        if expected == 0:
            receipt = json.loads(result.read_text())
            assert receipt["early_pei_dxe_ready_to_boot_passed"] and receipt["early_copy_calls"] == 2
            assert receipt["marker_present"] == (name != "mu-disabled")
            if name != "mu-disabled":
                assert [r["classification"] for r in receipt["regions"]] == [1, 1, 1, 3, 2, 2, 2, 2]

    enabled = output / "enabled/guest.adt"
    consume("mu-enabled", enabled, 0)
    consume("mu-disabled", output / "disabled/guest.adt", 0)
    consume("mu-allocated-owner", enabled, 20, extra=("--allocated", "0x10050000000", "0x4000"))
    consume("mu-uefi-overlap", enabled, 20, extra=("--uefi", "0x10050000000", "0x4000"))
    consume("mu-early-bootargs-overlap", enabled, extra=("--boot-dest", "0x10050000000"))
    consume("mu-early-adt-overlap", enabled, extra=("--adt-dest", "0x10050000000"))
    consume("mu-early-image-overlap", enabled, extra=("--fd-base", "0x10050000000"))
    consume("mu-map-drift", enabled, 21, ("--map-fault",))
    consume("mu-map-duplicate-gap", enabled, 21, ("--map-duplicate-gap",))
    consume("mu-ready-map-drift", enabled, 22, ("--ready-map-fault",))
    consume("mu-guest-drift", enabled, memory=["0x10020008000", "0x357ff8000", bounds[2]])
    consume("mu-installed-dram-drift", enabled, memory=[*bounds[:2], "0x7c000000"])
    for name in ("stale-adt-size", "truncated", "unknown-revision"):
        boot = bytearray((output / "enabled/guest-bootargs.bin").read_bytes())
        if name == "stale-adt-size":
            value = BootArgs_r2.parse(boot)
            value.devtree_size -= 4
            boot = BootArgs_r2.build(value)
        elif name == "truncated": boot = boot[:64]
        else: struct.pack_into("<H", boot, 0, 4)
        path = output / (name + "-bootargs.bin")
        path.write_bytes(boot)
        consume("mu-bootargs-" + name, enabled, boot_args=path)
    malformed = output / "truncated.adt"
    malformed.write_bytes(enabled.read_bytes()[:-1])
    consume("mu-truncated-adt", malformed, 10)
    cases = {"crc": (303, 1), "version": (4, 2), "flags": (20, 3),
             "reserved": (108, 1), "classification": (116, 2), "tag-order": (112, 8),
             "stale-base": (122, 1), "firmware-profile": (56, ord("X"))}
    for name, (offset, value) in cases.items():
        adt = load_adt(enabled.read_bytes())
        record = bytearray(g17.raw_property(adt["/chosen"], g17.PROPERTY))
        record[offset] = value
        if name != "crc":
            struct.pack_into("<I", record, 104, 0)
            struct.pack_into("<I", record, 104, zlib.crc32(record))
        adt["/chosen"]._properties[g17.PROPERTY] = bytes(record)
        path = output / ("tampered-" + name + ".adt")
        path.write_bytes(adt.build())
        consume("mu-tampered-" + name, path)
    adt = load_adt(enabled.read_bytes())
    adt["/arm-io/sgx"].gfx_handoff_base += 0x4000
    path = output / "stale-source.adt"
    path.write_bytes(adt.build())
    consume("mu-stale-source", path)
    adt = load_adt(enabled.read_bytes())
    record = bytearray(g17.raw_property(adt["/chosen"], g17.PROPERTY))
    # A self-consistent, in-guest firmware span that conflicts with the actual
    # early stack/workspace must be rejected before Mu writes those locations.
    workspace_overlap = passed.phys_base + passed.mem_size - 0x4000
    adt["/arm-io/sgx"].gfx_handoff_base = workspace_overlap
    struct.pack_into("<Q", record, 120, workspace_overlap)
    struct.pack_into("<I", record, 104, 0)
    struct.pack_into("<I", record, 104, zlib.crc32(record))
    adt["/chosen"]._properties[g17.PROPERTY] = bytes(record)
    path = output / "early-workspace-conflict.adt"
    path.write_bytes(adt.build())
    consume("mu-early-workspace-overlap", path)
    report["mu_consumer"] = json.loads((consumer_folder / "build-receipt.json").read_text())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mu-image", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"result": "failed", "hardware_accessed": False, "native_gpu_started": False,
              "scope": "host lifecycle and compiled Mu memory-preservation replay", "cases": [],
              "host_sources": source_manifest(), "commands": []}
    try:
        image = args.mu_image.resolve() if args.mu_image else output / "synthetic-mu.fd"
        if not args.mu_image: image.write_bytes(bytes(65536))
        report["image"] = {"path": str(image), "sha256": sha(image), "native": bool(args.mu_image)}
        cases = ("disabled", "enabled", "unknown-policy", "nonstring-policy", "wrong-board", "wrong-chip", "wrong-model", "wrong-width", "missing-region", "overlap",
                 "outside-dram", "guest-boundary", "heap-collision", "image-collision", "data-disagreement",
                 "short-tables", "unaligned", "zero-span", "wrong-segment-names", "unknown-build", "wrong-tlv",
                 "reservation-collision", "checksum-refused", "read-fault", "short-read", "missing-capability",
                 "heap-drift", "unaligned-guest", "source-drift", "bootargs-drift", "late-marker", "late-source", "late-tree-size",
                 "wire-feature-timeout", "wire-read-timeout", "wire-command-mismatch", "wire-data-checksum",
                 "wire-device-loss", "wire-semantic-rejection")
        for case in cases:
            with (output / (case + ".log")).open("w") as log, contextlib.redirect_stdout(log):
                report["cases"].append(run_host(case, output / case, image))
        # The companion Mu runner is built from production admission, ADT and
        # PEI/HOB code. Its receipt and native command logs are retained here.
        report["host_result"] = "passed"
        run_mu(args.mu.resolve(), output, report)
        report["result"] = "passed"
    except Exception as error:
        report["error"] = type(error).__name__ + ": " + str(error)
        report["traceback"] = traceback.format_exc()
    finally:
        report["sources"] = {str(path.relative_to(ROOT)): sha(path) for path in
                             (Path(__file__).resolve(), ROOT / "docs/windows-g17-failure-model.md",
                              ROOT / "proxyclient/tools/windows_hv_e2e.py")}
        report["artifacts"] = {str(path.relative_to(output)): sha(path) for path in sorted(output.rglob("*"))
                               if path.is_file() and path.suffix in (".adt", ".bin", ".fd", ".json")
                               and path.name != "receipt.json"}
        (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["result"], output / "receipt.json")
    return report["result"] != "passed"


if __name__ == "__main__":
    raise SystemExit(main())
