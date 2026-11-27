# SPDX-License-Identifier: GPL-2.0-only

import re
import subprocess
from pathlib import Path

from proxyclient.aura.proxy import CPUFeatures

REPO = Path(__file__).resolve().parents[4]


def test_cpu_features_wfi_flag_fits_existing_abi():
    assert CPUFeatures.sizeof() == 20
    features = CPUFeatures.parse(bytes(19) + b"\x01")
    assert features.unsafe_wfi is True
    assert features.apple_sysregs_unlocked is False


def test_cpu_features_match_the_c_struct(tmp_path):
    header = (REPO / "src/utils.h").read_text()
    blocks = [
        re.search(r"enum cpufeat_sleep_mode \{.*?\};", header, re.S).group(0),
        re.search(r"enum cpufeat_uncore_version \{.*?\};", header, re.S).group(0),
        re.search(r"struct midr_part_features \{.*?\n\};", header, re.S).group(0),
    ]
    fields = re.findall(r"^\s+(?:enum \w+|bool|u8) (\w+);", blocks[2], re.M)
    prints = "".join(
        f'    printf("{name} %zu\\n", offsetof(struct midr_part_features, {name}));\n'
        for name in fields
    )
    source = (
        "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n"
        + "typedef uint8_t u8;\n"
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
    layout = {name: int(value) for name, value in (line.split() for line in out.splitlines())}
    assert CPUFeatures.sizeof() == layout.pop("size")

    offset, python = 0, {}
    for sub in CPUFeatures.subcons:
        python[sub.name] = offset
        offset += sub.sizeof()
    assert python == layout
