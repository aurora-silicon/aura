# SPDX-License-Identifier: MIT
"""T8122's exact AMCC guest shadow is isolated from the host register bank."""

import re
import subprocess
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^static bool " + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_t8122_amcc_shadow(tmp_path):
    source = (REPO / "src/sptm_virt_s2.c").read_text()
    funcs = "\n".join(function(source, name) for name in
                       ("amcc_shadow_rw", "t8122_amcc_is_shadow"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
struct t8122_amcc_layout {
    u32 lower, upper, enable, lock, write_disable;
    u32 cache_status, cache_status_mask, cache_status_value;
};
static struct t8122_amcc_layout t8122_amcc = {
    .lower = 0x690, .upper = 0x694, .enable = 0x698,
    .lock = 0x69c, .write_disable = 0x6a0,
    .cache_status = 0x1c00, .cache_status_mask = 1,
};
static u64 amcc_lock_shadow[32][2];
''' + funcs + r'''
int main(void)
{
    const u64 bases[] = {0x220000000ULL, 0x220040000ULL,
                         0x222000000ULL, 0x222040000ULL};
    const u32 regs[] = {0x690, 0x694, 0x698, 0x69c, 0x6a0};
    for (unsigned b = 0; b < ARRAY_SIZE(bases); b++) {
        for (unsigned r = 0; r < ARRAY_SIZE(regs); r++) {
            u64 pa = bases[b] + regs[r], value = 0xfeed0000 + b * 16 + r;
            assert(t8122_amcc_is_shadow(pa));
            assert(amcc_shadow_rw(pa, &value, true));
            value = ~0ULL;
            assert(amcc_shadow_rw(pa, &value, false));
            assert(value == 0xfeed0000 + b * 16 + r);
        }
    }
    u64 fresh = ~0ULL;
    assert(amcc_shadow_rw(0x2220406a4ULL, &fresh, false));
    assert(fresh == 0);
    assert(!t8122_amcc_is_shadow(0x220001c00ULL));
    assert(!t8122_amcc_is_shadow(0x2200006a4ULL));
    return 0;
}
'''
    binary = tmp_path / "t8122-amcc-shadow"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                    "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_t8122_amcc_exact_adt_gate_is_complete():
    source = (REPO / "src/sptm_virt_s2.c").read_text()
    for token in ("0x220000000UL", "0x222000000UL", '"aperture-count", 2',
                  '"aperture-size", 0x200000', '"plane-count", 2',
                  '"plane-stride", 0x40000', '"lower-limit-reg-offset", 0x690',
                  '"upper-limit-reg-offset", 0x694', '"enable-reg-offset", 0x698',
                  '"lock-reg-offset", 0x69c', '"write-disable-reg-offset", 0x6a0',
                  '"cache-status-reg-offset", 0x1c00', '"page-size-shift", 14'):
        assert token in source
