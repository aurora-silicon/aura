#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guarded, RAM-only aura -> Linux boot for the 26A428 J873g.

Use the matching aura.bin/aura-raw.elf pair. Every hardware phase writes a
fresh PMU receipt and refuses to repeat a phase after an uncertain failure.
No setup.py side effects, resets, boot-object registration or code patching.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time
import types

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_SERIAL = None
sys.path.insert(0, str(ROOT / "proxyclient"))
from aura.proxy import UartInterface, AuraProxy, IODEV, ProxyCommandError
from aura.proxyutils import ProxyUtils
from aura.tgtypes import BootArgs_r3
from aura.adt import load_adt
from aura.asm import ARMAsm
from aura.utils import align
from aura.boot_cdc import start_cdc, require_upload_transport


def require(value, reason):
    if not value:
        raise RuntimeError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def image_pair(directory, expected, *, macho=False):
    raw = (directory / ("aura.macho" if macho else "aura.bin")).read_bytes()
    require(sha(raw) == expected, "aura image hash mismatch")
    if macho:
        from aura.macho import MachO
        raw = MachO(raw).prepare_image()
    from elftools.elf.elffile import ELFFile
    with (directory / ("aura.elf" if macho else "aura-raw.elf")).open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s.entry.st_value for s in elf.get_section_by_name(".symtab").iter_symbols()}
        # Check every allocated, file-backed section against the payload.
        for section in elf.iter_sections():
            if section["sh_flags"] & 2 and section["sh_type"] != "SHT_NOBITS":
                start, size = section["sh_addr"], section["sh_size"]
                require(raw[start:start + size] == section.data(), "ELF/image mismatch: " + section.name)
    return raw, symbols


class Session:
    def __init__(self, device, raw, symbols, *, target="j873", serial_number=None):
        self.f = UartInterface(device)
        try:
            self.f.dev.timeout = 10
            self.f.dev.write_timeout = 10
            self.f.nop()
            self.p = AuraProxy(self.f)
            self.base = self.p.get_base()
            self.raw, self.symbols = raw, symbols
            require(self.read(self.base, 0x4000) == raw[:0x4000], "running image identity mismatch")
            chip, model, target_type = {"j873": (0x8152, "Mac18,5", "J873g"),
                                        "j813": (0x8142, "Mac17,3", "J813")}[target]
            require(self.p.get_chipid() == chip and
                    self.p.read32(self.base + symbols["board_id"]) == 0x24, "wrong chip/board")
            require(self.p.get_exc_count() == 0, "existing proxy exception state")
            self.ba = BootArgs_r3.parse(self.read(self.base + symbols["cur_boot_args"], BootArgs_r3.sizeof()))
            require(0 < self.ba.devtree_size < 0x400000, "bad ADT size")
            self.adt_bytes = self.read(self.p.read64(self.base + symbols["adt"]), self.ba.devtree_size)
            self.adt = load_adt(self.adt_bytes)
            require(self.adt.model == model and self.adt.target_type == target_type, "wrong target/model")
            serial = self.adt._properties["serial-number"]
            self.serial = serial.rstrip(b"\0").decode() if isinstance(serial, bytes) else serial
            require(self.serial == (serial_number or EXPECTED_SERIAL), "not the selected physical target")
        except Exception:
            self.close()
            raise

    def read(self, address, size):
        return b"".join(self.f.readmem(address + i, min(16384, size - i)) for i in range(0, size, 16384))

    def write(self, address, data):
        self.f.writemem(address, data)
        require(sha(self.read(address, len(data))) == sha(data), f"RAM readback failed at {address:#x}")

    def call(self, name, *args):
        off = self.symbols[name]
        require(self.read(self.base + off, 32) == self.raw[off:off + 32], "function changed: " + name)
        return self.p.call(self.base + off, *args)

    def alloc(self, data, alignment=64):
        address = self.p.memalign(alignment, len(data))
        require(address, "allocation failed")
        self.write(address, data)
        return address

    def cstring(self, value):
        return self.alloc(value.encode() + b"\0")

    def close(self):
        self.f.dev.close()


