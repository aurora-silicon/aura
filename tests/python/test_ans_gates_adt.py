# SPDX-License-Identifier: MIT
"""Which gates pmgr_adt_power_disable_under() powers off for ANS, on real ADTs.

The real pmgr.c runs on the PMGR table and APCIe power-gates of each ADT text dump in
AURA_ADT_DUMPS (proxyclient's format, as in GravityLinux/adt-collection). Skipped without it.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
DUMPS = sorted(Path(os.environ["AURA_ADT_DUMPS"]).glob("*.txt")) if "AURA_ADT_DUMPS" in os.environ else []


def pmgr_devices(text):
    """id2 -> {name, parents, psreg, psidx, virtual}, from the PMGR node's devices list."""
    devices, cur, parents, in_u16 = {}, {}, [], False
    for line in text[text.index("            devices = [Container: "):].splitlines()[1:]:
        s = line.strip()
        m = re.match(r"name = u'([^']*)'", s)
        if m:
            # The next list of the PMGR node has entries without an id2.
            if "id2" not in cur or not line.startswith("    name"):
                break
            cur["name"], cur["parents"] = m.group(1), (parents + [0, 0])[:2]
            devices.setdefault(cur["id2"], cur)
            cur, parents, in_u16 = {}, [], False
        elif s == "u16id = Container:":
            in_u16, parents = True, []
        elif in_u16 and s.isdigit():
            parents.append(int(s))
        elif m := re.match(r"(id2|psreg|psidx|group|offset|no_ps) = (\S+)", s):
            cur[m[1]] = m[2] == "True" if m[1] == "no_ps" else int(m[2], 0)
            in_u16 = in_u16 and m[1] != "psreg"
    return devices


