# SPDX-License-Identifier: MIT
# Lifetime / ownership host tests for the tables the display DART view publishes
# into a locked DART's inherited roots (first found on J293, T8103/T8020).
#
# Scope:
#   - Lifetime, reservations and ownership of the retained legacy
#     tables: the global L2 record (dart_get_l2), dart_reserve_legacy_tables
#     (real libfdt), the kboot caller contract, rtkit_has_owned_buffers, and
#     the dcp_init failed-boot retain-vs-teardown decision.
#   - Admission and map/unmap semantics live in test_t8103_display_dart.py.
#     This file does NOT re-test map/unmap PTE behaviour.
#
# METHOD: actual-body. The C function bodies are EXTRACTED VERBATIM (by
#   brace-matching, robust to clang-format) from this source tree and
#   compiled against portable stand-in mocks. For dart_reserve_legacy_tables the
#   REAL in-tree libfdt is compiled and linked (fdt_add_mem_rsv etc. are real).
#
# PORTABILITY (CI matrix runs pytest on macos-14/15 too): these tests use NO
#   mmap / MAP_FIXED / Linux-only headers. Simulated "physical" pages are plain
#   integers from a bump allocator and table bytes are never dereferenced (memset
#   is a recording no-op in the get_l2 harness), so every body runs identically
#   on Linux and macOS. The only requirement is a working C compiler (`cc`);
#   if none is found the module skips cleanly rather than failing.  No warning
#   suppression (`-w`/pragmas) is used.
#

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

DRAFT = os.environ.get("DART_DISPLAY_SOURCE", os.environ.get("PR6_DRAFT", str(Path(__file__).resolve().parents[2])))
DART_C = os.path.join(DRAFT, "src/dart.c")
RTKIT_C = os.path.join(DRAFT, "src/rtkit.c")
DCP_C = os.path.join(DRAFT, "src/dcp.c")
KBOOT_C = os.path.join(DRAFT, "src/kboot.c")
LIBFDT = os.path.join(DRAFT, "src/libfdt")

CC = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
_work_directory = tempfile.TemporaryDirectory(prefix="pr6-lifetime-")
WORK = _work_directory.name

# ---------------------------------------------------------------------------
# verbatim-body extraction (brace matched)
# ---------------------------------------------------------------------------
def _read(path):
    with open(path, "r") as f:
        return f.read()

def brace_body(text, start):
    """Return text[start .. matching close brace], starting at first '{' >= start."""
    i = text.index("{", start)
    depth = 0
    j = i
    while j < len(text):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1
    raise RuntimeError("unbalanced braces")

def extract_func(path, sig_regex):
    text = _read(path)
    m = re.search(sig_regex, text)
    if not m:
        raise RuntimeError("signature not found in %s: %s" % (path, sig_regex))
    sig = text[m.start():text.index("{", m.start())].strip()
    return sig + "\n" + brace_body(text, m.start())

def extract_span(path, start_marker, end_marker, include_end=True):
    text = _read(path)
    s = text.index(start_marker)
    e = text.index(end_marker, s)
    if include_end:
        e += len(end_marker)
    return text[s:e]

def extract_if_block(path, if_marker):
    """Extract `if (...) { ... }` whose text starts at if_marker (the `if` keyword)."""
    text = _read(path)
    s = text.index(if_marker)
    return text[s:text.index("{", s)] + brace_body(text, s)

# Read real bodies and spans so source changes are reflected in the tests.
BODY_UNTRACK = extract_func(DART_C, r"static void dart_untrack_legacy_l2\s*\(")
BODY_IS_TRACKED = extract_func(DART_C, r"static bool dart_is_tracked_legacy_l2\s*\(")
BODY_GET_L2 = extract_func(DART_C, r"static u64 \*dart_get_l2\s*\(")
BODY_RELEASE_LOCKED = extract_func(DART_C, r"static bool dart_release_locked_l2\s*\(")
BODY_SHUTDOWN_CHECKED = extract_func(DART_C, r"bool dart_shutdown_checked\s*\(")
BODY_RESERVE = extract_func(DART_C, r"int dart_reserve_legacy_tables\s*\(")
BODY_HAS_OWNED = extract_func(RTKIT_C, r"bool rtkit_has_owned_buffers\s*\(")
DCP_IFBOOTED = extract_if_block(DCP_C, "if (!booted) {")
DCP_LABELS = extract_span(DCP_C, "out_afk:", "return NULL;",
                          include_end=True)  # out_afk: .. (first) return NULL;
# the first "return NULL;" after out_afk is inside out_running? No: out_afk falls
# through; the FIRST return NULL; textually after "out_afk:" is out_running's.
# Grab through out_running explicitly to be safe:
DCP_LABELS = extract_span(DCP_C, "out_afk:", "out_running:", include_end=True)
_text = _read(DCP_C)
_or = _text.index("out_running:")
_ret = _text.index("return NULL;", _or)
DCP_LABELS = _text[_text.index("out_afk:"):_ret + len("return NULL;")]

KBOOT_GUARD_START = "if (dart_reserve_legacy_tables(dt, cur_boot_args.phys_base, cur_boot_args.mem_size))"
KBOOT_GUARD_END = 'bail("FDT: couldn\'t reserve inherited display translation tables\\n");'


