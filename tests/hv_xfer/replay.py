#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""HvXfer end-to-end offline replay (see FAILURE_MODES.md and README.md).

Everything between the guest's register access and the host file is the real
code, minus the transport and hardware:

  guest:  the real AppleHvXferCore.c sequencer (AuroraSilicon AppleSerial),
          whose 32-bit accesses become real STR/LDR instructions
  EL2:    real src/hv/hv_vm.c (hv_handle_dabort, decoder, splitting, stage 2)
          + real src/hv/windows/hv_xfer.c, compiled with the real Aura headers
  host:   real aura.hv.HV.attach_xfer/handle_xfer/pt_update, the real
          XferHostDevice and ProxyTransport, launched through the real
          aura.windows.launch_profile() and plan_profile()

Simulated: physical memory (a fixed host mapping at J813's DRAM base, so
PA == host VA), the proxy pipe (readmem/writemem/gzdec become memory copies),
and the ADT (a handful of device ranges).

Writes <out>/result.json (checks + hashes; the deterministic part is digested
into result.digest) and keeps every intermediate artifact in <out>.
"""

import argparse
import ast
import ctypes
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import traceback
import zlib
from pathlib import Path
from types import SimpleNamespace

ap = argparse.ArgumentParser()
ap.add_argument("--lib", required=True)
ap.add_argument("--repo", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--guest-dir", required=True)
ap.add_argument("--build-json", default=None)
args = ap.parse_args()

REPO = Path(args.repo).resolve()
OUT = Path(args.out).resolve()
GUEST = Path(args.guest_dir).resolve()
sys.path.insert(0, str(REPO / "proxyclient"))

from aura import windows as W  # noqa: E402
from aura.hv import HV  # noqa: E402
from aura.hv import xfer as X  # noqa: E402
from aura.hv.types import HV_EVENT, TraceMode  # noqa: E402
from aura.malloc import Heap  # noqa: E402
from aura.proxy import EXC_RET, START, ExcInfo  # noqa: E402
from aura.utils import AddrLookup, irange  # noqa: E402

# --------------------------------------------------------------------------
# results
# --------------------------------------------------------------------------

CHECKS = {}
ERRORS = []


def check(name, ok, detail=None):
    ok = bool(ok)
    if name in CHECKS and not CHECKS[name]["pass"]:
        return False  # first failure sticks
    CHECKS[name] = {"pass": ok, "detail": detail}
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail is not None and not ok else ""),
          flush=True)
    return ok


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------------------
# the C world
# --------------------------------------------------------------------------

lib = ctypes.CDLL(args.lib)
u64, u32, i32 = ctypes.c_uint64, ctypes.c_uint32, ctypes.c_int32


def fn(name, res, *argt):
    f = getattr(lib, name)
    f.restype = res
    f.argtypes = list(argt)
    return f


hv_pt_init = fn("hv_pt_init", None)
hv_pt_walk = fn("hv_pt_walk", u64, u64)
hv_map = fn("hv_map", ctypes.c_int, u64, u64, u64, u64)
hv_map_hw = fn("hv_map_hw", ctypes.c_int, u64, u64, u64)
hv_map_xfer = fn("hv_map_xfer", ctypes.c_int, u64, u64, u64)
replay_access_c = fn("replay_access", ctypes.c_int, u64, u32, ctypes.c_int, u64, u64,
                     ctypes.POINTER(u64), ctypes.POINTER(u64))
replay_call_hook = fn("replay_call_hook", ctypes.c_int, u64, ctypes.POINTER(u64), ctypes.c_int,
                      ctypes.c_int)
replay_map_dummy_hook = fn("replay_map_dummy_hook", ctypes.c_int, u64, u64)
replay_dummy_count = fn("replay_dummy_count", u32)
replay_proxy_count = fn("replay_proxy_count", u32)
replay_civac = fn("replay_civac", u64, u32)
replay_guest_stats = fn("replay_guest_stats", u32, u32)
replay_abi = fn("replay_abi", u64, u32)
replay_set_dram = fn("replay_set_dram", None, u64, u64)
replay_unexpected_count = fn("replay_unexpected_count", u32)
replay_install_sigill = fn("replay_install_sigill", ctypes.c_int)
replay_open_log = fn("replay_open_log", ctypes.c_int, ctypes.c_char_p)
replay_sysreg_stats = fn("replay_sysreg_stats", ctypes.c_ulong, ctypes.c_int)
PROXY_CB = ctypes.CFUNCTYPE(None, u64, u32, u32)
replay_set_proxy_cb = fn("replay_set_proxy_cb", None, PROXY_CB)


class Probe(ctypes.Structure):
    _fields_ = [("id", u32), ("version", u32), ("features", u32), ("page_size", u32),
                ("win_size", u32), ("max_xfer", u32), ("win_phys", u64), ("reason", ctypes.c_int)]


class Request(ctypes.Structure):
    _fields_ = [("cmd", u32), ("tag", u32), ("off", u32), ("len", u32), ("set_name", ctypes.c_bool),
                ("name", ctypes.c_char * 64)]


class Completion(ctypes.Structure):
    _fields_ = [("status", i32), ("result", u32), ("seq", u32), ("errors", u32)]


OPS = ctypes.c_void_p(ctypes.addressof(ctypes.c_char.in_dll(lib, "replay_guest_ops")))
drv_probe = fn("apple_hvxfer_probe", ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(Probe))
drv_set_window = fn("apple_hvxfer_set_window", i32, ctypes.c_void_p, ctypes.c_void_p, u64, u32)
drv_reset_window = fn("apple_hvxfer_reset_window", None, ctypes.c_void_p, ctypes.c_void_p)
drv_issue = fn("apple_hvxfer_issue", None, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(Request),
               ctypes.POINTER(Completion))
drv_align = fn("apple_hvxfer_align_window", ctypes.c_bool, u64, u32, u32, ctypes.POINTER(u32),
               ctypes.POINTER(u32))

libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                      ctypes.c_longlong]

# J813-like layout. DRAM at 0x100_0000_0000 (where J813 RAM lives), 1 GiB.
RAM_BASE = 0x10000000000
RAM_SIZE = 0x40000000
RAM_END = RAM_BASE + RAM_SIZE
PHYS_BASE = RAM_BASE + 0x2000000           # boot_args phys_base
HEAP_TOP = PHYS_BASE + 0x10000000          # Aura resident + C heap + proxy heap below this
PROXY_BASE = HEAP_TOP - 0x8000000          # 128 MiB proxy heap, never in the guest map
ARM_IO = (0x200000000, 0x200000000)        # /arm-io ranges[0]: 8 GiB of MMIO
DOOR = 0x2fe000000                         # the profile's doorbell (an /arm-io gap)
VUART = 0x235200000
C_ONLY_HOOK = 0x3f0001000                  # a C-side sub-page hook the ADT never mentions
DEVICES = [("/arm-io/pmgr[0]", 0x200000000, 0x200400000),
           ("/arm-io/uart0[0]", VUART, VUART + 0x4000),
           ("/arm-io/aic[0]", 0x28e100000, 0x28e10c000)]
GUEST_ALLOC = HEAP_TOP + 0x4001000         # guest's contiguous allocation (4 KiB aligned only)
WIN_BYTES = 1 << 20
IPA_SPLIT, PA_SPLIT = 0x30000000000, HEAP_TOP + 0x10000000
IPA_DISCONTIG, PA_DISCONTIG = 0x31000000000, HEAP_TOP + 0x11000000
IPA_EDGE = 0x32000000000

MAP_PRIVATE, MAP_ANON, PROT_RW = 0x2, 0x1000, 0x3
addr = libc.mmap(ctypes.c_void_p(RAM_BASE), RAM_SIZE, PROT_RW, MAP_PRIVATE | MAP_ANON, -1, 0)
if addr != RAM_BASE:
    sys.exit(f"replay: could not place simulated DRAM at {RAM_BASE:#x} (got {addr!r})")


def mem_read(pa, size):
    return ctypes.string_at(pa, size)


def mem_write(pa, data):
    assert RAM_BASE <= pa and pa + len(data) <= RAM_END, f"write outside simulated DRAM {pa:#x}"
    ctypes.memmove(pa, bytes(data), len(data))


def mem_fill(pa, size, byte):
    assert RAM_BASE <= pa and pa + size <= RAM_END
    ctypes.memset(pa, byte, size)


def ipa_to_pa(ipa):
    pte = hv_pt_walk(ipa)
    assert pte and pte & 1, f"guest IPA {ipa:#x} not HW mapped"
    return (pte & 0x3ffffffffc000) | (ipa & 0x3fff)


def guest_read(ipa, size):
    """Read guest memory the way the guest sees it (through real stage 2)."""
    out = bytearray()
    while size:
        n = min(size, 0x4000 - (ipa & 0x3fff))
        out += mem_read(ipa_to_pa(ipa), n)
        ipa += n
        size -= n
    return bytes(out)


def guest_write(ipa, data):
    view = memoryview(bytes(data))
    while view:
        n = min(len(view), 0x4000 - (ipa & 0x3fff))
        mem_write(ipa_to_pa(ipa), view[:n])
        ipa += n
        view = view[n:]


def access(ipa, log2, write, value=0, value2=0):
    a, b = u64(0), u64(0)
    ok = replay_access_c(ipa, log2, 1 if write else 0, value, value2, ctypes.byref(a), ctypes.byref(b))
    return ok, a.value, b.value


def rd32(off, base=DOOR):
    ok, v, _ = access(base + off, 2, False)
    assert ok, f"32-bit read of {off:#x} faulted"
    return v & 0xffffffff


def wr32(off, value, base=DOOR):
    ok, _, _ = access(base + off, 2, True, value)
    assert ok, f"32-bit write of {off:#x} faulted"


R = X.REG

# --------------------------------------------------------------------------
# the host world: real HV with transport fakes
# --------------------------------------------------------------------------


class FakeIface:
    def __init__(self):
        self.extra = None
        self.fail_next_write = False
        self.window_writes = []

    def readmem(self, addr, size):
        return ctypes.string_at(addr, size)

    def writemem(self, addr, data):
        data = bytes(data)
        if self.fail_next_write:
            self.fail_next_write = False
            raise IOError("injected transport failure")
        if self.extra is not None and addr == self.extra[0] and len(data) <= self.extra[1]:
            ctypes.memmove(addr, data, len(data))
            return
        self.window_writes.append((addr, len(data)))
        mem_write(addr, data)

    def readstruct(self, addr, stype):
        return stype.parse(self.readmem(addr, stype.sizeof()))


C_CAPS = int(re.search(r"reply->retval = (0x4157494e[0-9a-f]+)ULL",
                       (REPO / "src/proxy.c").read_text()).group(1), 16)


class FakeProxy:
    def __init__(self, iface):
        self.iface = iface
        self.caps = C_CAPS
        self.exits = []
        self.gzdec_calls = 0

    def hv_windows_capabilities(self):
        return self.caps

    def hv_map_xfer(self, base, win, size):
        return hv_map_xfer(base, win, size)

    def hv_map(self, ipa, pte, size, incr):
        return hv_map(ipa, pte, size, incr)

    def gzdec(self, inbuf, insize, outbuf, outsize):
        self.gzdec_calls += 1
        data = zlib.decompress(mem_read(inbuf, insize), 31)
        if len(data) > outsize:
            return -1
        self.iface.writemem(outbuf, data)
        return len(data)

    def exit(self, ret):
        self.exits.append(int(ret))


class FakeADT:
    def build_addr_lookup(self):
        lookup = AddrLookup()
        for name, lo, hi in DEVICES:
            lookup.add(range(lo, hi), name)
        return lookup

    def __getitem__(self, key):
        assert key in ("arm-io", "/arm-io")
        return SimpleNamespace(ranges=[SimpleNamespace(parent_addr=ARM_IO[0], size=ARM_IO[1])])


iface = FakeIface()
proxy = FakeProxy(iface)
TLBI = []
utils = SimpleNamespace(ba=SimpleNamespace(phys_base=PHYS_BASE, mem_size_actual=RAM_SIZE,
                                           mem_size=RAM_END - PHYS_BASE),
                        heap=Heap(PROXY_BASE, HEAP_TOP), inst=TLBI.append)
hv = HV(iface, proxy, utils, windows=True)
hv.adt = FakeADT()
hv.print_tracer = SimpleNamespace(log_file=None)

UNEXPECTED_EVENTS = []


def proxy_cb(ctx_addr, reason, code):
    """The tethered host's START dispatch: (reason, code) -> handler."""
    try:
        if (reason, code) != (int(START.HV), int(HV_EVENT.XFER)):
            UNEXPECTED_EVENTS.append((reason, code))
            return
        ctx = ExcInfo.parse(ctypes.string_at(ctx_addr, ExcInfo.sizeof()))
        iface.extra = (ctx.data, X.XferExcInfo.sizeof())
        hv.handle_xfer(START.HV, HV_EVENT.XFER, ctx_addr)
    except Exception:  # noqa: BLE001 - must never unwind through C
        ERRORS.append(traceback.format_exc())
    finally:
        iface.extra = None


