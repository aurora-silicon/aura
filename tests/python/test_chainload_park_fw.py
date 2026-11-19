# SPDX-License-Identifier: MIT
"""Park chainloaded boot blobs when an in-place upload ends below inherited_top.

Failure model recorded before changing proxyclient/tools/chainload.py:
- On 27.x mBoot, Stage 1 can be enrolled with an expanded Mach-O execution
  window so u.ba.top_of_kernel_data (inherited_top) sits well above the Stage 2
  raw image end, with both image_end and packed_end below inherited_top. Packing
  SEPFW and BootArgs at image_end leaves non-executable firmware blobs inside
  [image_end, inherited_top), obstructing the OS execution window.
- When not relocating and image_end < inherited_top, chainload.py must allocate
  a separate 16 KiB-aligned firmware block above live_heap, restrict the stub's
  in-place copy span to image_only_size so [image_end, inherited_top) is never
  written, update /chosen/memory-map entries and BootArgs, and advance the next
  stage's top_of_kernel_data past the parked block.
- If memalign fails or overflows RAM, chainload.py must raise ValueError before
  copying; if memalign hands back memory below live_heap (or inside T8140 RO),
  it must reject the parked destination.
- Relocated uploads (--relocate) and in-place uploads where image_end already
  reaches inherited_top without overlapping live_heap keep the packed layout.
- --reclaim-stage1-heap drops Stage 1's heap from the next stage's
  top_of_kernel_data (the J714s SPTM_VIRT + MTE carve-out floor). A block parked
  above live_heap would hold that top above the heap and silently undo the
  option, so with it an image ending below inherited_top keeps the packed
  layout, and the next top stays at inherited_top.
"""

from pathlib import Path
import runpy
import struct
import sys
import types

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from aura.tgtypes import BootArgs_r3  # noqa: E402
from aura.utils import align  # noqa: E402


class MemoryMapNode:
    def __init__(self, sepfw):
        self._properties = {"SEPFW": sepfw}
        self._types = {}
        self.BootArgs = None


class ChosenNode:
    def __init__(self, mm):
        self.chip_id = 0x6041
        self._children = {"memory-map": mm}

    def __getitem__(self, key):
        return self._children[key]


class FakeADT:
    def __init__(self, sepfw):
        self.mm = MemoryMapNode(sepfw)
        self.chosen = ChosenNode(self.mm)

    def __getitem__(self, key):
        if key in ("chosen", "/chosen"):
            return self.chosen
        if key == "cpus":
            return []
        raise KeyError(key)


