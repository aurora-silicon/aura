# SPDX-License-Identifier: MIT
"""Host checks of the T8152 (J804) Windows launch rules in proxyclient/aura/windows.py.

Synthetic values only: no ADT, boot_args or address from a real machine.
"""

import hashlib
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from aura import windows  # noqa: E402

CAPS_OLD = 0x4157494E000107FF  # every capability before the T8152 policy bit
CAPS_T8152_V1 = CAPS_OLD | windows.T8152_POLICY  # 6bdac9ee: no 12-live-CPU refusal
CAPS_T8152_V2 = CAPS_T8152_V1 | windows.T8152_LIVE_CPUS  # cd5b10f1: no guest-WFE trap
CAPS_T8152_V3 = CAPS_T8152_V2 | windows.T8152_WFE_PLACEMENT  # 618d6d40: Apple ACTLR alias
CAPS_T8152_V4 = CAPS_T8152_V3 | windows.T8152_ACTLR_ARCH  # 32555bf9: hardware GIC cpuif
CAPS_T8152 = CAPS_T8152_V4 | windows.T8152_GIC_TRAPALL
GIB = 1 << 30


def write_profile(tmp_path, **fields):
    fd = tmp_path / "mu.fd"
    fd.write_bytes(bytes(range(256)) * 64)
    profile = {"schema": "aura.windows.boot.v1", "name": "t8152-fixture",
               "target": {"chip_id": "0x8152", "model": "Mac18,1"},
               "mu": {"path": fd.name, "sha256": hashlib.sha256(fd.read_bytes()).hexdigest(),
                      "entry_offset": "0x0"},
               "framebuffer": "bgra-shadow",
               "guest_ram": {"base": "0x10040000000", "top": "0x10080000000"}}
    profile.update(fields)
    for key in [k for k, v in profile.items() if v is None]:
        del profile[key]
    path = tmp_path / "profile.json"
    path.write_text(json.dumps(profile))
    return path


def test_t8152_profile_binds_window_and_policy(tmp_path):
    plan = windows.plan_profile(write_profile(tmp_path))
    assert plan.guest_ram == (0x10040000000, 0x10080000000)
    assert plan.ipi == "poll" and plan.on_reset == "stay"
    plan = windows.plan_profile(write_profile(tmp_path, ipi="fast", on_reset="reboot"))
    assert plan.ipi == "fast" and plan.on_reset == "reboot"
    plan = windows.plan_profile(write_profile(tmp_path, gic_cpuif="software"))
    assert plan.gic_cpuif == "software"
    # The software CPU interface is the T8152 default (measurement (f)).
    assert windows.plan_profile(write_profile(tmp_path)).gic_cpuif == "software"
    assert plan.wfi == "poll"  # no WFI at EL2: measurement (a) found WFI_WIPES


@pytest.mark.parametrize("fields, message", [
    ({"guest_ram": None}, "must bind guest_ram"),
    ({"guest_ram": {"base": "0x10040004000", "top": "0x10080000000"}}, "64 KiB aligned"),
    ({"guest_ram": {"base": "0x10040000000", "top": "0x10040100000"}}, "at least 256 MiB"),
    ({"guest_ram": {"base": "0x10040000000", "top": "0x10040000000"}}, "non-empty"),
    ({"framebuffer": "native"}, "bgra-shadow"),
    ({"ipi": "sev"}, "ipi must be"),
    ({"on_reset": "now"}, "on_reset must be"),
    ({"gic_cpuif": "hardware"}, "gic_cpuif must be"),
    ({"gic_cpuif": "carrier"}, "GIC_CPUIF_UNDEF"),
    ({"wfi": "wfe"}, "wfi must be"),
    ({"wfi": "wfi"}, "WFI_WIPES"),
    ({"reserved_memory": [{"name": "x", "base": "0x10050000000", "size": "0x1000"}]},
     "no reserved_memory"),
])
def test_t8152_profile_refusals(tmp_path, fields, message):
    with pytest.raises(ValueError, match=message):
        windows.plan_profile(write_profile(tmp_path, **fields))


