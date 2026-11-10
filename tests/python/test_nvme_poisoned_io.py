# SPDX-License-Identifier: MIT
"""A refused handoff must not let proxy I/O reuse retained DMA or caller buffers."""

import os
from pathlib import Path
import subprocess


def test_poisoned_session_refuses_io_before_buffer_changes(tmp_path):
    root = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get("NVME_SOURCE", root / "src/nvme.c")).read_text()
    declarations = source[source.index("#define NVME_TIMEOUT"):source.index("static u64 nvme_read64_lo_hi")]
    # Clang can diagnose the write-path flag when shutdown is outside this host fragment.
    declarations = declarations.replace("static bool nvme_wrote;",
                                        "static bool nvme_wrote __attribute__((unused));")
    execute = source[source.index("static bool nvme_exec_command("):
                     source.index("/* The ADT describes a post-M4 ANS")]
    # The ANS power helpers between these fragments need physical firmware state.
    execute += source[source.index("/* A write or flush, timed:"):
                      source.index("bool nvme_init(void)")]
    read = source[source.index("bool nvme_flush("):]
    read_limit = next(line for line in (root / "src/nvme.h").read_text().splitlines()
                      if line.startswith("#define NVME_MAX_READ_BLOCKS "))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int asc_dev_t; typedef int rtkit_dev_t; typedef int sart_dev_t;
struct sart_entry { u8 index, flags; u64 paddr, size; };
#define static_assert _Static_assert
#define BIT(n) (1ULL << (n))
#define GENMASK(h,l) (((~0ULL) >> (63-(h))) & ((~0ULL) << (l)))
#define FIELD_GET(mask,x) (((x)&(mask)) >> __builtin_ctzll(mask))
#define SZ_4K 4096
#define SZ_16K 16384
''' + declarations + read_limit + r'''
static void nvme_poll_syslog(void) { assert(!"unexpected firmware poll"); }
static u64 timeout_calculate(unsigned time) { assert(!"unexpected timeout"); return 0; }
static bool timeout_expired(u64 time) { assert(!"unexpected timeout poll"); return true; }
static void write32(u64 reg, u32 val) { assert(!"unexpected MMIO write"); }
static u32 read32(u64 reg) { assert(!"unexpected MMIO read"); return 0; }
static void dma_wmb(void) { assert(!"unexpected queue publication"); }
static void dma_rmb(void) { assert(!"unexpected queue access"); }
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_usecs(u64 ticks) { return ticks; }
static void *memalign(size_t align, size_t size) { assert(!"unexpected DMA allocation"); return NULL; }
''' + execute + read + r'''
int main(void)
{
    _Alignas(SZ_4K) u8 buffer[3 * SZ_4K];
    struct nvme_command slot = {}, cmd = {};
    struct apple_nvmmu_tcb tcb = {};
    struct nvme_completion cqe = {};
    nvme_initialized = nvme_reboot_required = true;
    ioq.cmds = &slot; ioq.tcbs = &tcb; ioq.cqes = &cqe;
    ioq.failed = false;
    memset(buffer, 0x3c, sizeof(buffer));
    u64 result = 0x12345678;
    assert(!nvme_exec_command(&ioq, &cmd, &result));
    assert(result == 0x12345678);
    assert(!nvme_flush(1));
    assert(!nvme_read_blocks(1, 0, buffer, 3));
    assert(!nvme_read(1, 0, buffer));
    for (size_t i = 0; i < sizeof(buffer); i++)
        assert(buffer[i] == 0x3c);
    assert(!nvme_prp_list && !nvme_verify_bounce);
    return 0;
}
'''
    binary = tmp_path / "nvme-poisoned-io"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-format",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