CB = PROXY_CB(proxy_cb)


class Guest:
    """The real guest driver core, issuing real traps."""

    def __init__(self, base=DOOR):
        self.base = u64(base)
        self.ctx = ctypes.c_void_p(ctypes.addressof(self.base))

    def probe(self):
        p = Probe()
        ok = drv_probe(OPS, self.ctx, ctypes.byref(p))
        return ok, p

    def set_window(self, phys, size):
        return drv_set_window(OPS, self.ctx, phys, size)

    def reset_window(self):
        drv_reset_window(OPS, self.ctx)

    def issue(self, cmd, tag=0, off=0, length=0, name=None):
        req = Request(cmd=cmd, tag=tag, off=off, len=length, set_name=name is not None)
        if name is not None:
            req.name = name if isinstance(name, bytes) else name.encode()
        comp = Completion()
        drv_issue(OPS, self.ctx, ctypes.byref(req), ctypes.byref(comp))
        return comp.status, comp.result, comp.seq, comp.errors


# --------------------------------------------------------------------------
# inputs
# --------------------------------------------------------------------------

if OUT.exists():
    shutil.rmtree(OUT)
OUT.mkdir(parents=True)
SERVE = OUT / "serve"
INBOX = OUT / "inbox"
SERVE.mkdir()
INBOX.mkdir()
replay_open_log(str(OUT / "el2-console.log").encode())


def det_bytes(seed, size):
    out, counter = bytearray(), 0
    while len(out) < size:
        out += hashlib.sha256(f"{seed}:{counter}".encode()).digest()
        counter += 1
    return bytes(out[:size])


WIFI = det_bytes("wifi-fw", 11 * (1 << 20) + (1 << 19) + 3)      # 11.5 MiB + 3: odd tail
PKG = b"".join(b"[Version]\r\nSignature=\"$Windows NT$\"\r\nline %08d\r\n" % i
               for i in range(70000))[:3 * (1 << 20)]           # compressible -> gzdec path