def test_t8152_fields_are_t8152_only(tmp_path):
    with pytest.raises(ValueError, match="T8152 profile fields"):
        windows.plan_profile(write_profile(tmp_path, target={"chip_id": "0x8142",
                                                             "model": "Mac17,3"}))
    plan = windows.plan_profile(write_profile(tmp_path, guest_ram=None,
                                              target={"chip_id": "0x8142", "model": "Mac17,3"}))
    assert plan.guest_ram is None


def test_older_resident_never_runs_a_t8152_guest():
    with pytest.raises(ValueError, match="T8152 stage-2 policy"):
        windows.validate_capabilities(CAPS_OLD, "bgra-shadow", t8152=True)
    with pytest.raises(ValueError, match="T8152 stage-2 policy"):
        windows.validate_capabilities(CAPS_OLD, ipi="fast")
    with pytest.raises(ValueError, match="T8152 stage-2 policy"):
        windows.validate_capabilities(CAPS_OLD, gic_cpuif="software")
    assert windows.validate_capabilities(CAPS_T8152, "bgra-shadow", t8152=True, ipi="fast") & \
        windows.T8152_POLICY
    # A T8152-policy resident without the 12-live-CPU refusal (6bdac9ee) is refused too.
    with pytest.raises(ValueError, match="12-live-CPU refusal"):
        windows.validate_capabilities(CAPS_T8152_V1, "bgra-shadow", t8152=True)
    # Nor one without the guest-WFE trap and the code placement rule (cd5b10f1).
    with pytest.raises(ValueError, match="guest-WFE trap and code placement"):
        windows.validate_capabilities(CAPS_T8152_V2, "bgra-shadow", t8152=True)
    # Nor one that still writes the implementation-defined ACTLR alias (618d6d40).
    with pytest.raises(ValueError, match="architectural ACTLR_EL12"):
        windows.validate_capabilities(CAPS_T8152_V3, "bgra-shadow", t8152=True)
    # Nor one without the trapped software GIC CPU interface (32555bf9 and older).
    with pytest.raises(ValueError, match="trapped software GIC CPU interface"):
        windows.validate_capabilities(CAPS_T8152_V4, "bgra-shadow", t8152=True)
    with pytest.raises(ValueError, match="trapped software GIC CPU interface"):
        windows.validate_capabilities(CAPS_T8152_V4, gic_cpuif="software")
    # A T8152 launch never takes the hardware CPU interface path, whatever the resident.
    with pytest.raises(ValueError, match="GIC_CPUIF_UNDEF"):
        windows.validate_capabilities(CAPS_T8152, "bgra-shadow", t8152=True, gic_cpuif="carrier")
    assert windows.validate_capabilities(CAPS_T8152, "bgra-shadow", t8152=True,
                                         gic_cpuif="software") & windows.T8152_GIC_TRAPALL
    # Not a T8152 launch: the older resident stays valid.
    windows.validate_capabilities(CAPS_OLD, "bgra-shadow")


DRAM, DRAM_SIZE = 0x10000000000, 4 * GIB
CARVEOUTS = {"region-id-24": (DRAM, DRAM_SIZE),               # the usable RAM itself
             "region-id-353": (0x10006000000, 0x1e000000),     # aura and its heaps
             "region-id-60": (0x10030000000, 0x4000000),
             "region-id-218": (0x100f0000000, 0x10000000)}     # MTE tag storage
MEMORY_MAP = {"BootArgs": (0x10007000000, 0x4000)}


def conflicts(base, top, occupied=()):
    return windows.t8152_window_conflicts(base, top, DRAM, DRAM_SIZE, CARVEOUTS, MEMORY_MAP,
                                          occupied)