class FakeTargetEnv:
    def __init__(self, tmp_path, *, inherited_top, live_heap, alloc_override=None):
        self.phys_base = 0x10000000000
        self.mem_size = 0x40000000
        self.base = self.phys_base + 0x1000000
        self.sepfw = (self.phys_base + 0x400000, 0x20000)
        self.live_heap = live_heap
        self.heap_next = live_heap
        self.alloc_override = alloc_override
        self.writes = []
        self.copies = []
        self.reloads = []
        self.adt = FakeADT(self.sepfw)

        raw_ba = BootArgs_r3.parse(bytes(BootArgs_r3.sizeof()))
        raw_ba.revision = 3
        raw_ba.version = 2
        raw_ba.virt_base = 0xFFFFFE0010000000
        raw_ba.phys_base = self.phys_base
        raw_ba.mem_size = self.mem_size
        raw_ba.top_of_kernel_data = inherited_top
        raw_ba.devtree = raw_ba.virt_base + 0x100000
        raw_ba.devtree_size = 0x40000
        self.ba = raw_ba

        self.payload = tmp_path / "aura.bin"
        self.payload.write_bytes(b"\x1f\x20\x03\xd5" * (0x48000 // 4))
        self.stub = tmp_path / "copy.stub"
        baked = (0xDEADBEEF12345678).to_bytes(8, "little")
        self.stub.write_bytes(baked + b"\x1f\x20\x03\xd5" * 4 + baked + b"\xc0\x03\x5f\xd6")

    def make_setup_module(self):
        env = self
        mod = types.ModuleType("aura.setup")

        class FakeIface:
            dev = None

            def writemem(self, addr, data):
                env.writes.append((addr, bytes(data)))

            def nop(self):
                pass

        class FakeProxy:
            P_VECTOR = 1

            def iodev_set_usage(self, *_args):
                pass

            def get_chipid(self):
                return 0x6041

            def heapblock_alloc(self, size):
                assert size == 0
                return env.live_heap

            def memalign(self, alignment, size):
                assert alignment == 0x4000
                if env.alloc_override is not None:
                    return env.alloc_override(size)
                addr = align(env.heap_next, alignment)
                env.heap_next = addr + align(size, alignment)
                return addr

            def memcpy8(self, dst, src, size):
                env.copies.append((dst, src, size))

            def dc_cvac(self, _addr, _size):
                pass

            def ic_ivau(self, _addr, _size):
                pass

            def reload(self, stub_addr, bootargs_addr, image_addr, dest_base, copy_size):
                env.reloads.append((stub_addr, bootargs_addr, image_addr, dest_base, copy_size))

        class FakeUtils:
            base = env.base
            ba = env.ba
            adt = env.adt

            def compressed_writemem(self, addr, data, _progress):
                env.writes.append((addr, bytes(data)))

            def push_adt(self):
                pass

        mod.iface = FakeIface()
        mod.p = FakeProxy()
        mod.u = FakeUtils()
        mod.align = align
        mod.IODEV = types.SimpleNamespace(FB=0)
        mod.ProxyCommandError = RuntimeError
        return mod

    def run(self, monkeypatch, extra_args=()):
        setup_mod = self.make_setup_module()
        monkeypatch.setitem(sys.modules, "aura.setup", setup_mod)
        monkeypatch.setattr(
            sys,
            "argv",
            ["chainload.py", "--raw", "--stub", str(self.stub), *extra_args, str(self.payload)],
        )
        runpy.run_path(str(REPO / "proxyclient/tools/chainload.py"), run_name="__main__")


def test_in_place_image_below_inherited_top_parks_firmware_block(tmp_path, monkeypatch):
    inherited_top = 0x10009000000  # 128 MiB enrolled execution window above 0x10001000000
    live_heap = 0x1000B000000
    env = FakeTargetEnv(tmp_path, inherited_top=inherited_top, live_heap=live_heap)
    env.run(monkeypatch)

    image_only_size = align(env.payload.stat().st_size + 4)
    image_end = env.base + image_only_size
    assert image_end < inherited_top

    assert len(env.reloads) == 1
    _stub_addr, bootargs_addr, _image_addr, dest_base, copy_size = env.reloads[0]
    assert dest_base == env.base
    assert copy_size == image_only_size, "stub must not overwrite [image_end, inherited_top)"

    sepfw_dest, sepfw_size = env.adt.mm._properties["SEPFW"]
    ba_dest, ba_size = env.adt.mm.BootArgs
    assert sepfw_dest >= live_heap
    assert sepfw_size == env.sepfw[1]
    assert ba_dest == bootargs_addr == sepfw_dest + align(sepfw_size)
    assert ba_size == 0x4000

    for addr, data in env.writes:
        assert addr + len(data) <= image_end or addr >= live_heap
    for dst, _src, size in env.copies:
        assert dst >= live_heap and size == sepfw_size

    ba_writes = [data for addr, data in env.writes if addr == bootargs_addr]
    assert len(ba_writes) == 1
    parsed = BootArgs_r3.parse(ba_writes[0])
    assert parsed.top_of_kernel_data >= ba_dest + ba_size


@pytest.mark.parametrize(
    ("override", "expected_msg"),
    [
        (lambda _size: 0, "No safe location for firmware block"),
        (lambda _size: 0x1003FFFF000, "No safe location for firmware block"),
        (lambda _size: 0x10002000000, "Parked firmware block overlaps protected/live memory"),
    ],
)
def test_parked_firmware_allocation_failures_are_rejected(
    tmp_path, monkeypatch, override, expected_msg
):
    env = FakeTargetEnv(
        tmp_path,
        inherited_top=0x10009000000,
        live_heap=0x1000B000000,
        alloc_override=override,
    )
    with pytest.raises(ValueError, match=expected_msg):
        env.run(monkeypatch)
    assert not env.reloads


def test_relocated_and_at_top_uploads_keep_packed_layout(tmp_path, monkeypatch):
    # Relocated upload allocates image + firmware together on the Stage 1 heap.
    reloc_env = FakeTargetEnv(tmp_path, inherited_top=0x10009000000, live_heap=0x1000B000000)
    reloc_env.run(monkeypatch, extra_args=("--relocate",))
    image_only_size = align(reloc_env.payload.stat().st_size + 4)
    fw_size = align(reloc_env.sepfw[1]) + 0x4000
    _stub_addr, bootargs_addr, _image_addr, dest_base, copy_size = reloc_env.reloads[0]
    assert dest_base >= reloc_env.live_heap
    assert copy_size == image_only_size + fw_size
    assert bootargs_addr == dest_base + image_only_size + align(reloc_env.sepfw[1])

    # In-place upload whose image_end already reaches inherited_top with no live_heap gap.
    at_top_top = 0x10001000000 + image_only_size
    at_top_env = FakeTargetEnv(tmp_path, inherited_top=at_top_top, live_heap=at_top_top)
    at_top_env.run(monkeypatch)
    _stub_addr, bootargs_addr, _image_addr, dest_base, copy_size = at_top_env.reloads[0]
    assert dest_base == at_top_env.base
    assert copy_size == image_only_size + fw_size
    assert bootargs_addr == at_top_env.base + image_only_size + align(at_top_env.sepfw[1])


def test_reclaim_keeps_packed_layout_below_inherited_top(tmp_path, monkeypatch):
    inherited_top = 0x10009000000
    live_heap = 0x1000B000000
    env = FakeTargetEnv(tmp_path, inherited_top=inherited_top, live_heap=live_heap)
    env.run(monkeypatch, extra_args=("--reclaim-stage1-heap",))

    image_only_size = align(env.payload.stat().st_size + 4)
    fw_size = align(env.sepfw[1]) + 0x4000
    assert env.base + image_only_size < inherited_top
    _stub_addr, bootargs_addr, _image_addr, dest_base, copy_size = env.reloads[0]
    assert dest_base == env.base
    assert copy_size == image_only_size + fw_size
    assert env.adt.mm._properties["SEPFW"][0] == env.base + image_only_size
    assert bootargs_addr == env.base + image_only_size + align(env.sepfw[1])

    ba_writes = [data for _addr, data in env.writes if len(data) == BootArgs_r3.sizeof()]
    assert len(ba_writes) == 1
    assert BootArgs_r3.parse(ba_writes[0]).top_of_kernel_data == inherited_top