def capture_consoles(args, session, seconds, end_marker):
    """Keep the existing KIS descriptor open so early handoff logs survive."""
    import serial
    captured = bytearray()
    proxy = session.f.dev
    proxy.timeout = 0
    with serial.Serial(args.console, timeout=0.2) as console, \
            (args.output / "linux-console.bin").open("xb") as log, \
            (args.output / "aura-handoff-console.bin").open("xb") as aura_log:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            aura_log.write(proxy.read(65536))
            aura_log.flush()
            data = console.read(65536)
            if data:
                captured.extend(data)
                log.write(data)
                log.flush()
                if end_marker in captured or b"Kernel panic" in captured:
                    break
        aura_log.write(proxy.read(65536))
    return captured


def guard(args, device, phase=None):
    phase = phase or args.action
    directory = args.output / (phase + "-pmu")
    with (args.output / (phase + "-pmu.log")).open("x") as log:
        subprocess.run([sys.executable, str(args.pmu_guard), "--device", device, "--serial", args.serial, "--output", str(directory)],
                       stdout=log, stderr=subprocess.STDOUT, timeout=80, check=True)
    receipt = json.loads((directory / "receipt.json").read_text())
    require(receipt.get("passed") is True and receipt.get("offline_only") is False and
            receipt.get("after_raw") == 0 and receipt.get("device") == device,
            "fresh physical PMU guard did not pass; no reset/retry")
    return receipt


def wait_startup(args):
    """Observe a fresh startup without sending any protocol command."""
    import serial
    require(args.action == "chainload" and args.device == args.kis_device,
            "startup observation requires the source aura KIS endpoint")
    print("Waiting for source aura's proxy-ready banner before PMU guard", flush=True)
    with args.lock_file.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with serial.Serial(args.device, timeout=0.2) as console, \
                (args.output / "source-startup.bin").open("xb") as log:
            deadline = time.monotonic() + 60
            tail = b""
            while time.monotonic() < deadline:
                data = console.read(4096)
                if data:
                    log.write(data)
                    log.flush()
                    tail = (tail + data)[-8192:]
                    if b"Initialization complete. Running proxy..." in tail:
                        return
    raise TimeoutError("source aura startup not observed; stop without guard or reset")


def chainload(args, raw, symbols):
    source_raw, source_symbols = image_pair(args.source_image, args.source_sha256)
    session = Session(args.device, source_raw, source_symbols)
    try:
        p, f = session.p, session.f
        require(not any(p.smp_is_alive(i) for i in range(12) if i != 6), "secondaries already running")
        require((session.ba.video.width, session.ba.video.height) != (640, 1136),
                "source has no real display; stop before chainload")
        u = ProxyUtils(p)
        # GET_BOOTARGS is iBoot's copy. The active display exists only in
        # cur_boot_args; carrying the stale dummy would restart live DCP.
        u.ba = session.ba
        f.writemem = lambda address, data, progress=False: verified(address, data)
        def verified(address, data):
            original_write(address, data)
            require(session.read(address, len(data)) == data, "chainload readback mismatch")
        # Save the unwrapped writer before replacing it.
        original_write = UartInterface.writemem.__get__(f, UartInterface)
        u.compressed_writemem = lambda address, data, progress=False: verified(address, data)
        setup = types.ModuleType("aura.setup")
        setup.iface, setup.p, setup.u, setup.IODEV = f, p, u, IODEV
        setup.align, setup.ProxyCommandError = align, ProxyCommandError
        sys.modules["aura.setup"] = setup
        sys.argv = ["chainload.py", "--raw", "--relocate", "--vector-only", "--capture",
                    str(args.output / "chainload-console.bin"), "--capture-seconds", "8",
                    str(args.image / "aura.bin")]
        try:
            runpy.run_path(str(Path(__file__).resolve().parent / "chainload.py"), run_name="__main__")
        except SystemExit as exit_status:
            if exit_status.code not in (0, None):
                raise
    finally:
        session.close()
    session = Session(args.device, raw, symbols)
    try:
        require((session.ba.video.width, session.ba.video.height) != (640, 1136), "display was not inherited")
        save(args.output / "chainload.json", dict(base=session.base, image_sha256=sha(raw),
             framebuffer=dict(base=session.ba.video.base, width=session.ba.video.width,
                              height=session.ba.video.height), adt_sha256=sha(session.adt_bytes)))
    finally:
        session.close()