MUTATING = det_bytes("mutating", 2 * (1 << 20))
PAYLOAD = det_bytes("guest-catalog", 2 * (1 << 20) + 12345)
(SERVE / "wifi-fw.bin").write_bytes(WIFI)
(SERVE / "driver-pkg.inf").write_bytes(PKG)
(SERVE / "mutating.bin").write_bytes(MUTATING)
(SERVE / "tiny.txt").write_bytes(b"hello")
(SERVE / "subdir").mkdir()
os.mkfifo(SERVE / "pipe")
os.symlink(SERVE / "wifi-fw.bin", SERVE / "link-in")
os.symlink("/etc/hosts", SERVE / "link-out")
with open(SERVE / "huge.bin", "wb") as f:  # sparse: one byte over the default cap
    f.truncate(X.DEFAULT_MAX_STREAM + 1)
(INBOX / "existing.cat").write_bytes(b"operator file, must survive")

# --------------------------------------------------------------------------
# phase 0: ABI and source facts
# --------------------------------------------------------------------------

replay_install_sigill()
replay_set_dram(RAM_BASE, RAM_SIZE)
hv_pt_init()
VADDR_BITS = ctypes.c_uint64.in_dll(lib, "vaddr_bits").value
check("env.stage2_init", VADDR_BITS == 44 and replay_sysreg_stats(1) == 0,
      {"vaddr_bits": VADDR_BITS, "sysregs_emulated": replay_sysreg_stats(0)})

fields = {}
offset = 0
for sub in X.XferExcInfo.subcons:
    fields[sub.name] = offset
    offset += sub.sizeof()
check("abi.exc_info_layout",
      replay_abi(0) == X.XferExcInfo.sizeof() == 56 and replay_abi(1) == fields["cmd"] and
      replay_abi(2) == fields["status"] and replay_abi(10) == fields["name_phys"] and
      replay_abi(11) == fields["result"],
      {"c_sizeof": replay_abi(0), "py_sizeof": X.XferExcInfo.sizeof()})
check("abi.event_number",
      replay_abi(3) == int(HV_EVENT.XFER) == 9 and replay_abi(4) == int(START.HV) and
      replay_abi(7) == int(HV_EVENT.TPM) and replay_abi(8) == int(HV_EVENT.SYSREG_ASSIST_READY) and
      replay_abi(9) == int(HV_EVENT.PANIC),
      {"C HV_XFER": replay_abi(3), "py HV_EVENT.XFER": int(HV_EVENT.XFER)})
check("abi.exc_info_extra_is_data", replay_abi(6) == ExcInfo.sizeof() - 8,
      {"offsetof(extra)": replay_abi(6), "ExcInfo.data": ExcInfo.sizeof() - 8})

c_src = (REPO / "src/hv/windows/hv_xfer.c").read_text()
c_hdr = (REPO / "src/hv/windows/hv_xfer.h").read_text()
max_window = re.search(r"#define HV_XFER_MAX_WINDOW \((\d+)UL << (\d+)\)", c_src)
check("abi.policy_mirrors",
      W.XFER_PAGE == X.HV_XFER_PAGE_SIZE == X.HV_XFER_REGS_SIZE == 0x4000 and
      W.XFER_MAX_WINDOW == X.HV_XFER_MAX_WINDOW == int(max_window.group(1)) << int(max_window.group(2)) and
      all(f"HV_XFER_REG_{k} " in c_hdr and re.search(rf"HV_XFER_REG_{k}\s+0x{v:03x}\b", c_hdr)
          for k, v in R.items()))
proxy_h = (REPO / "src/proxy.h").read_text()
proxy_py = (REPO / "proxyclient/aura/proxy.py").read_text()
check("abi.opcode",
      re.search(r"P_HV_MAP_XFER = 0x1700", proxy_h) and proxy_h.count("0x1700") == 1 and
      re.search(r"P_HV_MAP_XFER = 0x1700", proxy_py) is not None)
check("host.caps_bit",
      C_CAPS >> 32 == W.CAPS_MAGIC >> 32 and (C_CAPS >> 16) & 0xffff == W.CAPS_ABI and
      C_CAPS & W.XFER and C_CAPS & 0x1fff == 0x1fff and W.XFER == 0x2000,
      {"caps": hex(C_CAPS)})
old_caps = 0x4157494e00011fff
try:
    W.validate_capabilities(old_caps, xfer=True)
    refused_old = False
except ValueError:
    refused_old = True
check("host.caps_required", refused_old and W.validate_capabilities(old_caps) == 0x1fff and
      W.validate_capabilities(C_CAPS, xfer=True) == C_CAPS & 0xffff)

# The XFER handler must be registered unconditionally in HV.init().
tree = ast.parse((REPO / "proxyclient/aura/hv/__init__.py").read_text())
init = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == "init")
top_level_handler = any(
    isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call) and
    getattr(stmt.value.func, "attr", None) == "set_handler" and
    ast.unparse(stmt.value.args[1]) == "HV_EVENT.XFER" and ast.unparse(stmt.value.args[2]) == "self.handle_xfer"
    for stmt in init.body)
check("host.handler_unconditional", top_level_handler)

parity = subprocess.run([sys.executable, str(GUEST / "tests/check-hvxfer-abi-parity.py"),
                         str(GUEST / "AppleHvXferCore.h"), str(REPO / "src/hv/windows/hv_xfer.h")],
                        capture_output=True, text=True)
(OUT / "abi-parity.txt").write_text(parity.stdout + parity.stderr)
check("abi.guest_parity", parity.returncode == 0 and "PASS" in parity.stdout, parity.stdout.strip())

# --------------------------------------------------------------------------
# phase 1: profile admission (real plan_profile)
# --------------------------------------------------------------------------

PROFILE_DIR = OUT / "launch-input"
PROFILE_DIR.mkdir()
MU = det_bytes("mu", 65536)
(PROFILE_DIR / "mu.fd").write_bytes(MU)
(PROFILE_DIR / "a-file").write_bytes(b"x")
os.symlink(SERVE, PROFILE_DIR / "serve-link")
(SERVE / "nested-inbox").mkdir()
GOOD_XFER = {"doorbell": hex(DOOR), "window_bytes": WIN_BYTES, "serve_dir": str(SERVE),
             "inbox": str(INBOX)}


def profile(xfer=None, name="replay"):
    body = {"schema": "aura.windows.boot.v1", "name": name,
            "target": {"chip_id": "0x8142", "model": "Mac17,3"},
            "mu": {"path": "mu.fd", "sha256": sha256(MU), "entry_offset": 0}}
    if xfer is not None:
        body["xfer"] = xfer
    path = PROFILE_DIR / f"{name}.json"
    path.write_text(json.dumps(body, indent=2))
    return path


plan = W.plan_profile(profile(name="no-xfer"))
check("profile.default_disabled", plan.xfer is None and W.xfer_plan_summary(plan.xfer) == {"enabled": False})
plan = W.plan_profile(profile(GOOD_XFER, name="good"))
check("profile.valid", plan.xfer == {"doorbell": DOOR, "window_bytes": WIN_BYTES,
                                     "serve_dir": os.path.realpath(SERVE),
                                     "inbox": os.path.realpath(INBOX), "compress": True}, plan.xfer)
check("profile.auto_accepted",
      W.plan_profile(profile({**GOOD_XFER, "doorbell": "auto"}, name="auto")).xfer["doorbell"] == "auto")
