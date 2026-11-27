# SPDX-License-Identifier: MIT
"""Both DWC3 engines decode the whole 4-bit command status field.

The production command helpers from usb_dwc3_impl.inc run against modelled
registers, once as the HighSpeed and once as the SuperSpeed engine.
"""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


@pytest.mark.parametrize("superspeed", [0, 1])
def test_command_status_and_timeout(tmp_path, superspeed):
    source = (ROOT / "src/usb_dwc3_impl.inc").read_text()
    regs = (ROOT / "src/usb_dwc3_regs.h").read_text().splitlines()
    # The register header's macros, as both engines see them.
    macros = "\n".join(line for line in regs if line.startswith("#")
                       and not line.startswith(("#include", "#ifndef __", "#define __", "#endif")))
    harness = f"#define USB_DWC3_SUPERSPEED {superspeed}\n" + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
''' + macros + r'''
#define usb_debug_printf(...) ((void)0)
typedef struct { uintptr_t regs; } dwc3_dev_t;
static u64 addresses[4];
static u32 values[4], raw;
static unsigned writes, reads, polls;
static bool timeout;
static void write32(u64 address, u32 value)
{
    assert(writes < 4);
    addresses[writes] = address;
    values[writes++] = value;
}
static int poll32(u64 address, u32 mask, u32 value, unsigned usec)
{
    assert(address == addresses[writes - 1]);
    assert(mask == BIT(10) && value == 0 && usec == 1000);
    polls++;
    return timeout ? -1 : 0;
}
static u32 read32(u64 address)
{
    assert(address == addresses[writes - 1]);
    reads++;
    return raw;
}
'''
    harness += function(source, "static int usb_dwc3_command(")
    harness += function(source, "static int usb_dwc3_ep_command(")
    harness += r'''
int main(void)
{
    dwc3_dev_t dev = {.regs = 0x100000};
    for (unsigned status = 0; status < 16; status++) {
        raw = (0x55 << 16) | (status << 12);
        writes = reads = polls = 0;
        assert(usb_dwc3_command(&dev, 6, 0x1234) == (int)status);
        assert(writes == 2 && reads == 1 && polls == 1);
        assert(addresses[0] == dev.regs + DWC3_DGCMDPAR && values[0] == 0x1234);
        assert(values[1] == (6 | DWC3_DGCMD_CMDACT));
        writes = reads = polls = 0;
        assert(usb_dwc3_ep_command(&dev, 3, 6, 11, 22, 33) == (int)status);
        assert(writes == 4 && reads == 1 && polls == 1);
        assert(addresses[0] == dev.regs + DWC3_DEPCMDPAR0(3) && values[0] == 11);
        assert(addresses[1] == dev.regs + DWC3_DEPCMDPAR1(3) && values[1] == 22);
        assert(addresses[2] == dev.regs + DWC3_DEPCMDPAR2(3) && values[2] == 33);
        assert(values[3] == (6 | DWC3_DEPCMD_CMDACT));
    }
    timeout = true;
    writes = reads = polls = 0;
    assert(usb_dwc3_command(&dev, 6, 0) == -1);
    assert(writes == 2 && polls == 1 && !reads);
    writes = reads = polls = 0;
    assert(usb_dwc3_ep_command(&dev, 3, 6, 0, 0, 0) == -1);
    assert(writes == 4 && polls == 1 && !reads);
    return 0;
}
'''
    binary = tmp_path / "usb-command-status"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
         "-I", str(ROOT / "src"), "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
