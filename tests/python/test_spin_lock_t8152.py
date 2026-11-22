# SPDX-License-Identifier: GPL-2.0-only
"""Host run of aura's production spin_lock()/spin_unlock() (src/utils.c), B6.

On the T8152 Windows carrier spin_lock() must wait without WFE: a WFE that really sleeps
is unmeasured there (Phase 3 Part A's WFE rows used SEVL and never slept), and the
hypervisor lock is taken while a guest's SIMD/FP registers are live in hardware.

Failure modes checked here, natively on an arm64 host (the lock is AArch64 assembly):
- the WFE-free branch loses mutual exclusion (counter short under contention);
- it breaks recursion on the owning CPU, or leaves the lock held;
- on T8152 a waiter still reaches WFE (the compiled loop is inspected: the branch that
  skips the WFE must sit right before it), or other chips stop using WFE.
Synthetic CPU ids; nothing here touches a real machine.
"""

import platform
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

PRELUDE = r"""
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int64_t s64;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct {
    s64 lock;
    int count;
} spinlock_t;
#define AURA_WINDOWS
#define T8152 0x8152
#define T8142 0x8142
u32 chip_id;
static __thread int cpu;
static int smp_id(void) { return cpu; }
"""

HARNESS = r"""
#define THREADS 4
#define ROUNDS  200000
static spinlock_t lock = {-1, 0};
static volatile long counter;
static void *worker(void *arg)
{
    cpu = (int)(intptr_t)arg;
    for (int i = 0; i < ROUNDS; i++) {
        spin_lock(&lock);
        spin_lock(&lock); /* recursive on the owner */
        long c = counter;
        counter = c + 1;
        spin_unlock(&lock);
        spin_unlock(&lock);
    }
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    chip_id = (u32)strtoul(argv[1], NULL, 0);
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        assert(!pthread_create(&t[i], NULL, worker, (void *)(intptr_t)(i + 1)));
    for (int i = 0; i < THREADS; i++)
        assert(!pthread_join(t[i], NULL));
    assert(counter == (long)THREADS * ROUNDS);
    assert(lock.lock == -1 && lock.count == 0);
    printf("SPIN OK %ld\n", counter);
    return 0;
}
"""


def production(source, name):
    match = re.search(r"(?m)^void " + re.escape(name) + r"\(spinlock_t \*lock\)\n\{", source)
    assert match, name
    end = source.index("\n}\n", match.end()) + 3
    return source[match.start():end]


@pytest.mark.skipif(platform.machine() not in ("arm64", "aarch64"), reason="AArch64 host only")
def test_t8152_carrier_spin_lock_waits_without_wfe(tmp_path):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler")
    utils = (REPO / "src/utils.c").read_text()
    source = tmp_path / "spin.c"
    source.write_text(PRELUDE + "#include <stdlib.h>\n" + production(utils, "spin_lock")
                      + production(utils, "spin_unlock") + HARNESS)
    exe = tmp_path / "spin"
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(source), "-o", str(exe)], check=True)
    for chip in ("0x8152", "0x8142"):
        out = subprocess.run([str(exe), chip], check=True, capture_output=True, text=True,
                             timeout=120).stdout
        assert "SPIN OK 800000" in out, (chip, out)

    # Only spin_lock in this object: its one WFE must sit right after the T8152 skip branch.
    lock_only = tmp_path / "lock.c"
    lock_only.write_text(PRELUDE + production(utils, "spin_lock"))
    obj = tmp_path / "lock.o"
    subprocess.run([cc, "-std=gnu11", "-O2", "-c", str(lock_only), "-o", str(obj)], check=True)
    objdump = shutil.which("llvm-objdump") or shutil.which("objdump")
    if not objdump:
        pytest.skip("no objdump")
    dis = subprocess.run([objdump, "-d", str(obj)], check=True, capture_output=True,
                         text=True).stdout
    ops = [line.split("\t")[1].strip() for line in dis.splitlines()
           if re.match(r"\s+[0-9a-f]+:\s", line) and line.count("\t") >= 1]
    wfe = [i for i, op in enumerate(ops) if op == "wfe"]
    assert len(wfe) == 1, ops  # one WFE, used only when the chip is not T8152
    assert ops[wfe[0] - 1] == "cbnz", ops  # ...and skipped right before it on T8152
