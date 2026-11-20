# SPDX-License-Identifier: GPL-2.0-only
"""The HELLO after the run bit: a warm start drops other messages until the HELLO arrives (within
the HELLO second, at most RTKIT_WARM_STALE_MAX); a cold start takes the first message as the
answer, so the adopt path still sees a running coprocessor's traffic as no fresh boot."""

from pathlib import Path
import os
import subprocess

import pytest

from host_compiler import host_cc

HELLO = "0x0010000c000b0000"  # MGMT type 1 (HELLO), versions 11..12
PWR_ACK = "0x00b0000000000020"  # MGMT type 0xb (IOP power state ack)
CASES = {
    # name: (warm, mailbox [(msg0, ep)...], expected)
    "warm-hello-first": (True, [(HELLO, 0)], "ok got=1 no_hello=0"),
    "warm-stale-mgmt-then-hello": (True, [(PWR_ACK, 0), (PWR_ACK, 0), (HELLO, 0)], "ok got=3 no_hello=0"),
    "warm-stale-syslog-then-hello": (True, [("0x1234", 2), (HELLO, 0)], "ok got=2 no_hello=0"),
    "warm-only-stale": (True, [(PWR_ACK, 0)], "fail got=1 no_hello=1"),
    "warm-empty": (True, [], "fail got=0 no_hello=1"),
    "warm-flood": (True, [(PWR_ACK, 0)] * 70 + [(HELLO, 0)], "fail got=64 no_hello=0"),
    "cold-hello": (False, [(HELLO, 0)], "ok got=1 no_hello=0"),
    "cold-stale-first": (False, [(PWR_ACK, 0), (HELLO, 0)], "fail got=1 no_hello=0"),
    "cold-empty": (False, [], "fail got=0 no_hello=1"),
}


def extract(source, start, end_marker="\n}\n"):
    a = source.index(start)
    return source[a:source.index(end_marker, a) + len(end_marker)]


@pytest.mark.parametrize("case", sorted(CASES))
def test_wait_hello(tmp_path, case):
    warm, mbox, expected = CASES[case]
    source = Path(os.environ.get("RTKIT_SOURCE", Path(__file__).resolve().parents[2] / "src/rtkit.c")).read_text()
    fn = extract(source, "static bool rtkit_wait_hello(")
    queue = ",".join(f"{{{m}ULL, {ep}}}" for m, ep in mbox) or "{0, 0}"
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define USEC_PER_SEC 1000000
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzll(m))
#define MGMT_TYPE GENMASK_ULL(59, 52)
#define MGMT_MSG_HELLO 1
#define RTKIT_EP_MGMT 0
#define RTKIT_WARM_STALE_MAX 64
#define rtkit_printf(...) printf(__VA_ARGS__)
struct asc_message { u64 msg0; u32 msg1; };
typedef struct { int *asc; bool no_hello; } rtkit_dev_t;
static struct asc_message q[] = {@QUEUE@};
static int qn = @QN@, got;
static u64 timeout_calculate(u32 us) { return us; }
static bool timeout_expired(u64 t) { return false; }
static bool asc_recv_timeout(int *a, struct asc_message *m, u32 us)
{
    if (got >= qn)
        return false;
    *m = q[got++];
    return true;
}
''' + fn + r'''
int main(void)
{
    rtkit_dev_t rtk = {0};
    struct asc_message msg;
    bool ok = rtkit_wait_hello(&rtk, &msg, @WARM@);
    printf("\n%s got=%d no_hello=%d\n", ok ? "ok" : "fail", got, rtk.no_hello);
    return 0;
}
'''
    harness = (harness.replace("@QUEUE@", queue).replace("@QN@", str(len(mbox)))
               .replace("@WARM@", "true" if warm else "false"))
    c = tmp_path / "t.c"
    c.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-format", "-o", str(exe), str(c)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1] == expected
