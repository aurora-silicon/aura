# SPDX-License-Identifier: MIT
"""Run the production no-sleep setup helper, including real regression controls."""
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def helper():
    source = (ROOT / "src/sptm_virt_s2r.c").read_text()
    start = source.index("static void s2r_no_sleep_setup(")
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    assert "s2r_no_sleep_setup(cdc, flags, missing);" in source
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SPTM_VIRT_S2R 1UL
#define SPTM_VIRT_S2R_GO 2UL
#define SPTM_VIRT_S2R_FULL 4UL
#define SPTM_VIRT_S2R_COPROC 8UL
#define SPTM_VIRT_S2R_DROP_USB 16UL
#define SPTM_VIRT_S2R_MEM 256UL
#define S2R_RING_BYTES 4096
struct s2r_soc { int valid; };
static struct s2r_soc s2r_soc_adt;
static const struct s2r_soc *s2r_soc;
static void *adt;
static unsigned sptm_virt_link_phys[2];
static u64 sptm_virt_flags, sptm_virt_s2r_nrec;
static void *sptm_virt_s2r_ring;
static bool s2r_keep_always;
static int link_ok, ring_ok, kept, rings, lookups;
static int s2r_soc_link_only(void *a, const unsigned *p, int n, struct s2r_soc *out)
{ (void)a;(void)p;(void)n; ++lookups; out->valid=link_ok; return link_ok ? 0 : -1; }
static void s2r_keep_setup(void)
{ assert(s2r_soc == &s2r_soc_adt && s2r_soc->valid && s2r_keep_always); ++kept; }
static bool s2r_ring_alloc(u64 bytes)
{ assert(bytes==S2R_RING_BYTES); ++rings; sptm_virt_s2r_nrec=128; return ring_ok; }
'''
SUFFIX = r'''
int main(void)
{
 for(int cdc=0;cdc<2;++cdc) for(link_ok=0;link_ok<2;++link_ok)
 for(ring_ok=0;ring_ok<2;++ring_ok) for(int trace=0;trace<2;++trace) {
   const u64 sleep=SPTM_VIRT_S2R|SPTM_VIRT_S2R_GO|SPTM_VIRT_S2R_FULL|
      SPTM_VIRT_S2R_COPROC|SPTM_VIRT_S2R_DROP_USB|SPTM_VIRT_S2R_MEM;
   const u64 unrelated=1UL<<20;
   sptm_virt_flags=(sleep & ~SPTM_VIRT_S2R)|(trace?SPTM_VIRT_S2R:0)|unrelated;
   u64 asked=sptm_virt_flags;
   s2r_soc=0;s2r_soc_adt.valid=0;s2r_keep_always=false;kept=rings=lookups=0;
   s2r_no_sleep_setup(cdc,asked,"soc_misc");
   assert((sptm_virt_flags & sleep)==0);
   assert(sptm_virt_flags==unrelated);
   assert(lookups==cdc);
   assert(kept==(cdc&&link_ok));
   assert(rings==trace);
 }
 return 0;
}
'''


@pytest.mark.parametrize("mutation", ["production", "early-return", "keep-mem"])
def test_production_no_sleep_setup(tmp_path, mutation):
    body = helper()
    if mutation == "early-return":
        assert "s2r_keep_setup();" in body
        body = body.replace("s2r_keep_setup();", "s2r_keep_setup(); return;", 1)
    elif mutation == "keep-mem":
        assert " | SPTM_VIRT_S2R_MEM" in body
        body = body.replace(" | SPTM_VIRT_S2R_MEM", "", 1)
    source, exe = tmp_path / "setup.c", tmp_path / "setup"
    source.write_text(PREFIX + body + SUFFIX)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(exe)], check=True,
                   capture_output=True, text=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    if mutation == "production":
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode != 0
        assert "Assertion" in result.stderr or "assertion" in result.stderr