def thermal_preflight(s, output, sensor_keys, resume=False):
    """Read-only SMC keys through a bounded native RTKit session."""
    p = s.p
    require(p.read32(0x30c600044) & 0x10, "SMC is not already running")
    if resume:
        state = json.loads((output / "thermal.json").read_text())
        require(state["base"] == s.base and state["stage"] == "keys_ready",
                "cannot resume a different or incomplete SMC session")
        rtkit = state["rtkit"]
        require(s.call("rtkit_get_timeout", rtkit) == 2000000, "SMC timeout changed")
    else:
        asc = s.call("asc_init", s.cstring("/arm-io/smc"))
        require(asc and p.read64(asc + 8) == 0x30c710080, "unexpected SMC mailbox")
        code = p.memalign(16384, 16384)
        require(code, "SMC wrapper allocation failed")
        wrapper = ARMAsm(f"mov x5, #1\nldr x16, ={s.base + s.symbols['rtkit_init']}\nbr x16\n", code)
        s.write(code, wrapper.data)
        p.dc_cvac(code, len(wrapper.data))
        p.ic_ivau(code, len(wrapper.data))
        rtkit = p.call(code, s.cstring("smc"), asc, 0, 0, 0)
        state = dict(base=s.base, rtkit=rtkit, stage="allocated")
        save(output / "thermal.json", state)
        require(rtkit and s.call("rtkit_boot_timed", rtkit, 2000000) == 1, "SMC handshake failed")
        require(s.call("rtkit_start_ep", rtkit, 0x20) == 1, "SMC endpoint failed")
    buf = s.alloc(bytes(64))
    def exchange(value):
        s.write(buf, struct.pack("<B7xQ", 0x20, value))
        require(s.call("rtkit_send", rtkit, buf) == 1, "SMC send failed")
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            count = s.call("rtkit_recv", rtkit, buf + 32)
            require(count in (0, 1), "SMC receive failed")
            if count:
                ep, answer = struct.unpack("<B7xQ", s.read(buf + 32, 16))
                if ep == 0x20 and (state["stage"] != "keys_ready" or answer & 255 != 0x18):
                    return answer
        raise TimeoutError("SMC response timed out; retain session")
    if not resume:
        shmem = exchange(0x17)
        require(0x30c050000 <= shmem < 0x30c053f00 or 0x30de00000 <= shmem < 0x30df1ff00,
                "SMC shared memory outside validated windows")
        state.update(shmem=shmem, stage="keys_ready", msgid=1)
        save(output / "thermal.json", state)
    shmem = state["shmem"]
    def command(kind, key=0, length=0):
        mid = state["msgid"] & 15
        state["msgid"] += 1
        answer = exchange(kind | (mid << 12) | (length << 16) | (key << 32))
        require((answer >> 12) & 15 == mid, "SMC reply ID mismatch")
        if answer & 255:
            raise ValueError(f"SMC status {answer & 255:#x}")
        return answer
    def read_key(key):
        integer = int.from_bytes(key.encode(), "big")
        command(0x13, integer)
        info = s.read(shmem, 6)
        size, kind = info[0], info[1:5]
        require(0 < size <= 255, "bad SMC key size")
        answer = command(0x10, integer, size)
        require((answer >> 16) & 65535 == size, "SMC key length mismatch")
        data = struct.pack("<I", answer >> 32)[:size] if size <= 4 else s.read(shmem, size)
        return kind, data
    _, data = read_key("#KEY")
    count = int.from_bytes(data, "big")
    require(0 < count < 8192, "bad SMC key count")
    samples, errors = {}, {}
    for index in range(count):
        key = struct.pack("<I", command(0x12, index) >> 32).decode("ascii")
        if not key.startswith("T") and key != "F0Ac":
            continue
        try:
            kind, data = read_key(key)
            if kind == b"flt ":
                value = struct.unpack("<f", data)[0]
            elif kind == b"ioft":
                value = int.from_bytes(data, "little") / 65536
            elif kind == b"fpe2":
                value = int.from_bytes(data, "big") / 4
            elif kind == b"sp78":
                value = int.from_bytes(data, "big", signed=True) / 256
            else:
                continue
            samples[key] = value
        except ValueError as error:
            errors[key] = str(error)
    require(len(sensor_keys) == len(set(sensor_keys)) == 4, "unexpected preflight sensor inventory")
    require(set(sensor_keys) <= samples.keys(), "declared SMC temperature sensor missing")
    temps = [samples[key] for key in sensor_keys]
    state.update(telemetry=samples, read_errors=errors, temperatures=len(temps), sensor_keys=sensor_keys)
    save(output / "thermal.json", state)
    require(len(temps) == 4 and all(5 <= v <= 120 for v in temps) and max(temps) < 70 and
            samples.get("F0Ac", 0) >= 900, "thermal preflight did not pass")
    state["preflight_passed"] = True
    save(output / "thermal.json", state)
    print("THERMAL_PREFLIGHT_PASS", len(temps), max(temps), samples["F0Ac"], flush=True)
    return rtkit