BAD_PROFILES = {
    "unknown_key": {**GOOD_XFER, "base": "0x1"},
    "missing_serve_dir": {"doorbell": hex(DOOR)},
    "not_object": ["0x2fe000000"],
    "doorbell_unaligned": {**GOOD_XFER, "doorbell": hex(DOOR + 0x1000)},
    "doorbell_zero": {**GOOD_XFER, "doorbell": "0x0"},
    "doorbell_vgic": {**GOOD_XFER, "doorbell": "0xf00010000"},
    "doorbell_beyond_ipa": {**GOOD_XFER, "doorbell": hex(1 << 44)},
    "doorbell_bool": {**GOOD_XFER, "doorbell": True},
    "window_zero": {**GOOD_XFER, "window_bytes": 0},
    "window_unaligned": {**GOOD_XFER, "window_bytes": 0x5000},
    "window_too_big": {**GOOD_XFER, "window_bytes": 128 << 20},
    "serve_dir_relative": {**GOOD_XFER, "serve_dir": "serve"},
    "serve_dir_missing": {**GOOD_XFER, "serve_dir": str(OUT / "nope")},
    "serve_dir_symlink": {**GOOD_XFER, "serve_dir": str(PROFILE_DIR / "serve-link")},
    "serve_dir_file": {**GOOD_XFER, "serve_dir": str(PROFILE_DIR / "a-file")},
    "inbox_overlap_equal": {**GOOD_XFER, "inbox": str(SERVE)},
    "inbox_overlap_nested": {**GOOD_XFER, "inbox": str(SERVE / "nested-inbox")},
    "inbox_overlap_parent": {**GOOD_XFER, "inbox": str(OUT)},
    "compress_not_bool": {**GOOD_XFER, "compress": "yes"},
}
for label, xfer in BAD_PROFILES.items():
    try:
        W.plan_profile(profile(xfer, name="bad-" + label))
        check(f"profile.refuse.{label}", False, "accepted")
    except ValueError as error:
        check(f"profile.refuse.{label}", True, str(error))
check("profile.inbox_overlap", all(CHECKS[f"profile.refuse.inbox_overlap_{k}"]["pass"]
                                    for k in ("equal", "nested", "parent")))

# --------------------------------------------------------------------------
# phase 2: pre-attach world (what HV.init / the C hv_init leave behind)
# --------------------------------------------------------------------------

def phase_pre_attach():
    assert replay_map_dummy_hook(VUART, 0x4000) == 0                 # vUART-like C hook
    hv.add_tracer(irange(VUART, 0x4000), "VUART", TraceMode.RESERVED)
    assert replay_map_dummy_hook(C_ONLY_HOOK, 0x1000) == 0           # C-only, sub-page, no tracer
    hv.add_tracer(irange(*ARM_IO), "HW", TraceMode.OFF)              # "Map MMIO ranges as HW by default"

    ok, _, _ = access(DOOR + R["ID"], 2, False)
    check("map.disabled_doorbell_faults", not ok and replay_proxy_count() == 0,
          "doorbell read with xfer disabled must fail the dabort (unmapped IPA)")

    WIN_OK = PROXY_BASE  # valid, unmapped, 16 KiB aligned
    assert hv_map_hw(RAM_BASE + 0x100000, RAM_BASE + 0x100000, 0x4000) == 0  # already-mapped DRAM
    MAP_REFUSALS = {
        "map.base_unaligned": (DOOR + 0x1000, WIN_OK, 0x4000),
        "map.base_in_dram": (RAM_BASE + 0x200000, WIN_OK, 0x4000),
        "map.base_beyond_ipa": (1 << 44, WIN_OK, 0x4000),
        "map.base_overlaps_hook": (VUART, WIN_OK, 0x4000),
        "map.base_overlaps_subpage_hook": (C_ONLY_HOOK & ~0x3fff, WIN_OK, 0x4000),
        "map.window_unaligned": (DOOR, WIN_OK + 0x1000, 0x4000),
        "map.window_zero": (DOOR, WIN_OK, 0),
        "map.window_too_big": (DOOR, WIN_OK, (64 << 20) + 0x4000),
        "map.window_outside_dram": (DOOR, 0x200000000, 0x4000),
        "map.window_wraps": (DOOR, 0xffffffffffffc000, 0x8000),
        "map.window_already_mapped": (DOOR, RAM_BASE + 0x100000, 0x4000),
    }
    for label, (b, w, s) in MAP_REFUSALS.items():
        check(label, hv_map_xfer(b, w, s) < 0 and hv_pt_walk(DOOR) == 0 and hv_pt_walk(WIN_OK) == 0)



# --------------------------------------------------------------------------
# phase 3: launch through the real launcher
# --------------------------------------------------------------------------

STATE = {}


def pre_attach_refusals():
    engine = X.XferHostDevice(SERVE, None, verbose=False)
    attempts = {"adt_device": 0x28e100000, "dram": RAM_BASE + 0x8000000, "vuart_tracer": VUART,
                "synthetic_vgic": 0xF00004000, "c_only_subpage_hook": C_ONLY_HOOK & ~0x3fff,
                "unaligned": DOOR + 0x1000}
    for label, base in attempts.items():
        try:
            hv.attach_xfer(base, WIN_BYTES, engine)
            check(f"launch.doorbell_overlap.{label}", False, "attached")
        except (ValueError, RuntimeError) as error:
            check(f"launch.doorbell_overlap.{label}",
                  hv.xfer_info is None and hv_pt_walk(DOOR) == 0, str(error))
    proxy.caps = old_caps
    try:
        hv.attach_xfer(DOOR, WIN_BYTES, engine)
        check("host.attach_requires_caps", False, "attached without the capability")
    except RuntimeError as error:
        check("host.attach_requires_caps", hv.xfer_info is None, str(error))
    proxy.caps = C_CAPS
    picked = hv.xfer_pick_doorbell()
    check("launch.auto_pick", picked == 0x200400000 and not hv.xfer_doorbell_conflicts(picked),
          hex(picked) if picked else None)
    engine.close()


def load_guest_ram():
    """What load_raw() + start() do to the page tables."""
    hv.add_tracer(irange(RAM_BASE, PHYS_BASE - RAM_BASE), "RAM-LOW", TraceMode.OFF)
    hv.add_tracer(irange(HEAP_TOP, RAM_END - HEAP_TOP), "RAM-HIGH", TraceMode.OFF)
    hv.pt_update()
    win = hv.xfer_info["window"]
    door_pte = hv_pt_walk(DOOR)
    check("launch.reserved_tracers",
          door_pte and not door_pte & 1 and (door_pte >> 50) & 7 == 1 and
          ipa_to_pa(win) == win and ipa_to_pa(win + WIN_BYTES - 1) == win + WIN_BYTES - 1 and
          hv_pt_walk(VUART) and not hv_pt_walk(VUART) & 1 and
          hv_pt_walk(PHYS_BASE + 0x100000) == 0 and ipa_to_pa(GUEST_ALLOC) == GUEST_ALLOC,
          {"doorbell_pte": hex(door_pte)})
    # Extra guest mappings the fixtures need (made after pt_update, as Mu-time
    # remaps would be): a non-identity window, a discontiguous pair, and a pair
    # that runs off the end of DRAM.
    assert hv_map_hw(IPA_SPLIT, PA_SPLIT, WIN_BYTES) == 0
    assert hv_map_hw(IPA_DISCONTIG, PA_DISCONTIG, 0x4000) == 0
    assert hv_map_hw(IPA_DISCONTIG + 0x4000, PA_DISCONTIG + 0x8000, 0x4000) == 0
    assert hv_map_hw(IPA_EDGE, RAM_END - 0x4000, 0x8000) == 0


class ReplaySession:
    def __init__(self, device, log):
        self.device = device
        self.log = log
        self.hv = hv
        self.xfer_receipt = None
        self.serial_capture = None

    def capabilities(self):
        return proxy.caps

    def identity(self):
        return {"chip_id": 0x8142, "model": "Mac17,3", "resident_base": hex(PHYS_BASE),
                "bootargs_revision": 3, "framebuffer_depth": 30}

    def prepare(self, plan):
        W.validate_capabilities(self.capabilities(), plan.framebuffer, plan.guest_compatibility,
                                plan.n1, bool(plan.reserved_memory), plan.serial, plan.mtp,
                                plan.cpufreq, xfer=plan.xfer is not None)
        pre_attach_refusals()
        self.xfer_receipt = W.attach_windows_xfer(hv, plan.xfer, Path(self.log.name).parent)
        replay_set_proxy_cb(CB)

    xfer_summary = W.HardwareSession.xfer_summary

    def load(self, plan):
        load_guest_ram()
        return {"guest_base": hex(HEAP_TOP), "simulated": True}

    def start(self):
        run_guest()
        return 0

    def close(self):
        if hv.xfer_dev is not None:
            hv.xfer_dev.close()