def test_window_checks_mirror_the_resident():
    assert conflicts(0x10040000000, 0x100f0000000) == []
    assert any("region-id-218" in p for p in conflicts(0x10040000000, 0x100f0010000))
    assert any("region-id-353" in p for p in conflicts(0x10020000000, 0x10040000000))
    assert any("region-id-60" in p for p in conflicts(0x10028000000, 0x10040000000))
    assert any("boot_args RAM" in p for p in conflicts(0x100c0000000, 0x10110000000))
    assert any("aura" in p for p in conflicts(0x10040000000, 0x10050000000,
                                              [("aura", 0x10048000000, 0x100000)]))
    with pytest.raises(ValueError, match="64 KiB"):
        conflicts(0x10040004000, 0x10050000000)


class Node:
    def __init__(self, name, children=(), compatible=None):
        self.name = name
        self._children = list(children)
        if compatible is not None:
            self.compatible = compatible

    def __iter__(self):
        return iter(self._children)


class FakeADT:
    def __init__(self, root):
        self.root = root

    def __iter__(self):
        return iter(self.root)

    def __getitem__(self, path):
        node = self.root
        for part in path.strip("/").split("/"):
            node = next(c for c in node if c.name == part)
        return node


def test_hidden_nodes_cover_nvram_audio_and_amps():
    adt = FakeADT(Node("", [
        Node("arm-io", [
            Node("qspi", [Node("spinor", [Node("anvram", compatible=["nvram,nor"]),
                                          Node("syscfg")])]),
            Node("i2c1"), Node("i2c3", [Node("audio-speaker")]),
            Node("aop"), Node("aop-gpio"), Node("admac-base-ns"),
            Node("iop-audio-controller"), Node("dp-audio0"),
            Node("uart0"), Node("aic"), Node("sep"),
            Node("other-bus", [Node("nvram-elsewhere", compatible="nvram,nor")]),
        ]),
        Node("chosen"),
    ]))
    hidden = windows.t8152_hidden_nodes(adt)
    for path in ("/arm-io/qspi", "/arm-io/i2c1", "/arm-io/i2c3", "/arm-io/aop",
                 "/arm-io/aop-gpio", "/arm-io/admac-base-ns", "/arm-io/iop-audio-controller",
                 "/arm-io/dp-audio0", "/arm-io/other-bus/nvram-elsewhere"):
        assert path in hidden, path
    for path in ("/arm-io/uart0", "/arm-io/aic", "/arm-io/qspi/spinor/anvram"):
        assert path not in hidden, path  # kept (or already gone with its parent)


def test_system_reset_shuts_nvme_down_first():
    calls = []
    proxy = SimpleNamespace(nvme_shutdown=lambda: calls.append("nvme_shutdown"),
                            reboot=lambda: calls.append("reboot"))
    windows.windows_system_reset(proxy)
    assert calls == ["nvme_shutdown", "reboot"]

    def failing():
        calls.append("nvme_shutdown-failed")
        raise RuntimeError("S_ERROR")
    calls.clear()
    proxy.nvme_shutdown = failing
    with pytest.raises(RuntimeError):
        windows.windows_system_reset(proxy)
    assert calls == ["nvme_shutdown-failed"]  # never reset after a failed shutdown


def test_c_and_python_agree_on_abi():
    proxy_c = (REPO / "src/proxy.c").read_text()
    proxy_h = (REPO / "src/proxy.h").read_text()
    hv_h = (REPO / "src/hv/windows/hv.h").read_text()
    # This branch builds no N1 producer: the resident clears N1_HANDOFF (bit 5).
    assert "0x4157494e0001ffdfULL" in proxy_c
    assert CAPS_T8152 & ~windows.N1_HANDOFF == 0x4157494E0001FFDF
    assert "P_HV_WINDOWS_GUEST_RAM = 0xc8c" in proxy_h
    assert "HV_WINDOWS_T8152_FAST_IPI BIT(5)" in hv_h and windows.CONFIG_T8152_FAST_IPI == 1 << 5
    assert "HV_WINDOWS_T8152_SOFT_GIC BIT(6)" in hv_h and windows.CONFIG_T8152_SOFT_GIC == 1 << 6
    assert "HV_WINDOWS_T8152_WFI BIT(7)" in hv_h and windows.CONFIG_T8152_WFI == 1 << 7
