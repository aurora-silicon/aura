# SPDX-License-Identifier: MIT
"""Host lifecycle of the T8152 (J804) Windows launch against a recording proxy model.

Runs the real HV.init / load_raw / start of proxyclient/aura/hv against a synthetic
T8152 ADT and checks what the host asks the resident to map, which ADT nodes the
guest sees, and where the guest is placed. It does not execute the C resident (see
test_windows_t8152_policy.py for that) and qualifies no hardware.
"""

import struct
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from construct import Array, Container, Int64ul  # noqa: E402
from aura.adt import ADTNode, load_adt  # noqa: E402
from aura.hv import HV  # noqa: E402
from aura.proxy import IODEV, ProxyRemoteError, UartTimeout  # noqa: E402
from aura.tgtypes import BootArgs_r2  # noqa: E402
from aura import windows  # noqa: E402

DRAM, DRAM_SIZE = 0x10000000000, 0x100000000
WINDOW = (0x10040000000, 0x100f0000000)
AIC = (0x2a0000000, 0x100000)
QSPI = 0x2b0000000
VALID = 1
CAPS = (windows.CAPS_MAGIC | 1 << 16 | windows.REQUIRED_CAPS | windows.CONFIGURATION |
        windows.FRAMEBUFFER_SHADOW | windows.SERIAL_CDC_ISOLATED | windows.T8152_POLICY |
        windows.T8152_LIVE_CPUS | windows.T8152_WFE_PLACEMENT | windows.T8152_ACTLR_ARCH |
        windows.T8152_GIC_TRAPALL)


def u64_pair(node, name, value):
    setattr(node, name, value)
    node._types[name] = (Array(2, Int64ul), False)


def fixture():
    root = ADTNode()
    root.name = "device-tree"
    root.model = "Mac18,1"
    root.address_cells = root.size_cells = 2
    product = root.create_node("product")
    product.product_name = product.product_description = "Synthetic Mac"
    product.product_soc_name = "T8152"
    root.create_node("defaults")
    chosen = root.create_node("chosen")
    chosen.chip_id = 0x8152
    memory = chosen.create_node("memory-map")
    for name, value in (("SEPFW", (0x10007100000, 0x4000)),
                        ("TrustCache", (0x10007104000, 0x4000)),
                        ("DeviceTree", (0x10007200000, 0x20000)),
                        ("BootArgs", (0x10007300000, 0x4000))):
        u64_pair(memory, name, value)
    carve = chosen.create_node("carveout-memory-map")
    for name, value in (("region-id-24", (DRAM, DRAM_SIZE)),
                        ("region-id-353", (0x10006000000, 0x1e000000)),
                        ("region-id-218", (WINDOW[1], DRAM + DRAM_SIZE - WINDOW[1]))):
        u64_pair(carve, name, value)
    cpus = root.create_node("cpus")
    cpu = cpus.create_node("cpu6")
    cpu.cpu_id, cpu.cluster_id, cpu.state = 6, 1, "running"
    arm = root.create_node("arm-io")
    arm.address_cells = arm.size_cells = 2
    arm.ranges = struct.pack("<QQQ", 0, 0, 1 << 40)
    for name, base in (("aic", AIC[0]), ("uart0", 0x2c0000000), ("qspi", QSPI),
                       ("i2c3", 0x2b4000000), ("aop", 0x2b5000000), ("admac-base-ns", 0x2b6000000),
                       ("iop-audio-controller", 0x2b7000000), ("sep", 0x2a8000000)):
        node = arm.create_node(name)
        node.reg = [Container(addr=base, size=AIC[1] if name == "aic" else 0x4000)]
        if name == "uart0":
            node.interrupts = (10,)
            setattr(node, "AAPL,phandle", 1)
    spinor = arm["qspi"].create_node("spinor")
    nvram = spinor.create_node("anvram")
    nvram.compatible = b"nvram,nor\0"
    arm.create_node("usb-drd0")
    pmgr = arm.create_node("pmgr")
    pmgr.compatible = b"pmgr,synthetic\0"
    pmgr.devices = bytes(48 * 2)
    blob = root.build()
    args = Container(revision=2, version=2, virt_base=0xfffffe0010000000, phys_base=DRAM,
                     mem_size=DRAM_SIZE, mem_size_actual=DRAM_SIZE,
                     top_of_kernel_data=0x10012000000,
                     video=Container(base=0x10180000000, display=1, stride=4096, width=1024,
                                     height=768, depth=30),
                     machine_type=0, devtree=0xfffffe0011200000, devtree_size=0x20000,
                     cmdline="", boot_flags=0)
    return blob, args


