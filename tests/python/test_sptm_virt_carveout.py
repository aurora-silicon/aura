# SPDX-License-Identifier: MIT
"""The virtualized-SPTM carve-out math leaves SPTM's managed RAM intact.

Compiles the pure sptm_virt_compute_layout() out of src/sptm_virt.c in a host
harness and checks it against a J700's live numbers and the constraints it must
never violate.
"""

import os
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|u64|void) " + name + r"\([^;]*?\)\s*\{",
                      source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_sptm_virt_carveout(tmp_path):
    source = (REPO / "src/sptm_virt.c").read_text()
    body = function(source, "sptm_virt_compute_layout")
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define SPTM_VIRT_ALIGN  (2UL << 20)
struct sptm_virt_layout {
    u64 ram_base, ram_top, managed_base, managed_end;
    u64 aura_base, aura_top, fw_high, aura_size;
};
''' + body + r'''
int main(void)
{
    struct sptm_virt_layout l;
    const u64 RAM = 0x10000000000UL, TOP = 0x10200000000UL;
    const u64 PHYS = 0x1000342c000UL, MEM = 0x1e3a04000UL;
    const u64 SAPT = 0x101eb494000UL;      /* earliest high-firmware PA */
    const u64 HEAP = 128UL << 20;

    /* Neo live layout: the natural iBoot window end (below SAPT) binds. */
    assert(sptm_virt_compute_layout(PHYS, MEM, RAM, TOP, SAPT, HEAP, &l) == 0);
    assert(l.managed_base == PHYS);
    assert(l.aura_top == 0x101e6e00000UL);         /* ALIGN_DOWN(PHYS+MEM, 2MiB) */
    assert(l.aura_size == HEAP);
    assert(l.aura_base == l.aura_top - HEAP);       /* 0x101e6600000 */
    assert(l.managed_end == l.aura_base);           /* SPTM window ends here */
    assert(l.aura_base > PHYS);                     /* managed RAM still present */
    /* aura sits strictly inside RAM and below the high-firmware floor. */
    assert(l.aura_top <= SAPT && l.aura_base >= PHYS && l.aura_top <= TOP);
    assert(l.aura_size % SPTM_VIRT_ALIGN == 0);

    /* When the fixed high-firmware floor is lower than the window end, it binds
     * instead, so aura never overlaps SAPT / the GPU handoff. 0x101a0000000 is
     * below PHYS+0x1f0000000 (the natural window end for this case). */
    assert(sptm_virt_compute_layout(PHYS, 0x1f0000000UL, RAM, TOP, 0x101a0000000UL, HEAP, &l) == 0);
    assert(l.aura_top == ALIGN_DOWN(0x101a0000000UL, SPTM_VIRT_ALIGN));
    assert(l.aura_top <= 0x101a0000000UL);

    /* Reject layouts that cannot leave managed RAM, rather than guessing. */
    assert(sptm_virt_compute_layout(PHYS, MEM, RAM, TOP, SAPT, 0x40000000000UL, &l) < 0);
    assert(sptm_virt_compute_layout(PHYS, MEM, RAM, TOP, PHYS - 0x1000, HEAP, &l) < 0);
    assert(sptm_virt_compute_layout(RAM - 1, MEM, RAM, TOP, SAPT, HEAP, &l) < 0);
    assert(sptm_virt_compute_layout(PHYS, 0, RAM, TOP, SAPT, HEAP, &l) < 0);
    assert(sptm_virt_compute_layout(PHYS, MEM, RAM, RAM, SAPT, HEAP, &l) < 0);

    printf("ok\n");
    return 0;
}
'''
    binary = tmp_path / "sptm_virt_carveout"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
