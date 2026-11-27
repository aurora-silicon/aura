# SPDX-License-Identifier: MIT
"""SPTM_VIRT builds program SPRR on every core that reports GXF, not only features_m4."""

from test_locked_dart_tables import function
from test_smp_timeouts import REPO, compile_and_run


def _run_sprr_case(tmp_path, name, prelude):
    utils = (REPO / "src/utils.c").read_text()
    mmu_uses_sprr = function(utils, "mmu_uses_sprr")
    supports_gxf = function(utils, "supports_gxf")
    harness = prelude + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t u64;
#define AIDR_EL1_GXF (1ULL << 16)

struct midr_part_features {
    bool mmu_sprr;
};

static u64 fake_aidr;
static struct midr_part_features fake_features;
static const struct midr_part_features *cpu_features = &fake_features;

#define mrs(reg) (fake_aidr)
''' + mmu_uses_sprr + supports_gxf + r'''
int main(void)
{
    /* No GXF in AIDR_EL1: only cpu_features->mmu_sprr enables SPRR, never GXF. */
    fake_aidr = 0;
    fake_features.mmu_sprr = false;
    assert(!mmu_uses_sprr());
    assert(!supports_gxf());

    fake_features.mmu_sprr = true;
    assert(mmu_uses_sprr());
    assert(!supports_gxf());

    /* Core reports GXF in AIDR_EL1, cpu_features->mmu_sprr = false (M4 Pro/Max, M5 Pro/Max/Ultra): */
    fake_aidr = AIDR_EL1_GXF;
    fake_features.mmu_sprr = false;
#ifdef SPTM_VIRT
    assert(mmu_uses_sprr());
    assert(supports_gxf());
#else
    assert(!mmu_uses_sprr());
    assert(!supports_gxf());
#endif

    /* Core reports GXF and cpu_features->mmu_sprr = true (M1-M3): always on. */
    fake_features.mmu_sprr = true;
    assert(mmu_uses_sprr());
    assert(supports_gxf());
    return 0;
}
'''
    compile_and_run(tmp_path, name, harness)


def test_mmu_uses_sprr_follows_gxf_under_sptm_virt(tmp_path):
    _run_sprr_case(tmp_path, "sprr_plain", "")
    _run_sprr_case(tmp_path, "sprr_sptm_virt", "#define SPTM_VIRT 1\n")
