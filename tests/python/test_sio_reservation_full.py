# SPDX-License-Identifier: GPL-2.0-only
"""A failed SIO firmware reservation must leave its FDT node disabled."""

from pathlib import Path
import subprocess


def test_sio_reservation_and_parameter_failures(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    start = source.index("static int dt_set_sio_fwdata(")
    # dt_set_sio_fwdata() and dt_setup_sio(), up to the closing brace of the latter.
    setup = source.index("static int dt_setup_sio(void)", start)
    helper = source[start:source.index("\n}", setup) + 2] + "\n"
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long u64;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define bail(...) do { return -1; } while (0)
#define bail_cleanup(...) do { ret = -1; goto err; } while (0)
struct sio_mapping { u64 phys, iova, size; };
struct sio_fwparam { u32 key, value; };
struct sio_data {
    struct sio_mapping fwdata[1];
    struct sio_fwparam fwparams[2];
    int num_fwdata, num_fwparams;
};
static void *dt;
static int enabled, reserve_calls, fault, append_calls;
static int fdt_path_offset(const void *tree, const char *path)
{ (void)tree; return strcmp(path, "sio") == 0 ? 1 : -1; }
static u32 fdt_get_phandle(const void *tree, int node)
{ (void)tree; (void)node; return 1; }
static int fdt_find_max_phandle(const void *tree, u32 *value)
{ (void)tree; *value = 1; return 0; }
static int fdt_setprop_u32(void *tree, int node, const char *name, u32 value)
{ (void)tree; (void)node; (void)name; (void)value; return 0; }
static struct sio_data *sio_setup_fwdata(const char *path)
{
    assert(strcmp(path, "/arm-io/sio") == 0);
    struct sio_data *data = calloc(1, sizeof(*data));
    assert(data);
    data->num_fwparams = 2;
    data->num_fwdata = 1;
    data->fwdata[0] = (struct sio_mapping){0x100000, 0x200000, 0x4000};
    return data;
}
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                      bool no_map, u64 phys, u64 size)
{
    assert(strncmp(name, "sio-firmware-data@", 18) == 0);
    assert(strcmp(compat, "apple,asc-mem") == 0 && no_map);
    assert(phys == 0x100000 && size == 0x4000);
    reserve_calls++;
    return fault == 1 ? -3 : 2;
}
static int dt_device_set_reserved_mem(int node, const char *name, u32 phandle,
                                      u64 iova, u64 size)
{ (void)node; (void)name; (void)phandle; (void)iova; (void)size; return fault == 2 ? -3 : 0; }
static int dt_device_add_mem_region(const char *path, u32 phandle, const char *name)
{ (void)path; (void)phandle; (void)name; return fault == 3 ? -3 : 0; }
static int fdt_appendprop_u32(void *tree, int node, const char *name, u32 value)
{ (void)tree; (void)node; (void)name; (void)value; append_calls++; return fault == 3 + append_calls ? -3 : 0; }
static int dt_reserve_asc_firmware(const char *adt_path, const char *alt,
                                   const char *fdt_path, bool no_map, u64 offset)
{ (void)adt_path; (void)alt; (void)fdt_path; (void)no_map; (void)offset; return 0; }
static int fdt_setprop_string(void *tree, int node, const char *name, const char *value)
{ (void)tree; (void)node; (void)name; (void)value; enabled++; return 0; }
''' + helper + r'''
int main(void)
{
    for (fault = 1; fault <= 8; fault++) {
        enabled = reserve_calls = append_calls = 0;
        assert(dt_setup_sio() == 0);
        assert(reserve_calls == 1);
        assert(enabled == (fault == 8));
    }
    return 0;
}
'''
    binary = tmp_path / "sio-reservation-full"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                    "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
