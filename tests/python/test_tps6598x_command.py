# SPDX-License-Identifier: GPL-2.0-only
import ctypes
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WRITE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint8,
                         ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t)
READ = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint8,
                        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t)
NOW = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p)
DELAY = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint)


class Ops(ctypes.Structure):
    _fields_ = [("write", WRITE), ("read", READ),
                ("now_ms", NOW), ("delay_us", DELAY)]


def command_lib(tmp_path):
    library = tmp_path / "libtps_command.dylib"
    subprocess.run(["cc", "-shared", "-fPIC", "-DTPS6598X_COMMAND_HOST_TEST",
                    str(ROOT / "src/tps6598x_command_core.c"), "-o", str(library)],
                   check=True)
    lib = ctypes.CDLL(str(library))
    lib.tps6598x_command_execute.argtypes = [ctypes.POINTER(Ops), ctypes.c_void_p,
                                              ctypes.c_char_p, ctypes.c_void_p,
                                              ctypes.c_size_t, ctypes.c_void_p,
                                              ctypes.c_size_t]
    lib.tps6598x_command_execute.restype = ctypes.c_int
    return lib


def test_stuck_register_selection_uses_whole_command_deadline(tmp_path):
    lib = command_lib(tmp_path)
    clock = [0]
    reads = []

    def write(ctx, reg, data, length):
        if reg == 0x08:
            clock[0] += 1600  # selection never completes
        return length

    def read(ctx, reg, data, length):
        reads.append(reg)
        return -1

    ops = Ops(WRITE(write), READ(read), NOW(lambda ctx: clock[0]),
              DELAY(lambda ctx, usec: None))
    assert lib.tps6598x_command_execute(ctypes.byref(ops), None, b"TEST",
                                         None, 0, None, 0) == -2
    assert reads == []


def test_perpetually_busy_command_times_out_without_10000_polls(tmp_path):
    lib = command_lib(tmp_path)
    clock = [0]
    polls = []

    def read(ctx, reg, data, length):
        polls.append(reg)
        clock[0] += 100
        ctypes.memmove(data, b"BUSY", 4)
        return 4

    ops = Ops(WRITE(lambda ctx, reg, data, length: length), READ(read),
              NOW(lambda ctx: clock[0]), DELAY(lambda ctx, usec: None))
    assert lib.tps6598x_command_execute(ctypes.byref(ops), None, b"TEST",
                                         None, 0, None, 0) == -2
    assert 1 <= len(polls) <= 15


def test_status_and_power_reads_require_complete_values(tmp_path):
    source = (ROOT / "src/tps6598x.c").read_text()
    status = source[source.index("int tps6598x_cmd_status("):
                    source.index("int tps6598x_disable_irqs(")]
    power_start = source.index("int tps6598x_powerup(")
    power = source[power_start:source.index("\n}", power_start) + 2]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int tps6598x_dev_t;
#define TPS_REG_CMD1 8
#define TPS_CMD_INVALID 0x444d4321
#define TPS_REG_POWER_STATE 0x20
static int count;
static int tps6598x_read_reg(tps6598x_dev_t *d, u8 reg, u8 *data, size_t len)
{
    memset(data, 0, len);
    return count;
}
static int tps6598x_command(tps6598x_dev_t *d, const char *cmd, const u8 *in,
                            size_t ilen, u8 *out, size_t olen) { return 0; }
''' + status + power + r'''
int main(void)
{
    for (count = -1; count <= 4; count++)
        assert((tps6598x_cmd_status(NULL, "LOCK") == 0) == (count == 4));
    for (count = -1; count <= 1; count++)
        assert((tps6598x_powerup(NULL) == 0) == (count == 1));
    return 0;
}
'''
    binary = tmp_path / "tps-status"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
