# SPDX-License-Identifier: MIT
"""An AOP whose firmware segments are not reserved is disabled only while it is halted."""
from pathlib import Path
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


@pytest.fixture(scope="module")
def kboot():
    return (ROOT / "src/kboot.c").read_text()


@pytest.fixture(scope="module")
def aop_binary(tmp_path_factory, kboot):
    harness = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
typedef struct { int unused; } asc_dev_t;
static void *dt;
static asc_dev_t the_asc;
static bool have_asc, running, setprop_fails;
static int frees;
static char status[16] = "okay";
static int status_len = sizeof("okay");
static asc_dev_t *asc_init(const char *path)
{
    return have_asc && !strcmp(path, "/arm-io/aop") ? &the_asc : NULL;
}
static void asc_free(asc_dev_t *asc) { frees++; }
static bool asc_cpu_running(asc_dev_t *asc) { return running; }
static int fdt_path_offset(const void *fdt, const char *path) { return 7; }
static const void *fdt_getprop(const void *fdt, int node, const char *name, int *len)
{
    *len = status_len;
    return status_len ? status : NULL;
}
static int fdt_setprop_inplace(void *fdt, int node, const char *name, const void *val, int len)
{
    if (setprop_fails || len != status_len)
        return -1;
    memcpy(status, val, len);
    return 0;
}
static int fdt_setprop_string(void *fdt, int node, const char *name, const char *val)
{
    if (setprop_fails)
        return -1;
    strcpy(status, val);
    status_len = strlen(val) + 1;
    return 0;
}
''' + function(kboot, "static int dt_disable_aop(") + \
        function(kboot, "static int dt_aop_segments_unreserved(") + r'''
int main(int argc, char **argv)
{
    have_asc = atoi(argv[1]);
    running = atoi(argv[2]);
    setprop_fails = atoi(argv[3]);
    if (!atoi(argv[4]))
        status_len = 0; /* no status property */
    int ret = dt_aop_segments_unreserved("/soc/aop@1234");
    fprintf(stderr, "ret=%d status=%s frees=%d\n", ret, status_len ? status : "-", frees);
    return 0;
}
'''
    out = tmp_path_factory.mktemp("aop_unreserved")
    source = out / "aop.c"
    binary = out / "aop"
    source.write_text(harness)
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-parameter", "-o", str(binary), str(source)], check=True)
    return binary


def run(binary, have_asc, running, setprop_fails=0, has_status=1):
    result = subprocess.run([str(binary), str(have_asc), str(running), str(setprop_fails),
                             str(has_status)], check=True, capture_output=True, text=True)
    line = result.stderr.strip().splitlines()[-1]
    return dict(item.split("=") for item in line.split()), result.stdout


def test_halted_aop_is_disabled_in_place(aop_binary):
    r, log = run(aop_binary, 1, 0)
    assert r == {"ret": "0", "status": "fail", "frees": "1"}
    assert "disabling the halted AOP" in log


def test_halted_aop_without_status_is_disabled(aop_binary):
    r, _ = run(aop_binary, 1, 0, has_status=0)
    assert r == {"ret": "0", "status": "disabled", "frees": "1"}


def test_running_aop_refuses_the_handoff(aop_binary):
    # iBoot started it: it runs from pages the OS would get, and a disabled node would not stop
    # it. The node is left as it was and the handoff fails.
    r, log = run(aop_binary, 1, 1)
    assert r == {"ret": "-1", "status": "okay", "frees": "1"}
    assert "while the AOP runs from them" in log


def test_unknown_cpu_state_refuses_the_handoff(aop_binary):
    r, log = run(aop_binary, 0, 0)
    assert r == {"ret": "-1", "status": "okay", "frees": "0"}
    assert "CPU state is unknown" in log


def test_halted_aop_that_cannot_be_disabled_refuses(aop_binary):
    r, log = run(aop_binary, 1, 0, setprop_fails=1)
    assert r["ret"] == "-1" and r["status"] == "okay"
    assert "cannot disable the AOP" in log


def test_every_unreserved_segment_path_checks_the_cpu(kboot):
    body = function(kboot, "static int dt_set_aop_firmware(")
    reserve = body.index('dt_reserve_asc_segments("/arm-io/aop"')
    after = body[reserve:body.index(";", reserve) + 1] + body[body.index(";", reserve) + 1:]
    assert "return dt_aop_segments_unreserved(path);" in after.split("\n")[1]
    assert "dt_disable_aop(" not in body