def power_gates(text, node):
    m = re.search(r"\n( *)" + re.escape(node) + r" \{\n", text)
    if not m:
        return None
    for line in text[m.end():].splitlines():
        if not line.startswith(m.group(1) + "    "):
            return None
        if line.strip().startswith("power-gates = ["):
            return [int(x) for x in line.split("[", 1)[1].rstrip("]").split(",")]
    return None


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define FIELD_GET(f, v) (((v) & (f)) / ((f) & -(f)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PACKED __attribute__((packed))
#define COLD_TEXT
#define T8140 0x8140
#define ADT_GETPROP(a, n, p, v) (-1)
static int chip_id;
static void *adt = (void *)1;
static const void *devices_prop;
static u32 devices_len;
extern const u32 ps_regs[], gates[];
extern const u32 ps_regs_len, gates_len;
extern const bool ps_groups;
static int adt_path_offset(const void *a, const char *path) { return 1; }
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    trace[0] = 1;
    trace[1] = 0;
    return 1;
}
static bool adt_is_compatible(const void *a, int node, const char *compat) { return false; }
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    if (!strcmp(name, ps_groups ? "ps-groups" : "ps-regs")) {
        *len = ps_regs_len;
        return ps_regs;
    }
    if (!strcmp(name, "devices")) {
        *len = devices_len;
        return devices_prop;
    }
    if (!strcmp(name, "clock-gates") || !strcmp(name, "power-gates")) {
        *len = gates_len;
        return gates;
    }
    *len = 0;
    return NULL;
}
static int adt_get_reg(const void *a, const int *path, const char *prop, int idx, u64 *addr,
                       u64 *size)
{
    *addr = 0x2d0000000;
    return 0;
}
static u32 read32(uintptr_t addr) { return 0; }
static void write32(uintptr_t addr, u32 v) {}
static u32 mask32(uintptr_t addr, u32 clear, u32 set)
{
    printf("LOWER %lx\n", (unsigned long)addr);
    return 0;
}
static u32 set32(uintptr_t addr, u32 set) { return 0; }
static u32 clear32(uintptr_t addr, u32 clear) { return 0; }
static int poll32(uintptr_t addr, u32 mask, u32 target, u32 timeout) { return 0; }
static void udelay(u32 us) {}
"""


@pytest.mark.skipif(not DUMPS, reason="set AURA_ADT_DUMPS to a directory of ADT text dumps")
@pytest.mark.parametrize("dump", DUMPS, ids=lambda p: p.stem)
def test_ans_gates(tmp_path, dump):
    text = dump.read_text()
    if "            devices = [Container: " not in text:
        pytest.skip(f"{dump.stem}: no PMGR device table")
    devices = pmgr_devices(text)
    node = next((n for n in ("apcie", "apcie0") if power_gates(text, n)), None)
    if node is None:
        pytest.skip(f"{dump.stem}: no apcie or apcie0 node with power-gates")
    gates = power_gates(text, node)

    strip = lambda t: "\n".join(l for l in t.splitlines() if not l.startswith("#include"))
    # Newer PMGRs place each power word by group and offset (ps-groups), older ones by register
    # and index (ps-regs).
    groups = any(d.get("offset") for d in devices.values())
    reg = (lambda d: d.get("group", 0)) if groups else (lambda d: d.get("psreg", 0))
    nregs = max(reg(d) for d in devices.values()) + 1
    table = ",\n".join(
        "{.flags = %d, .id2 = %d, .u16id.parent = {%d, %d}, .psreg_idx = %d, .addr_offset = %d, "
        ".group_and_offset = {.offset = %d, .group = %d}, .name = \"%s\"}"
        % (0x10 if d.get("no_ps") else 0, d["id2"], *d["parents"], d.get("psreg", 0),
           d.get("psidx", 0), d.get("offset", 0), d.get("group", 0), d["name"][:15])
        for d in devices.values())
    addr = (lambda d: 0x2d0000000 + reg(d) * 0x10000 +
            (d.get("offset", 0) if groups else d.get("psidx", 0) * 8))
    program = HARNESS + strip((REPO / "src/pmgr.h").read_text()) + strip((REPO / "src/pmgr.c").read_text())
    program += "const u32 ps_regs[] = {%s};\nconst u32 ps_regs_len = sizeof(ps_regs);\n" % ", ".join(
        "0, %d, 0" % (r * 0x10000) for r in range(nregs))
    program += "const u32 gates[] = {%s};\nconst u32 gates_len = sizeof(gates);\n" % ", ".join(map(str, gates))
    program += "const bool ps_groups = %d;\n" % groups
    program += "static struct pmgr_device table[] = {\n%s};\n" % table
    program += r"""
int main(void)
{
    devices_prop = table;
    devices_len = sizeof(table);
    if (pmgr_init())
        return 2;
    printf("RESULT %d\n", pmgr_adt_power_disable_under("/arm-io/%s", "ANS"));
    return 0;
}
""".replace("%s", node)
    binary = tmp_path / "ans-gates"
    subprocess.run(["cc", "-std=gnu11", "-w", "-x", "c", "-", "-o", str(binary)], input=program,
                   text=True, check=True)
    out = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout

    by_addr = {addr(d): d["name"] for d in devices.values() if not d.get("no_ps")}
    lowered = [by_addr[int(l.split()[1], 16)] for l in out.splitlines() if l.startswith("LOWER ")]
    print(f"{dump.stem}: /arm-io/{node} gates {gates}: lowered {lowered}")
    assert "RESULT 0" in out
    by_name = {d["name"]: d for d in devices.values()}

    def ancestors(name, seen=()):
        d = by_name.get(name)
        if not d or name in seen:
            return set()
        names = {devices[p]["name"] for p in d["parents"] if p in devices}
        return names.union(*(ancestors(n, seen + (name,)) for n in names))

    assert lowered[-1] == "ANS"
    for i, name in enumerate(lowered):
        # Each one is ANS or depends on it, and nothing lowered later depends on it.
        assert name == "ANS" or "ANS" in ancestors(name)
        assert not any(name in ancestors(later) for later in lowered[i + 1:])
