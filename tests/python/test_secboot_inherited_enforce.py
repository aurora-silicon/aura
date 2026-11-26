# SPDX-License-Identifier: GPL-2.0-only
"""A Stage 2 that cannot verify anything continues an enforcing chain.

The way this fails on a Mac needs an enforcing Stage 1 enrolled from 1TR, so the logic is
exercised here on the production source. What could go wrong:

1. A Stage 2 built without checks ignores that the Stage 1 which verified it enforced, and loads
   ESP boot= files, which nothing signs.
2. The same Stage 2 chainloads a next stage whose trailer it did not verify.
3. An audit Stage 1's measurement makes Stage 2 refuse, and audit boots break.
4. A /chosen line that is not a measurement, but holds ";enforce;", changes the policy.
5. A measurement that does not fit in the /chosen slots is dropped before its policy is noted.
6. An enforcing build no longer refuses what it cannot verify.
"""

from pathlib import Path
import subprocess

SRC = Path(__file__).resolve().parents[2] / "src"


def between(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def function(source, head):
    at = source.index(head)
    return source[at:source.index("\n}\n", at) + 3]


def test_an_enforcing_earlier_stage_carries_over(tmp_path):
    source = (SRC / "secboot.c").read_text()
    parts = [
        between(source, "static bool inherited_enforce;", "static const char *policy_name(void)"),
        function(source, "bool secboot_allow_unverified(const char *what)"),
        function(source, "int secboot_check_chainload("),
    ]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define UNUSED(x) (void)(x)
#define CHAINLOADING 1
#define ESP_STAGE2 1
#define FLAVOUR_STR "dev"
#define SIG_TAG "aura_sig"
#define SIG_TAG_LEN 8
#define TRAILER_MAGIC "AURSIGT1"
enum secboot_verdict { SECBOOT_VERIFIED = 0, SECBOOT_BAD_FORMAT = 1, SECBOOT_UNSIGNED = 5,
                       SECBOOT_MEASURED = 6 };
struct measurement {
    char component[24];
    u8 sha256[32];
    u8 key_id[8];
    u32 security_version;
    bool signed_record;
    enum secboot_verdict verdict;
};
static const char stage_name[] = "stage2";
static bool all_verified = true, enforcing;
static struct measurement entry;
bool secboot_enforcing(void) { return enforcing; }
static struct measurement *add(const char *c, enum secboot_verdict v)
{ (void)c; memset(&entry, 0, sizeof(entry)); entry.verdict = v; return &entry; }
static int settle(struct measurement *m, const char *what)
{ (void)what; return enforcing && m->verdict != SECBOOT_VERIFIED ? -1 : 0; }
static void aura_sha256(const void *d, size_t s, u8 out[32]) { (void)d; (void)s; memset(out, 0, 32); }
static void hex(char *out, const u8 *in, size_t n) { (void)in; memset(out, '0', 2 * n); out[2 * n] = 0; }
static const char *verdict_name(enum secboot_verdict v) { return v ? "measured" : "verified"; }
static const char *policy_name(void) { return enforcing ? "enforce" : "off"; }
''' + "\n".join(parts) + r'''
static int chainload(u8 *image, size_t size)
{
    char *vars[4];
    size_t cnt = 0;
    return secboot_check_chainload(image, &size, vars, &cnt, 4);
}

int main(void)
{
    /* A next stage with an aura_sig trailer this build cannot check: hashed only. */
    static u8 image[64];
    u64 off = 16;
    memcpy(image + off, SIG_TAG, SIG_TAG_LEN);
    memcpy(image + sizeof(image) - 16, &off, 8);
    memcpy(image + sizeof(image) - 8, TRAILER_MAGIC, 8);

    /* No earlier stage: as before, nothing is refused. */
    assert(secboot_allow_unverified("an ESP boot= kernel"));
    assert(chainload(image, sizeof(image)) == 0);

    /* 3, 4: an audit Stage 1, and lines that are not its measurements, change nothing. */
    secboot_note_chosen("chosen.aurora,measure-0=stage1;audit;next-stage;verified;00;1;00");
    secboot_note_chosen("chosen.asahi,efi-system-partition=x;enforce;y");
    secboot_note_chosen("chosen.aurora,measured=stage1;enforce;next-stage");
    assert(secboot_allow_unverified("an ESP boot= kernel"));
    assert(chainload(image, sizeof(image)) == 0);

    /* 1, 2: the Stage 1 that verified this one enforced. */
    secboot_note_chosen("chosen.aurora,measure-0=stage1;enforce;next-stage;verified;00;1;00");
    assert(!secboot_allow_unverified("an ESP boot= kernel"));
    assert(!all_verified);
    assert(chainload(image, sizeof(image)) == -1);

    /* 6: this stage's own enforce policy refuses as before. */
    enforcing = true;
    assert(!secboot_allow_unverified("an ESP boot= kernel"));
    return 0;
}
'''
    binary = tmp_path / "secboot-inherited-enforce"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_every_chosen_line_is_noted_before_the_slots_are_counted():
    # 5: a measurement that does not fit still carries its policy over.
    source = (SRC / "payload.c").read_text()
    branch = between(source, 'if (IS_VAR("chosen.")) {', "} else if")
    assert branch.index("secboot_note_chosen(") < branch.index("MAX_CHOSEN_VARS")