def _strip_c_comments(text):
    """Replace C block and line comments with spaces while preserving strings."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                ch = text[i]
                out.append(ch)
                i += 1
                if ch == "\\" and i < n:
                    out.append(text[i])
                    i += 1
                elif ch == quote:
                    break
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i + 1 < n:
                out.append("  ")
                i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _depth1_code(func_body):
    """Return only characters at brace depth 1 inside a brace-delimited function body."""
    out = []
    depth = 0
    i = 0
    n = len(func_body)
    while i < n:
        c = func_body[i]
        if c == '"' or c == "'":
            quote = c
            if depth == 1:
                out.append(c)
            i += 1
            while i < n:
                ch = func_body[i]
                if depth == 1:
                    out.append(ch)
                i += 1
                if ch == "\\" and i < n:
                    if depth == 1:
                        out.append(func_body[i])
                    i += 1
                elif ch == quote:
                    break
            continue
        if c == "{":
            depth += 1
            if depth == 1:
                out.append(c)
        elif c == "}":
            if depth == 1:
                out.append(c)
            depth -= 1
        elif depth == 1:
            out.append(c)
        i += 1
    return "".join(out)


def extract_kboot_reserve_guards(kboot_text):
    """Find every function in kboot.c that calls dt_set_memory() and verify each
    unconditionally runs the dart_reserve_legacy_tables() + bail guard before it."""
    stripped = _strip_c_comments(kboot_text)
    guards = {}
    func_re = re.compile(r"^(?:static\s+)?[a-zA-Z_][\w\s\*]*?\b([a-zA-Z_]\w*)\s*\([^;{}]*\)\s*\n?\{", re.M)
    for m in func_re.finditer(stripped):
        name = m.group(1)
        body_start = m.end() - 1
        body = brace_body(stripped, body_start)
        if not re.search(r"\bdt_set_memory\s*\(", body):
            continue
        top = _depth1_code(body)
        mem_m = re.search(r"\bdt_set_memory\s*\(", top)
        if not mem_m:
            raise AssertionError(
                f"{name}() calls dt_set_memory() inside a nested block without top-level guard check"
            )
        prefix = top[:mem_m.start()]
        if KBOOT_GUARD_START not in prefix:
            raise AssertionError(
                f"{name}() calls dt_set_memory() without calling dart_reserve_legacy_tables() before it"
            )
        g_start = prefix.index(KBOOT_GUARD_START)
        if KBOOT_GUARD_END not in prefix[g_start:]:
            raise AssertionError(
                f"{name}() calls dart_reserve_legacy_tables() without the expected bail() before dt_set_memory()"
            )
        g_end = prefix.index(KBOOT_GUARD_END, g_start) + len(KBOOT_GUARD_END)
        guards[name] = prefix[g_start:g_end]
    for required in ("dt_prepare_t8152", "kboot_prepare_dt"):
        if required not in guards:
            raise AssertionError(f"expected dt_set_memory() caller {required}() not found in kboot.c")
    return guards


KBOOT_GUARDS = extract_kboot_reserve_guards(_read(KBOOT_C))

# ---------------------------------------------------------------------------
# shared C preamble (aura-faithful macros/types)
# ---------------------------------------------------------------------------
PRE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
/* Match aura's aarch64 LP64 u64 (unsigned long) so %lx format checks pass on macOS too. */
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef unsigned long u64;
typedef int32_t s32; typedef long s64;
#define BIT(x)                 (1UL << (x))
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))
#define SZ_16K (1 << 14)
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do { if (cond) { g_pass++; printf("PASS %s\n", name); } \
    else { g_fail++; printf("FAIL %s  (line %d)\n", name, __LINE__); } } while (0)
"""

