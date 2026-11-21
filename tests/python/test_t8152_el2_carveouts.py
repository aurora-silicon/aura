# SPDX-License-Identifier: GPL-2.0-only
"""G6 r6 (track C3): the fatal report names the carve-out that L2C_ERR_ADR points into.  Host
only.

Two G6 runs had a guest SError minutes after Windows entry, before any GPU work.  The second
caught L2C_ERR_STS 0x82 (access fault, recursive), and the low 42 bits of its L2C_ERR_ADR named
a DRAM address inside region-id-2, the ANS (NVMe coprocessor) carve-out above the top of normal
RAM.  aura never addresses that carve-out and stage 2 never maps it; only aura's own identity
map covered it.  Upstream's mcc_unmap_adt_carveouts() (src/mcc.c) now takes region-id-4 and
region-id-2 out of the EL2 map on every chip without a usable MCC TZ layout, T8152 included,
so this branch carries no unmap of its own.

Failure modes:
  C6 the upstream unmap stops covering region-id-2 or region-id-4 on T8152, or a second,
     T8152-only unmap comes back into memory.c;
  C7 the report decodes L2C_ERR_ADR with the bits above 41 included, or names the wrong
     carve-out when entries nest (the smallest one that holds the address is meant), or says
     nothing when no carve-out holds it, or does not say that the address lies in a
     carve-out mcc took out of the EL2 map;
  C8 the report calls the decoder when the register read trapped or read zero, or before the
     register line.
All addresses below are synthetic.  The artifact is the transcript the harness prints, written
to $AURA_CARVEOUT_TRANSCRIPT when that is set.
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]

DRAM = 0x10000000000
RAM_TOP = 0x10400000000

COMMON = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
''',
    "adt.h": r'''
#pragma once
#include "types.h"
struct adt_property {
    char name[32];
    u32 size;
    u8 value[16];
};
extern void *adt;
extern struct adt_property stub_props[];
extern int stub_nprops, stub_have_map, stub_lookups;
int adt_path_offset(const void *adt, const char *path);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
#define ADT_FOREACH_PROPERTY(adt, node, prop)                                                   \
    for (const struct adt_property *prop = stub_props; prop < stub_props + stub_nprops; prop++)
''',
}

HARNESS_COMMON = r'''
void *adt = (void *)1;
struct adt_property stub_props[32];
int stub_nprops, stub_have_map = 1, stub_lookups;

int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    stub_lookups++;
    if (strcmp(path, "/chosen/carveout-memory-map"))
        return -1;
    return stub_have_map ? 8 : -1;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    if (node != 8)
        return NULL;
    for (int i = 0; i < stub_nprops; i++)
        if (!strcmp(stub_props[i].name, name)) {
            *lenp = stub_props[i].size;
            return stub_props[i].value;
        }
    return NULL;
}

static void reset(void)
{
    memset(stub_props, 0, sizeof(stub_props));
    stub_nprops = 0;
    stub_have_map = 1;
}

static void add(const char *name, u64 start, u64 size)
{
    struct adt_property *p = &stub_props[stub_nprops++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->size = 16;
    memcpy(p->value, &start, 8);
    memcpy(p->value + 8, &size, 8);
}

static void add_raw(const char *name, u32 size, u64 start)
{
    struct adt_property *p = &stub_props[stub_nprops++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->size = size;
    memcpy(p->value, &start, 8);
}
'''

APL_STUBS = {
    "cpu_regs.h": '#pragma once\n',
    "exception.h": '#pragma once\n',
    "utils.h": r'''
#pragma once
#include "types.h"
extern int is_ecore_stub;
static inline bool is_ecore(void) { return is_ecore_stub; }
''',
    "hv_fatal.h": '#pragma once\n#include "../../types.h"\n#define HV_FATAL_APL_REGS 6\n'
                  'bool hv_fatal_apl_err_read(unsigned int which, const char **name, u64 *val);\n'
                  'void hv_fatal_apl_report_adr(u64 adr);\n',
    "mcc.h": '#pragma once\n#include "types.h"\nstruct mcc_carveout {\n    u64 base, size;\n};\n'
             'extern size_t mcc_carveout_count;\nextern struct mcc_carveout mcc_carveouts[];\n',
}

APL_HARNESS = r'''
#include "types.h"
#include "adt.h"
''' + HARNESS_COMMON + r'''
int is_ecore_stub;
void hv_fatal_apl_report_adr(u64 adr);
struct mcc_carveout {
    u64 base, size;
};
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[3];

static void run(const char *name, u64 adr)
{
    printf("case-begin %s\n", name);
    hv_fatal_apl_report_adr(adr);
    printf("case-end %s\n", name);
}

int main(void)
{
    reset();
    add("region-id-24", 0x10000000000UL, 0x400000000UL); /* the usable RAM itself */
    add("region-id-4", 0x10400000000UL, 0x2000000UL);
    add("region-id-2", 0x10410000000UL, 0x20000000UL);
    add("region-id-9", 0x10420000000UL, 0x1000000UL);  /* nested inside region-id-2 */
    add("region-id-77", 0x10440000000UL, 0);             /* empty: never a match */
    add_raw("not-a-range", 8, 0x10410000000UL);
    /* Bits above 41 set, as in the run; the address itself is 0x10411234200.  mcc unmapped
     * region-id-4 and region-id-2. */
    mcc_carveouts[0].base = 0x10400000000UL;
    mcc_carveouts[0].size = 0x2000000UL;
    mcc_carveouts[1].base = 0x10410000000UL;
    mcc_carveouts[1].size = 0x20000000UL;
    mcc_carveout_count = 2;
    run("in_region2", 0x3800000000000000UL | 0x0000c00000000000UL | 0x10411234200UL);
    run("nested", 0x10420000080UL);
    run("in_ram", 0x10001000000UL);
    run("nowhere", 0x20000000000UL);
    run("region4_end", 0x10401ffff80UL);
    stub_have_map = 0;
    run("no_map", 0x10411234200UL);
    return 0;
}
'''


def cc_or_skip():
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    return cc


def build_and_run(cc, out, harness, sources):
    binary = out / "host"
    (out / "harness.c").write_text(harness)
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-iquote", str(out),
                            str(out / "harness.c"), *[str(s) for s in sources],
                            "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    artifact = os.environ.get("AURA_CARVEOUT_TRANSCRIPT")
    if artifact:
        with open(artifact, "a") as f:
            f.write(result.stdout)
    return result.stdout


def case(text, name):
    start = text.index(f"case-begin {name}\n")
    return text[start:text.index(f"case-end {name}\n", start)]


def test_c6_upstream_unmaps_region_2_and_4_and_memory_c_has_no_second_unmap():
    mcc = (REPO / "src/mcc.c").read_text()
    names = mcc[mcc.index("mcc_adt_carveout_names[]"):]
    names = names[:names.index("};")]
    assert re.findall(r'"(region-id-\d+)"', names) == ["region-id-4", "region-id-2"]
    assert "return mcc_unmap_adt_carveouts();" in mcc
    memory = (REPO / "src/memory.c").read_text()
    body = memory[memory.index("static void mmu_add_default_mappings(void)"):]
    body = body[:body.index("\n}\n")]
    assert "mcc_unmap_carveouts()" in body
    assert "mmu_unmap_t8152" not in memory


@pytest.fixture(scope="module")
def apl(tmp_path_factory):
    cc = cc_or_skip()
    out = tmp_path_factory.mktemp("apl")
    win = out / "src/hv/windows"
    win.mkdir(parents=True)
    for name, text in COMMON.items():
        (out / name).write_text(text)
        (out / "src" / name).write_text(text)
    for name, text in APL_STUBS.items():
        (out / "src" / name).write_text(text)
    (win / "hv_fatal.h").write_text(APL_STUBS["hv_fatal.h"])
    src = (REPO / "src/hv/windows/hv_fatal_apl.c").read_text()
    # Only the decoder is replayed; the guarded MRS reads need the real cores.
    start = src.index("void hv_fatal_apl_report_adr(u64 adr)")
    head = src[:src.index("static void guard_on(void)")]
    body = src[start:src.index("\n}\n", start) + 3]
    pre = head[head.index("#include"):]
    pre = pre.replace('#include "../../cpu_regs.h"', '#include "../../types.h"')
    (win / "decoder.c").write_text(pre + "\n" + body)
    return build_and_run(cc, out, APL_HARNESS, [win / "decoder.c"])


def test_c7_the_address_is_bits_41_0_in_the_smallest_carveout(apl):
    assert case(apl, "in_region2") == (
        "case-begin in_region2\n"
        "HV: fatal: L2C_ERR_ADR bits 41:0 (taken as the physical address): 0x10411234200, in "
        "carve-out region-id-2 [0x10410000000, 0x10430000000) at +0x1234200 (EL2-unmapped by "
        "mcc)\n")
    assert "in carve-out region-id-9 [0x10420000000, 0x10421000000) at +0x80 (EL2-unmapped " \
        "by mcc)\n" in case(apl, "nested")
    assert "in carve-out region-id-24 [0x10000000000, 0x10400000000) at +0x1000000\n" in \
        case(apl, "in_ram")
    assert "0x20000000000, in no carve-out\n" in case(apl, "nowhere")
    assert "in carve-out region-id-4 [0x10400000000, 0x10402000000) at +0x1ffff80 " \
        "(EL2-unmapped by mcc)\n" in case(apl, "region4_end")
    assert "0x10411234200, carve-out map missing\n" in case(apl, "no_map")


def test_c8_the_report_decodes_only_a_read_nonzero_address():
    src = (REPO / "src/hv/windows/hv_fatal.c").read_text()
    body = src[src.index("static void report_apple_errors(void)"):]
    body = body[:body.index("\n}\n")]
    assert body.count("hv_fatal_apl_report_adr(") == 1
    call = body.index("hv_fatal_apl_report_adr(")
    # After the register line is finished, and only for a register that read and is nonzero.
    assert body.rindex('printf("\\n");', 0, call) > body.index("for (unsigned int i = 0;")
    assert re.search(r"if \(adr_ok && adr\)\s*\n\s*hv_fatal_apl_report_adr\(adr\);", body)
    assert "void hv_fatal_apl_report_adr(u64 adr);" in \
        (REPO / "src/hv/windows/hv_fatal.h").read_text()
