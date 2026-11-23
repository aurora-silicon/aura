# SPDX-License-Identifier: MIT
"""src/secboot.c on the host: what a stage decides from its build and from the records it checks.

The real secboot.c is built with the defines a stage gets from the Makefile. The Ed25519 check
(rust/src/secboot.rs) is stubbed: these tests are about the decisions around it.
"""

from pathlib import Path
import shutil
import subprocess
import sys

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

HEADERS = {
    # src/types.h is for the freestanding target: it defines uintptr_t, ptrdiff_t and ssize_t
    # itself, which a hosted libc whose 64-bit types are not "long" (Darwin) refuses. The host's
    # own types carry the same names here.
    "types.h": r'''
#pragma once
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define UNUSED(x) (void)(x)
''',
    "adt.h": r'''
#pragma once
#include "types.h"
extern void *adt;
struct adt_property { char name[32]; u32 size; u8 value[]; };
int adt_path_offset(const void *adt, const char *path);
const void *adt_getprop(const void *adt, int node, const char *name, u32 *len);
#define ADT_FOREACH_PROPERTY(adt, node, prop)                                                      \
    for (const struct adt_property *prop = NULL; prop; prop = NULL)
''',
    "string.h": "#pragma once\n#include_next <string.h>\n#include <stdlib.h>\n",
    "utils.h": r'''
#pragma once
#include <stdio.h>
#define min(a, b) ((a) < (b) ? (a) : (b))
''',
    "libfdt/libfdt.h": r'''
#pragma once
#include "types.h"
static inline int fdt_path_offset(void *dt, const char *p) { return -1; }
static inline int fdt_add_subnode(void *dt, int n, const char *p) { return -1; }
static inline int fdt_subnode_offset(void *dt, int n, const char *p) { return -1; }
static inline int fdt_setprop(void *dt, int n, const char *p, const void *v, int l) { return 0; }
static inline int fdt_setprop_u32(void *dt, int n, const char *p, u32 v) { return 0; }
static inline int fdt_setprop_string(void *dt, int n, const char *p, const char *v) { return 0; }
static inline const void *fdt_getprop(void *dt, int n, const char *p, int *l) { return NULL; }
static inline int fdt_delprop(void *dt, int n, const char *p) { return 0; }
''',
    "tinf/tinf.h": r'''
#pragma once
unsigned int tinf_crc32(const void *data, unsigned int length);
''',
}

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "secboot.h"
void *adt;
int adt_path_offset(const void *a, const char *p) { return -1; }
const void *adt_getprop(const void *a, int n, const char *p, u32 *l) { return NULL; }
unsigned int tinf_crc32(const void *data, unsigned int length)
{
    unsigned int crc = ~0u;
    for (const unsigned char *p = data; length--; p++) {
        crc ^= *p;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}
void aura_sha256(const void *data, size_t size, u8 digest[32]) { memset(digest, 0x5a, 32); }
/* The Ed25519 check: answers what the test sets. */
static u32 verify_verdict = 1; /* bad-format */
static u8 verify_key_id[8];
static u32 verify_form;
u32 aura_secboot_verify(const u8 *body, size_t body_len, const u8 *covered, size_t covered_len,
                        const u8 *pubkey, u32 min_security_version, u32 form, u8 *digest,
                        u32 *security_version, u8 *key_id)
{
    verify_form = form;
    memset(digest, 0x5a, 32);
    *security_version = 1;
    memcpy(key_id, verify_key_id, 8);
    return verify_verdict;
}
'''


def host_source(text, platform=sys.platform):
    """secboot.c as the host's object format takes it.

    The Stage 1 key block sits in an ELF section the linker script places; Mach-O wants the name
    as "segment,section". Nothing here depends on where the block lands.
    """
    if platform == "darwin":
        text = text.replace('section(".data.stage1_config")', 'section("__DATA,stage1_config")')
    return text


def build(tmp_path, name, defines, test, key=None):
    """Build src/secboot.c with build_cfg.h defines, the stubs and a test main()."""
    src = tmp_path / name / "src"
    stubs = tmp_path / name / "stubs"
    src.mkdir(parents=True)
    (stubs / "libfdt").mkdir(parents=True)
    (stubs / "tinf").mkdir()
    (src.parent / "build").mkdir()
    shutil.copy(ROOT / "src/secboot.h", src / "secboot.h")
    (src / "secboot.c").write_text(host_source((ROOT / "src/secboot.c").read_text()))
    for f, text in HEADERS.items():
        (stubs / f).write_text(text)
    (src.parent / "build/build_cfg.h").write_text("".join(f"#define {d} 1\n" for d in defines))
    pub = key or ROOT / "data/secboot/test-ed25519.pub"
    tool = ["python3", "-I", str(ROOT / "tools/secboot_sign.py"), "c-header", str(pub)]
    # As the Makefile makes it: with the id of the published test key.
    made = subprocess.run(tool + ["--test-pub", str(ROOT / "data/secboot/test-ed25519.pub")],
                          capture_output=True, text=True)
    if made.returncode:  # an older signer
        made = subprocess.run(tool, capture_output=True, text=True, check=True)
    header = made.stdout
    (src.parent / "build/secboot_key.h").write_text(header)
    (src / "test.c").write_text(STUBS + test)
    binary = src.parent / name
    subprocess.run(
        host_cc() + ["-std=gnu11", "-w", "-I", str(src), "-I", str(stubs), str(src / "secboot.c"),
                     str(src / "test.c"), "-o", str(binary)],
        check=True)
    return binary


def run(binary):
    r = subprocess.run([str(binary)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


UNREQUESTED = r'''
/* Older trees have no secboot_unrequested_proxy_allowed(): every window went by the other check. */
bool secboot_unrequested_proxy_allowed(void) __attribute__((weak));
int main(void)
{
    bool asked = secboot_host_proxy_allowed();
    bool unasked = secboot_unrequested_proxy_allowed ? secboot_unrequested_proxy_allowed() : asked;
    printf("asked %d unasked %d\n", asked, unasked);
    return 0;
}
'''


def test_only_a_lab_stage1_gives_an_unrequested_proxy(tmp_path):
    cases = {
        # A release Stage 1: windows someone asks for, nothing else.
        "stage1": (["CHAINLOADING"], "asked 1 unasked 0"),
        "stage1-audit": (["CHAINLOADING", "SECBOOT_AUDIT"], "asked 1 unasked 0"),
        # A lab Stage 1 keeps the recovery window and the fallbacks.
        "stage1-lab": (["CHAINLOADING", "LAB_PROXY"], "asked 1 unasked 1"),
        # Later stages, which an earlier stage chose to start, keep their fallbacks.
        "stage2": (["CHAINLOADING", "ESP_STAGE2"], "asked 1 unasked 1"),
        "plain": ([], "asked 1 unasked 1"),
        # An enforcing stage gives a host nothing.
        "stage1-lab-enforce": (["CHAINLOADING", "LAB_PROXY", "SECBOOT_ENFORCE"],
                               "asked 0 unasked 0"),
        "stage2-enforce": (["CHAINLOADING", "ESP_STAGE2", "SECBOOT_ENFORCE"],
                           "asked 0 unasked 0"),
    }
    for name, (defines, want) in cases.items():
        out = run(build(tmp_path, name, defines, UNREQUESTED))
        assert want in out, (name, out)


TEST_KEY_ID = "0x6e, 0xf2, 0xba, 0xe2, 0xee, 0xe5, 0x77, 0xcb"  # key_id(test-ed25519.pub)

CHAINLOAD = r"""
/* An image with an aura_sig trailer; the stubbed check answers verify_verdict. */
static int chainload(void)
{
    static u8 img[0x100 + 0x1000 + 16];
    size_t size = sizeof(img);
    memcpy(img + 0x100, "aura_sig", 8);
    u64 off = 0x100;
    memcpy(img + size - 16, &off, 8);
    memcpy(img + size - 8, "AURSIGT1", 8);
    char *vars[4];
    size_t n = 0;
    return secboot_check_chainload(img, &size, vars, &n, 4);
}
"""

TEST_KEY = CHAINLOAD + r"""
int main(int argc, char **argv)
{
    static const u8 test_id[8] = {TEST_KEY_ID};
    static const u8 other_id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    verify_verdict = 0;
    memcpy(verify_key_id, argv[1][0] == 't' ? test_id : other_id, 8);
    printf("ret %d\n", chainload());
    return 0;
}
""".replace("TEST_KEY_ID", TEST_KEY_ID)


def test_a_test_key_signature_is_not_reported_as_verified(tmp_path):
    (tmp_path / "own.pub").write_bytes(bytes(range(32)))
    for defines in (["CHAINLOADING", "SECBOOT_ENFORCE"], ["CHAINLOADING", "SECBOOT_AUDIT"],
                    ["CHAINLOADING", "ESP_STAGE2", "SECBOOT_ENFORCE"]):
        binary = build(tmp_path, "-".join(defines), defines, TEST_KEY)
        out = subprocess.run([str(binary), "test"], capture_output=True, text=True).stdout
        # The trusted key signed it: it runs, but as test-key, not verified.
        assert "next stage: test-key" in out and "ret 0" in out, out
        # Another trusted key: verified, as before.
        other = build(tmp_path, "-".join(defines) + "-own", defines, TEST_KEY,
                      key=tmp_path / "own.pub")
        out = subprocess.run([str(other), "own"], capture_output=True, text=True).stdout
        assert "next stage: verified" in out and "ret 0" in out, out


def make_dry(*args):
    return subprocess.run(["make", "-n", "-C", str(ROOT), "CHAINLOADING=1", *args, "build-cfg"],
                          capture_output=True, text=True)


def test_enforcing_and_secure_builds_refuse_the_test_key(tmp_path):
    own = tmp_path / "own.pub"
    own.write_bytes(bytes(range(32)))
    for build_args in (["SECBOOT=enforce"], ["SECBOOT=audit", "SECBOOT_FLAVOUR=secure"],
                       ["SECBOOT=enforce", "SECBOOT_FLAVOUR=secure"],
                       ["SECBOOT=enforce", f"SECBOOT_PUBKEY={ROOT}/data/secboot/test-ed25519.pub"]):
        r = make_dry(*build_args)
        assert r.returncode != 0 and "published test key" in r.stderr, (build_args, r.stderr)
        assert make_dry(*build_args, "SECBOOT_TEST_KEY=1").returncode == 0, build_args
        own_key = [a for a in build_args if not a.startswith("SECBOOT_PUBKEY=")]
        assert make_dry(*own_key, f"SECBOOT_PUBKEY={own}").returncode == 0, build_args
    # Builds that do not enforce keep the default.
    assert make_dry("SECBOOT=audit").returncode == 0
    assert make_dry().returncode == 0


FORMS = CHAINLOAD + r"""
int main(void)
{
    static u8 payloads[0x1000 + 64];
    memcpy(payloads, "aura_sig", 8);
    u32 size = 0x1000;
    memcpy(payloads + 8, &size, 4);
    u64 covered = 64;
    memcpy(payloads + 12 + 24, &covered, 8);
    u8 *next;
    verify_verdict = 0;
    secboot_check_payloads(payloads, &next);
    printf("payloads form %u\n", verify_form);
    chainload();
    printf("image form %u\n", verify_form);
    return 0;
}
"""


def test_each_check_asks_for_its_own_form(tmp_path):
    out = run(build(tmp_path, "forms", ["CHAINLOADING", "SECBOOT_AUDIT"], FORMS))
    assert "payloads form 1" in out and "image form 2" in out, out


CHAIN = r"""
/* Older trees ignore the line. */
void secboot_note_chosen(const char *line) __attribute__((weak));
int main(int argc, char **argv)
{
    char line[200];
    snprintf(line, sizeof(line), "chosen.aurora,measure-0=stage1;%s;next-stage;verified;"
             "6ef2bae2eee577cb;1;%064d", argv[1], 0);
    if (secboot_note_chosen) {
        secboot_note_chosen("chosen.aurora,stage1-flavour=dev");
        secboot_note_chosen(line);
    }
    printf("boot= %d proxy %d\n", secboot_allow_unverified("an ESP boot= kernel"),
           secboot_host_proxy_allowed());
    return 0;
}
"""


def test_a_stage_started_by_an_enforcing_stage_enforces_too(tmp_path):
    # Stage 2 as the distributions build it (no SECBOOT), and an audit one.
    for defines in (["CHAINLOADING", "ESP_STAGE2"], ["CHAINLOADING", "ESP_STAGE2", "SECBOOT_AUDIT"]):
        binary = build(tmp_path, "-".join(defines), defines, CHAIN)
        out = subprocess.run([str(binary), "enforce"], capture_output=True, text=True).stdout
        assert "boot= 0 proxy 0" in out, (defines, out)
        for policy in ("audit", "off"):
            out = subprocess.run([str(binary), policy], capture_output=True, text=True).stdout
            assert "boot= 1 proxy 1" in out, (defines, policy, out)


def test_the_payload_scan_hands_chosen_lines_to_secboot():
    payload = (ROOT / "src/payload.c").read_text()
    branch = payload[payload.index('if (IS_VAR("chosen.")) {'):]
    assert branch.index("secboot_note_chosen((char *)*p);") < branch.index("} else if")
    # boot= and the proxy are decided after the scan that reads the lines.
    run = payload[payload.index("int payload_run(void)"):]
    assert run.index("payload_scanned = true;") < run.index("boot_storage_load(")


def test_the_harness_builds_for_a_darwin_host():
    # CI runs these tests on macOS too: the stubs carry no target-only type, and the key block's
    # section is named the way Mach-O accepts.
    assert "uintptr_t;" not in HEADERS["types.h"] and "ssize_t;" not in HEADERS["types.h"]
    source = (ROOT / "src/secboot.c").read_text()
    assert 'section(".data.stage1_config")' in source
    assert 'section("__DATA,stage1_config")' in host_source(source, "darwin")
    assert host_source(source, "linux") == source
