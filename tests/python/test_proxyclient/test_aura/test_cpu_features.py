# SPDX-License-Identifier: GPL-2.0-only

import re
import subprocess
from pathlib import Path

from proxyclient.aura.proxy import CPUFeatures

REPO = Path(__file__).resolve().parents[4]


def _c_layout(tmp_path):
    header = (REPO / "src/utils.h").read_text()
    blocks = [
        re.search(r"enum cpufeat_sleep_mode \{.*?\};", header, re.S).group(0),
        re.search(r"enum cpufeat_uncore_version \{.*?\};", header, re.S).group(0),
        re.search(r"struct midr_part_features \{.*?\n\};", header, re.S).group(0),
    ]
    struct = blocks[2]
    fields = re.findall(r"^\s+(?:enum \w+|bool) (\w+);", struct, re.M)
    prints = "".join(
        f'    printf("{name} %zu\\n", offsetof(struct midr_part_features, {name}));\n'
        for name in fields
    )
    source = (
        "#include <stdbool.h>\n#include <stddef.h>\n#include <stdio.h>\n"
        + "\n".join(blocks)
        + "\nint main(void)\n{\n"
        + '    printf("size %zu\\n", sizeof(struct midr_part_features));\n'
        + prints
        + "    return 0;\n}\n"
    )
    binary = tmp_path / "cpu-features-layout"
    subprocess.run(["cc", "-std=c11", "-x", "c", "-", "-o", str(binary)],
                   input=source, text=True, check=True)
    out = subprocess.run([str(binary)], text=True, check=True, capture_output=True).stdout
    layout = dict(line.split() for line in out.splitlines())
    return {name: int(value) for name, value in layout.items()}


def test_cpu_features_match_the_c_struct(tmp_path):
    layout = _c_layout(tmp_path)
    assert CPUFeatures.sizeof() == layout.pop("size")

    offset = 0
    python = {}
    for sub in CPUFeatures.subcons:
        if sub.name:
            python[sub.name] = offset
        offset += sub.sizeof()
    assert python == layout


def test_cpu_features_keep_the_legacy_flag_in_place():
    raw = bytearray(CPUFeatures.sizeof())
    raw[10] = 1
    features = CPUFeatures.parse(bytes(raw))
    assert features.legacy_impl_regs is True
    assert features.apple_sysregs_unlocked is False
    assert features.cyc_ovrd is False
