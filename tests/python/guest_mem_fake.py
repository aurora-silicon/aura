#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A simulated machine for tools/dump_guest_mem.py and tools/guest_mem.py (no device); run by
test_dump_guest_mem.py in its own process, since it puts fake aura.proxy modules in place.

The fake proxy models DRAM, DART registers and tables (3- and 4-level), the GPU UAT, PMGR power states, a powered-off
DART (whose registers must never be touched: on a J815 that read loops in the proxy), a DART with no power info, an
exclave stream, an MMIO leaf and a faulting DRAM page. The run must walk and dump exactly what is safe. Prints one
line per check and exits 1 if any fails.
"""
import importlib.util, os, pathlib, runpy, struct, subprocess, sys, tempfile, types
from enum import IntFlag

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "proxyclient" / "tools"
AURA = TOOLS.parent / "aura"
PAGE = 0x4000
DRAM0, DRAM1 = 0x100_0000_0000, 0x100_0000_0000 + (32 << 30)
mem, regs = {}, {}
faulting_pages, forbidden = set(), []
state = dict(exc=0, guard=0, reads=0)
alloc = [DRAM0 + 0x1000_0000]


def page(pa):
    return mem.setdefault(pa & ~(PAGE - 1), bytearray(PAGE))


def w64(pa, v):
    struct.pack_into("<Q", page(pa), pa & (PAGE - 1), v)


def new_page():
    a = alloc[0]
    alloc[0] += PAGE
    page(a)
    return a


class GUARD(IntFlag):
    OFF = 0
    SKIP = 1
    SILENT = 0x100


class UartInterface:
    def readmem(self, addr, size):
        out, a = bytearray(), addr
        while a < addr + size:
            pg = a & ~(PAGE - 1)
            n = min(addr + size - a, pg + PAGE - a)
            if not DRAM0 <= a < DRAM1 or pg in faulting_pages:
                state["exc"] += 1
                out += b"\xee" * n
            else:
                out += page(pg)[a - pg:a - pg + n]
            a += n
        state["reads"] += size
        return bytes(out)


class AuraProxy:
    def __init__(self, iface, debug=False):
        pass

    def set_exc_guard(self, m):
        state["guard"] = int(m)

    def get_exc_count(self):
        c, state["exc"] = state["exc"], 0
        return c

    def read32(self, a):
        assert state["guard"] & 1, "MMIO read without the guard"
        for lo, hi, why in forbidden:
            if lo <= a < hi:
                raise SystemExit(f"FAIL: touched {why} at {a:#x}")
        return regs.get(a, 0)


class Node:
    def __init__(self, props, reg=()):
        self.p, self.r = props, list(reg)

    def getprop(self, n, d=None):
        return self.p.get(n, d)

    def __getattr__(self, attr):  # like ADTNode: devices, ps_regs, ...
        p = self.__dict__.get("p", {})
        for k in (attr, attr.replace("_", "-")):
            if k in p:
                return p[k]
        raise AttributeError(attr)

    def get_reg(self, i):
        return self.r[i]


class ADT(dict):
    _pmgr_path = "/arm-io/pmgr"

    def __getitem__(self, k):
        if k not in self:
            raise KeyError(k)
        return dict.__getitem__(self, k)

    @staticmethod
    def pmgr_dev_get_id(d):
        return d.id

    @staticmethod
    def pmgr_dev_get_addr(d):
        return PMGR + d.off

    @staticmethod
    def pmgr_dev_get_parents(d):
        return d.parents


class ProxyUtils:
    def __init__(self, p):
        self.adt = TREE


def dart_pte(pa):
    return ((pa >> 4) & (((1 << 38) - 1) & ~((1 << 10) - 1))) | 1


# ---------------------------------------------------------------- the machine
B0, B1, BOFF, BNOG, PMGR = 0x2_1180_0000, 0x2_1182_0000, 0x2_8618_0000, 0x2_9000_0000, 0x2_8e08_0000
l1, l2 = new_page(), new_page()
w64(l1, dart_pte(l2))


def mapit(iova, pa):
    w64(l2 + 8 * ((iova >> 14) & 0x7ff), dart_pte(pa))


sp = new_page(); page(sp)[0:16] = b"ANE-TASK-DESC-01"; mapit(0x4000, sp)
sp2 = new_page(); page(sp2)[0:8] = struct.pack("<Q", 0x4000); mapit(0x8000, sp2)
fp = new_page(); faulting_pages.add(fp); mapit(0xc000, fp)
for k in range(512):  # an 8 MiB frame: only its head is read
    pg = new_page(); page(pg)[0:4] = struct.pack("<I", k); mapit(0x100000 + k * PAGE, pg)
mapit(0x2000000 - PAGE, 0x2_1000_0000)  # MMIO leaf: listed, never read
regs[B0 + 0x1000], regs[B0 + 0x1400] = 1, ((l1 >> 14) << 2) | 1
regs[B0 + 0x1004], regs[B0 + 0x1404] = 1, ((l1 >> 14) << 2) | 1  # sid 1: exclave
l0b, l1b, l2b = new_page(), new_page(), new_page()
w64(l0b + 8 * 1, dart_pte(l1b)); w64(l1b, dart_pte(l2b))
fourp = new_page(); page(fourp)[0:6] = b"FOURLV"; w64(l2b + 8 * 3, dart_pte(fourp))
regs[B0 + 0x1008], regs[B0 + 0x1408] = 9, ((l0b >> 14) << 2) | 1  # sid 2: 4-level
forbidden += [(B1, B1 + 0xc000, "the dead DART instance's registers")]
regs[B1 + 0x1000] = 0
inst = b"".join(struct.pack("<4sI8s", t[::-1], i, n)
                for t, i, n in [(b"DART", 0, b"DARTLLT"), (b"DART", 1, b"DARTBRD"), (b"DAPF", 0, b"DAPFLLT")])
# instance 1 is "registers fault": model that with the exception count instead of a forbidden range
forbidden.pop()
_r32 = AuraProxy.read32


def read32(self, a):
    if B1 <= a < B1 + 0xc000:
        state["exc"] += 1
        return 0xacce5515
    return _r32(self, a)


AuraProxy.read32 = read32
ane = Node({"instance": inst, "sid-count": struct.pack("<I", 16), "exclave-sid": struct.pack("<II", 1, 7)},
           [(B0, 0xc000), (B1, 0xc000), (0x2_1181_0000, 0x4000)])
# PMGR, shaped like T8142's: ane's gate 0x14b is virtual (ANE-SYS-V) -> real ANE_SYS 0x49, active; dart-off's gate
# 0x165 is virtual -> real 0x4f, powered off; dart-nogate's gate 0x164 is virtual with no parent (unknown)
dev = lambda i, off, name, no_ps=False, parents=(): types.SimpleNamespace(
    id=i, off=off, name=name, flags=types.SimpleNamespace(no_ps=no_ps), parents=list(parents))
regs[PMGR + 0x100], regs[PMGR + 0x108] = 0xff, 0x0f
forbidden += [(BOFF, BOFF + 0xc000, "a powered-off DART"), (BNOG, BNOG + 0xc000, "a DART of unknown power")]
off = Node({"clock-gates": struct.pack("<I", 0x165), "instance": inst[:16]}, [(BOFF, 0xc000)])
nog = Node({"clock-gates": struct.pack("<I", 0x164), "instance": inst[:16]}, [(BNOG, 0xc000)])
# GPU
gpu_region, shared = DRAM0 + 0x7_fffb_8000, DRAM0 + 0x7_fff3_8000
for a in range(shared, shared + 0x80000, PAGE):
    page(a)
page(gpu_region)


def uat_map(root, va, pa):
    i1, i2, i3 = (va >> 36) & 0x3f, (va >> 25) & 0x7ff, (va >> 14) & 0x7ff
    e1 = struct.unpack_from("<Q", page(root), 8 * i1)[0]
    if not e1:
        e1 = new_page() | 3; w64(root + 8 * i1, e1)
    t2 = e1 & ~0x3fff & ((1 << 48) - 1)
    e2 = struct.unpack_from("<Q", page(t2), 8 * i2)[0]
    if not e2:
        e2 = new_page() | 3; w64(t2 + 8 * i2, e2)
    w64((e2 & ~0x3fff & ((1 << 48) - 1)) + 8 * i3, (pa & ~0x3fff) | 3 | (1 << 10))


initp = new_page(); page(initp)[0:8] = b"INITDATA"
uat_map(shared, 0xffff_fc20_001a_8000, initp)
uat_map(shared + 0x40000, 0xffff_fc20_001b_0000, initp)
w64(gpu_region + 8, shared | 1)
user = new_page(); uat_map(user, 0x15_0000_0000, new_page())
w64(gpu_region + 16 * 3, user | (5 << 48) | 1)
TREE = ADT({"/chosen": Node({"dram-base": DRAM0, "dram-size": 32 << 30}), "/arm-io/dart-ane": ane,
            "/arm-io/ane": Node({"clock-gates": struct.pack("<II", 0x14b, 0x20a)}),
            "/arm-io/dart-off": off, "/arm-io/dart-nogate": nog,
            "/arm-io/pmgr": Node({"devices": [dev(0x14b, 0, "ANE-SYS-V", True, [0x49]), dev(0x49, 0x100, "ANE_SYS"),
                                             dev(0x165, 0, "X-V", True, [0x4f, 0]), dev(0x4f, 0x108, "DISP_FE"),
                                             dev(0x164, 0, "AUDIO-LLT-V", True)]}),
            "/arm-io/sgx": Node({"gpu-region-base": gpu_region, "gfx-shared-region-base": shared})})

# dart-off is off at the freeze, but the ring holds its last TCR/TTBR writes: walked from DRAM, registers untouched
rl1, rl2 = new_page(), new_page()
w64(rl1, dart_pte(rl2))
ringp = new_page(); page(ringp)[0:8] = b"RINGPAGE"; w64(rl2 + 8 * 2, dart_pte(ringp))
ring_txt = tempfile.NamedTemporaryFile("w", suffix="-gpu.txt", delete=False)
ring_txt.write("# gpu ring: records 1..3 (oldest first); t = CNTPCT \n"
               f"1 t=0x1 c6 W32 {BOFF + 0x1000:#x} = 0x0 pc=0x0  /device-tree/arm-io/dart-off[0]+0x1000\n"
               f"2 t=0x2 c6 W32 {BOFF + 0x1400:#x} = {((rl1 >> 14) << 2) | 1:#x} pc=0x0  /device-tree/arm-io/dart-off[0]+0x1400\n"
               f"3 t=0x3 c6 W32 {BOFF + 0x1000:#x} = 0x1 pc=0x0  /device-tree/arm-io/dart-off[0]+0x1000\n")
ring_txt.close()

# ---------------------------------------------------------------- run
aura = types.ModuleType("aura")
px = types.ModuleType("aura.proxy"); px.AuraProxy, px.UartInterface, px.GUARD = AuraProxy, UartInterface, GUARD
pu = types.ModuleType("aura.proxyutils"); pu.ProxyUtils, pu.bootstrap_port = ProxyUtils, lambda i, p: None
spec = importlib.util.spec_from_file_location("aura.pmgr_power", AURA / "pmgr_power.py")
pw = importlib.util.module_from_spec(spec); spec.loader.exec_module(pw); aura.pmgr_power = pw
sys.modules.update({"aura": aura, "aura.proxy": px, "aura.proxyutils": pu, "aura.pmgr_power": pw})
out = pathlib.Path(tempfile.mkdtemp(prefix="guest-mem-test-"))
os.environ["AURADEVICE"] = "fake"
sys.argv = ["dump_guest_mem.py", "--out", str(out), "--dart", "dart-ane", "--dart", "dart-missing", "--dart", "dart-off",
            "--dart", "dart-nogate", "--gpu", "--max-mib", "2", "--big-mib", "4", "--ring", ring_txt.name]
runpy.run_path(str(TOOLS / "dump_guest_mem.py"), run_name="__main__")

log = (out / "dump.log").read_text()
hv = lambda *a: subprocess.run([sys.executable, str(TOOLS / "guest_mem.py"), str(out), *a], capture_output=True, text=True).stdout
checks = [
    ("ane powered via its virtual gate", "dart-ane: powered (ANE_SYS" in log and "sid 0:" in log),
    ("exclave stream not walked", "sid 1: exclave stream, not walked" in log),
    ("4-level walked", "4-level" in log),
    ("dead instance skipped", "DARTBRD" in log and "registers fault" in log),
    ("powered-off DART not touched", "dart-off: powered off" in log),
    ("powered-off DART walked from the ring", "registers from the ring" in log
     and "RINGPAGE" in hv("x", "dart-off", "0x8000", "0x8")),
    ("unknown-power DART not touched", "dart-nogate: power unknown" in log),
    ("guard off at the end", state["guard"] == 0),
    ("descriptor readable", "ANE-TASK-DESC-01" in hv("x", "dart-ane", "0x4000", "0x10")),
    ("4-level readable", "FOURLV" in hv("x", "dart-ane:2", "0x100000c000", "0x10")),
    ("frame head read", "00000003" in hv("x", "dart-ane", "0x10c000", "0x4")),
    ("frame tail not read", "not mapped or not read" in hv("x", "dart-ane", "0x110000", "0x4")
     or hv("x", "dart-ane", "0x110000", "0x4") == ""),
    ("faulting page not read", hv("x", "dart-ane", "0xc000", "0x4") == ""),
    ("GPU kernel root (exact name)", "INITDATA" in hv("x", "gpu/0,kernel-gfx", "0xfffffc20001a8000", "0x8") and "INITDATA" in hv("x", "gpu/kernel-gfx1", "0xfffffc20001b0000", "0x8")),
    ("GPU app context maps only", "listed-only 1" in hv("summary")),
]
bad = [n for n, ok in checks if not ok]
for n, ok in checks:
    print(f"{'ok  ' if ok else 'FAIL'} {n}")
sys.exit(1 if bad else 0)
