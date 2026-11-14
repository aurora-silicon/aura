# SPDX-License-Identifier: MIT
"""A DPTX connect reply's unk field echoes the request; it is not a success flag.

EPIC v1 firmware (T6021 / J475c, OS firmware 13.5) is sent unk 0 and answers
unk 0. Rejecting that reply aborted display setup after a connect that had
succeeded, so the HDMI output stayed black until a hotplug. These compile the
real dptxport_connect() and dptxport_validate_connection() against a stand-in
afk_service_call() that returns a chosen reply.
"""
from pathlib import Path
import os
import re
import subprocess

from host_compiler import host_cc

REPO = Path(os.environ.get("AURA_DCP_SOURCE", Path(__file__).resolve().parents[2]))


def function(source, name):
    match = re.search(r"^int " + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint16_t u16;
#define FIELD_PREP(mask, value) ((u32)(value) << (mask))
#define DCPDPTX_REMOTE_PORT_CORE 0
#define DCPDPTX_REMOTE_PORT_DFP 4
#define DCPDPTX_REMOTE_PORT_DIE 8
#define DCPDPTX_REMOTE_PORT_CONNECTED 0x8000
struct dcpdptx_connection_cmd { u32 unk; u32 target; } __attribute__((packed));
struct dcpdptx_connection_reply {
    u32 unk; u32 target; u64 reserved; u32 status; u8 padding[12];
} __attribute__((packed));
typedef struct { bool v2; } afk_epic_ep_t;
typedef struct { afk_epic_ep_t *epic; } afk_epic_service_t;
static bool afk_epic_is_v2(const afk_epic_ep_t *epic) { return epic->v2; }

static u32 sent_unk, reply_unk, reply_target_xor;
static int afk_service_call(afk_epic_service_t *service, u16 group, u32 command, const void *in,
                            size_t in_len, size_t in_extra, void *out, size_t out_len,
                            size_t out_extra)
{
    const struct dcpdptx_connection_cmd *cmd = in;
    struct dcpdptx_connection_reply *resp = out;
    sent_unk = cmd->unk;
    resp->unk = reply_unk;
    resp->target = cmd->target ^ reply_target_xor;
    resp->status = 0;
    return 0;
}
'''

MAIN = r'''
static int connect(bool v2, u32 unk, u32 target_xor)
{
    afk_epic_ep_t ep = {.v2 = v2};
    afk_epic_service_t svc = {.epic = &ep};
    reply_unk = unk;
    reply_target_xor = target_xor;
    return dptxport_connect(&svc, 0, 1, 0);
}

static int validate(u32 unk)
{
    afk_epic_ep_t ep = {.v2 = true};
    afk_epic_service_t svc = {.epic = &ep};
    reply_unk = unk;
    reply_target_xor = 0;
    return dptxport_validate_connection(&svc, 0, 1, 0);
}

int main(void)
{
    /* EPIC v1 sends unk 0, and the DCP echoes it: the connect succeeded */
    assert(connect(false, 0, 0) == 0 && sent_unk == 0);
    /* EPIC v2 sends 0x100 and gets it back */
    assert(connect(true, 0x100, 0) == 0 && sent_unk == 0x100);
    /* A v1 reply of 0x100 was always accepted */
    assert(connect(false, 0x100, 0) == 0);
    /* Any other value is logged, not fatal */
    assert(connect(false, 0x7, 0) == 0);
    /* A reply for another port is still refused */
    assert(connect(false, 0, 0x1) == -1);
    assert(connect(true, 0x100, 0x1) == -1);
    /* validate_connection always sends 0x100 and keeps its check */
    assert(validate(0x100) == 0 && sent_unk == 0x100);
    assert(validate(0) == -1);
    return 0;
}
'''


def test_connect_accepts_the_echoed_unk(tmp_path):
    source = (REPO / "src/dcp/dptx_port_ep.c").read_text()
    harness = HARNESS + "\n".join(function(source, name)
                         for name in ("dptxport_validate_connection", "dptxport_connect"))
    harness += MAIN
    binary = tmp_path / "dptx-connect"
    subprocess.run(
        host_cc() + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
