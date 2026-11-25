/* SPDX-License-Identifier: MIT */
/*
 * Host check of fdt_disable_node() (src/fdt_disable.c) with libfdt: one tree with room and one
 * packed with no free space at all are built with libfdt's sequential writer, and each case runs on
 * a fresh copy of one.
 *
 * src/fdt_disable.c is built with -Dfdt_setprop=fdt_setprop_test (tests/run-fdt-disable.sh), so its
 * status edit can be made to fail on a tree that has room.
 */
#include <stdio.h>
#include <string.h>

#include "fdt_disable.h"

#include "libfdt/libfdt.h"

#define TREE_SIZE 4096

static int fail_status;

int fdt_setprop_test(void *fdt, int node, const char *name, const void *val, int len)
{
    if (fail_status)
        return -FDT_ERR_NOSPACE;
    return fdt_setprop(fdt, node, name, val, len);
}

static void node(void *fdt, const char *name, const char *status, uint32_t phandle)
{
    fdt_begin_node(fdt, name);
    fdt_property_string(fdt, "compatible", "test,node");
    fdt_property_u32(fdt, "phandle", phandle);
    if (status)
        fdt_property_string(fdt, "status", status);
    fdt_property_u32(fdt, "reg", 1);
    fdt_begin_node(fdt, "audio");
    fdt_property_u32(fdt, "phandle", phandle + 100);
    fdt_end_node(fdt);
    fdt_end_node(fdt);
}

/* Three enabled AOPs, whose status is "okay", "ok" or absent; packed: no free space */
static int build(void *fdt)
{
    fdt_create(fdt, TREE_SIZE);
    fdt_finish_reservemap(fdt);
    fdt_begin_node(fdt, "");
    node(fdt, "okay@1", "okay", 11);
    node(fdt, "ok@2", "ok", 22);
    node(fdt, "none@3", NULL, 33);
    fdt_end_node(fdt);
    fdt_finish(fdt);
    return fdt_totalsize(fdt);
}

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

static const char *prop(const void *fdt, const char *path, const char *name)
{
    int off = fdt_path_offset(fdt, path);
    if (off < 0)
        return "<gone>";
    const char *s = fdt_getprop(fdt, off, name, NULL);
    return s ? s : "-";
}

int main(void)
{
    static char packed[TREE_SIZE], roomy[TREE_SIZE], fdt[TREE_SIZE];
    int size = build(packed), ret;

    memcpy(roomy, packed, TREE_SIZE);
    fdt_open_into(roomy, roomy, TREE_SIZE);
    check(!fdt_check_full(packed, size) && !fdt_check_full(roomy, TREE_SIZE),
          "the test trees are valid");

    static const char *const nodes[] = {"/okay@1", "/ok@2", "/none@3"};
    for (int i = 0; i < 3; i++) {
        char what[96];
        memcpy(fdt, roomy, TREE_SIZE);
        ret = fdt_disable_node(fdt, fdt_path_offset(fdt, nodes[i]));
        snprintf(what, sizeof(what), "with room, %s: status \"disabled\", compatible kept",
                 nodes[i]);
        check(!ret && !strcmp(prop(fdt, nodes[i], "status"), "disabled") &&
                  !strcmp(prop(fdt, nodes[i], "compatible"), "test,node") &&
                  !fdt_check_full(fdt, TREE_SIZE),
              what);
    }

    memcpy(fdt, packed, TREE_SIZE);
    ret = fdt_disable_node(fdt, fdt_path_offset(fdt, "/okay@1"));
    check(!ret && !strcmp(prop(fdt, "/okay@1", "status"), "fail") &&
              fdt_size_dt_struct(fdt) == fdt_size_dt_struct(packed) && !fdt_check_full(fdt, size),
          "no free space, \"okay\": \"fail\" in place, the tree the same size");

    memcpy(fdt, packed, TREE_SIZE);
    ret = fdt_disable_node(fdt, fdt_path_offset(fdt, "/ok@2"));
    check(!ret && fdt_path_offset(fdt, "/ok@2") < 0 && fdt_path_offset(fdt, "/ok@2/audio") < 0 &&
              fdt_node_offset_by_phandle(fdt, 22) < 0 && fdt_node_offset_by_phandle(fdt, 122) < 0 &&
              fdt_path_offset(fdt, "/okay@1") >= 0 && fdt_path_offset(fdt, "/none@3") >= 0 &&
              fdt_size_dt_struct(fdt) == fdt_size_dt_struct(packed) && !fdt_check_full(fdt, size),
          "no free space, \"ok\": whole node, child and phandles NOP'd, siblings kept");

    memcpy(fdt, packed, TREE_SIZE);
    ret = fdt_disable_node(fdt, fdt_path_offset(fdt, "/none@3"));
    check(!ret && fdt_path_offset(fdt, "/none@3") < 0 &&
              fdt_path_offset(fdt, "/none@3/audio") < 0 &&
              fdt_node_offset_by_phandle(fdt, 33) < 0 && fdt_node_offset_by_phandle(fdt, 133) < 0 &&
              fdt_path_offset(fdt, "/okay@1") >= 0 && fdt_path_offset(fdt, "/ok@2") >= 0 &&
              fdt_size_dt_struct(fdt) == fdt_size_dt_struct(packed) && !fdt_check_full(fdt, size),
          "no free space, no status: whole node, child and phandles NOP'd, siblings kept");

    memcpy(fdt, roomy, TREE_SIZE);
    fail_status = 1;
    ret = fdt_disable_node(fdt, fdt_path_offset(fdt, "/okay@1"));
    fail_status = 0;
    check(!ret && !strcmp(prop(fdt, "/okay@1", "status"), "fail") &&
              !fdt_check_full(fdt, TREE_SIZE),
          "the status edit fails on \"okay\": \"fail\" in place");

    memcpy(fdt, roomy, TREE_SIZE);
    fail_status = 1;
    ret = fdt_disable_node(fdt, fdt_path_offset(fdt, "/ok@2"));
    fail_status = 0;
    check(!ret && fdt_path_offset(fdt, "/ok@2") < 0 && fdt_node_offset_by_phandle(fdt, 22) < 0 &&
              fdt_path_offset(fdt, "/okay@1") >= 0 && !fdt_check_full(fdt, TREE_SIZE),
          "the status edit fails on \"ok\": whole node NOP'd");

    memcpy(fdt, packed, TREE_SIZE);
    ret = fdt_disable_node(fdt, -FDT_ERR_NOTFOUND);
    check(ret < 0 && !memcmp(fdt, packed, size), "no node: an error, the tree unchanged");

    printf(failures ? "fdt disable: %d FAILED\n" : "fdt disable: all cases pass\n", failures);
    return !!failures;
}