def boot_linux(args, raw, symbols, receipt):
    s = Session(args.device, raw, symbols)
    try:
        require(int(receipt["aura_base"], 16) == s.base, "proxy changed after PMU guard")
        p = s.p
        if args.action == "boot":
            state = json.loads((args.output / "linux.json").read_text())
            require(state["stage"] == "prepared" and state["base"] == s.base and
                    state["image_sha256"] == sha(raw), "prepared state identity mismatch")
            require(sha(s.read(state["dt"], state["dt_size"])) == state["dt_sha256"], "FDT changed")
            audit = json.loads((args.output / "prepare-audit.json").read_text())
            require(audit.get("passed") is True and audit.get("dt_sha256") == state["dt_sha256"] and
                    audit.get("base") == s.base and audit.get("image_sha256") == sha(raw),
                    "independent preparation audit missing or stale")
            require(p.iodev_whoami() == IODEV.DOCKCHANNEL_UART, "boot requires restored aura KIS ch-1")
            thermal = json.loads((args.output / "thermal.json").read_text())
            require(thermal["base"] == s.base and thermal["stage"] == "keys_ready" and thermal.get("preflight_passed") is True, "SMC state changed")
            require(s.call("rtkit_get_timeout", thermal["rtkit"]) == 2000000 and
                    s.call("rtkit_quiesce", thermal["rtkit"]) == 1, "SMC quiesce failed")
            thermal["stage"] = "quiesced_for_linux"
            save(args.output / "thermal.json", thermal)
            require(all(p.smp_is_alive(i) for i in range(12)), "CPU missing before park")
            require(not s.call("smp_has_pending_calls"), "secondary callbacks still pending")
            p.dc_cvac(state["dt"], state["dt_size"])
            p.dc_cvac(state["kernel"], state["kernel_size"])
            p.ic_ivau(state["kernel"], state["kernel_size"])
            state["stage"] = "handoff_sent"
            save(args.output / "linux.json", state)
            p.request(p.P_KBOOT_BOOT, state["kernel"], no_reply=True)
            captured = capture_consoles(args, s, 150, b"J873_ARTIFACT_BASE64_END")
            state.update(console_has_thermal_marker=b"J873_LINUX_THERMAL_TELEMETRY_READY" in captured,
                         console_has_ready_marker=b"AURORA_J873_LINUX_INITRAMFS_READY" in captured,
                         console_sha256=sha(captured))
            state["stage"] = "linux_ready" if state["console_has_thermal_marker"] and state["console_has_ready_marker"] else "linux_unqualified"
            save(args.output / "linux.json", state)
            print("Console captured; inspect linux-console.bin for the kernel result.", flush=True)
            return

        require(args.artifacts and args.artifacts.is_dir(), "prepare requires the pinned artifact directory")
        require_upload_transport(args, s, sha(raw))
        hashes = {}
        for line in (args.artifacts / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split()
            name = name.lstrip("*")
            require(Path(name).name == name, "artifact path is not local")
            require(sha((args.artifacts / name).read_bytes()) == digest, "artifact changed: " + name)
            hashes[name] = digest
        dtb_name = next((n for n in ("t8152-j873g-smc-test.dtb", "t8152-j873g.dtb") if n in hashes), None)
        require(dtb_name and "Image" in hashes and "initramfs.cpio.gz" in hashes, "incomplete kernel artifacts")
        for cluster in range(2):
            require(s.call("cpufreq_set_cluster_pstate", cluster, 2) == 0, "low clock request failed")
        require([s.call("cpufreq_get_cluster_hz", i) for i in range(2)] == [972000000, 1440000000],
                "low clocks did not take effect")
        p.smp_start_secondaries()
        require(all(p.smp_is_alive(i) for i in range(12)), "not all 12 CPUs started")
        kernel = (args.artifacts / "Image").read_bytes()
        text_offset, image_size, flags = struct.unpack_from("<3Q", kernel, 8)
        require(kernel[56:60] == b"ARMd" and text_offset == 0 and flags == 0xc and
                len(kernel) <= image_size < 0x4000000, "unsupported arm64 image")
        highwater = p.heapblock_alloc(0)
        kernel_addr = (highwater + (128 << 20) + 0x1fffff) & ~0x1fffff
        initrd_addr = (kernel_addr + image_size + 0x1fffff) & ~0x1fffff
        initrd = (args.artifacts / "initramfs.cpio.gz").read_bytes()
        require(s.ba.phys_base < highwater < kernel_addr and
                initrd_addr + len(initrd) < s.ba.phys_base + s.ba.mem_size, "payloads outside usable RAM")
        p.heapblock_set_limit(kernel_addr)
        state = dict(base=s.base, image_sha256=sha(raw), hashes=hashes, stage="uploading",
                     artifacts=str(args.artifacts.resolve()), kernel=kernel_addr, kernel_size=image_size,
                     initrd=initrd_addr, initrd_size=len(initrd))
        save(args.output / "linux.json", state)
        for address, data, name in [(kernel_addr, kernel, "Image"), (initrd_addr, initrd, "initramfs.cpio.gz")]:
            start = time.monotonic()
            s.write(address, data)
            print("RAM_UPLOAD_VERIFIED", name, len(data), sha(data), time.monotonic() - start, flush=True)
        state["stage"] = "payloads_verified"
        save(args.output / "linux.json", state)
        finish_preparation(s, args, state, dtb_name)
    finally:
        s.close()


def finish_preparation(s, args, state, dtb_name):
    p, symbols, kernel_addr = s.p, s.symbols, state["kernel"]
    p.kboot_set_initrd(state["initrd"], state["initrd_size"])
    dt_input = s.alloc((args.artifacts / dtb_name).read_bytes(), 16384)
    sensor_keys = ["TCMb", "THSP", "Tg0a", "Tg0b"]
    thermal_preflight(s, args.output, sensor_keys)
    bootargs = ("earlycon=apple_dockchannel,0x308128000 console=tty0 console=ttyDC0 "
                "rdinit=/init loglevel=8 ignore_loglevel printk.time=1 maxcpus=1 idle=wfi "
                "arm64.nowfxt nokaslr panic=0 apple_mailbox.ascwrap_v8_cpu_control=1 log_buf_len=16M")
    require(s.call("kboot_set_chosen", s.cstring("bootargs"), s.cstring(bootargs)) == 0, "bootargs failed")
    require(p.kboot_prepare_dt(dt_input) == 0, "native DT preparation failed")
    dt = p.read64(s.base + symbols["dt"])
    require(dt, "native FDT missing")
    release_slots = {str(cpu): s.call("smp_get_kernel_release_addr", cpu)
                     for cpu in range(12) if cpu != 6}
    require(p.heapblock_alloc(0) < kernel_addr, "native heap overlaps kernel")
    # Native preparation packs the tree. Restore its allocated capacity before
    # adding the final reservation; never grow beyond Aura's buffer.
    capacity = p.read32(s.base + symbols["dt_bufsize"])
    packed_size = struct.unpack_from(">I", s.read(dt, 8), 4)[0]
    require(0 < packed_size <= capacity < 0x80000, "invalid FDT capacity")
    require(s.call("fdt_open_into", dt, dt, capacity) == 0, "FDT reopen failed")
    require(s.call("fdt_add_mem_rsv", dt, s.ba.phys_base, kernel_addr - s.ba.phys_base) == 0,
            "retained aura heap reservation failed")
    require(s.call("fdt_pack", dt) == 0, "final DT pack failed")
    total = struct.unpack_from(">I", s.read(dt, 40), 4)[0]
    require(0 < total < 0x80000, "invalid final DT size")
    final = s.read(dt, total)
    require(b"apple,t8152-aic3\0apple,t8122-aic3\0" in final and
            b"G18G-RTKit-3514.0.15.release\0" in final, "final DT lost AIC fallback or GPU receipt")
    (args.output / "prepared.dtb").write_bytes(final)
    (args.output / "live.adt").write_bytes(s.adt_bytes)
    state.update(stage="prepared", dt=dt, dt_size=total, dt_sha256=sha(final), release_slots=release_slots,
                 bootargs=bootargs, retained_native_ranges=[[s.ba.phys_base, kernel_addr - s.ba.phys_base]])
    save(args.output / "linux.json", state)

def read_fdt(blob):
    """Parse a flattened device tree into {path: {property: bytes}}."""
    magic, total, off_struct, off_strings, _, version, _, _, size_strings, size_struct = \
        struct.unpack_from('>10I', blob, 0)
    if magic != 0xd00dfeed or total > len(blob) or version < 17:
        raise ValueError('Invalid FDT header')
    strings = blob[off_strings:off_strings + size_strings]
    tree, stack, offset, end = {}, [], off_struct, off_struct + size_struct
    while offset < end:
        token = struct.unpack_from('>I', blob, offset)[0]
        offset += 4
        if token == 1:
            name_end = blob.index(b'\0', offset)
            name = blob[offset:name_end].decode()
            offset = (name_end + 4) & ~3
            stack.append(name)
            path = '/' + '/'.join(stack[1:])
            tree[path] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, nameoff = struct.unpack_from('>II', blob, offset)
            offset += 8
            key = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            tree['/' + '/'.join(stack[1:])][key] = bytes(blob[offset:offset + length])
            offset = (offset + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            raise ValueError('Invalid FDT token')
    return tree


def audit_prepared(args, raw, symbols):
    state = json.loads((args.output / "linux.json").read_text())
    require(state["stage"] == "prepared", "not prepared")
    s = Session(args.device, raw, symbols)
    try:
        require(s.base == state["base"], "candidate changed")
        blob = s.read(state["dt"], state["dt_size"])
        require(sha(blob) == state["dt_sha256"], "FDT changed")
        tree = read_fdt(blob)
        require(tree["/chosen"].get("aurora,g18g-gpu-firmware-profile") ==
                b"G18G-RTKit-3514.0.15.release\0", "missing native firmware validation")
        rsv=[];off=struct.unpack_from('>I',blob,16)[0]
        while True:
         base,size=struct.unpack_from('>QQ',blob,off);off+=16
         if not base and not size:break
         require(size and base+size<1<<64,'bad memreserve')
         rsv.append((base,size))
        checked=[]
        def covered(base,size,reason):
         require(size>0 and any(start<=base and base+size<=start+count for start,count in rsv),'unreserved '+reason)
         checked.append(dict(base=base,size=size,reason=reason))
        for cpu,slot in state['release_slots'].items():covered(slot,128,'native CPU park '+cpu)
        covered(state['dt'],state['dt_size'],'FDT')
        covered(state['initrd'],state['initrd_size'],'initrd')
        for base,size in state['retained_native_ranges']:covered(base,size,'retained aura')
        for path in ('/arm-io/dcp','/arm-io/dcp/iop-dcp-nub'):
         segments=s.adt[path].getprop('segment-ranges')
         require(isinstance(segments,bytes) and len(segments)%32==0,'invalid DCP ranges')
         for i in range(0,len(segments),32):
          phys,_,remap,size,_=struct.unpack_from('<QQQII',segments,i)
          covered(phys,size,path+' physical');covered(remap,size,path+' remap')
        covered(*s.adt['/vram'].get_reg(0),'VRAM')
        fb=s.ba.video
        covered(fb.base,fb.stride*fb.height,'framebuffer')
        fbs=[v for v in tree.values() if b'simple-framebuffer' in v.get('compatible',b'') and v.get('status',b'okay\0')==b'okay\0']
        require(len(fbs)==1,'missing/ambiguous enabled framebuffer')
        require(struct.unpack('>QQ',fbs[0]['reg'])[0]==fb.base,'wrong framebuffer')
        require(any(v.get('compatible')==b'apple,t8152-aic3\0apple,t8122-aic3\0' for v in tree.values()),'AIC fallback changed')
        chosen=tree['/chosen']
        require(int.from_bytes(chosen['linux,initrd-start'],'big')==state['initrd'] and int.from_bytes(chosen['linux,initrd-end'],'big')==state['initrd']+state['initrd_size'],'initrd bounds changed')
        affinities=[0,1,2,3,4,5,0x10100,0x10101,0x40102,0x40103,0x40104,0x40105]
        for cpu,affinity in enumerate(affinities):
         props=tree[f'/cpus/cpu@{affinity:x}']
         if cpu!=6:require(props['cpu-release-addr']==struct.pack('>Q',state['release_slots'][str(cpu)]),'wrong CPU release slot')
        # Kernel's complete decompressed span must remain outside every reservation.
        start,end=state['kernel'],state['kernel']+state['kernel_size']
        require(all(end<=b or b+z<=start for b,z in rsv),'kernel overlaps a reservation')
        require(end<=state['initrd'],'kernel overlaps initrd')
        audit = dict(passed=True, base=s.base, image_sha256=sha(raw), dt_sha256=sha(blob),
                     checked_ranges=checked, reservations=rsv)
        save(args.output / "prepare-audit.json", audit)
        print("J873_PREPARE_AUDIT_PASS", len(checked), flush=True)
    finally:
        s.close()


def main():
    require(__debug__ and not os.environ.get("PYTHONOPTIMIZE"), "run normal Python without -O")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=["chainload", "cdc", "prepare", "audit", "boot"])
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--image-sha256", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--pmu-guard", type=Path, default=Path(__file__).with_name("j873_pmu_guard.py"))
    ap.add_argument("--serial", required=True)
    ap.add_argument("--console", required=True, help="Linux console port, separate from the Aura proxy")
    ap.add_argument("--device", required=True)
    ap.add_argument("--transport", choices=["kis", "cdc"], default="kis",
                    help="Payload preparation transport; CDC requires a successful cdc phase")
    ap.add_argument("--cdc-device", help="Explicit CDC primary port for the cdc phase")
    ap.add_argument("--source-image", type=Path)
    ap.add_argument("--source-sha256")
    ap.add_argument("--artifacts", type=Path)
    ap.add_argument("--wait-startup", action="store_true",
                    help="Passively wait for a fresh source aura proxy-ready banner before the guard")
    ap.add_argument("--lock-file", type=Path, default=None)
    args = ap.parse_args()
    global EXPECTED_SERIAL
    EXPECTED_SERIAL = args.serial
    require(args.serial.isalnum(), "invalid serial number")
    args.kis_device = args.device
    if args.lock_file is None:
        args.lock_file = Path("/tmp/aura-j873-" + args.serial + ".lock")
    if args.action == "chainload" and (not args.source_image or not args.source_sha256):
        ap.error("chainload needs --source-image and --source-sha256")
    if args.action == "cdc" and (not args.cdc_device or args.cdc_device in (args.device, args.console)):
        ap.error("cdc requires a separate --cdc-device primary port")
    raw, symbols = image_pair(args.image, args.image_sha256)
    args.output.mkdir(parents=True, exist_ok=True)
    require(args.device != args.console, "Aura proxy and Linux console must be distinct")
    with (args.output / (args.action + "-started.json")).open("x") as stream:
        json.dump(dict(image_sha256=sha(raw), device=args.device, started=time.time()), stream)
    if args.wait_startup:
        wait_startup(args)
    receipt = None if args.action == "audit" else guard(args, args.device)
    with args.lock_file.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.action == "chainload":
            chainload(args, raw, symbols)
        elif args.action == "cdc":
            start_cdc(args, sha(raw), lambda device: Session(device, raw, symbols),
                      receipt)
        elif args.action == "audit":
            audit_prepared(args, raw, symbols)
        else:
            boot_linux(args, raw, symbols, receipt)
    print("AURA_J873_" + args.action.upper() + "_COMPLETE", flush=True)


if __name__ == "__main__":
    main()