# --------------------------------------------------------------------------
# phase 4: the guest
# --------------------------------------------------------------------------

def get_stream(g, name, sid, win_ipa, win_pa, chunk, off=0, expect=None, mutate=None):
    """OPEN/GET-until-EOF/CLOSE, verifying the fence and cache sync each time."""
    status, size, _, _ = g.issue(X.CMD_OPEN, tag=sid, name=name)
    if status != X.ST_OK:
        return status, None, {}
    got, gets, fence_ok, sync_ok = bytearray(), 0, True, True
    while True:
        mem_fill(win_pa, WIN_BYTES, 0xa5)
        civac_before = replay_civac(0)
        writes_before = len(iface.window_writes)
        status, result, _, _ = g.issue(X.CMD_GET, tag=sid, off=off, length=chunk)
        gets += 1
        # Every host write lands inside [off, off+chunk) of the window, or in
        # proxy-heap scratch (the gzip staging buffer gzdec reads from).
        fence_ok &= all(win_pa + off <= a and a + n <= win_pa + off + chunk or
                        PROXY_BASE <= a and a + n <= HEAP_TOP
                        for a, n in iface.window_writes[writes_before:])
        if status != X.ST_OK:
            return status, bytes(got), {"gets": gets}
        window = guest_read(win_ipa, WIN_BYTES)
        got += window[off:off + result]
        untouched = window[:off] + window[off + result:]
        fence_ok &= untouched == b"\xa5" * len(untouched)
        if result:
            sync_ok &= (replay_civac(0) == civac_before + 1 and replay_civac(1) == win_pa + off and
                        replay_civac(2) == result)
        if mutate and gets == 1:
            mutate()
        if result == 0:
            break
    status, closed, _, _ = g.issue(X.CMD_CLOSE, tag=sid)
    return status, bytes(got), {"gets": gets, "fence_ok": fence_ok, "sync_ok": sync_ok,
                                "open_size": size, "close_result": closed}


