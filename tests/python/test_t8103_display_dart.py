# SPDX-License-Identifier: MIT
"""T8103 legacy display DART view: admission, mapping and root lifetime.

The C unit is compiled from the production sources named by DART_DISPLAY_SOURCE
(default the repository containing this test): src/types.h, the chip IDs of src/soc.h,
the ALIGN_UP/min macros of src/utils.h, src/adt.h (property layout and
prototypes), src/dart.h and all of src/dart.c. Its structs, constants, DART
parameter tables, constructors and map/unmap/setup bodies are the real ones.

Only these are host models:
- MMIO is a register file that records writes. TLBI completion, DMA ordering
  and the hardware's own use of the tables are not modelled.
- The ADT and FDT are small in-memory trees answering the adt.h/libfdt calls
  dart.c makes. adt_getprop_copy() fails on a missing or wrong-sized property.
- memalign() hands out pages from a fixed low "heap" range; is_heap() reports
  that range; free() of any other low page aborts.
- Physical addresses are fixed low host mappings (MAP_FIXED_NOREPLACE), so the
  unit runs without AddressSanitizer (its shadow occupies that range); it is
  built with UndefinedBehaviorSanitizer and -Wall -Wextra -Werror.
- dart_gen3_tlb_invalidate() is aarch64 inline asm. The unit replaces only that
  body with a stub that aborts, so any Gen3 invalidation fails the test, except
  in the T8152 cases, which allow and count it.

Setup-region validation and dcp_init() early cleanup are deliberately not
covered here: the narrowed candidate keeps dart_setup_pt_region() unchanged,
refusing every locked handle. FDT reservation of the recorded tables and the
boot-failure lifetime are tested separately; fdt_add_mem_rsv() aborts here.
The record checks show that no path publishes an unrecorded table and that a
refused allocation publishes nothing. The order of the record and the L1 store
inside one call is not observable in this single-threaded model.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

import pytest

pytestmark = pytest.mark.skipif(
    sys.platform != "linux", reason="fixed low DART mappings require Linux MAP_FIXED_NOREPLACE"
)

SOURCE = Path(os.environ.get("DART_DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


def read(name):
    return (SOURCE / "src" / name).read_text()


def without_includes(text):
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#include"))


def block(text, start):
    end, depth = text.index("{", start) + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return end


def define_lines(text, names):
    lines = []
    for name in names:
        match = re.search(r"^#define " + re.escape(name) + r"\b.*$", text, re.M)
        assert match, name
        lines.append(match.group(0))
    return "\n".join(lines)


def production_unit():
    """types.h, soc.h chip IDs, utils.h macros, adt.h, dart.h and dart.c."""
    soc = "\n".join(re.findall(r"^#define T[0-9]{4}[A-Z]?\s+0x[0-9a-fA-F]+\s*$", read("soc.h"), re.M))
    assert "#define T8103 0x8103" in soc
    utils = define_lines(read("utils.h"), ("ALIGN_UP", "min", "ARRAY_SIZE"))
    dart = without_includes(read("dart.c"))
    marker = re.search(r"^static void dart_gen3_tlb_invalidate\(dart_dev_t \*dart\)\s*\{", dart, re.M)
    assert marker and dart.count("dart_gen3_tlb_invalidate(dart_dev_t *dart)") == 1
    stub = ("static void dart_gen3_tlb_invalidate(dart_dev_t *dart)\n{\n"
            "    /* Host limit: the production body is aarch64 inline asm. */\n"
            "    (void)dart;\n    assert(gen3_tlbi_allowed && \"Gen3 TLBI reached\");\n"
            "    gen3_tlbis++;\n}")
    dart = dart[:marker.start()] + stub + dart[block(dart, marker.start()):]
    assert "dart_dev_t *dart_init_adt_display(" in dart, "source has no display view"
    return "\n".join((without_includes(read("types.h")), soc, utils, without_includes(read("adt.h")),
                      without_includes(read("dart.h")), MOCK_DECLARATIONS, dart))


MOCK_DECLARATIONS = r'''
/* Host models of the interfaces dart.c uses; definitions follow dart.c. */
u32 chip_id;
static bool gen3_tlbi_allowed;
static unsigned int gen3_tlbis;
static u32 read32(u64 addr);
static void write32(u64 addr, u32 data);
static u32 set32(u64 addr, u32 set);
static int poll32(u64 addr, u32 mask, u32 target, u32 timeout);
#define dma_wmb() ((void)0)
bool is_heap(void *addr);
static void *test_memalign(size_t align, size_t size);
static void *test_calloc(size_t count, size_t size);
static void test_free(void *ptr);
#define memalign(align, size) test_memalign(align, size)
#define calloc(count, size) test_calloc(count, size)
#define free(ptr) test_free(ptr)
int fdt_node_offset_by_phandle(const void *fdt, uint32_t phandle);
const char *fdt_get_name(const void *fdt, int nodeoffset, int *len);
int fdt_node_check_compatible(const void *fdt, int nodeoffset, const char *compatible);
u64 dt_get_address(void *dt, int node);
int fdt_add_mem_rsv(void *fdt, uint64_t address, uint64_t size);
'''

HARNESS = r'''
/* ---- host models ---- */
#define MEM_BASE 0x200000000UL
#define MEM_PAGES 256
#define PAGE(n) (MEM_BASE + (u64)(n) * SZ_16K)
enum { ROOT = 0, L2 = 4, DATA = 16, HEAP = 64, HEAP_PAGES = 64, REGION = 128 };
#define REGS 0x23b000000UL
static u32 regs[0x5000 / 4];
struct reg_write { u64 addr; u32 value; };
static struct reg_write writes[512];
static unsigned int nwrites, reg_reads, tlbis;
static unsigned int heap_used, heap_frees, live_objects, adt_lookups;
static bool heap_exhausted, hand_out_far;
static unsigned int far_frees;
/* Above the T8020 PTE address field (bits 39:14) and never mapped: touching it faults. */
#define FAR_PAGE (1UL << 40)
static u8 *snapshot;

static u32 *reg(u64 addr)
{
    assert(addr >= REGS && addr < REGS + sizeof(regs) && !(addr & 3));
    return &regs[(addr - REGS) / 4];
}
static u32 read32(u64 addr) { reg_reads++; return *reg(addr); }
static void write32(u64 addr, u32 data)
{
    assert(nwrites < 512);
    writes[nwrites++] = (struct reg_write){addr, data};
    *reg(addr) = data;
    if ((addr == REGS + DART_T8020_STREAM_COMMAND && (data & DART_T8020_STREAM_COMMAND_INVALIDATE)) ||
        addr == REGS + DART_T8110_TLB_CMD)
        tlbis++;
}
static u32 set32(u64 addr, u32 set) { write32(addr, read32(addr) | set); return *reg(addr); }
static int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    (void)timeout;
    return (read32(addr) & mask) == target ? 0 : -1;
}
bool is_heap(void *addr) { return (u64)addr >= PAGE(HEAP) && (u64)addr < PAGE(HEAP + HEAP_PAGES); }
static void *test_memalign(size_t align, size_t size)
{
    assert(align == SZ_16K && size == SZ_16K);
    if (hand_out_far)
        return (void *)FAR_PAGE;
    if (heap_exhausted || heap_used == HEAP_PAGES)
        return NULL;
    return (void *)PAGE(HEAP + heap_used++);
}
static void *test_calloc(size_t count, size_t size)
{
    void *p = (calloc)(count, size);
    live_objects += !!p;
    return p;
}
static void test_free(void *ptr)
{
    if (!ptr)
        return;
    if ((u64)ptr == FAR_PAGE) {
        far_frees++;
        return;
    }
    if ((u64)ptr >= MEM_BASE && (u64)ptr < PAGE(MEM_PAGES)) {
        assert(is_heap(ptr) && "freed a page that is not heap");
        heap_frees++;
        return;
    }
    live_objects--;
    (free)(ptr);
}

/* ---- ADT: DART nodes with reg, compatible and properties ---- */
struct node { const char *path, *compat; u64 reg, size; };
static struct node nodes[] = {
    {"/arm-io/dart-dcp", "dart,t8020", REGS, 0x4000},
    {"/arm-io/dart-tlbi-aperture", "", 0x30000000UL, 0x1000},
    {"/arm-io/dart-disp0", "dart,t8020", REGS, 0x4000},
};
#define DART_NODE 0
#define TLBI_NODE 1
#define DISP_NODE 2
struct prop { int node; _Alignas(8) u8 raw[sizeof(struct adt_property) + 32]; };
static struct prop props[16];
static unsigned int nprops, setprops;
static int dummy_adt;
void *adt = &dummy_adt;

static struct adt_property *prop_of(struct prop *p) { return (struct adt_property *)p->raw; }
static void put_prop(int node, const char *name, const void *value, u32 size)
{
    struct prop *p = NULL;
    for (unsigned int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(prop_of(&props[i])->name, name))
            p = &props[i];
    if (!p) {
        assert(nprops < 16);
        p = &props[nprops++];
    }
    assert(size <= 32 && strlen(name) < 32);
    memset(p, 0, sizeof(*p));
    p->node = node;
    strcpy(prop_of(p)->name, name);
    prop_of(p)->size = size;
    memcpy(prop_of(p)->value, value, size);
}
static int node_of(const char *path)
{
    adt_lookups++;
    for (int i = 0; i < (int)ARRAY_SIZE(nodes); i++)
        if (!strcmp(nodes[i].path, path))
            return i;
    return -1;
}
int adt_path_offset(const void *tree, const char *path) { assert(tree == adt); return node_of(path); }
int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    assert(tree == adt);
    int node = node_of(path);
    offsets[0] = node;
    offsets[1] = 0;
    return node;
}
int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    assert(tree == adt && !strcmp(prop, "reg") && idx == 0 && path[0] >= 0);
    *addr = nodes[path[0]].reg;
    if (size)
        *size = nodes[path[0]].size;
    return 0;
}
bool adt_is_compatible(const void *tree, int node, const char *compat)
{
    assert(tree == adt);
    return !strcmp(nodes[node].compat, compat);
}
const struct adt_property *adt_get_property(const void *tree, int node, const char *name)
{
    assert(tree == adt);
    for (unsigned int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(prop_of(&props[i])->name, name))
            return prop_of(&props[i]);
    return NULL;
}
const void *adt_getprop(const void *tree, int node, const char *name, u32 *lenp)
{
    const struct adt_property *p = adt_get_property(tree, node, name);
    if (p && lenp)
        *lenp = p->size;
    return p ? p->value : NULL;
}
int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    const struct adt_property *p = adt_get_property(tree, node, name);
    if (!p || p->size != len)
        return -1;
    memcpy(out, p->value, len);
    return len;
}
int adt_setprop(void *tree, int node, const char *name, void *value, size_t len)
{
    assert(tree == adt);
    setprops++;
    put_prop(node, name, value, len);
    return 0;
}

/* ---- FDT: one DART node, phandle 7 ---- */
static const char *fdt_compat = "apple,t8103-dart";
int fdt_node_offset_by_phandle(const void *fdt, uint32_t phandle) { (void)fdt; return phandle == 7 ? 7 : -1; }
const char *fdt_get_name(const void *fdt, int nodeoffset, int *len) { (void)fdt; (void)nodeoffset; if (len) *len = 4; return "dart"; }
int fdt_node_check_compatible(const void *fdt, int nodeoffset, const char *compatible)
{
    (void)fdt;
    assert(nodeoffset == 7);
    return strcmp(fdt_compat, compatible) ? 1 : 0;
}
u64 dt_get_address(void *dt, int node) { (void)dt; assert(node == 7); return REGS; }
int fdt_add_mem_rsv(void *fdt, uint64_t address, uint64_t size)
{
    (void)fdt; (void)address; (void)size;
    assert(!"reservation is tested separately");
    return -1;
}

/* ---- fixtures ---- */
#define SID 5
#define DCP_PATH "/arm-io/dart-dcp"
static u64 table(unsigned int page) { return FIELD_PREP(DART_T8020_PTE_OFFSET, PAGE(page) >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID; }
static u64 leaf(unsigned int page) { return FIELD_PREP(DART_T8020_PTE_OFFSET, PAGE(page) >> DART_PTE_OFFSET_SHIFT) | dart_t8020.pte_flags; }
static u64 *at(unsigned int page) { return (u64 *)PAGE(page); }
static u64 iova(unsigned int root, unsigned int l1, unsigned int l2) { return ((u64)root << 36) | ((u64)l1 << 25) | ((u64)l2 << 14); }
static u64 tcr_addr(void) { return REGS + DART_T8020_TCR_OFF + 4 * SID; }
static u64 ttbr_addr(int i) { return REGS + DART_T8020_TTBR_OFF + 4 * 4 * SID + 4 * i; }

/* Roots 0-3 hold an L2 at L1 index 5 (page L2 + root), unless absent from @roots. */
static void reset(bool locked, unsigned int roots, u32 tcr, bool enabled)
{
    memset((void *)MEM_BASE, 0, MEM_PAGES * SZ_16K);
    memset(regs, 0, sizeof(regs));
    nwrites = reg_reads = tlbis = heap_used = heap_frees = adt_lookups = nprops = setprops = 0;
    heap_exhausted = hand_out_far = gen3_tlbi_allowed = false;
    far_frees = gen3_tlbis = 0;
    legacy_l2_count = 0;
    memset(legacy_l2_pages, 0, sizeof(legacy_l2_pages));
    chip_id = T8103;
    nodes[DART_NODE].compat = "dart,t8020";
    fdt_compat = "apple,t8103-dart";
    *reg(REGS + DART_T8020_CONFIG) = locked ? DART_T8020_CONFIG_LOCK : 0;
    *reg(REGS + DART_T8020_ENABLED_STREAMS) = enabled ? BIT(SID) : 0;
    *reg(tcr_addr()) = tcr;
    for (int i = 0; i < 4; i++) {
        if (!(roots & BIT(i)))
            continue;
        *reg(ttbr_addr(i)) = DART_T8020_TTBR_VALID |
                             FIELD_PREP(DART_T8020_TTBR_ADDR, PAGE(ROOT + i) >> DART_T8020_TTBR_SHIFT);
        at(ROOT + i)[5] = table(L2 + i);
    }
    u32 vm_base = 0;
    put_prop(DART_NODE, "vm-base", &vm_base, sizeof(vm_base));
}
static void locked_view(void) { reset(true, 0xf, DART_T8020_TCR_TRANSLATE_ENABLE, true); }
static void take_snapshot(void) { memcpy(snapshot, (void *)MEM_BASE, MEM_PAGES * SZ_16K); }
static bool page_unchanged(unsigned int page)
{
    return !memcmp(snapshot + (u64)page * SZ_16K, (void *)PAGE(page), SZ_16K);
}
static bool memory_unchanged(void) { return !memcmp(snapshot, (void *)MEM_BASE, MEM_PAGES * SZ_16K); }
static unsigned int protected_writes(void)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < nwrites; i++) {
        u64 a = writes[i].addr;
        count += a == REGS + DART_T8020_CONFIG || a == REGS + DART_T8020_ENABLED_STREAMS ||
                 a == tcr_addr() || (a >= ttbr_addr(0) && a <= ttbr_addr(3));
    }
    return count;
}
static dart_dev_t *open_view(void)
{
    dart_dev_t *dart = dart_init_adt_display(DCP_PATH, 0, SID);
    assert(dart);
    return dart;
}
/* A locked handle: nothing in its roots, L2s or protected registers may change. */
static void assert_immutable(dart_dev_t *dart)
{
    u64 page = PAGE(DATA);
    at(L2)[8] = leaf(DATA);
    take_snapshot();
    unsigned int before = nwrites;
    assert(dart_map(dart, iova(0, 5, 7), (void *)page, SZ_16K) == -1);
    assert(dart_map(dart, iova(0, 9, 7), (void *)page, SZ_16K) == -1);
    dart_unmap(dart, iova(0, 5, 8), SZ_16K);
    dart_free_l2(dart, iova(0, 5, 0));
    assert(memory_unchanged() && nwrites == before && !heap_used && !heap_frees);
}

/* ---- admission ---- */
static void admit_locked_translating(void)
{
    locked_view();
    dart_dev_t *dart = open_view();
    for (int i = 0; i < 4; i++)
        assert(dart->l1[i] == at(ROOT + i));
    /* Exactly one initial invalidation, of this stream, and no other write. */
    assert(tlbis == 1 && nwrites == 2 && !protected_writes() && !heap_used);
    assert(writes[0].addr == REGS + DART_T8020_STREAM_SELECT && writes[0].value == BIT(SID));
    assert(writes[1].addr == REGS + DART_T8020_STREAM_COMMAND);
    /* The view admits a mapping into an existing L2 of a borrowed root. */
    assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), SZ_16K) == 0);
    assert(at(L2)[7] == leaf(DATA) && !protected_writes() && !heap_used);
    dart_shutdown(dart);
    assert(!live_objects);
}
static void refuse_device(int device)
{
    locked_view();
    assert(!dart_init_adt_display(DCP_PATH, 0, device));
    assert(!adt_lookups && !reg_reads && !nwrites && !heap_used && !live_objects);
}
static void refuse_device_range(void)
{
    refuse_device(-1);
    refuse_device(32);
    refuse_device(255);
}
static void refuse_stream(u32 tcr, bool enabled)
{
    reset(true, 0xf, tcr, enabled);
    take_snapshot();
    assert(!dart_init_adt_display(DCP_PATH, 0, SID));
    assert(!nwrites && !tlbis && !heap_used && !live_objects && memory_unchanged());
}
static void refuse_disabled_stream(void) { refuse_stream(DART_T8020_TCR_TRANSLATE_ENABLE, false); }
static void refuse_untranslated_stream(void)
{
    refuse_stream(0, true);
    refuse_stream(DART_T8020_TCR_TRANSLATE_ENABLE | DART_T8020_TCR_BYPASS_DART, true);
    refuse_stream(DART_T8020_TCR_TRANSLATE_ENABLE | DART_T8020_TCR_BYPASS_DAPF, true);
    refuse_stream(DART_T8020_TCR_BYPASS_DART | DART_T8020_TCR_BYPASS_DAPF, true);
}
static void stock_unlocked_view(void)
{
    /* Unlocked: the stock keep-PTs constructor adopts valid roots, creates the
     * missing one, enables the stream, then one initial invalidation. */
    reset(false, 0x7, 0, false);
    dart_dev_t *dart = open_view();
    for (int i = 0; i < 3; i++)
        assert(dart->l1[i] == at(ROOT + i));
    assert(dart->l1[3] == at(HEAP) && heap_used == 1);
    assert(*reg(ttbr_addr(3)) == (DART_T8020_TTBR_VALID |
           FIELD_PREP(DART_T8020_TTBR_ADDR, PAGE(HEAP) >> DART_T8020_TTBR_SHIFT)));
    assert(*reg(REGS + DART_T8020_ENABLED_STREAMS) & BIT(SID));
    assert(*reg(tcr_addr()) == 0 && tlbis == 1);
    dart_shutdown(dart);
    assert(!live_objects);
}
static void other_chip_unchanged(void)
{
    /* Any other chip gets the generic keep-PTs handle: no invalidation, immutable. */
    locked_view();
    chip_id = T8112;
    dart_dev_t *dart = dart_init_adt_display(DCP_PATH, 0, SID);
    assert(dart && !tlbis && !nwrites);
    assert_immutable(dart);
    dart_shutdown(dart);
    assert(!live_objects);
}
static void gen3_unchanged(void)
{
    /* A Gen3 node on T8103 keeps the Gen3 adoption; the stub aborts on any TLBI. */
    reset(true, 0, 0, false);
    nodes[DART_NODE].compat = "dart,gen3";
    u64 offset = 0x100;
    u32 sid_count = 32;
    put_prop(DART_NODE, "tlbi-aperture-offset", &offset, sizeof(offset));
    put_prop(DART_NODE, "sid-count", &sid_count, sizeof(sid_count));
    *reg(REGS) = 14U << 24;
    *reg(REGS + 8) = 0x300;
    *reg(REGS + DART_T8110_TCR_OFF + 4 * SID) = 1;
    *reg(REGS + DART_T8110_ENABLE_STREAMS) = BIT(SID);
    *reg(REGS + DART_T8110_TTBR_OFF + 4 * SID) =
        DART_T8110_TTBR_VALID | FIELD_PREP(DART_T8110_TTBR_ADDR, PAGE(ROOT) >> DART_T8110_TTBR_SHIFT);
    *reg(REGS + 0x200) = *reg(REGS + 0x208) = 1;
    dart_dev_t *dart = dart_init_adt_display(DCP_PATH, 0, SID);
    assert(dart && dart->type == DART_GEN3 && dart->locked && !nwrites);
    assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), SZ_16K) == -1 && !nwrites);
    dart_shutdown(dart);
    assert(!live_objects);
}

/* A locked two-level Gen3 display stream on @chip, with an iBoot L2 table
 * (page L2) at L1 index 5 of its root. */
static u64 gen3_table(unsigned int page)
{
    return FIELD_PREP(DART_GEN3_PTE_OFFSET, PAGE(page) >> DART_PTE_OFFSET_SHIFT) | DART_PTE_VALID;
}
static u64 gen3_leaf(unsigned int page)
{
    return FIELD_PREP(DART_GEN3_PTE_OFFSET, PAGE(page) >> DART_PTE_OFFSET_SHIFT) |
           dart_gen3.pte_flags;
}
static dart_dev_t *gen3_locked_view(u32 chip)
{
    reset(true, 0, 0, false);
    chip_id = chip;
    nodes[DART_NODE].compat = "dart,gen3";
    u64 offset = 0x100;
    u32 sid_count = 32;
    put_prop(DART_NODE, "tlbi-aperture-offset", &offset, sizeof(offset));
    put_prop(DART_NODE, "sid-count", &sid_count, sizeof(sid_count));
    *reg(REGS) = 14U << 24;
    *reg(REGS + 8) = 0x300;
    *reg(REGS + DART_T8110_TCR_OFF + 4 * SID) = 1;
    *reg(REGS + DART_T8110_ENABLE_STREAMS) = BIT(SID);
    *reg(REGS + DART_T8110_TTBR_OFF + 4 * SID) =
        DART_T8110_TTBR_VALID | FIELD_PREP(DART_T8110_TTBR_ADDR, PAGE(ROOT) >> DART_T8110_TTBR_SHIFT);
    *reg(REGS + 0x200) = *reg(REGS + 0x208) = 1;
    at(ROOT)[5] = gen3_table(L2);
    dart_dev_t *dart = dart_init_adt_display(DCP_PATH, 0, SID);
    assert(dart && dart->type == DART_GEN3 && dart->locked && !nwrites);
    return dart;
}
static void gen3_t8152_tables_writable(void)
{
    /* T8152: the stream configuration stays locked, its page tables do not. */
    dart_dev_t *dart = gen3_locked_view(T8152);
    gen3_tlbi_allowed = true;
    /* Into the inherited L2, and into an absent L1 slot (a new heap L2). */
    assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), SZ_16K) == 0);
    assert(at(L2)[7] == gen3_leaf(DATA));
    assert(dart_translate(dart, iova(0, 5, 7) + 0x10) == (void *)(PAGE(DATA) + 0x10));
    assert(dart_map(dart, iova(0, 6, 0), (void *)PAGE(DATA + 1), SZ_16K) == 0);
    assert(heap_used == 1 && at(ROOT)[6] == gen3_table(HEAP) && at(HEAP)[0] == gen3_leaf(DATA + 1));
    /* Only the tables change: no TTBR/TCR/stream register write, one TLBI each. */
    assert(!nwrites && gen3_tlbis == 2);
    dart_unmap(dart, iova(0, 5, 7), SZ_16K);
    dart_unmap(dart, iova(0, 6, 0), SZ_16K);
    assert(!at(L2)[7] && !at(HEAP)[0] && gen3_tlbis == 4);
    /* Our empty L2 is released; the inherited one stays in its slot. */
    dart_free_l2(dart, iova(0, 6, 0));
    assert(!at(ROOT)[6] && heap_frees == 1);
    dart_free_l2(dart, iova(0, 5, 0));
    assert(at(ROOT)[5] == gen3_table(L2) && heap_frees == 1);
    /* An L2 that still maps a page is kept. */
    assert(dart_map(dart, iova(0, 6, 0), (void *)PAGE(DATA + 1), SZ_16K) == 0);
    assert(at(ROOT)[6] == gen3_table(HEAP + 1));
    dart_free_l2(dart, iova(0, 6, 0));
    assert(at(ROOT)[6] == gen3_table(HEAP + 1) && heap_frees == 1);
    /* Teardown keeps every table the firmware may still walk. */
    take_snapshot();
    dart_shutdown(dart);
    assert(memory_unchanged() && !nwrites && heap_frees == 1);
    assert(!live_objects);
}
static void gen3_other_chip_immutable(void)
{
    /* Every other chip keeps the locked Gen3 stream read-only, tables included. */
    static const u32 chips[] = {T8103, T8122, T6040};
    for (unsigned int i = 0; i < ARRAY_SIZE(chips); i++) {
        dart_dev_t *dart = gen3_locked_view(chips[i]);
        take_snapshot();
        assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), SZ_16K) == -1);
        assert(dart_map(dart, iova(0, 6, 0), (void *)PAGE(DATA), SZ_16K) == -1);
        dart_unmap(dart, iova(0, 5, 7), SZ_16K);
        dart_free_l2(dart, iova(0, 5, 0));
        assert(dart_setup_pt_region(dart, DCP_PATH, SID, 0) == -1);
        assert(memory_unchanged() && !nwrites && !heap_used && !heap_frees && !gen3_tlbis);
        dart_shutdown(dart);
        assert(memory_unchanged() && !live_objects);
    }
}

/* ---- generic and FDT inspection handles stay immutable ---- */
static void generic_locked_immutable(void)
{
    locked_view();
    dart_dev_t *dart = dart_init_adt(DCP_PATH, 0, SID, true);
    assert(dart && !tlbis && !nwrites);
    assert_immutable(dart);
    assert(dart_setup_pt_region(dart, DCP_PATH, SID, 0) == -1);
    dart_shutdown(dart);
    dart = dart_init(REGS, SID, true, DART_T8020);
    assert(dart);
    assert_immutable(dart);
    dart_shutdown(dart);
    assert(!live_objects);
}
static void fdt_locked_immutable(void)
{
    locked_view();
    dart_dev_t *dart = dart_init_fdt(NULL, 7, SID, true);
    assert(dart && !tlbis && !nwrites);
    assert_immutable(dart);
    dart_shutdown(dart);
    /* The inherited-inspection constructor admits only a locked T8110. */
    assert(!dart_init_fdt_locked(NULL, 7, SID));
    assert(!live_objects);
}

/* ---- map and unmap on the view ---- */
static void map_selected_root(void)
{
    for (unsigned int root = 0; root < 4; root++) {
        locked_view();
        dart_dev_t *dart = open_view();
        take_snapshot();
        unsigned int before = tlbis;
        assert(dart_map_flags(dart, iova(root, 5, 7), (void *)PAGE(DATA + root), SZ_16K, 0) == 0);
        /* Exactly the selected root's page: other roots, their L2s and the roots untouched. */
        assert(at(L2 + root)[7] == leaf(DATA + root));
        assert(dart_translate(dart, iova(root, 5, 7) + 0x10) == (void *)(PAGE(DATA + root) + 0x10));
        for (unsigned int page = 0; page < MEM_PAGES; page++)
            if (page != L2 + root)
                assert(page_unchanged(page));
        at(L2 + root)[7] = 0;
        assert(page_unchanged(L2 + root));
        assert(tlbis == before + 1 && !heap_used && !protected_writes() && !legacy_l2_count);
        dart_shutdown(dart);
    }
}
static void unmap_selected_root(void)
{
    for (unsigned int root = 0; root < 4; root++) {
        locked_view();
        for (unsigned int r = 0; r < 4; r++)
            at(L2 + r)[7] = leaf(DATA + r);
        dart_dev_t *dart = open_view();
        take_snapshot();
        unsigned int before = tlbis;
        dart_unmap(dart, iova(root, 5, 7), SZ_16K);
        assert(at(L2 + root)[7] == 0);
        at(L2 + root)[7] = leaf(DATA + root);
        assert(memory_unchanged());
        assert(tlbis == before + 1 && !heap_used && !protected_writes());
        dart_shutdown(dart);
    }
}
static void unmap_absent_allocates_nothing(void)
{
    /* An absent L1 entry in a present root, and an absent root. */
    reset(true, 0xb, DART_T8020_TCR_TRANSLATE_ENABLE, true);
    dart_dev_t *dart = open_view();
    take_snapshot();
    dart_unmap(dart, iova(1, 9, 7), 2 * SZ_16K);
    dart_unmap(dart, iova(2, 5, 7), SZ_16K);
    assert(memory_unchanged() && !heap_used && !protected_writes());
    dart_shutdown(dart);
}
static void map_absent_root_refused(void)
{
    reset(true, 0xb, DART_T8020_TCR_TRANSLATE_ENABLE, true);
    dart_dev_t *dart = open_view();
    take_snapshot();
    assert(dart_map(dart, iova(2, 5, 7), (void *)PAGE(DATA), SZ_16K) == -1);
    assert(memory_unchanged() && !heap_used && !protected_writes());
    dart_shutdown(dart);
}
static void map_new_l2_in_present_root(void)
{
    /* A missing L2 in a present root: one page, published in that root only. */
    locked_view();
    dart_dev_t *dart = open_view();
    take_snapshot();
    assert(dart_map(dart, iova(2, 9, 3), (void *)PAGE(DATA), SZ_16K) == 0);
    assert(heap_used == 1 && at(ROOT + 2)[9] == table(HEAP) && at(HEAP)[3] == leaf(DATA));
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == PAGE(HEAP));
    at(ROOT + 2)[9] = 0;
    at(HEAP)[3] = 0;
    assert(memory_unchanged() && !protected_writes());
    dart_shutdown(dart);
}
static void map_conflict_rolls_back(void)
{
    /* Page two already mapped: page one is withdrawn again and the call fails. */
    locked_view();
    at(L2)[8] = leaf(DATA + 8);
    dart_dev_t *dart = open_view();
    take_snapshot();
    assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), 2 * SZ_16K) == -1);
    assert(memory_unchanged() && !heap_used);
    dart_shutdown(dart);
}
static void map_misaligned_refused(void)
{
    locked_view();
    dart_dev_t *dart = open_view();
    take_snapshot();
    unsigned int before = nwrites;
    assert(dart_map(dart, iova(0, 5, 7), (void *)PAGE(DATA), SZ_16K + 4096) == -1);
    assert(dart_map(dart, iova(0, 5, 7), (void *)(PAGE(DATA) + 4096), SZ_16K) == -1);
    assert(dart_map(dart, iova(0, 5, 7) + 4096, (void *)PAGE(DATA), SZ_16K) == -1);
    assert(memory_unchanged() && nwrites == before && !heap_used);
    dart_shutdown(dart);
}

/* ---- lifetime ---- */
static void shutdown_retains_roots(void)
{
    locked_view();
    dart_dev_t *dart = open_view();
    assert(dart_map(dart, iova(1, 9, 3), (void *)PAGE(DATA), SZ_16K) == 0);
    assert(dart_map(dart, iova(3, 5, 7), (void *)PAGE(DATA + 1), SZ_16K) == 0);
    take_snapshot();
    unsigned int before = nwrites;
    /* A locked view frees no L2 it published and no inherited table. */
    dart_free_l2(dart, iova(1, 9, 0));
    dart_shutdown(dart);
    assert(memory_unchanged() && nwrites == before && !heap_frees && !live_objects);
    /* The record outlives the handle, for the reservation at handoff. */
    assert(legacy_l2_count == 1 && legacy_l2_pages[0] == PAGE(HEAP));
    for (int i = 0; i < 4; i++)
        assert((*reg(ttbr_addr(i)) & DART_T8020_TTBR_VALID));
}
static void setup_refused_on_locked_view(void)
{
    /* Narrowed contract: dart_setup_pt_region() stays read-only for a locked handle. */
    locked_view();
    u64 region[2] = {PAGE(REGION), PAGE(REGION + 4)};
    u64 slot[2] = {0, 0};
    put_prop(DART_NODE, "pt-region-5", region, sizeof(region));
    put_prop(DART_NODE, "l2-tt-5", slot, sizeof(slot));
    dart_dev_t *dart = open_view();
    take_snapshot();
    unsigned int before = tlbis;
    assert(dart_setup_pt_region(dart, DCP_PATH, SID, 0) == -1);
    assert(memory_unchanged() && !setprops && tlbis == before && !heap_used);
    dart_shutdown(dart);
}
static void setup_stock_unlocked(void)
{
    /* Stock positive control: an unlocked handle preallocates its first two L2s. */
    reset(false, 0xf, 0, true);
    u64 region[2] = {PAGE(REGION), PAGE(REGION + 4)};
    u64 slot[2] = {0, 0};
    put_prop(DART_NODE, "pt-region-5", region, sizeof(region));
    put_prop(DART_NODE, "l2-tt-5", slot, sizeof(slot));
    dart_dev_t *dart = open_view();
    unsigned int before = tlbis;
    assert(dart_setup_pt_region(dart, DCP_PATH, SID, 0) == 0);
    assert(at(ROOT)[0] == table(REGION + 1) && at(ROOT)[1] == table(REGION + 2));
    const struct adt_property *p = adt_get_property(adt, DART_NODE, "l2-tt-5");
    u64 published[2];
    memcpy(published, p->value, sizeof(published));
    assert(setprops == 1 && published[0] == PAGE(REGION) && published[1] == 2);
    assert(tlbis == before + 1);
    dart_shutdown(dart);
}

static void disp_path_generic(void)
{
    /* Only the DCP DART gets the view: the DISP DART stays a generic locked handle. */
    locked_view();
    dart_dev_t *dart = dart_init_adt_display("/arm-io/dart-disp0", 0, SID);
    assert(dart && !tlbis && !nwrites);
    assert_immutable(dart);
    dart_shutdown(dart);
    assert(!live_objects && !legacy_l2_count);
}
static void record_list_full(void)
{
    locked_view();
    for (unsigned int i = 0; i < LEGACY_L2_MAX_PAGES - 1; i++)
        legacy_l2_pages[legacy_l2_count++] = PAGE(REGION + i);
    dart_dev_t *dart = open_view();
    /* The last free record admits one new table. */
    assert(dart_map(dart, iova(1, 9, 3), (void *)PAGE(DATA), SZ_16K) == 0);
    assert(legacy_l2_count == LEGACY_L2_MAX_PAGES && heap_used == 1);
    assert(legacy_l2_pages[LEGACY_L2_MAX_PAGES - 1] == PAGE(HEAP) && at(ROOT + 1)[9] == table(HEAP));
    /* Full: refused before allocating, nothing published or recorded. */
    take_snapshot();
    assert(dart_map(dart, iova(2, 9, 3), (void *)PAGE(DATA + 1), SZ_16K) == -1);
    assert(memory_unchanged() && heap_used == 1 && legacy_l2_count == LEGACY_L2_MAX_PAGES);
    /* Existing tables still take mappings. */
    assert(dart_map(dart, iova(2, 5, 7), (void *)PAGE(DATA + 1), SZ_16K) == 0);
    assert(at(L2 + 2)[7] == leaf(DATA + 1) && heap_used == 1);
    dart_shutdown(dart);
}
static void record_unencodable_refused(void)
{
    /* A page the PTE cannot encode is released before it is cleared or published. */
    locked_view();
    dart_dev_t *dart = open_view();
    take_snapshot();
    hand_out_far = true;
    assert(dart_map(dart, iova(1, 9, 3), (void *)PAGE(DATA), SZ_16K) == -1);
    assert(far_frees == 1 && memory_unchanged() && !legacy_l2_count && !protected_writes());
    dart_shutdown(dart);
}

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"admit-locked-translating", admit_locked_translating},
    {"refuse-device-range", refuse_device_range},
    {"refuse-disabled-stream", refuse_disabled_stream},
    {"refuse-untranslated-stream", refuse_untranslated_stream},
    {"stock-unlocked-view", stock_unlocked_view},
    {"other-chip-unchanged", other_chip_unchanged},
    {"gen3-unchanged", gen3_unchanged},
    {"disp-path-generic", disp_path_generic},
    {"generic-locked-immutable", generic_locked_immutable},
    {"fdt-locked-immutable", fdt_locked_immutable},
    {"map-selected-root", map_selected_root},
    {"unmap-selected-root", unmap_selected_root},
    {"unmap-absent-allocates-nothing", unmap_absent_allocates_nothing},
    {"map-absent-root-refused", map_absent_root_refused},
    {"map-new-l2-in-present-root", map_new_l2_in_present_root},
    {"map-conflict-rolls-back", map_conflict_rolls_back},
    {"map-misaligned-refused", map_misaligned_refused},
    {"shutdown-retains-roots", shutdown_retains_roots},
    {"setup-refused-on-locked-view", setup_refused_on_locked_view},
    {"setup-stock-unlocked", setup_stock_unlocked},
    {"gen3-t8152-tables-writable", gen3_t8152_tables_writable},
    {"gen3-other-chip-immutable", gen3_other_chip_immutable},
    {"record-list-full", record_list_full},
    {"record-unencodable-refused", record_unencodable_refused},
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *memory = mmap((void *)MEM_BASE, MEM_PAGES * SZ_16K, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory == (void *)MEM_BASE);
    snapshot = (malloc)(MEM_PAGES * SZ_16K);
    assert(snapshot);
    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        if (strcmp(argv[1], cases[i].name))
            continue;
        cases[i].run();
        printf("PASS %s\n", cases[i].name);
        return 0;
    }
    if (!strcmp(argv[1], "--list")) {
        for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++)
            printf("%s\n", cases[i].name);
        return 0;
    }
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
'''

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
'''

CASES = (
    "admit-locked-translating", "refuse-device-range", "refuse-disabled-stream",
    "refuse-untranslated-stream", "stock-unlocked-view", "other-chip-unchanged",
    "gen3-unchanged", "disp-path-generic", "generic-locked-immutable", "fdt-locked-immutable",
    "map-selected-root", "unmap-selected-root", "unmap-absent-allocates-nothing",
    "map-absent-root-refused", "map-new-l2-in-present-root", "map-conflict-rolls-back",
    "map-misaligned-refused", "shutdown-retains-roots", "setup-refused-on-locked-view",
    "setup-stock-unlocked", "gen3-t8152-tables-writable", "gen3-other-chip-immutable",
    "record-list-full", "record-unencodable-refused",
)


@pytest.fixture(scope="module")
def binary(tmp_path_factory):
    work = tmp_path_factory.mktemp("t8103-display-dart")
    code = work / "t8103-display-dart.c"
    code.write_text(PRELUDE + production_unit() + HARNESS)
    program = work / "t8103-display-dart"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer",
               str(code), "-o", str(program)]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    listed = subprocess.run([str(program), "--list"], capture_output=True, text=True, check=True)
    assert tuple(listed.stdout.split()) == CASES
    return program


@pytest.mark.parametrize("case", CASES)
def test_case(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    output = result.stdout[-4000:] + result.stderr[-4000:]
    assert result.returncode == 0 and ("PASS " + case) in result.stdout, output
