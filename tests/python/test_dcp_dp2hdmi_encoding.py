# SPDX-License-Identifier: GPL-2.0-only
"""The dp2hdmi/hdmi power path follows the ADT function encoding, not the chip."""

from pathlib import Path
import subprocess

SOURCE = Path(__file__).resolve().parents[2] / "src/dcp.c"

# function-dp2hdmi_pwr_en / function-hdmi_pwr_en as the public ADT dumps print them.
ADT = {
    "t8132-j773": (("pKW4", "gP1a", 0x0), ("pKW4", "gP0f", 0x800000)),
    "t8132-j604": (("pKW4", "gP1a", 0x0), ("pKW4", "gP0f", 0x800000)),
    "t6040-j773s": (("pKW4", "gP1a", 0x0), ("pKW4", "gP0f", 0x800000)),
    "t6041-j616c": (("pKW4", "gP1a", 0x0), ("pKW4", "gP0f", 0x800000)),
    "t8152-j873": (("pKW8", "pcIO", 0x0, 0x140000), ("pKW8", "pcIO", 0x800000, 0xf0000)),
    "t6050-j873s": (("pKW8", "pcIO", 0x0, 0x1a0000), ("pKW8", "pcIO", 0x800000, 0xf0000)),
}


def blob(fn):
    name, key, *args = fn
    # phandle, byte-reversed function name, key as a u32, then the arguments
    words = [0x1234, int.from_bytes(name[::-1].encode(), "little"),
             int.from_bytes(key.encode(), "big"), *args]
    return b"".join(w.to_bytes(4, "little") for w in words)


def test_no_chip_gate():
    source = SOURCE.read_text()
    body = source[source.index("static int dcp_hdmi_dptx_init("):]
    body = body[:body.index("\n}\n")]
    assert "chip_id" not in body


def test_pw8_selected_by_encoding(tmp_path):
    source = SOURCE.read_text()
    structs = source[source.index("struct adt_function_smc_gpio {"):
                     source.index("/* T8152 PMGR2 PSD service blocks")]
    cases = []
    for board, (dp2hdmi, hdmi) in ADT.items():
        for prop, fn in (("dp2hdmi", dp2hdmi), ("hdmi", hdmi)):
            data = ", ".join(f"0x{b:02x}" for b in blob(fn))
            want = 1 if fn[0] == "pKW8" else 0
            cases.append(f'    {{"{board} {prop}", {{{data}}}, {len(blob(fn))}, {want}}},')
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
static void *adt;
static const unsigned char *cur;
static u32 cur_len;
static const void *adt_getprop(void *dt, int node, const char *name, u32 *len)
{
    (void)dt; (void)node; (void)name;
    *len = cur_len;
    return cur;
}
''' + structs + r'''
static const struct { const char *name; unsigned char data[20]; u32 len; int pw8; } cases[] = {
''' + "\n".join(cases) + r'''
};
int main(void)
{
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cur = cases[i].data;
        cur_len = cases[i].len;
        const struct adt_function_smc_pw8 *fn = dcp_adt_pw8(0, "x");
        if (!!fn != cases[i].pw8) {
            fprintf(stderr, "%s: pw8=%d\n", cases[i].name, !!fn);
            return 1;
        }
        if (fn)
            assert(fn->key == 0x7063494f); /* 'pcIO' */
    }
    /* A pKW8 name with a short body, or a 20-byte non-pKW8 function, is not pw8. */
    static const unsigned char short_pw8[16] = {0, 0, 0, 0, '8', 'W', 'K', 'p'};
    cur = short_pw8; cur_len = sizeof(short_pw8);
    assert(!dcp_adt_pw8(0, "x"));
    static const unsigned char wide_pw4[20] = {0, 0, 0, 0, '4', 'W', 'K', 'p'};
    cur = wide_pw4; cur_len = sizeof(wide_pw4);
    assert(!dcp_adt_pw8(0, "x"));
    return 0;
}
'''
    binary = tmp_path / "dcp-dp2hdmi"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