def run_guest():
    g = Guest()
    engine = hv.xfer_dev
    before_proxy = replay_proxy_count()

    # -- identification ------------------------------------------------------
    ok, p = g.probe()
    own_win = hv.xfer_info["window"]
    check("decode.id_version",
          ok and p.id == X.HV_XFER_ID and p.version == 1 and p.page_size == 0x4000 and
          p.features & 2 and p.win_phys == own_win and p.win_size == WIN_BYTES and
          p.max_xfer == WIN_BYTES and p.reason == 0,
          {"id": hex(p.id), "features": p.features, "win": hex(p.win_phys)})
    check("decode.unimplemented_zero",
          all(rd32(off) == 0 for off in (0x04c, 0x07c, 0x0c0, 0x100, 0x3ffc)))
    check("decode.probe_no_host_call", replay_proxy_count() == before_proxy)

    # -- bind the guest-owned window, the driver's default --------------------
    off_out, size_out = u32(0), u32(0)
    aligned = drv_align(GUEST_ALLOC, WIN_BYTES + 0x4000, WIN_BYTES, ctypes.byref(off_out), ctypes.byref(size_out))
    win_ipa = GUEST_ALLOC + off_out.value
    status = g.set_window(win_ipa, size_out.value)
    win_pa = ipa_to_pa(win_ipa)
    check("win.bind_guest_window",
          aligned and status == X.ST_OK and win_ipa % 0x4000 == 0 and size_out.value == WIN_BYTES and
          rd32(R["WIN_LO"]) | rd32(R["WIN_HI"]) << 32 == win_ipa and rd32(R["WIN_SIZE"]) == WIN_BYTES,
          {"win_ipa": hex(win_ipa), "status": status})
    check("win.block_mapping", win_pa == win_ipa and hv_pt_walk(win_ipa) & 1)
    seq0, err0 = rd32(R["SEQ"]), rd32(R["ERRORS"])

    # -- the payload: 11.5 MiB firmware, 1 MiB GETs --------------------------
    gz_before = proxy.gzdec_calls
    t0 = time.monotonic()
    status, data, info = get_stream(g, "wifi-fw.bin", 1, win_ipa, win_pa, WIN_BYTES)
    wifi_seconds = time.monotonic() - t0
    rec = next(r for r in engine.records if r["name"] == "wifi-fw.bin")
    check("e2e.get_multi_mb",
          status == X.ST_OK and data == WIFI and info["open_size"] == len(WIFI) and
          info["close_result"] == len(WIFI) and info["gets"] == len(WIFI) // WIN_BYTES + 2 and
          rec["sha256"] == rec["served_sha256"] == sha256(WIFI) and rec["complete"] and rec["closed"],
          {"bytes": len(data or b""), "sha256": sha256(data or b""), "gets": info.get("gets"),
           "host_seconds": round(wifi_seconds, 3)})
    check("e2e.window_fence", info.get("fence_ok"))
    check("cmd.sync_range", info.get("sync_ok"))
    check("e2e.incompressible_skips_gzdec", proxy.gzdec_calls == gz_before)

    # -- compressible package, partial-window GETs at an offset ---------------
    status, data, info = get_stream(g, "driver-pkg.inf", 2, win_ipa, win_pa, 0x40000, off=0x8000)
    check("e2e.get_offset_compressed",
          status == X.ST_OK and data == PKG and info["fence_ok"] and info["sync_ok"] and
          proxy.gzdec_calls > gz_before,
          {"gzdec_calls": proxy.gzdec_calls - gz_before, "gets": info.get("gets")})

    # -- lazily resolved, snapshot at OPEN ------------------------------------
    late = det_bytes("late-catalog", 300000)
    (SERVE / "catalog-late.cat").write_bytes(late)
    status, data, _ = get_stream(g, "catalog-late.cat", 3, win_ipa, win_pa, WIN_BYTES)
    check("engine.lazy_open", status == X.ST_OK and data == late)

    def mutate():
        (SERVE / "mutating.bin").write_bytes(b"\0" * len(MUTATING))
        tmp = SERVE / "mutating.tmp"
        tmp.write_bytes(det_bytes("replacement", 4096))
        os.replace(tmp, SERVE / "mutating.bin")

    status, data, _ = get_stream(g, "mutating.bin", 4, win_ipa, win_pa, WIN_BYTES, mutate=mutate)
    rec = next(r for r in engine.records if r["name"] == "mutating.bin")
    check("engine.snapshot", status == X.ST_OK and data == MUTATING and
          rec["sha256"] == rec["served_sha256"] == sha256(MUTATING))

    # -- guest -> host PUT, create-only ----------------------------------------
    sid = X.TAG_WRITE | 0x20
    status, _, _, _ = g.issue(X.CMD_OPEN, tag=sid, name="guest-catalog.cat")
    sent, put_ok = 0, status == X.ST_OK
    while sent < len(PAYLOAD):
        piece = PAYLOAD[sent:sent + WIN_BYTES]
        guest_write(win_ipa, piece)
        civac_before = replay_civac(0)
        st, res, _, _ = g.issue(X.CMD_PUT, tag=sid, off=0, length=len(piece))
        put_ok &= st == X.ST_OK and res == len(piece) and replay_civac(0) == civac_before + 1 and \
            replay_civac(1) == win_pa and replay_civac(2) == len(piece)
        sent += len(piece)
    st, res, _, _ = g.issue(X.CMD_CLOSE, tag=sid)
    rec = next(r for r in engine.records if r["name"] == "guest-catalog.cat")
    landed = (INBOX / "guest-catalog.cat").read_bytes() if (INBOX / "guest-catalog.cat").exists() else b""
    check("e2e.put_roundtrip",
          put_ok and st == X.ST_OK and res == len(PAYLOAD) and landed == PAYLOAD and
          rec["sha256"] == sha256(PAYLOAD) and rec["committed"] and
          not any(p.name.endswith(".part") for p in INBOX.iterdir()),
          {"bytes": len(landed), "sha256": sha256(landed)})

    st_exist, _, _, _ = g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 0x21, name="existing.cat")
    st_again, _, _, _ = g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 0x22, name="guest-catalog.cat")
    g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 0x23, name="race.cat")
    guest_write(win_ipa, b"guest bytes")
    g.issue(X.CMD_PUT, tag=X.TAG_WRITE | 0x23, off=0, length=11)
    (INBOX / "race.cat").write_bytes(b"host created first")
    st_race, _, _, _ = g.issue(X.CMD_CLOSE, tag=X.TAG_WRITE | 0x23)
    parts = [p.name for p in INBOX.iterdir() if p.name.endswith(".part")]
    check("engine.put_create_only",
          st_exist == X.ST_IO and (INBOX / "existing.cat").read_bytes() == b"operator file, must survive" and
          st_again == X.ST_IO and (INBOX / "guest-catalog.cat").read_bytes() == PAYLOAD and
          st_race == X.ST_IO and (INBOX / "race.cat").read_bytes() == b"host created first" and
          any(n.startswith(".race.cat.") for n in parts),
          {"open_existing": st_exist, "open_again": st_again, "close_race": st_race, "parts": parts})

    g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 7, name="reopen.cat")
    guest_write(win_ipa, b"A" * 0x4000)
    g.issue(X.CMD_PUT, tag=X.TAG_WRITE | 7, off=0, length=0x4000)
    st1, _, _, _ = g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 7, name="reopen.cat")
    guest_write(win_ipa, b"B" * 0x2000)
    g.issue(X.CMD_PUT, tag=X.TAG_WRITE | 7, off=0, length=0x2000)
    st2, res2, _, _ = g.issue(X.CMD_CLOSE, tag=X.TAG_WRITE | 7)
    reopen_recs = [r for r in engine.records if r["name"] == "reopen.cat"]
    check("engine.reopen",
          st1 == X.ST_OK and st2 == X.ST_OK and res2 == 0x2000 and
          (INBOX / "reopen.cat").read_bytes() == b"B" * 0x2000 and len(reopen_recs) == 2 and
          reopen_recs[0].get("abandoned") and Path(reopen_recs[0]["part"]).exists())

    saved_cap = engine.max_stream_bytes
    engine.max_stream_bytes = 0x8000
    g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 8, name="capped.cat")
    caps = [g.issue(X.CMD_PUT, tag=X.TAG_WRITE | 8, off=0, length=0x4000)[0] for _ in range(3)]
    g.issue(X.CMD_CLOSE, tag=X.TAG_WRITE | 8)
    engine.max_stream_bytes = saved_cap
    check("engine.put_cap", caps == [X.ST_OK, X.ST_OK, X.ST_IO], caps)

    no_inbox = X.XferHostDevice(SERVE, None, verbose=False)
    hv.xfer_dev = no_inbox
    st_put = g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 9, name="nowhere.cat")[0]
    hv.xfer_dev = engine
    no_inbox.close()
    check("engine.put_disabled", st_put == X.ST_NODEV and not (INBOX / "nowhere.cat").exists())

    # -- engine refusals through the full stack ---------------------------------
    bad_names = [b"../etc/passwd", b"/etc/passwd", b"a/b", b"a\\b", b".hidden", b"", b"x" * 64,
                 b"caf\xe9.bin", b"a..b", b"\0" * 8, b"wifi-fw.bin "]
    statuses = {n.decode("latin-1"): g.issue(X.CMD_OPEN, tag=30, name=n)[0] for n in bad_names}
    check("engine.bad_names", all(s == X.ST_INVAL for s in statuses.values()) and 30 not in engine.streams,
          statuses)
    st_in = g.issue(X.CMD_OPEN, tag=31, name="link-in")[0]
    st_out = g.issue(X.CMD_OPEN, tag=31, name="link-out")[0]
    check("engine.symlink_refused", st_in == X.ST_INVAL and st_out == X.ST_INVAL, [st_in, st_out])
    t = time.monotonic()
    st_dir = g.issue(X.CMD_OPEN, tag=32, name="subdir")[0]
    st_fifo = g.issue(X.CMD_OPEN, tag=32, name="pipe")[0]
    check("engine.nonregular_refused", st_dir == X.ST_INVAL and st_fifo == X.ST_INVAL and
          time.monotonic() - t < 5, [st_dir, st_fifo])
    check("engine.size_cap", g.issue(X.CMD_OPEN, tag=33, name="huge.bin")[0] == X.ST_IO)
    check("cmd.host_declined", g.issue(X.CMD_OPEN, tag=34, name="absent.bin")[:2] == (X.ST_NODEV, 0))

    g.issue(X.CMD_OPEN, tag=X.TAG_WRITE | 40, name="wrong.cat")
    g.issue(X.CMD_OPEN, tag=41, name="tiny.txt")
    wrong = [g.issue(X.CMD_GET, tag=X.TAG_WRITE | 40, off=0, length=16)[0],
             g.issue(X.CMD_PUT, tag=41, off=0, length=16)[0],
             g.issue(X.CMD_GET, tag=42, off=0, length=16)[0],
             g.issue(X.CMD_CLOSE, tag=43)[0]]
    g.issue(X.CMD_CLOSE, tag=X.TAG_WRITE | 40)
    g.issue(X.CMD_CLOSE, tag=41)
    check("engine.wrong_stream", wrong == [X.ST_INVAL] * 4, wrong)

    g.issue(X.CMD_OPEN, tag=44, name="tiny.txt")
    iface.fail_next_write = True
    exits = len(proxy.exits)
    st_exc = g.issue(X.CMD_GET, tag=44, off=0, length=16)[0]
    check("engine.exception_contained", st_exc == X.ST_IO and proxy.exits[exits:] == [int(EXC_RET.HANDLED)]
          and not ERRORS)
    g.issue(X.CMD_CLOSE, tag=44)

    # -- EL2 command checks -------------------------------------------------------
    def counters():
        return rd32(R["SEQ"]), rd32(R["ERRORS"]), replay_proxy_count()

    s0 = counters()
    range_cases = {"cmd.range_len0": (0, 0), "cmd.range_off": (WIN_BYTES, 16),
                   "cmd.range_wrap": (0x10, 0xfffffff8), "cmd.range_past_end": (WIN_BYTES - 8, 16)}
    for label, (o, n) in range_cases.items():
        before = counters()
        wr32(R["OFF"], o)
        wr32(R["LEN"], n)
        wr32(R["TAG"], 1)
        wr32(R["CMD"], X.CMD_GET)
        after = counters()
        check(label, rd32(R["STATUS"]) == X.ST_RANGE & 0xffffffff and rd32(R["RESULT"]) == 0 and
              after == (before[0], before[1] + 1, before[2]))
    before = counters()
    wr32(R["CMD"], 99)
    check("cmd.badcmd", rd32(R["STATUS"]) == X.ST_BADCMD & 0xffffffff and
          counters() == (before[0], before[1] + 1, before[2]))
    before = counters()
    wr32(R["CMD"], X.CMD_NOP)
    check("cmd.counters", counters() == (before[0] + 1, before[1], before[2]) and s0[0] > seq0)

    hv.xfer_dev = None
    st_none = g.issue(X.CMD_PING)[:2]
    hv.xfer_dev = engine
    replay_set_proxy_cb(PROXY_CB())
    st_silent = g.issue(X.CMD_PING)[:2]
    replay_set_proxy_cb(CB)
    check("cmd.no_engine", st_none == (X.ST_NODEV, 0) and st_silent == (X.ST_NODEV, 0),
          {"no_engine": st_none, "host_never_answers": st_silent})
    check("host.no_engine_nodev", st_none == (X.ST_NODEV, 0))

    real_dispatch = engine.dispatch
    engine.dispatch = lambda tr, cmd, tag, off, length, *a: ((X.ST_OK, length + 16) if cmd == X.CMD_GET
                                                             else real_dispatch(tr, cmd, tag, off, length, *a))
    st_over = g.issue(X.CMD_GET, tag=1, off=0, length=64)[:2]
    engine.dispatch = real_dispatch
    check("cmd.result_overclaim", st_over == (X.ST_IO, 0), st_over)
    check("cmd.ping", g.issue(X.CMD_PING)[:2] == (X.ST_OK, X.HV_XFER_VERSION) and rd32(R["FEATURES"]) & 1)

    # -- decode refusals ------------------------------------------------------------
    wr32(R["OFF"], 0x1234)
    snapshot = counters()
    sub = [access(DOOR + R["OFF"], 0, False)[0], access(DOOR + R["OFF"], 0, True, 0xff)[0],
           access(DOOR + R["OFF"], 1, False)[0], access(DOOR + R["OFF"], 1, True, 0xffff)[0],
           access(DOOR + R["CMD"], 1, True, X.CMD_PING)[0], access(DOOR + R["ID"], 0, False)[0]]
    check("decode.subword_refused", sub == [0] * 6 and rd32(R["OFF"]) == 0x1234 and counters() == snapshot, sub)
    una = [access(DOOR + R["OFF"] + 1, 2, True, 0)[0], access(DOOR + R["CMD"] + 2, 2, True, 1)[0],
           access(DOOR + R["ID"] + 1, 2, False)[0]]
    check("decode.unaligned_refused", una == [0] * 3 and rd32(R["OFF"]) == 0x1234 and counters() == snapshot, una)
    ok64, _, _ = access(DOOR + R["CMD"], 3, True, (0x40 << 32) | X.CMD_PING)
    after = counters()
    check("decode.cmd64_refused",
          ok64 and rd32(R["STATUS"]) == X.ST_INVAL & 0xffffffff and after[1] == snapshot[1] + 1 and
          after[2] == snapshot[2] and rd32(R["OFF"]) == 0x40)
    ok_pair, _, _ = access(DOOR + R["LEN"], 3, True, (0x77 << 32) | 0x55)
    okr, v64, _ = access(DOOR + R["LEN"], 3, False)
    check("decode.pair64", ok_pair and okr and v64 == (0x77 << 32) | 0x55 and rd32(R["TAG"]) == 0x77)
    wide = [access(DOOR + R["CMD"] - 0x20 + 0x20, 4, True, X.CMD_PING, 0)[0],  # STP at 0x20
            access(DOOR + R["NAME"], 4, True, 0x41414141, 0)[0],
            access(DOOR + R["ID"], 4, False)[0]]
    check("decode.wide_refused", wide == [0] * 3 and counters()[2] == snapshot[2], wide)
    wr32(R["NAME"], int.from_bytes(b"ABCD", "little"))
    byte_ok, b1, _ = access(DOOR + R["NAME"] + 1, 0, False)
    check("decode.name_words", byte_ok and b1 == ord("B") and rd32(R["NAME"]) == int.from_bytes(b"ABCD", "little"))
    v = u64(0)
    direct = replay_call_hook(DOOR + R["NAME"] + 62, ctypes.byref(v), 0, 2)
    check("decode.name_overrun_refused", direct == 0, "direct hook call, 4 bytes at NAME+62")
    id_before = rd32(R["ID"])
    for off in (R["ID"], R["STATUS"], R["SEQ"], R["WIN_LO"]):
        wr32(off, 0xdeadbeef)
    check("decode.ro_absorbed", rd32(R["ID"]) == id_before and rd32(R["WIN_LO"]) == win_ipa & 0xffffffff)

    # -- SET_WINDOW refusals ------------------------------------------------------------
    def raw_set_window(ipa, size):
        wr32(R["UWIN_LO"], ipa & 0xffffffff)
        wr32(R["UWIN_HI"], (ipa >> 32) & 0xffffffff)
        wr32(R["UWIN_SIZE"], size)
        wr32(R["CMD"], X.CMD_SET_WINDOW)
        return rd32(R["STATUS"])

    def bound():
        return rd32(R["WIN_LO"]) | rd32(R["WIN_HI"]) << 32, rd32(R["WIN_SIZE"])

    current = bound()
    fault = X.ST_FAULT & 0xffffffff
    win_cases = {
        "win.reject_shape.zero": (win_ipa, 0),
        "win.reject_shape.too_big": (HEAP_TOP, (64 << 20) + 0x4000),
        "win.reject_shape.ipa_unaligned": (win_ipa + 0x1000, 0x4000),
        "win.reject_shape.size_unaligned": (win_ipa, 0x5000),
        "win.reject_beyond_ipa": (1 << 44, 0x4000),
        "win.reject_beyond_ipa.wrap": (0xffffffffffffc000, 0x8000),
        "win.reject_unmapped": (PHYS_BASE + 0x100000, 0x4000),
        "win.reject_hook.doorbell": (DOOR, 0x4000),
        "win.reject_hook.vuart": (VUART, 0x4000),
        "win.reject_discontig": (IPA_DISCONTIG, 0x8000),
        "win.reject_mmio": (0x3f8000000, 0x4000),
        "win.reject_dram_edge": (IPA_EDGE, 0x8000),
        "win.reject_partly_unmapped": (RAM_END - 0x4000, 0x8000),
    }
    for label, (ipa, size) in win_cases.items():
        st = raw_set_window(ipa, size)
        check(label, st == fault and bound() == current, {"status": st})
    check("win.refusal_keeps_old", all(CHECKS[k]["pass"] for k in win_cases))

    # Non-identity stage 2: the guest names its IPA, the host must get the PA.
    status = g.set_window(IPA_SPLIT, WIN_BYTES)
    seen = []
    real = engine.dispatch
    engine.dispatch = lambda tr, cmd, tag, off, length, win_phys, *a: (
        seen.append(win_phys), real(tr, cmd, tag, off, length, win_phys, *a))[1]
    g.issue(X.CMD_OPEN, tag=50, name="tiny.txt")
    st, res, _, _ = g.issue(X.CMD_GET, tag=50, off=0x100, length=0x100)
    g.issue(X.CMD_CLOSE, tag=50)
    engine.dispatch = real
    check("win.ipa_pa_split",
          status == X.ST_OK and bound() == (IPA_SPLIT, WIN_BYTES) and seen and
          all(w == PA_SPLIT for w in seen) and st == X.ST_OK and res == 5 and
          guest_read(IPA_SPLIT + 0x100, 5) == b"hello" and mem_read(PA_SPLIT + 0x100, 5) == b"hello",
          {"win_phys_seen": sorted({hex(w) for w in seen})})

    g.reset_window()
    check("win.reset", bound() == (own_win, WIN_BYTES))

    # The Aura-owned window (driver's -HostWindow fallback) still works.
    status, data, info = get_stream(g, "tiny.txt", 51, own_win, own_win, WIN_BYTES)
    check("e2e.aura_window", status == X.ST_OK and data == b"hello" and info["fence_ok"])

    # Second channel refused while one is live.
    check("map.second_refused", hv_map_xfer(DOOR - 0x4000, PROXY_BASE + 0x400000, 0x4000) < 0)

    # Guest driver discipline: every access 32-bit, none faulted.
    check("guest.accesses_32bit_only",
          replay_guest_stats(1) == 0 and replay_guest_stats(2) == 0 and replay_guest_stats(0) > 0,
          {"accesses": replay_guest_stats(0), "faults": replay_guest_stats(1)})
    STATE["guest_accesses"] = replay_guest_stats(0)


