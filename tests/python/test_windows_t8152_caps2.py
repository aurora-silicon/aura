# SPDX-License-Identifier: GPL-2.0-only
"""P_HV_WINDOWS_CAPS2: one meaning per bit, the same on both sides.

gpu3 (G2) and D5 (NVMe disk) both claimed bit 5 and a plain merge kept both constants without a
conflict marker: a merged launcher would have read "NVMe disk" as "G2" and the other way round.
These checks parse every HV_WINDOWS_CAPS2_* define in the carrier and every CAPS2_* constant in
proxyclient/aura/windows.py and fail on any bit claimed twice, a name that exists on one side
only, a value that differs, or a resident that does not advertise exactly the merged set.
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
WIN = REPO / "src/hv/windows"

# What the G5b carrier advertised (G5B-BUILD 2.3.1); every session gate pins it exactly.
G5B_CAPS2 = 0x415749320000007F
# G6-S adds bit 7 (the scanout service): a G6 session gate pins this value instead.
G6S_CAPS2 = G5B_CAPS2 | 1 << 7
# On j813-windows the T8152 carrier rules moved from CAPS bits 11-15 to CAPS2 bits 8-12.
J813_CAPS2 = G6S_CAPS2 | 0x1F00


def _c_bits():
    bits = {}
    for h in sorted(WIN.glob("*.h")):
        for name, bit in re.findall(r"#define HV_WINDOWS_CAPS2_(\w+)\s+BIT\((\d+)\)",
                                    h.read_text()):
            assert name not in bits, f"HV_WINDOWS_CAPS2_{name} defined twice"
            bits[name] = (int(bit), h.name)
    return bits


def _py_bits():
    src = (REPO / "proxyclient/aura/windows.py").read_text()
    out = {}
    for name, value in re.findall(r"^CAPS2_(\w+) = (0x[0-9a-fA-F]+)", src, re.M):
        if name == "MAGIC":
            continue
        v = int(value, 16)
        assert v and v & (v - 1) == 0, f"CAPS2_{name} = {value} is not one bit"
        assert name not in out, f"CAPS2_{name} assigned twice"
        out[name] = v.bit_length() - 1
    return out


def test_windows_caps2_bits_unique():
    c = _c_bits()
    seen = {}
    for name, (bit, where) in c.items():
        assert bit not in seen, (f"CAPS2 bit {bit}: HV_WINDOWS_CAPS2_{name} ({where}) and "
                                 f"HV_WINDOWS_CAPS2_{seen[bit]} both claim it")
        seen[bit] = name
    py = _py_bits()
    seen = {}
    for name, bit in py.items():
        assert bit not in seen, f"CAPS2 bit {bit}: CAPS2_{name} and CAPS2_{seen[bit]} in windows.py"
        seen[bit] = name
    assert set(py) == set(c), f"C only {set(c) - set(py)}, Python only {set(py) - set(c)}"
    for name, bit in py.items():
        assert c[name][0] == bit, f"CAPS2_{name}: C bit {c[name][0]}, Python bit {bit}"


def test_windows_caps2_resident_value():
    proxy = (REPO / "src/proxy.c").read_text()
    body = proxy.split("case P_HV_WINDOWS_CAPS2:", 1)[1].split("#else", 1)[0]
    expr = body.split("reply->retval =", 1)[1].split(";", 1)[0]
    names = re.findall(r"HV_WINDOWS_CAPS2_(\w+)", expr)
    assert names[0] == "MAGIC" and len(names) == len(set(names))
    c = _c_bits()
    assert set(names[1:]) == set(c), "the resident must advertise every CAPS2 feature it defines"
    value = 0x4157493200000000
    for n in names[1:]:
        value |= 1 << c[n][0]
    assert value == J813_CAPS2, hex(value)
    from aura import windows as w
    assert w.CAPS2_MAGIC | sum(1 << b for b in _py_bits().values()) == J813_CAPS2
    assert w.CAPS2_AHCI_NVME == 1 << 5 and w.CAPS2_GPU_G2 == 1 << 6
    assert w.CAPS2_SCANOUT == 1 << 7