def run(case="success"):
    blob, args = fixture()
    trace, maps, writes = [], [], {}

    class Proxy:
        def hv_windows_capabilities(self):
            trace.append("caps")
            if case == "old-resident":
                return CAPS & ~(windows.T8152_POLICY | windows.T8152_LIVE_CPUS |
                                windows.T8152_WFE_PLACEMENT)
            if case == "no-live-cpu-refusal":
                return CAPS & ~(windows.T8152_LIVE_CPUS | windows.T8152_WFE_PLACEMENT)
            if case == "no-wfe-trap":
                return CAPS & ~(windows.T8152_WFE_PLACEMENT | windows.T8152_ACTLR_ARCH |
                                windows.T8152_GIC_TRAPALL)
            if case == "apple-actlr":
                return CAPS & ~(windows.T8152_ACTLR_ARCH | windows.T8152_GIC_TRAPALL)
            if case == "hardware-gic-cpuif":
                return CAPS & ~windows.T8152_GIC_TRAPALL
            return CAPS

        def smp_is_alive(self, cpu):
            trace.append(("alive", cpu))
            if case == "secondaries-not-started":
                return cpu == 6
            return not (case == "cpu-missing" and cpu == 9)

        def hv_windows_configure(self, flags):
            trace.append(("config", flags))
            return -1 if case == "configure-refused" else 0

        def iodev_whoami(self): return IODEV.USB0
        def iodev_set_usage(self, *a): pass
        def hv_init(self):
            trace.append("hv_init")
            if case == "resident-hv-init-fails":
                raise ProxyRemoteError("Reply error: Remote operation failed")
            if case == "link-lost":
                raise UartTimeout("timeout")
            return 0
        def hv_map_vuart(self, *a): trace.append(("vuart", *a))

        def hv_windows_guest_ram(self, base, size, host_top):
            trace.append(("guest_ram", base, size, host_top))
            return -1 if case == "resident-refuses" else 0

        def hv_map(self, ipa, to, size, incr):
            maps.append((ipa, to, size, incr))
            return 0

        def mcc_get_carveouts(self): raise AssertionError("T8152 has no MCC carve-out list")
        def dc_cvau(self, *a): pass
        def ic_ivau(self, *a): pass
        def memcpy8(self, *a): pass
        def fb_improve_logo(self): pass
        def fb_shutdown(self, *a): pass

        def hv_start(self, entry, bootargs):
            trace.append(("start", entry, bootargs))
            return 2

        def write64(self, *a): raise AssertionError("RVBAR write")

    class Interface:
        dev = SimpleNamespace(timeout=3)
        def set_handler(self, *a): pass
        def set_event_handler(self, *a): pass
        def writemem(self, address, data): writes[address] = bytes(data)

    class Utils:
        def __init__(self):
            self.adt, self.ba = load_adt(blob), args
            self.heap_top = 0x10024000000
            self.cpu_features = SimpleNamespace(apple_sysregs_unlocked=False)
        def get_adt(self): return blob
        def compressed_writemem(self, address, data, *a): writes[address] = bytes(data)
        def inst(self, instruction): assert instruction == 0xd50c83df  # tlbi after pt_update
        def msr(self, *a): raise AssertionError("host wrote a C-owned register")
        def mrs(self, *a): raise AssertionError("host read a C-owned register")

    hv = HV(Interface(), Proxy(), Utils(), windows=True)
    hv.windows_framebuffer = "bgra-shadow"
    hv.windows_serial = "cdc"
    hv.windows_guest_ram = WINDOW
    if case == "no-window":
        hv.windows_guest_ram = None
    if case == "wfi":
        hv.windows_wfi = "wfi"
    if case == "carrier-cpuif":
        hv.windows_gic_cpuif = "carrier"
    hv.init()
    payload = bytes(range(256)) * 64
    hv.load_raw(payload, 0)
    result = hv.start()
    return SimpleNamespace(hv=hv, trace=trace, maps=maps, writes=writes, result=result)


def test_t8152_host_maps_only_the_window_and_the_aic():
    r = run()
    assert r.result == 2
    ram = ("guest_ram", WINDOW[0], WINDOW[1] - WINDOW[0], 0x10024000000)  # + the launcher heap top
    assert ram in r.trace and r.trace.index("hv_init") < r.trace.index(ram)
    hw = [(ipa, to & ~0x3fff, size) for ipa, to, size, incr in r.maps if to & VALID]
    assert hw, "nothing mapped"
    for ipa, pa, size in hw:
        in_window = WINDOW[0] <= pa and pa + size <= WINDOW[1]
        in_aic = AIC[0] <= pa and pa + size <= AIC[0] + AIC[1]
        assert ipa == pa and (in_window or in_aic), (hex(ipa), hex(pa), hex(size))
    assert sum(size for ipa, pa, size in hw if WINDOW[0] <= pa < WINDOW[1]) == WINDOW[1] - WINDOW[0]
    assert not any(pa <= QSPI < pa + size for ipa, pa, size in hw)
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert config[1] & windows.CONFIG_FRAMEBUFFER_SHADOW
    # B1: the software GIC CPU interface is always requested on T8152 (measurement (f)).
    assert config[1] & windows.CONFIG_T8152_SOFT_GIC
    assert not config[1] & (windows.CONFIG_T8152_FAST_IPI | windows.CONFIG_T8152_WFI)