class RefusedSession(ReplaySession):
    def prepare(self, plan):
        self.xfer_receipt = W.attach_windows_xfer(hv, plan.xfer, Path(self.log.name).parent)


def phase_launch():
    global summary
    # A profile whose doorbell collides with the live vUART passes offline
    # validation, so the launch must die at attach -- before any guest entry --
    # and its receipt must still say what was planned.
    refused_dir = OUT / "launch-refused"
    try:
        W.launch_profile(profile({**GOOD_XFER, "doorbell": hex(VUART)}, name="launch-refused"),
                         "replay-device", refused_dir, session_factory=RefusedSession)
        refused = False
    except ValueError:
        refused = True
    r = json.loads((refused_dir / "launch.json").read_text())
    check("launch.refused_receipt",
          refused and r["result"] == "failed" and "loaded" not in r and r["xfer"].get("attached") is False and
          r["xfer"].get("doorbell") == hex(VUART) and hv.xfer_info is None and hv_pt_walk(DOOR) == 0,
          {"result": r.get("result"), "xfer": r.get("xfer"), "error": r.get("error")})

    launch_dir = OUT / "launch"
    report = W.launch_profile(profile(GOOD_XFER, name="launch"), "replay-device", launch_dir,
                              session_factory=ReplaySession)
    receipt = json.loads((launch_dir / "launch.json").read_text())
    xr = receipt.get("xfer", {})
    check("launch.receipt",
          receipt["result"] == "guest-returned-unqualified" and xr.get("enabled") and xr.get("attached") is True and
          xr.get("doorbell") == hex(DOOR) and xr.get("aura_window") == hex(hv.xfer_info["window"]) and
          xr.get("aura_window_bytes") == WIN_BYTES and xr.get("guest_attach", {}).get("DoorbellBase") == hex(DOOR) and
          any(s["name"] == "wifi-fw.bin" and s["sha256"] == sha256(WIFI) for s in xr["engine"]["streams"]) and
          "aura/hv/xfer.py" in receipt["host_sources"],
          {"doorbell": xr.get("doorbell"), "aura_window": xr.get("aura_window")})

    log_lines = [json.loads(line) for line in (launch_dir / "xfer-commands.jsonl").read_text().splitlines()]
    summary = xr.get("engine", {})
    check("engine.log_complete",
          len(log_lines) == summary.get("commands") and all({"status", "result", "cmd", "n"} <= set(e) for e in log_lines) and
          [e["n"] for e in log_lines] == list(range(1, len(log_lines) + 1)) and
          all(s.get("sha256") for s in summary.get("streams", []) if s["direction"] == "get") and
          all(s.get("sha256") for s in summary.get("streams", []) if s["direction"] == "put" and s.get("closed")),
          {"logged": len(log_lines), "engine_commands": summary.get("commands")})
    check("replay.no_unexpected",
          not ERRORS and not UNEXPECTED_EVENTS and replay_unexpected_count() == 0 and replay_sysreg_stats(1) == 0 and
          replay_dummy_count() == 0, {"errors": ERRORS[:2], "events": UNEXPECTED_EVENTS[:4]})