# ---------------------------------------------------------------------------
# C1 : global L2 record (dart_get_l2) -- before-publish / overflow / no-free-across-handles
# ---------------------------------------------------------------------------
C_GET_L2 = PRE + r"""
#define DART_PTE_OFFSET_SHIFT     14
#define DART_PTE_VALID            BIT(0)
#define DART_T8020_PTE_OFFSET     GENMASK(39, 14)
#define DART_MAX_TTBR_COUNT 4
#define LEGACY_L2_MAX_PAGES 32

struct dart_dev;
struct dart_params { int ttbr_count; u64 offset_mask; void (*tlb_invalidate)(struct dart_dev *); };
typedef struct dart_dev {
    bool locked; bool keep; bool legacy_mutable;
    uintptr_t regs;
    u8 device;
    struct dart_params *params;
    u64 *l1[DART_MAX_TTBR_COUNT];
} dart_dev_t;

/* file-scope record under test (matches the draft) */
static u64 legacy_l2_pages[LEGACY_L2_MAX_PAGES];
static size_t legacy_l2_count;

/* ---- portable mocks: simulated 16K-aligned phys pages, never dereferenced ---- */
#define DRAM_BASE 0x800000000UL      /* 2^35, 16K-aligned, encodable in GENMASK(39,14) */
static u64 g_cursor = DRAM_BASE;
static u64 g_force_phys = 0;         /* if set, next memalign returns this (round-trip test) */
static int g_memalign_calls = 0, g_free_calls = 0;
static void *mock_memalign(size_t a, size_t s) {
    (void)a; (void)s; g_memalign_calls++;
    u64 p; if (g_force_phys) { p = g_force_phys; g_force_phys = 0; } else { p = g_cursor; g_cursor += SZ_16K; }
    return (void *)(uintptr_t)p;
}
static void mock_free(void *p) { (void)p; g_free_calls++; }
static void *mock_memset(void *d, int v, size_t n) { (void)v; (void)n; return d; } /* table bytes not under test */
#define memalign mock_memalign
#define free      mock_free
#define memset    mock_memset

""" + BODY_GET_L2 + r"""
#undef memalign
#undef free
#undef memset

static struct dart_params P = { .ttbr_count = 4, .offset_mask = DART_T8020_PTE_OFFSET };
static u64 l1A[2048], l1B[2048];

static u64 decode(u64 pte) { return FIELD_GET(DART_T8020_PTE_OFFSET, pte) << DART_PTE_OFFSET_SHIFT; }
static int in_record(u64 page) { for (size_t i = 0; i < legacy_l2_count; i++) if (legacy_l2_pages[i] == page) return 1; return 0; }

int main(void) {
    dart_dev_t A = { .locked = true, .keep = true, .legacy_mutable = true, .params = &P };
    A.l1[0] = l1A;

    /* (1) fresh legacy alloc: recorded BEFORE/at publish, published entry backed by the recorded page */
    size_t c0 = legacy_l2_count; int ma0 = g_memalign_calls, fr0 = g_free_calls;
    u64 *t = dart_get_l2(&A, 5);
    CHECK(t != NULL, "get_l2.legacy.alloc_returns_table");
    CHECK(legacy_l2_count == c0 + 1, "get_l2.record.count_incremented_by_one");
    CHECK(legacy_l2_pages[legacy_l2_count - 1] == (u64)t, "get_l2.record.page_is_returned_table");
    CHECK((l1A[5] & DART_PTE_VALID) != 0, "get_l2.publish.l1_entry_valid");
    CHECK(decode(l1A[5]) == (u64)t, "get_l2.publish.l1_decodes_to_table");
    /* safety invariant: every PUBLISHED legacy table is in the record (record superset of published) */
    CHECK(in_record((u64)t), "get_l2.invariant.published_implies_recorded");
    CHECK(g_free_calls == fr0, "get_l2.legacy.no_free_on_success");
    CHECK(g_memalign_calls == ma0 + 1, "get_l2.legacy.one_alloc");

    /* (2) already-valid (adopted firmware L2): returned, NOT re-recorded, no alloc -> dedup across re-adoption */
    u64 fw = 0x900000000UL;
    l1A[7] = FIELD_PREP(DART_T8020_PTE_OFFSET, fw >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
    size_t c1 = legacy_l2_count; int ma1 = g_memalign_calls;
    u64 *t7 = dart_get_l2(&A, 7);
    CHECK((u64)t7 == fw, "get_l2.adopted.returns_firmware_page");
    CHECK(legacy_l2_count == c1, "get_l2.adopted.not_recorded");
    CHECK(g_memalign_calls == ma1, "get_l2.adopted.no_alloc");

    /* (3) list overflow: at cap, refuse (NULL), no publish, no record, no alloc */
    size_t saved = legacy_l2_count; legacy_l2_count = LEGACY_L2_MAX_PAGES;
    int ma2 = g_memalign_calls;
    u64 *tof = dart_get_l2(&A, 6);
    CHECK(tof == NULL, "get_l2.overflow.returns_null");
    CHECK(legacy_l2_count == LEGACY_L2_MAX_PAGES, "get_l2.overflow.count_unchanged");
    CHECK((l1A[6] & DART_PTE_VALID) == 0, "get_l2.overflow.not_published");
    CHECK(g_memalign_calls == ma2, "get_l2.overflow.no_alloc");
    legacy_l2_count = saved;

    /* (4) offset round-trip reject: unencodable phys -> free + NULL, not recorded, not published */
    g_force_phys = 0x10000000000UL;          /* bit 40 set: cannot survive GENMASK(39,14) */
    size_t c3 = legacy_l2_count; int fr3 = g_free_calls;
    u64 *tbad = dart_get_l2(&A, 20);
    CHECK(tbad == NULL, "get_l2.roundtrip.rejects_unencodable");
    CHECK(legacy_l2_count == c3, "get_l2.roundtrip.not_recorded");
    CHECK((l1A[20] & DART_PTE_VALID) == 0, "get_l2.roundtrip.not_published");
    CHECK(g_free_calls == fr3 + 1, "get_l2.roundtrip.frees_the_rejected_page");

    /* (5) no frees across handles: handle A's records persist when a 2nd handle allocates */
    size_t before = legacy_l2_count; int frA = g_free_calls;
    u64 *a10 = dart_get_l2(&A, 10);
    u64 *a11 = dart_get_l2(&A, 11);
    (void)a10; (void)a11;
    size_t afterA = legacy_l2_count;
    u64 snapshot[LEGACY_L2_MAX_PAGES];
    for (size_t i = 0; i < afterA; i++) snapshot[i] = legacy_l2_pages[i];
    /* a locked dart's shutdown frees nothing and never touches the global record;
       model handle B as a brand-new device with its own (fresh, invalid) L1 root */
    dart_dev_t B = { .locked = true, .keep = true, .legacy_mutable = true, .params = &P };
    B.l1[0] = l1B;
    u64 *b10 = dart_get_l2(&B, 10);
    u64 *b11 = dart_get_l2(&B, 11);
    CHECK(legacy_l2_count == afterA + 2, "get_l2.handles.record_grows_monotonically");
    int preserved = 1; for (size_t i = 0; i < afterA; i++) if (legacy_l2_pages[i] != snapshot[i]) preserved = 0;
    CHECK(preserved, "get_l2.handles.prior_records_preserved");
    CHECK(b10 != a10 && b11 != a11, "get_l2.handles.fresh_root_gets_new_pages");
    CHECK(g_free_calls == frA, "get_l2.handles.no_frees_across_handles");
    (void)before;

    /* (6) locked-only DART (legacy_mutable = false, locked = true): e.g. writable locked Gen3 */
    static u64 l1C[2048];
    dart_dev_t C = { .locked = true, .keep = true, .legacy_mutable = false,
                     .regs = 0x2f0000000UL, .device = 23, .params = &P };
    C.l1[0] = l1C;
    size_t c6 = legacy_l2_count; int ma6 = g_memalign_calls, fr6 = g_free_calls;
    u64 *tc = dart_get_l2(&C, 3);
    CHECK(tc != NULL, "get_l2.locked_only.alloc_returns_table");
    CHECK(legacy_l2_count == c6 + 1, "get_l2.locked_only.count_incremented");
    CHECK(legacy_l2_pages[legacy_l2_count - 1] == (u64)tc, "get_l2.locked_only.page_recorded");
    CHECK((l1C[3] & DART_PTE_VALID) != 0 && decode(l1C[3]) == (u64)tc, "get_l2.locked_only.published");
    CHECK(in_record((u64)tc), "get_l2.locked_only.published_implies_recorded");
    CHECK(g_free_calls == fr6 && g_memalign_calls == ma6 + 1, "get_l2.locked_only.one_alloc_no_free");

    /* locked-only overflow: cap is enforced without allocating or publishing */
    size_t saved6 = legacy_l2_count; legacy_l2_count = LEGACY_L2_MAX_PAGES;
    int ma6_of = g_memalign_calls;
    CHECK(dart_get_l2(&C, 4) == NULL, "get_l2.locked_only.overflow_returns_null");
    CHECK((l1C[4] & DART_PTE_VALID) == 0, "get_l2.locked_only.overflow_not_published");
    CHECK(g_memalign_calls == ma6_of, "get_l2.locked_only.overflow_no_alloc");
    legacy_l2_count = saved6;

    /* locked-only round-trip reject: unencodable phys -> free + NULL, not recorded or published */
    g_force_phys = 0x10000000000UL;
    size_t c6_rt = legacy_l2_count; int fr6_rt = g_free_calls;
    CHECK(dart_get_l2(&C, 5) == NULL, "get_l2.locked_only.roundtrip_rejects");
    CHECK(legacy_l2_count == c6_rt, "get_l2.locked_only.roundtrip_not_recorded");
    CHECK((l1C[5] & DART_PTE_VALID) == 0, "get_l2.locked_only.roundtrip_not_published");
    CHECK(g_free_calls == fr6_rt + 1, "get_l2.locked_only.roundtrip_frees_rejected");

    printf("RESULT %d %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""

# ---------------------------------------------------------------------------
# C2 : dart_reserve_legacy_tables with REAL libfdt (bounds + partial reservation)
# ---------------------------------------------------------------------------
C_RESERVE = PRE + r"""
#include "libfdt.h"
#define T8103 0x8103
#define LEGACY_L2_MAX_PAGES 32
u32 chip_id;
static u64 legacy_l2_pages[LEGACY_L2_MAX_PAGES];
static size_t legacy_l2_count;

""" + BODY_RESERVE + r"""

#define DRAM_BASE 0x800000000UL
_Alignas(8) static char big[65536];
_Alignas(8) static char small[256];

