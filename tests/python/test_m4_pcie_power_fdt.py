# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""PCIe handoff keeps OS ownership and requires the native J713 gP13 action."""
from pathlib import Path
import subprocess


def test_j713_pcie_power_contract(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    text = (repo / "src/pcie_fdt.c").read_text()
    production = "\n".join(line for line in text.splitlines() if not line.startswith("#include"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int smc_dev_t;
#define BIT(n) (1U << (n))
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
/* The T8140 APCIe tables aren't under test: no ADT. */
static void *adt;
static int adt_path_offset(const void *a, const char *path) { return -1; }
static const void *adt_get_property(const void *a, int node, const char *name) { return NULL; }
static int adt_path_offset_trace(const void *a, const char *path, int *offsets) { return -1; }
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len) { return NULL; }
static int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{ return -1; }
static int writes, bypasses;
static int smc_result;
static u32 last_value, last_key;
static u8 expected_sid = 16;
static smc_dev_t smc;
static smc_dev_t *smc_init(void) { return &smc; }
static int smc_write_u32(smc_dev_t *s, u32 key, u32 value)
{ writes++; last_key = key; last_value = value; return smc_result; }
static void smc_shutdown(smc_dev_t *s) {}
static int dart_set_bypass_fdt(void *dt, u32 phandle, u8 sid)
{ assert(phandle == 2 && sid == expected_sid); bypasses++; return 0; }
''' + production + r'''
static void setup(void *dt, bool j713, int fault)
{
 assert(!fdt_create_empty_tree(dt, 8192));
 assert(!fdt_setprop_string(dt, 0, "compatible", j713 ? "apple,j713" : "apple,j715"));
 int gpio = fdt_add_subnode(dt, 0, "gpio");
 assert(gpio >= 0 && !fdt_setprop_string(dt, gpio, "compatible",
                                      fault == 2 ? "apple,smc-low-gpio" : "apple,smc-gpio"));
 assert(!fdt_setprop_u32(dt, gpio, "phandle", 3));
 int dart = fdt_add_subnode(dt, 0, "dart");
 assert(dart >= 0 && !fdt_setprop_string(dt, dart, "compatible", "apple,t8132-dart"));
 assert(!fdt_setprop_u32(dt, dart, "phandle", 2));
 assert(!fdt_setprop_u32(dt, dart, "#iommu-cells", 1));
 int host = fdt_add_subnode(dt, 0, "pcie");
 assert(host >= 0 && !fdt_setprop_string(dt, host, "compatible", "apple,t8132-pcie"));
 fdt32_t map[] = {cpu_to_fdt32(0x100), cpu_to_fdt32(2), cpu_to_fdt32(16), cpu_to_fdt32(1)};
 assert(!fdt_setprop(dt, host, "iommu-map", map, sizeof(map)));
 assert(!fdt_setprop_u32(dt, host, "iommu-map-mask", 0xff00));
 int port = fdt_add_subnode(dt, host, "port");
 fdt32_t pwren[] = {cpu_to_fdt32(3), cpu_to_fdt32(fault == 3 ? 0x14 : 0x13),
                    cpu_to_fdt32(fault == 1 ? 1 : fault == 4 ? 2 : 0)};
 assert(!fdt_setprop(dt, port, "pwren-gpios", pwren, sizeof(pwren)));
 int endpoint = fdt_add_subnode(dt, port, "wifi");
 fdt32_t reg[] = {cpu_to_fdt32(0x10000), 0, 0, 0, 0};
 assert(!fdt_setprop(dt, endpoint, "reg", reg, sizeof(reg)));
}
static void check_admission(void *dt, bool valid)
{
 writes = bypasses = 0;
 assert(pcie_prepare_fdt(dt) == (valid ? 1 : 0));
 assert(writes == valid);
 assert(pcie_handoff_fdt(dt) == (valid ? 1 : -1));
 assert(bypasses == valid);
}
static void test_iommu_admission(void *dt)
{
 const struct { u32 base, offset; bool valid; } streams[] = {
  {0xffffffffU, 1, false}, /* Must not wrap to SID 0. */
  {256, 0, false}, {255, 1, false},
  {255, 0, true}, {254, 1, true}, {0, 0, true},
 };
 for (unsigned i = 0; i < sizeof(streams) / sizeof(*streams); i++) {
  setup(dt, true, 0);
  fdt32_t map[] = {cpu_to_fdt32(0x100), cpu_to_fdt32(2),
                  cpu_to_fdt32(streams[i].base), cpu_to_fdt32(streams[i].offset + 1)};
  assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map", map, sizeof(map)));
  assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/pcie"), "iommu-map-mask", 0xffff));
  fdt32_t reg[] = {cpu_to_fdt32((0x100 + streams[i].offset) << 8), 0, 0, 0, 0};
  assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie/port/wifi"), "reg", reg, sizeof(reg)));
  expected_sid = streams[i].base + streams[i].offset;
  check_admission(dt, streams[i].valid);
 }
 const struct { const char *path, *property; int len; } malformed[] = {
  {"/pcie", "iommu-map", 0}, {"/pcie", "iommu-map", 1},
  {"/pcie", "iommu-map", 15}, {"/pcie", "iommu-map", 17},
  {"/pcie", "iommu-map", 20},
  {"/pcie", "iommu-map-mask", 0}, {"/pcie", "iommu-map-mask", 1},
  {"/pcie", "iommu-map-mask", 3}, {"/pcie", "iommu-map-mask", 5},
  {"/pcie", "iommu-map-mask", 8},
  {"/dart", "#iommu-cells", 0}, {"/dart", "#iommu-cells", 1},
  {"/dart", "#iommu-cells", 3}, {"/dart", "#iommu-cells", 5},
  {"/dart", "#iommu-cells", 8},
  {"/pcie/port/wifi", "reg", 0}, {"/pcie/port/wifi", "reg", 1},
  {"/pcie/port/wifi", "reg", 3}, {"/pcie/port/wifi", "reg", 4},
  {"/pcie/port/wifi", "reg", 5}, {"/pcie/port/wifi", "reg", 8},
  {"/pcie/port/wifi", "reg", 16}, {"/pcie/port/wifi", "reg", 19},
  {"/pcie/port/wifi", "reg", 21}, {"/pcie/port/wifi", "reg", 24},
 };
 for (unsigned i = 0; i < sizeof(malformed) / sizeof(*malformed); i++) {
  setup(dt, true, 0);
  int node = fdt_path_offset(dt, malformed[i].path), len;
  const void *original = fdt_getprop(dt, node, malformed[i].property, &len);
  unsigned char value[32] = {0};
  assert(original && len <= (int)sizeof(value));
  memcpy(value, original, len);
  assert(!fdt_setprop(dt, node, malformed[i].property, value, malformed[i].len));
  check_admission(dt, false);
 }
 /* The optional mask may be absent; the provider cell count may not. */
 setup(dt, true, 0); expected_sid = 16;
 assert(!fdt_delprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map-mask"));
 check_admission(dt, true);
 setup(dt, true, 0);
 assert(!fdt_delprop(dt, fdt_path_offset(dt, "/dart"), "#iommu-cells"));
 check_admission(dt, false);
 setup(dt, true, 0);
 assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/dart"), "#iommu-cells", 2));
 check_admission(dt, false);
 setup(dt, true, 0);
 assert(!fdt_delprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map"));
 check_admission(dt, false);
 /* Linux rejects an id-base with bits outside the mask; do not normalize it. */
 setup(dt, true, 0);
 fdt32_t map[] = {cpu_to_fdt32(0x1ff), cpu_to_fdt32(2), cpu_to_fdt32(16), cpu_to_fdt32(1)};
 assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map", map, sizeof(map)));
 check_admission(dt, false);
 /* A malformed enabled sibling must not redirect selection to a later RID. */
 for (int disabled = 0; disabled <= 1; disabled++) {
  setup(dt, true, 0);
  int bad = fdt_add_subnode(dt, fdt_path_offset(dt, "/pcie/port"), "bad");
  assert(bad >= 0 && !fdt_setprop_u32(dt, bad, "reg", 0x10000));
  if (disabled) assert(!fdt_setprop_string(dt, bad, "status", "disabled"));
  check_admission(dt, disabled);
 }
 /* Skip zero-count and nonmatching entries, but require a supported provider. */
 for (int empty = 0; empty <= 1; empty++) {
  setup(dt, true, 0);
  fdt32_t entries[] = {cpu_to_fdt32(0x200), cpu_to_fdt32(2), 0, cpu_to_fdt32(empty ? 0 : 1),
                      cpu_to_fdt32(0x100), cpu_to_fdt32(2), cpu_to_fdt32(16), cpu_to_fdt32(1)};
  assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map", entries, sizeof(entries)));
  check_admission(dt, true);
 }
 setup(dt, true, 0); map[0] = cpu_to_fdt32(0x100); map[3] = 0;
 assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map", map, sizeof(map)));
 check_admission(dt, false);
 setup(dt, true, 0);
 assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/dart"), "compatible", "apple,t8103-dart"));
 check_admission(dt, false);
}
/* 7.3 trees map the radio to managed ordinary streams; SID16 is still bypassed. */
static void test_managed_streams(void *dt)
{
 const struct { u32 base; fdt32_t managed[3]; int count; bool valid; } cases[] = {
  {1, {cpu_to_fdt32(1), cpu_to_fdt32(2)}, 2, true},
  {2, {cpu_to_fdt32(1), cpu_to_fdt32(2)}, 2, true},
  {3, {cpu_to_fdt32(1), cpu_to_fdt32(2)}, 2, false}, /* Mapped outside the list. */
  {1, {cpu_to_fdt32(16), cpu_to_fdt32(1)}, 2, false}, /* SID16 is never managed, */
  {1, {cpu_to_fdt32(1), cpu_to_fdt32(16)}, 2, false}, /* in either order, */
  {1, {cpu_to_fdt32(1), cpu_to_fdt32(2), cpu_to_fdt32(16)}, 3, false}, /* nor last. */
  {1, {cpu_to_fdt32(1), cpu_to_fdt32(0x100)}, 2, false}, /* A trailing cell that is no SID. */
  {1, {cpu_to_fdt32(1)}, 1, true},
  {1, {0}, 0, false}, /* An empty list is malformed, not absent. */
 };
 for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
  setup(dt, true, 0);
  int host = fdt_path_offset(dt, "/pcie");
  fdt32_t map[] = {cpu_to_fdt32(0x100), cpu_to_fdt32(2), cpu_to_fdt32(cases[i].base),
                   cpu_to_fdt32(1)};
  assert(!fdt_setprop(dt, host, "iommu-map", map, sizeof(map)));
  assert(!fdt_setprop_u32(dt, host, "iommu-map-mask", 0xffff));
  assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "apple,managed-sids", cases[i].managed,
                      cases[i].count * sizeof(fdt32_t)));
  expected_sid = 16;
  check_admission(dt, cases[i].valid);
 }
 /* A list that is not whole cells is refused, even when its first cell matches. */
 setup(dt, true, 0);
 fdt32_t map[] = {cpu_to_fdt32(0x100), cpu_to_fdt32(2), cpu_to_fdt32(1), cpu_to_fdt32(1)};
 assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "iommu-map", map, sizeof(map)));
 assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/pcie"), "iommu-map-mask", 0xffff));
 assert(!fdt_setprop(dt, fdt_path_offset(dt, "/pcie"), "apple,managed-sids", "\0\0\0\1\0", 5));
 expected_sid = 16;
 check_admission(dt, false);
}
static void test_smc_failure_admission(void *dt)
{
 /* smc_cmd returns a positive firmware status, not a negative errno. */
 for (smc_result = -1; smc_result <= 255; smc_result++) {
  if (!smc_result)
   continue;
  setup(dt, true, 0); writes = bypasses = 0;
  assert(pcie_prepare_fdt(dt) < 0);
  assert(writes == 1 && !bypasses);
  /* Follow kboot's optional-radio failure path: no DART bypass follows. */
  assert(!pcie_disable_j713_fdt(dt));
  assert(!pcie_handoff_fdt(dt) && !bypasses);
 }
 smc_result = 0;
}
int main(void)
{
 char blob[8192];
 for (int j713 = 0; j713 <= 1; j713++)
 for (int fault = 0; fault < 5; fault++) {
  setup(blob, j713, fault); writes = bypasses = 0;
  assert(pcie_prepare_fdt(blob) == (!j713 ? 0 : fault ? -1 : 1));
  assert(writes == (j713 && !fault));
  if (writes) {
   assert(last_key == 0x67503133); /* gP13 */
   assert(last_value == 0x00800001U); /* Native pKW4 template + high argument. */
  }
  assert(pcie_handoff_fdt(blob) == j713 && bypasses == j713);
  assert(fdt_getprop(blob, fdt_path_offset(blob, "/pcie/port"), "pwren-gpios", NULL));
  assert(pcie_disable_j713_fdt(blob) == (j713 ? 0 : -1));
  const char *status = fdt_getprop(blob, fdt_path_offset(blob, "/pcie"), "status", NULL);
  assert(j713 ? status && !strcmp(status, "disabled") : !status);
  /* A disabled controller cannot have an endpoint re-admitted by handoff. */
  assert(!pcie_prepare_fdt(blob) && !pcie_handoff_fdt(blob));
 }
 test_iommu_admission(blob);
 test_managed_streams(blob);
 test_smc_failure_admission(blob);
 return 0;
}
'''
    source = tmp_path / "pcie-power.c"
    source.write_text(harness)
    binary = tmp_path / "pcie-power"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / (n + ".c") for n in ("fdt", "fdt_ro", "fdt_rw", "fdt_sw",
               "fdt_wip", "fdt_empty_tree", "fdt_check", "fdt_strerror", "fdt_addresses")]
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-format", "-I", str(libfdt), str(source),
                    *map(str, sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
