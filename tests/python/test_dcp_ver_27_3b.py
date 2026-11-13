# SPDX-License-Identifier: MIT
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proxyclient"))

from aura.constructutils import Ver
from aura.proxyutils import VERSION_MAP


@pytest.fixture
def v27_3b():
    saved = dict(Ver._version)
    Ver.set_version_key("V", VERSION_MAP["mBoot-20457.60.65.0.1"])
    yield
    Ver._version.update(saved)


def test_m5_iboot_string_selects_a_version_the_matrix_knows():
    assert VERSION_MAP["mBoot-20457.60.65.0.1"] == "V27_3B"
    assert "V27_3B" in Ver.MATRIX["V"]


def test_v27_3b_orders_after_v14_7(v27_3b):
    # Importing aura.fw.dcp.ipc evaluates Ver("V >= V13_5", ...) at module level;
    # an unknown version raised ValueError there (hv trace_dcp.py on the M5).
    assert Ver.check("V >= V13_5")
    assert Ver.check("V >= V14_7")
    assert not Ver.check("V < V14_7")


def test_dcp_ipc_imports_with_the_m5_version():
    import subprocess

    code = (
        "from aura.constructutils import Ver\n"
        "from aura.proxyutils import VERSION_MAP\n"
        "Ver.set_version_key('V', VERSION_MAP['mBoot-20457.60.65.0.1'])\n"
        "import aura.fw.dcp.ipc\n"
    )
    root = Path(__file__).resolve().parents[2] / "proxyclient"
    subprocess.run([sys.executable, "-c", code], cwd=root, check=True)


class _FakeNode:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class _FakeADT:
    def __init__(self, nodes):
        self.nodes = nodes

    def __getitem__(self, path):
        return self.nodes[path]


class _FakeProxyUtils:
    """What Ver.set_version reads on the M5: firmware version and ADT /arm-io soc-generation H17."""

    version = "V27_3B"
    adt = _FakeADT({"/arm-io": _FakeNode(soc_generation="H17"), "/chosen": _FakeNode(chip_id=0x6050)})


def test_set_version_on_the_m5_accepts_soc_generation_h17():
    saved = dict(Ver._version)
    try:
        Ver.set_version(_FakeProxyUtils())
        assert Ver._version == {"V": "V27_3B", "G": "G17"}
        assert Ver.check("G >= G15X")
        assert not Ver.check("G < G15X")
    finally:
        Ver._version.update(saved)