static void fresh(void *buf, int sz) { int r = fdt_create_empty_tree(buf, sz); if (r) { printf("FATAL fdt_create_empty_tree=%d\n", r); exit(2);} }
static void set_pages(const u64 *p, size_t n) { legacy_l2_count = n; for (size_t i = 0; i < n; i++) legacy_l2_pages[i] = p[i]; }

int main(void) {
    u64 ok4[4] = { DRAM_BASE, DRAM_BASE + SZ_16K, DRAM_BASE + 2*SZ_16K, DRAM_BASE + 3*SZ_16K };

    /* any chip: the rule is not chip-specific, recorded pages are reserved */
    chip_id = 0x6000; fresh(big, sizeof big); set_pages(ok4, 4);
    CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == 0, "reserve.anychip.returns_0");
    CHECK(fdt_num_mem_rsv(big) == 4, "reserve.anychip.four_reservations");

    /* empty record: no-op */
    chip_id = T8103; fresh(big, sizeof big); legacy_l2_count = 0;
    CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == 0, "reserve.empty.returns_0");
    CHECK(fdt_num_mem_rsv(big) == 0, "reserve.empty.no_reservations");

    /* happy path: 4 valid pages reserved, each entry matches */
    chip_id = T8103; fresh(big, sizeof big); set_pages(ok4, 4);
    CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == 0, "reserve.happy.returns_0");
    CHECK(fdt_num_mem_rsv(big) == 4, "reserve.happy.four_reservations");
    int entries_ok = 1;
    for (int i = 0; i < 4; i++) { uint64_t a, s; fdt_get_mem_rsv(big, i, &a, &s); if (a != ok4[i] || s != (uint64_t)SZ_16K) entries_ok = 0; }
    CHECK(entries_ok, "reserve.happy.entries_match_pages_and_16k");

    /* NULL dt -> -1 */
    chip_id = T8103; set_pages(ok4, 4);
    CHECK(dart_reserve_legacy_tables(NULL, DRAM_BASE, 0x40000000UL) == -1, "reserve.bounds.null_dt");

    /* dram_size < SZ_16K -> -1, nothing reserved */
    chip_id = T8103; fresh(big, sizeof big); set_pages(ok4, 4);
    CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, SZ_16K - 1) == -1, "reserve.bounds.dram_too_small");
    CHECK(fdt_num_mem_rsv(big) == 0, "reserve.bounds.dram_too_small.no_rsv");

    /* dram_base + dram_size overflow -> -1 */
    chip_id = T8103; fresh(big, sizeof big); set_pages(ok4, 4);
    CHECK(dart_reserve_legacy_tables(big, UINT64_MAX - SZ_16K + 1, SZ_16K) == -1, "reserve.bounds.base_plus_size_overflow");
    CHECK(fdt_num_mem_rsv(big) == 0, "reserve.bounds.overflow.no_rsv");

    /* count > MAX (defensive) -> -1 */
    chip_id = T8103; fresh(big, sizeof big); set_pages(ok4, 4); legacy_l2_count = LEGACY_L2_MAX_PAGES + 1;
    CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == -1, "reserve.bounds.count_over_max");

    /* per-page: unaligned -> -1, validated before any reservation added */
    { u64 p[3] = { DRAM_BASE, DRAM_BASE + SZ_16K + 1, DRAM_BASE + 2*SZ_16K };
      chip_id = T8103; fresh(big, sizeof big); set_pages(p, 3);
      CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == -1, "reserve.page.unaligned");
      CHECK(fdt_num_mem_rsv(big) == 0, "reserve.page.unaligned.validate_before_reserve"); }

    /* per-page: below dram_base -> -1 */
    { u64 p[2] = { DRAM_BASE, DRAM_BASE - SZ_16K };
      chip_id = T8103; fresh(big, sizeof big); set_pages(p, 2);
      CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, 0x40000000UL) == -1, "reserve.page.below_base");
      CHECK(fdt_num_mem_rsv(big) == 0, "reserve.page.below_base.no_rsv"); }

    /* per-page: end beyond DRAM -> -1 (page - base > size - 16K) */
    { u64 sz = 4 * (u64)SZ_16K; u64 p[2] = { DRAM_BASE, DRAM_BASE + 4*SZ_16K };
      chip_id = T8103; fresh(big, sizeof big); set_pages(p, 2);
      CHECK(dart_reserve_legacy_tables(big, DRAM_BASE, sz) == -1, "reserve.page.end_beyond_dram");
      CHECK(fdt_num_mem_rsv(big) == 0, "reserve.page.end_beyond_dram.no_rsv"); }

    /* PARTIAL RESERVATION: force real libfdt NOSPACE mid-loop; function does NOT roll back */
    fresh(small, sizeof small);
    int kmax = 0; while (fdt_add_mem_rsv(small, DRAM_BASE + (u64)kmax * SZ_16K, SZ_16K) == 0) kmax++;
    CHECK(kmax >= 1, "reserve.partial.small_tree_holds_at_least_one");
    if (kmax >= 1) {
        size_t n = (size_t)kmax + 1;                 /* one more page than fits */
        u64 *pp = malloc(n * sizeof(u64));
        for (size_t i = 0; i < n; i++) pp[i] = DRAM_BASE + (u64)i * SZ_16K;   /* all valid */
        chip_id = T8103; fresh(small, sizeof small); set_pages(pp, n);
        int r = dart_reserve_legacy_tables(small, DRAM_BASE, 0x40000000UL);
        CHECK(r == -1, "reserve.partial.returns_-1_on_nospace");
        CHECK(fdt_num_mem_rsv(small) == (kmax), "reserve.partial.leaves_k_reservations_no_rollback");
        free(pp);
    }

    printf("RESULT %d %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""

# ---------------------------------------------------------------------------
# C3 : kboot caller contract -- verbatim guards from EVERY dt_set_memory() path
# ---------------------------------------------------------------------------
C_KBOOT = PRE + r"""
/* verbatim aura bail() macro (src/kboot.c) */
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

static int   g_seq = 0;
static int   g_reserve_seq = 0, g_set_memory_seq = 0;
static u64   g_seen_base = 0, g_seen_size = 0;
static int   g_reserve_ret = 0;
static int dart_reserve_legacy_tables(void *dt, u64 b, u64 s) {
    (void)dt; g_seen_base = b; g_seen_size = s; g_reserve_seq = ++g_seq; return g_reserve_ret;
}
static int dt_set_memory(void) { g_set_memory_seq = ++g_seq; return 0; }
static struct { u64 phys_base; u64 mem_size; } cur_boot_args = { 0xAAAA0000UL, 0xBBBB0000UL };
static void *dt = (void *)0x1234;

static int prepare_dt_main(void) {
""" + "    " + KBOOT_GUARDS["kboot_prepare_dt"] + r"""
    return dt_set_memory();
}

static int prepare_dt_t8152(void) {
""" + "    " + KBOOT_GUARDS["dt_prepare_t8152"] + r"""
    return dt_set_memory();
}

static void check_path(const char *tag, int (*fn)(void)) {
    /* reserve fails -> caller bails: returns -1 and dt_set_memory() never runs */
    g_reserve_ret = -1; g_seq = g_reserve_seq = g_set_memory_seq = 0;
    g_seen_base = g_seen_size = 0;
    int r = fn();
    CHECK(r == -1, tag);
    CHECK(g_set_memory_seq == 0, "kboot.bail.aborts_before_dt_set_memory");
    CHECK(g_seen_base == cur_boot_args.phys_base, "kboot.args.phys_base_threaded");
    CHECK(g_seen_size == cur_boot_args.mem_size, "kboot.args.mem_size_threaded");

    /* reserve ok -> dart_reserve_legacy_tables() runs strictly before dt_set_memory() */
    g_reserve_ret = 0; g_seq = g_reserve_seq = g_set_memory_seq = 0;
    r = fn();
    CHECK(r == 0, "kboot.ok.continues");
    CHECK(g_reserve_seq == 1 && g_set_memory_seq == 2, "kboot.ok.reserve_before_dt_set_memory");
}

int main(void) {
    check_path("kboot.main.bail", prepare_dt_main);
    check_path("kboot.t8152.bail", prepare_dt_t8152);

    printf("RESULT %d %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""

# ---------------------------------------------------------------------------
# C4 : rtkit_has_owned_buffers (real body) + dcp_init retain-vs-teardown decision
# ---------------------------------------------------------------------------
C_DCP = PRE + r"""
struct rtkit_buffer { void *bfr; u64 dva; size_t sz; bool owned; };
typedef struct rtkit_dev {
    struct rtkit_buffer oslog_bfr;
    struct rtkit_buffer syslog_bfr;
    struct rtkit_buffer crashlog_bfr;
    struct rtkit_buffer ioreport_bfr;
} rtkit_dev_t;

""" + BODY_HAS_OWNED + r"""

/* ---- dcp_init failed-boot decision + teardown labels (verbatim) ---- */
typedef struct { void *dart_dcp, *dart_disp, *iovad_dcp, *afk, *asc; rtkit_dev_t *rtkit; } dcp_dev_t;

static int c_dart_shutdown, c_iovad_shutdown, c_rtkit_free, c_rtkit_quiesce,
           c_afk_shutdown, c_free_dcp, c_mark_running;
static void dart_shutdown(void *d) { (void)d; c_dart_shutdown++; }
static void iovad_shutdown(void *i, void *d) { (void)i; (void)d; c_iovad_shutdown++; }
static void rtkit_free(void *r) { (void)r; c_rtkit_free++; }
static bool rtkit_quiesce(void *r) { (void)r; c_rtkit_quiesce++; return true; }
static int  afk_epic_shutdown(void *a) { (void)a; c_afk_shutdown++; return 0; }
static void dcp_mark_running(void) { c_mark_running++; }
#define free(p) do { (void)(p); c_free_dcp++; } while (0)

static void reset_counts(void) {
    c_dart_shutdown = c_iovad_shutdown = c_rtkit_free = c_rtkit_quiesce =
        c_afk_shutdown = c_free_dcp = c_mark_running = 0;
}

static dcp_dev_t *run_failed_boot(rtkit_dev_t *rtk) {
    static int dummy;
    dcp_dev_t _d = { .dart_dcp = &dummy, .dart_disp = &dummy, .iovad_dcp = &dummy,
                     .afk = &dummy, .asc = &dummy, .rtkit = rtk };
    dcp_dev_t *dcp = &_d;
    bool booted = false;
    """ + DCP_IFBOOTED + r"""
    /* mark the verbatim error labels used under -Werror without altering flow:
       all locals above are already initialised, so this forward goto (never
       taken) crosses no initialisation. out_running is already referenced. */
    if (0) { goto out_afk; goto out_rtkit; goto out_dart_dcp; goto out_free; }
    return dcp;   /* success path, never reached (both branches goto) */
""" + DCP_LABELS + r"""
}
#undef free

int main(void) {
    /* Part A: rtkit_has_owned_buffers truth table */
    CHECK(rtkit_has_owned_buffers(NULL) == false, "owned.null_false");
    rtkit_dev_t z = {0};
    CHECK(rtkit_has_owned_buffers(&z) == false, "owned.none_false");
    { rtkit_dev_t r = {0}; r.syslog_bfr.owned = true;   CHECK(rtkit_has_owned_buffers(&r), "owned.syslog_true"); }
    { rtkit_dev_t r = {0}; r.crashlog_bfr.owned = true; CHECK(rtkit_has_owned_buffers(&r), "owned.crashlog_true"); }
    { rtkit_dev_t r = {0}; r.ioreport_bfr.owned = true; CHECK(rtkit_has_owned_buffers(&r), "owned.ioreport_true"); }
    { rtkit_dev_t r = {0}; r.oslog_bfr.owned = true;    CHECK(rtkit_has_owned_buffers(&r), "owned.oslog_true"); }
    { rtkit_dev_t r = {0}; r.ioreport_bfr.owned = true; r.oslog_bfr.owned = true; CHECK(rtkit_has_owned_buffers(&r), "owned.combo_true"); }

    /* Part B: OWNED buffers on failed boot -> out_running, NO teardown (retain mappings/buffers) */
    { rtkit_dev_t r = {0}; r.ioreport_bfr.owned = true;   /* the EP4 16KiB IOReport grant */
      reset_counts();
      dcp_dev_t *res = run_failed_boot(&r);
      CHECK(res == NULL, "dcp.owned.returns_null");
      CHECK(c_mark_running == 1, "dcp.owned.marks_running");
      CHECK(c_dart_shutdown == 0, "dcp.owned.no_dart_shutdown");
      CHECK(c_iovad_shutdown == 0, "dcp.owned.no_iovad_shutdown");
      CHECK(c_rtkit_free == 0, "dcp.owned.no_rtkit_free");
      CHECK(c_rtkit_quiesce == 0, "dcp.owned.no_quiesce");
      CHECK(c_free_dcp == 0, "dcp.owned.no_free_dcp");
      CHECK(c_afk_shutdown == 0, "dcp.owned.no_afk_shutdown"); }

    /* Part B': NO owned buffers on failed boot -> out_iovad teardown (contrast) */
    { rtkit_dev_t r = {0};
      reset_counts();
      dcp_dev_t *res = run_failed_boot(&r);
      CHECK(res == NULL, "dcp.unowned.returns_null");
      CHECK(c_mark_running == 0, "dcp.unowned.not_marked_running");
      CHECK(c_iovad_shutdown == 1, "dcp.unowned.iovad_shutdown_once");
      CHECK(c_dart_shutdown == 2, "dcp.unowned.dart_shutdown_disp_and_dcp");
      CHECK(c_free_dcp == 1, "dcp.unowned.frees_dcp");
      CHECK(c_rtkit_free == 0, "dcp.unowned.skips_rtkit_free");   /* goto out_iovad bypasses out_rtkit */
      CHECK(c_afk_shutdown == 0, "dcp.unowned.skips_afk"); }

    printf("RESULT %d %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""

# ---------------------------------------------------------------------------
# C5 : locked-only DART (legacy_mutable=false, locked=true, e.g. Gen3) end-to-end
#      allocate -> shutdown -> libfdt reserve + pool-full fallback diagnostic
# ---------------------------------------------------------------------------
C_LOCKED_SHUTDOWN = PRE + r"""
#include <string.h>
#include "libfdt.h"
#define DART_PTE_OFFSET_SHIFT     14
#define DART_PTE_VALID            BIT(0)
#define DART_MAX_TTBR_COUNT 4
#define LEGACY_L2_MAX_PAGES 32
/* 52-bit host-safe offset mask so static aligned pages round-trip on Linux & macOS */
#define HOST_PTE_OFFSET           GENMASK(51, 14)

struct dart_dev;
struct dart_params {
    int ttbr_count;
    u64 offset_mask;
    u32 tcr_disabled;
    void (*tlb_invalidate)(struct dart_dev *);
};
typedef struct dart_dev {
    bool failed;
    bool locked;
    bool keep;
    bool legacy_mutable;
    uintptr_t regs;
    u8 device;
    const struct dart_params *params;
    u64 *l1[DART_MAX_TTBR_COUNT];
} dart_dev_t;

static u64 legacy_l2_pages[LEGACY_L2_MAX_PAGES];
static size_t legacy_l2_count;

static u64 l2_pool[4][SZ_16K / 8] __attribute__((aligned(SZ_16K)));
static u64 l1_root[2048];
static size_t g_pool_next = 0;
static int g_l2_frees = 0, g_tlbi_calls = 0;

static bool is_heap(const void *p) {
    return (const u8 *)p >= (const u8 *)l2_pool &&
           (const u8 *)p < (const u8 *)l2_pool + sizeof(l2_pool);
}
static void *mock_memalign(size_t a, size_t s) {
    (void)a; (void)s;
    if (g_pool_next >= 4)
        return NULL;
    return l2_pool[g_pool_next++];
}
static void mock_free(void *p) {
    if (!p)
        return;
    if (is_heap(p)) {
        g_l2_frees++;
        return;
    }
    free(p);
}
static void write32(u64 a, u32 v) { (void)a; (void)v; }
#define DART_TCR(d)     ((void)(d), 0UL)
#define DART_TTBR(d, i) ((void)(d), (void)(i), 0UL)

#define memalign mock_memalign
#define free     mock_free

""" + BODY_UNTRACK + "\n" + BODY_IS_TRACKED + "\n" + BODY_GET_L2 + "\n" + BODY_RELEASE_LOCKED + "\n" + BODY_SHUTDOWN_CHECKED + "\n" + BODY_RESERVE + r"""

#undef memalign
#undef free

static void mock_tlbi(dart_dev_t *dart) { (void)dart; g_tlbi_calls++; }
static const struct dart_params P_HOST = {
    .ttbr_count = 1,
    .offset_mask = HOST_PTE_OFFSET,
    .tcr_disabled = 0,
    .tlb_invalidate = mock_tlbi,
};
_Alignas(8) static char fdt_buf[65536];

static void reset_state(void) {
    legacy_l2_count = 0;
    memset(legacy_l2_pages, 0, sizeof(legacy_l2_pages));
    memset(l2_pool, 0, sizeof(l2_pool));
    memset(l1_root, 0, sizeof(l1_root));
    g_pool_next = 0;
    g_l2_frees = 0;
    g_tlbi_calls = 0;
}

static dart_dev_t *make_locked_gen3(void) {
    dart_dev_t *d = calloc(1, sizeof(*d));
    d->locked = true;
    d->keep = true;
    d->legacy_mutable = false;
    d->regs = 0x2f0000000UL;
    d->device = 23;
    d->params = &P_HOST;
    d->l1[0] = l1_root;
    return d;
}

int main(void) {
    /* (1) locked-only DART with mapped L2: tracked on alloc, kept on shutdown, reserved in FDT */
    reset_state();
    dart_dev_t *d = make_locked_gen3();
    u64 *l2 = dart_get_l2(d, 2);
    CHECK(l2 != NULL, "locked_shutdown.mapped.alloc_ok");
    CHECK(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)l2, "locked_shutdown.mapped.tracked_on_alloc");
    l2[0] = DART_PTE_VALID;
    CHECK(dart_shutdown_checked(d), "locked_shutdown.mapped.shutdown_ok");
    CHECK((l1_root[2] & DART_PTE_VALID) != 0, "locked_shutdown.mapped.l1_kept");
    CHECK(g_l2_frees == 0, "locked_shutdown.mapped.not_freed");
    CHECK(legacy_l2_count == 1 && legacy_l2_pages[0] == (u64)l2, "locked_shutdown.mapped.stays_tracked");
    CHECK(fdt_create_empty_tree(fdt_buf, sizeof(fdt_buf)) == 0, "locked_shutdown.mapped.fdt_init");
    CHECK(dart_reserve_legacy_tables(fdt_buf, (u64)l2_pool, sizeof(l2_pool)) == 0,
          "locked_shutdown.mapped.reserve_ok");
    CHECK(fdt_num_mem_rsv(fdt_buf) == 1, "locked_shutdown.mapped.one_rsv");
    uint64_t raddr = 0, rsize = 0;
    fdt_get_mem_rsv(fdt_buf, 0, &raddr, &rsize);
    CHECK(raddr == (uint64_t)(uintptr_t)l2 && rsize == SZ_16K, "locked_shutdown.mapped.rsv_matches_l2");

    /* (2) locked-only DART with empty L2: unlinked, TLBI'd, untracked and freed on shutdown */
    reset_state();
    d = make_locked_gen3();
    u64 *l2e = dart_get_l2(d, 5);
    CHECK(l2e != NULL && legacy_l2_count == 1, "locked_shutdown.empty.tracked_on_alloc");
    CHECK(dart_shutdown_checked(d), "locked_shutdown.empty.shutdown_ok");
    CHECK(g_tlbi_calls == 1, "locked_shutdown.empty.one_tlbi");
    CHECK(l1_root[5] == 0, "locked_shutdown.empty.l1_cleared");
    CHECK(g_l2_frees == 1, "locked_shutdown.empty.freed");
    CHECK(legacy_l2_count == 0, "locked_shutdown.empty.untracked");

    /* (3) untracked mapped heap L2 when pool is full: fails closed and logs diagnostic */
    reset_state();
    for (size_t i = 0; i < LEGACY_L2_MAX_PAGES; i++)
        legacy_l2_pages[i] = 0x800000000UL + (u64)i * SZ_16K;
    legacy_l2_count = LEGACY_L2_MAX_PAGES;
    d = make_locked_gen3();
    l2_pool[1][0] = DART_PTE_VALID;
    l1_root[6] = FIELD_PREP(HOST_PTE_OFFSET, ((u64)l2_pool[1]) >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
    CHECK(!dart_shutdown_checked(d), "locked_shutdown.pool_full.fails_closed");
    CHECK((l1_root[6] & DART_PTE_VALID) != 0 && g_l2_frees == 0, "locked_shutdown.pool_full.table_kept");
    CHECK(legacy_l2_count == LEGACY_L2_MAX_PAGES, "locked_shutdown.pool_full.count_capped");
    free(d);

    printf("RESULT %d %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""

# ---------------------------------------------------------------------------
# build + run
# ---------------------------------------------------------------------------
def build_run(name, src, extra_srcs=(), includes=()):
    cpath = os.path.join(WORK, name + ".c")
    bpath = os.path.join(WORK, name)
    with open(cpath, "w") as f:
        f.write(src)
    # -Werror: a harness warning is a failure, not something to parse around.
    cmd = [CC, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
           "-fno-sanitize-recover=undefined", "-o", bpath, cpath]
    for inc in includes:
        cmd += ["-I", inc]
    cmd += list(extra_srcs)
    comp = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if comp.returncode != 0:
        return {"name": name, "built": False, "compile_stderr": comp.stderr,
                "compile_warnings": comp.stderr.strip(), "returncode": None,
                "pass": 0, "fail": 0, "n_result": 0, "lines": []}
    run = subprocess.run([bpath], capture_output=True, text=True, timeout=30)
    lines = run.stdout.splitlines()
    p = f = 0
    n_result = 0
    for l in lines:
        if l.startswith("RESULT"):
            n_result += 1
            try:
                _, p, f = l.split()
                p, f = int(p), int(f)
            except ValueError:
                n_result = -1  # malformed RESULT line -> force failure
    return {"name": name, "built": True, "compile_warnings": comp.stderr.strip(),
            "returncode": run.returncode, "pass": p, "fail": f,
            "n_result": n_result, "lines": lines}


def _ok(r):
    """Strict pass: compiled, exited 0, exactly one well-formed RESULT, a
    nonzero number of assertions, and no failures. A crash with no RESULT (which
    would otherwise parse as pass=fail=0) is rejected here."""
    return (r.get("built") and r.get("returncode") == 0 and r.get("n_result") == 1
            and r.get("pass", 0) > 0 and r.get("fail", 0) == 0)


def _fdt_srcs():
    return [os.path.join(LIBFDT, x) for x in
            ("fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c", "fdt_strerror.c")]

CASE_NAMES = ["c1_get_l2", "c2_reserve", "c3_kboot", "c4_dcp", "c5_locked_shutdown"]

def _case(name):
    # name -> (C source, extra link srcs, include dirs)
    return {
        "c1_get_l2":          (C_GET_L2, (), ()),
        "c2_reserve":         (C_RESERVE, tuple(_fdt_srcs()), (LIBFDT,)),
        "c3_kboot":           (C_KBOOT, (), ()),
        "c4_dcp":             (C_DCP, (), ()),
        "c5_locked_shutdown": (C_LOCKED_SHUTDOWN, tuple(_fdt_srcs()), (LIBFDT,)),
    }[name]

# ---- pytest entry points (so CI actually runs the harness, incl. macos matrix) ----
import pytest

@pytest.mark.skipif(CC is None, reason="no C compiler (cc/clang/gcc) on PATH")
@pytest.mark.parametrize("name", CASE_NAMES)
def test_actual_body(name):
    src, extra, inc = _case(name)
    r = build_run(name, src, extra_srcs=extra, includes=inc)
    assert r["built"], "compile failed:\n" + r.get("compile_stderr", "")
    assert r["returncode"] == 0, "nonzero exit %r (crash / no RESULT?)\n%s" % (
        r["returncode"], "\n".join(r["lines"]))
    assert r["n_result"] == 1, "expected exactly one RESULT line, got %r" % r["n_result"]
    assert r["pass"] > 0, "no assertions ran"
    fails = [l for l in r["lines"] if l.startswith("FAIL")]
    assert r["fail"] == 0, "assertion failures:\n" + "\n".join(fails)


def test_kboot_every_dt_set_memory_path_reserves():
    kboot_text = _read(KBOOT_C)
    guards = extract_kboot_reserve_guards(kboot_text)
    assert "dt_prepare_t8152" in guards and "kboot_prepare_dt" in guards

    # Mutate dt_prepare_t8152() to drop its reservation guard -> must fail.
    t8152_body = extract_func(KBOOT_C, r"static int dt_prepare_t8152\s*\(")
    assert guards["dt_prepare_t8152"] in t8152_body
    mutated_drop_t8152 = kboot_text.replace(
        t8152_body, t8152_body.replace(guards["dt_prepare_t8152"], "", 1), 1
    )
    with pytest.raises(AssertionError, match="dt_prepare_t8152"):
        extract_kboot_reserve_guards(mutated_drop_t8152)

    # Mutate dt_prepare_t8152() to run dart_reserve_legacy_tables() AFTER dt_set_memory() -> must fail.
    mem_stmt = "    if (dt_set_memory())\n        return -1;"
    assert mem_stmt in t8152_body
    reordered_body = t8152_body.replace(guards["dt_prepare_t8152"], "", 1).replace(
        mem_stmt, mem_stmt + "\n    " + guards["dt_prepare_t8152"], 1
    )
    mutated_after_t8152 = kboot_text.replace(t8152_body, reordered_body, 1)
    with pytest.raises(AssertionError, match="dt_prepare_t8152"):
        extract_kboot_reserve_guards(mutated_after_t8152)

    # Mutate kboot_prepare_dt() to drop its reservation guard -> must fail.
    main_body = extract_func(KBOOT_C, r"int kboot_prepare_dt\s*\(")
    assert guards["kboot_prepare_dt"] in main_body
    mutated_drop_main = kboot_text.replace(
        main_body, main_body.replace(guards["kboot_prepare_dt"], "", 1), 1
    )
    with pytest.raises(AssertionError, match="kboot_prepare_dt"):
        extract_kboot_reserve_guards(mutated_drop_main)


@pytest.mark.skipif(CC is None, reason="no C compiler (cc/clang/gcc) on PATH")
def test_pool_full_diagnostics():
    for name, needle in (
        ("c1_get_l2", "legacy L2 reservation pool full"),
        ("c5_locked_shutdown", "legacy L2 reservation pool full"),
    ):
        src, extra, inc = _case(name)
        r = build_run("diag_" + name, src, extra_srcs=extra, includes=inc)
        assert _ok(r), r
        out = "\n".join(r["lines"])
        assert needle in out, f"{name} did not log {needle!r}:\n{out}"


@pytest.mark.skipif(CC is None, reason="no C compiler (cc/clang/gcc) on PATH")
def test_mutants_all_killed():
    k, n = run_mutants()
    assert k == n, "surviving mutant(s): only %d of %d killed" % (k, n)

# ---------------------------------------------------------------------------
# runtime mutant kills: corrupt one invariant in-memory (never the draft file),
# rebuild, and require the suite to FAIL (returncode != 0). A surviving mutant
# means a test is not actually load-bearing.
# ---------------------------------------------------------------------------
def run_mutants():
    fdt_srcs = _fdt_srcs()
    MUT = [
        # name, template, find, replace, extra_srcs, includes
        ("get_l2: drop the record write", C_GET_L2,
         "legacy_l2_pages[legacy_l2_count++] = (u64)tbl;", "(void)0;", (), ()),
        ("get_l2: drop || dart->locked", C_GET_L2,
         "dart->legacy_mutable || dart->locked", "dart->legacy_mutable", (), ()),
        ("locked_shutdown: drop || dart->locked", C_LOCKED_SHUTDOWN,
         "dart->legacy_mutable || dart->locked", "dart->legacy_mutable", fdt_srcs, (LIBFDT,)),
        ("get_l2: overflow off-by-one (>= -> >)", C_GET_L2,
         "legacy_l2_count >= LEGACY_L2_MAX_PAGES", "legacy_l2_count > LEGACY_L2_MAX_PAGES", (), ()),
        # runtime wrong-free: keep the free (so mock_free stays referenced and the
        # TU still builds under -Werror) but drop the reject return, so an
        # unencodable page is freed and then wrongly recorded/published.
        ("get_l2: round-trip frees then wrongly continues", C_GET_L2,
         "        free(tbl);\n        return NULL;", "        free(tbl);", (), ()),
        # NB: `page < dram_base` is an EQUIVALENT mutant -- a below-base page
        # underflows `page - dram_base` (u64) so the end-of-DRAM term rejects it
        # regardless. The load-bearing bound is the
        # end-of-DRAM term, mutated here:
        ("reserve: drop end-of-DRAM bound", C_RESERVE,
         "page - dram_base > dram_size - SZ_16K", "0", fdt_srcs, (LIBFDT,)),
        ("reserve: drop alignment check", C_RESERVE,
         "(page & (SZ_16K - 1)) ||", "0 ||", fdt_srcs, (LIBFDT,)),
        # ioreport is the failed EP4 16KiB request observed on J293; dropping
        # it from the owned-buffer test flips dcp_init onto the teardown path.
        ("has_owned: drop ioreport term", C_DCP,
         "rtk->ioreport_bfr.owned ||", "", (), ()),
        ("dcp: owned path tears down (goto out_iovad)", C_DCP,
         "goto out_running;", "goto out_iovad;", (), ()),
    ]
    print("\n---------------- runtime mutant kills ----------------")

    # valid control BEFORE: the unmutated suite must be strictly green, else a
    # "kill" could be an artefact of a broken baseline rather than the mutation.
    def _control(tag):
        ok = True
        for nm in CASE_NAMES:
            src, extra, inc = _case(nm)
            r = build_run("ctl_" + nm, src, extra_srcs=extra, includes=inc)
            if not _ok(r):
                ok = False
                print(f"[CONTROL-{tag} FAIL] {nm}: built={r.get('built')} rc={r.get('returncode')} "
                      f"n_result={r.get('n_result')} pass={r.get('pass')} fail={r.get('fail')}")
        if ok:
            print(f"[CONTROL-{tag} OK] all {len(CASE_NAMES)} baseline files strictly green")
        return ok

    control_before = _control("BEFORE")

    kills = 0
    nobuild = 0
    for name, tmpl, find, repl, extra, inc in MUT:
        if find not in tmpl:
            # the mutation point is gone from the draft -> harness is stale, NOT a kill
            print(f"[STALE] mutation pattern not present (cannot mutate): {name}")
            continue
        mutated = tmpl.replace(find, repl, 1)
        r = build_run("mut", mutated, extra_srcs=extra, includes=inc)
        ran = bool(r["built"]) and (r.get("returncode") is not None)
        if not r["built"]:
            # a compile error is NOBUILD: it proves nothing at runtime, so it does
            # NOT count as a kill (and makes kills<total so the gate fails loudly).
            nobuild += 1
            print(f"[NOBUILD] {name} (compile error -- not a valid kill)")
            continue
        killed = ran and not _ok(r)   # must have compiled AND executed AND been caught
        kills += 1 if killed else 0
        print(f"[{'KILL' if killed else 'SURVIVED'}] {name}")

    # valid control AFTER: baseline still green -> mutants left no state behind.
    control_after = _control("AFTER")

    total = len(MUT)
    valid = control_before and control_after and nobuild == 0
    print(f"mutant kills: {kills}/{total}  (nobuild={nobuild}, "
          f"control_before={'ok' if control_before else 'FAIL'}, "
          f"control_after={'ok' if control_after else 'FAIL'})")
    # a NOBUILD or a broken control invalidates the run: report kills as short of
    # total so the pytest gate (k == n) fails.
    return (kills if valid else min(kills, total - 1)), total