def test_t8152_guest_placement_and_adt():
    r = run()
    hv = r.hv
    assert hv.guest_base % 0x10000 == 0
    assert WINDOW[0] <= hv.adt_base < hv.guest_base < WINDOW[1]
    start = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "start")
    assert start[1] == hv.guest_base and start[2] == hv.guest_base + hv.bootargs_off
    boot = BootArgs_r2.parse(r.writes[start[2]])
    assert boot.phys_base == WINDOW[0] and boot.mem_size == WINDOW[1] - WINDOW[0]
    guest = load_adt(r.writes[hv.adt_base])
    for path in ("/arm-io/qspi", "/arm-io/i2c3", "/arm-io/aop", "/arm-io/admac-base-ns",
                 "/arm-io/iop-audio-controller", "/arm-io/usb-drd0"):
        assert path not in guest, path
    assert "/arm-io/uart0" in guest and "/arm-io/aic" in guest
    marker = next(p for p in guest["/chosen"].tostruct()["properties"]
                  if p["name"] == "aura,windows-nvram-hidden")
    assert marker["value"] == b"\x01\x00\x00\x00"


@pytest.mark.parametrize("case, message", [
    ("old-resident", "T8152 stage-2 policy"),
    ("no-window", "explicit guest RAM window"),
    ("resident-refuses", "refused the T8152 guest RAM window"),
    ("no-live-cpu-refusal", "12-live-CPU refusal"),
    ("no-wfe-trap", "guest-WFE trap and code placement"),
    ("apple-actlr", "architectural ACTLR_EL12"),
    ("hardware-gic-cpuif", "trapped software GIC CPU interface"),
    ("carrier-cpuif", "GIC_CPUIF_UNDEF"),
    ("cpu-missing", r"only 11 of 12 CPUs .*not live: \[9\]"),
    ("secondaries-not-started", "only 1 of 12 CPUs"),
    ("wfi", "WFI_WIPES"),
])
def test_t8152_launch_refusals(case, message):
    with pytest.raises((ValueError, RuntimeError), match=message):
        run(case)


def test_t8152_vuart_gets_no_physical_interrupt():
    # B8: uart0's ADT interrupt (10 in the fixture) is not passed on T8152, so even an older
    # resident never clears uart0's UTRSTAT or drives an AIC line for the guest's serial.
    r = run()
    vuart = [t for t in r.trace if isinstance(t, tuple) and t[0] == "vuart"]
    assert vuart == [("vuart", 0x2c0000000, 0, IODEV.USB0)]


def test_t8152_live_cpu_check_runs_before_the_resident_starts_anything():
    r = run()
    alive = [t[1] for t in r.trace if isinstance(t, tuple) and t[0] == "alive"]
    assert alive == list(range(12))
    first_alive = next(i for i, t in enumerate(r.trace) if isinstance(t, tuple) and t[0] == "alive")
    config = next(i for i, t in enumerate(r.trace) if isinstance(t, tuple) and t[0] == "config")
    assert first_alive < config < r.trace.index("hv_init")


@pytest.mark.parametrize("case", ["cpu-missing", "secondaries-not-started", "configure-refused"])
def test_t8152_launcher_refusals_are_typed(case):
    # run_guest.py reports only these (and ValueError) as "refused, proxy still up".
    with pytest.raises(windows.WindowsLaunchRefused):
        run(case)


@pytest.mark.parametrize("case, error", [("resident-hv-init-fails", ProxyRemoteError),
                                         ("link-lost", UartTimeout)])
def test_t8152_resident_and_link_failures_are_not_refusals(case, error):
    with pytest.raises(error) as info:
        run(case)
    assert not isinstance(info.value, windows.WindowsLaunchRefused)


def test_run_guest_does_not_call_a_link_failure_a_refusal():
    source = (REPO / "proxyclient/tools/run_guest.py").read_text()
    init = source.split("hv.init()", 1)[1].split("hv.set_logfile", 1)[0]
    assert "except (ValueError, WindowsLaunchRefused)" in init
    assert "except ProxyRemoteError" in init
    assert "except (UartError, ProxyReplyError)" in init and "raise" in init
    assert "RuntimeError" not in init