summary = {}
try:
    phase_pre_attach()
    phase_launch()
    check("replay.completed", True)
except Exception:  # noqa: BLE001 - a crash is a failed check, and result.json is still written
    print(traceback.format_exc(), file=sys.stderr)
    check("replay.completed", False, traceback.format_exc()[-2000:])


# --------------------------------------------------------------------------
# build facts (from build.sh, when it has been run)
# --------------------------------------------------------------------------

build = None
if args.build_json and Path(args.build_json).exists():
    build = json.loads(Path(args.build_json).read_text())
    check("build.default_image_identical", build.get("default_identical") is True)
    check("build.windows_clean", build.get("windows_warning_set_identical") is True and
          build.get("windows_link_ok") is True)

# --------------------------------------------------------------------------
# result
# --------------------------------------------------------------------------

def file_sha(path):
    return sha256(Path(path).read_bytes())


inputs = {str(p.relative_to(REPO)): file_sha(p) for p in sorted([
    REPO / "src/hv/windows/hv_xfer.c", REPO / "src/hv/windows/hv_xfer.h", REPO / "src/hv/hv_vm.c",
    REPO / "src/hv/windows/hv.h", REPO / "src/proxy.c", REPO / "src/proxy.h",
    REPO / "proxyclient/aura/hv/xfer.py", REPO / "proxyclient/aura/hv/__init__.py",
    REPO / "proxyclient/aura/windows.py", REPO / "proxyclient/aura/proxy.py",
    REPO / "proxyclient/aura/hv/types.py", REPO / "tests/hv_xfer/replay.py",
    REPO / "tests/hv_xfer/replay_glue.c", REPO / "tests/hv_xfer/replay_host.c"])}
inputs.update({"guest:" + n: file_sha(GUEST / n) for n in ("AppleHvXferCore.c", "AppleHvXferCore.h")})
deterministic = {
    "schema": "aura.hvxfer.replay.v1",
    "verdict": "pass" if all(c["pass"] for c in CHECKS.values()) else "fail",
    "checks": {k: v["pass"] for k, v in sorted(CHECKS.items())},
    "inputs_sha256": inputs,
    "data_sha256": {"wifi-fw.bin": sha256(WIFI), "driver-pkg.inf": sha256(PKG),
                    "guest-catalog.cat": sha256(PAYLOAD), "mutating.bin": sha256(MUTATING)},
    "layout": {"ram_base": hex(RAM_BASE), "doorbell": hex(DOOR),
               "aura_window": hex((hv.xfer_info or {}).get("window", 0)),
               "window_bytes": WIN_BYTES, "vaddr_bits": VADDR_BITS},
}
digest = sha256(json.dumps(deterministic, sort_keys=True).encode())
result = dict(deterministic)
result.update({
    "digest": digest,
    "passed": sum(c["pass"] for c in CHECKS.values()),
    "failed": [k for k, c in CHECKS.items() if not c["pass"]],
    "details": {k: v["detail"] for k, v in sorted(CHECKS.items()) if v["detail"] is not None},
    "stats": {"guest_register_accesses": STATE.get("guest_accesses"),
              "el2_sysregs_emulated": replay_sysreg_stats(0),
              "el2_proxy_events": replay_proxy_count(), "engine": {k: summary.get(k) for k in
              ("commands", "failures", "bytes_out", "bytes_in", "host_seconds")}},
    "build": build,
    "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "hardware_accessed": False,
})
# The inputs are regenerated deterministically on every run and their hashes
# are above (and per stream in launch/launch.json). Do not leave a sparse
# 256 MiB file, a FIFO and symlinks lying in the source tree.
for name in ("serve", "inbox", "launch-input"):
    shutil.rmtree(OUT / name, ignore_errors=True)
(OUT / "result.json").write_text(json.dumps(result, indent=2) + "\n")
(OUT / "result.digest").write_text(digest + "\n")
print(f"\nHvXfer replay: {result['verdict'].upper()} ({result['passed']}/{len(CHECKS)} checks), digest {digest}")
sys.exit(0 if result["verdict"] == "pass" else 1)
