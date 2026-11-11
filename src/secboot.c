/* SPDX-License-Identifier: MIT */

/*
 * Signed payloads and the measured-boot hand-off record (docs/secure-boot.md).
 *
 * Every aura stage measures (SHA-256) what it hands control or data to: the DTB it selected, the
 * kernel or U-Boot payload, an initramfs, the next aura stage. With SECBOOT=audit or enforce it
 * also checks an Ed25519 signature over them with the key built into this stage. The results go
 * to the next stage in /chosen/aurora,boot-security, together with what iBoot reports about the
 * LocalPolicy that admitted Stage 1 (ADT /chosen/asmb), so Linux can say how it was booted.
 *
 * A stage that is not itself verified by the stage before it can claim anything, so the record
 * is only as good as the first stage that enforces: see the design note's threat model.
 */

#include "../build/build_cfg.h"

#include "secboot.h"
#include "adt.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"
#include "tinf/tinf.h"

#if defined(SECBOOT_ENFORCE) || defined(SECBOOT_AUDIT)
#include "../build/secboot_key.h" /* secboot_pubkey[32], from SECBOOT_PUBKEY */
#define SECBOOT_CHECKS 1
#endif

/*
 * Two flavours (SECBOOT_FLAVOUR): "dev" keeps the USB host windows a lab needs (recovery window,
 * aura-proxy, DebugUSB) unless the stage enforces; "secure" never opens one.
 */
#ifdef SECBOOT_SECURE_FLAVOUR
#define FLAVOUR_STR "secure"
#else
#define FLAVOUR_STR "dev"
#endif
static const char flavour_name[] = FLAVOUR_STR;

#ifdef ESP_STAGE2
static const char stage_name[] = "stage2";
#elif defined(CHAINLOADING)
static const char stage_name[] = "stage1";
#else
static const char stage_name[] = "aura";
#endif

#ifndef SECBOOT_MIN_SECURITY_VERSION
#define SECBOOT_MIN_SECURITY_VERSION 0
#endif

#define KEY_FLAG_ENFORCE 1u /* key block flags: the installer chose enforce for this Stage 1 */

u32 aura_secboot_verify(const u8 *body, size_t body_len, const u8 *covered, size_t covered_len,
                        const u8 *pubkey, u32 min_security_version, u8 *digest,
                        u32 *security_version, u8 *key_id);
void aura_sha256(const void *data, size_t size, u8 digest[32]);

#define SIG_TAG       "aura_sig"
#define SIG_TAG_LEN   8
#define SIG_BODY      104 /* header 40 + signature 64 */
#define TRAILER_MAGIC "AURSIGT1"

#define MAX_MEASUREMENTS 12

struct measurement {
    char component[24];
    u8 sha256[32];
    u8 key_id[8];
    u32 security_version;
    bool signed_record;
    enum secboot_verdict verdict;
};

static struct measurement mlog[MAX_MEASUREMENTS];
static size_t log_cnt;
/* Every link this stage checked was verified, and at least one was. */
static bool all_verified = true;
static bool any_verified = false;
#ifdef SECBOOT_CHECKS
/* The end of the bytes a verified aura_sig record covers. */
static const u8 *covered_end;
#endif

static const char *verdict_name(enum secboot_verdict v)
{
    switch (v) {
        case SECBOOT_VERIFIED:
            return "verified";
        case SECBOOT_BAD_FORMAT:
            return "bad-format";
        case SECBOOT_UNKNOWN_KEY:
            return "unknown-key";
        case SECBOOT_BAD_SIGNATURE:
            return "bad-signature";
        case SECBOOT_ROLLBACK:
            return "rollback";
        case SECBOOT_UNSIGNED:
            return "unsigned";
        case SECBOOT_MEASURED:
        default:
            return "measured";
    }
}

static void hex(char *out, const u8 *in, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * len] = 0;
}

static struct measurement *add(const char *component, enum secboot_verdict verdict)
{
    if (log_cnt >= MAX_MEASUREMENTS) {
        printf("secboot: measurement log full, %s not recorded\n", component);
        all_verified = false;
        return NULL;
    }
    struct measurement *m = &mlog[log_cnt++];
    memset(m, 0, sizeof(*m));
    strncpy(m->component, component, sizeof(m->component) - 1);
    m->verdict = verdict;
    return m;
}

void secboot_measure(const char *component, const void *data, size_t size,
                     enum secboot_verdict verdict)
{
    struct measurement *m = add(component, verdict);
    if (!m)
        return;
    if (data && size)
        aura_sha256(data, size, m->sha256);
    char h[17];
    hex(h, m->sha256, 8);
    printf("secboot: %s %s 0x%lx bytes sha256 %s...\n", component, verdict_name(verdict), size, h);
}

#ifdef SECBOOT_CHECKS
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
/*
 * Keys an installer fills into a Stage 1 image before it is enrolled, so they are covered by the
 * LocalPolicy's coih like the rest of the image (tools/secboot_sign.py fill-keys). An unfilled
 * block leaves the built-in key in charge.
 */
#define KEY_BLOCK_KEYS 4
struct secboot_key_block {
    char magic[16];
    u32 version;
    u32 count;
    u32 min_security_version;
    u32 flags;
    u8 keys[KEY_BLOCK_KEYS][32];
    u32 crc32;
};
_Static_assert(sizeof(struct secboot_key_block) == 164, "secboot key block layout changed");

static struct secboot_key_block key_block __attribute__((section(".data.stage1_config"), used)) = {
    .magic = "AURORA-S1-KEY01",
    .version = 1,
};

static bool key_block_valid(void)
{
    static int valid = -1;
    if (valid < 0) {
        u32 crc = tinf_crc32(&key_block.version, offsetof(struct secboot_key_block, crc32) -
                                                     offsetof(struct secboot_key_block, version));
        valid = key_block.version == 1 && key_block.count >= 1 &&
                key_block.count <= KEY_BLOCK_KEYS && !(key_block.flags & ~KEY_FLAG_ENFORCE) &&
                crc == key_block.crc32;
        if (key_block.count && !valid)
            printf("secboot: the Stage 1 key block is damaged; using the built-in key\n");
    }
    return valid;
}

static bool block_enforces(void)
{
    return key_block_valid() && (key_block.flags & KEY_FLAG_ENFORCE);
}
#else
static bool block_enforces(void)
{
    return false;
}
#endif
#else
static bool block_enforces(void)
{
    return false;
}
#endif

bool secboot_enforcing(void)
{
#ifdef SECBOOT_ENFORCE
    return true;
#else
    return block_enforces();
#endif
}

static const char *policy_name(void)
{
#ifdef SECBOOT_CHECKS
    return secboot_enforcing() ? "enforce" : "audit";
#else
    return "off";
#endif
}

#ifdef SECBOOT_CHECKS
/* The trusted key whose id the record names, and the floor for its security version. */
static const u8 *trusted_key(const u8 *record, u32 *min_secver)
{
    *min_secver = SECBOOT_MIN_SECURITY_VERSION;
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
    if (key_block_valid()) {
        if (key_block.min_security_version > *min_secver)
            *min_secver = key_block.min_security_version;
        for (u32 i = 0; i < key_block.count; i++) {
            u8 id[32];
            aura_sha256(key_block.keys[i], 32, id);
            if (!memcmp(id, record + SIG_TAG_LEN + 4 + 32, 8))
                return key_block.keys[i];
        }
        return key_block.keys[0]; /* reported as unknown-key */
    }
#else
    UNUSED(record);
#endif
    return secboot_pubkey;
}

static enum secboot_verdict check_record(const u8 *record, size_t record_size, const u8 *covered,
                                         size_t covered_len, struct measurement *m)
{
    if (record_size < SIG_TAG_LEN + 4 + SIG_BODY)
        return SECBOOT_BAD_FORMAT;
    u32 min_secver;
    const u8 *key = trusted_key(record, &min_secver);
    u32 v = aura_secboot_verify(record + SIG_TAG_LEN + 4, record_size - SIG_TAG_LEN - 4, covered,
                                covered_len, key, min_secver, m->sha256, &m->security_version,
                                m->key_id);
    m->signed_record = true;
    return v <= SECBOOT_ROLLBACK ? (enum secboot_verdict)v : SECBOOT_BAD_FORMAT;
}
#endif

static int settle(struct measurement *m, const char *what)
{
    char h[17], k[17];
    hex(h, m->sha256, 8);
    hex(k, m->key_id, 8);
    printf("secboot: %s: %s (key %s, security version %u, sha256 %s...), policy %s\n", what,
           verdict_name(m->verdict), k, m->security_version, h, policy_name());
    if (m->verdict == SECBOOT_VERIFIED) {
        any_verified = true;
        return 0;
    }
    all_verified = false;
    if (secboot_enforcing()) {
        printf("secboot: refusing %s\n", what);
        return -1;
    }
    return 0;
}

int secboot_check_payloads(u8 *p, u8 **next)
{
    *next = p;
    bool is_sig = !memcmp(p, SIG_TAG, SIG_TAG_LEN) || !memcmp(p, "m1n1_sig", SIG_TAG_LEN);
    if (!is_sig) {
#ifdef SECBOOT_CHECKS
        struct measurement *m = add("payloads", SECBOOT_UNSIGNED);
        if (!m)
            return -1;
        return settle(m, "payloads");
#else
        return 0;
#endif
    }
    u32 record_size;
    memcpy(&record_size, p + SIG_TAG_LEN, 4);
    *next = p + record_size;
#ifdef SECBOOT_CHECKS
    struct measurement *m = add("payloads", SECBOOT_BAD_FORMAT);
    if (!m)
        return -1;
    u64 covered_len = 0;
    if (record_size >= SIG_TAG_LEN + 4 + SIG_BODY)
        memcpy(&covered_len, p + SIG_TAG_LEN + 4 + 24, 8);
    /* Bounded by what an ESP Stage 2 can be; the hash runs over this many bytes from *next. */
    if (covered_len && covered_len <= 0x40000000UL)
        m->verdict = check_record(p, record_size, *next, covered_len, m);
    if (m->verdict == SECBOOT_VERIFIED)
        covered_end = *next + covered_len;
    return settle(m, "payloads");
#else
    printf("secboot: payload signature present, not checked (policy off)\n");
    return 0;
#endif
}

u8 *secboot_payload_next(u8 *p)
{
#ifdef SECBOOT_CHECKS
    if (!secboot_enforcing())
        return p;
    static const char *const loader_lines[] = {
        /*
         * What an earlier aura stage appends: /chosen strings (its version, the ESP, the trial,
         * its log, measurements) and the NVMe hand-over. Never a payload, chainload=, boot=,
         * display=, mitigations= or tso=: those must come from the signed bytes.
         */
        "chosen.", "nvme.adopt=", "aura.host-window=", "m1n1.config-lost=", NULL,
    };
    /*
     * Skip what is refused instead of stopping: a stop would also drop the allowed lines after
     * it (nvme.adopt above all, without which a T8140 never reaches its disk again).
     */
    while (covered_end && p >= covered_end && p[0] && memcmp(p + 5, "\x16\x04IMG4", 6)) {
        bool ok = false;
        for (const char *const *l = loader_lines; *l; l++)
            if (!strncmp((const char *)p, *l, strlen(*l)))
                ok = true;
        if (ok)
            return p;
        u8 *nl = memchr(p, '\n', 2048);
        bool text = nl && memchr(p, '=', nl - p);
        for (u8 *c = p; text && c < nl; c++)
            if (*c < 0x20 || *c >= 0x7f)
                text = false;
        if (!text) {
            printf("secboot: ignoring unsigned data after the signed payloads at %p\n", p);
            return NULL;
        }
        printf("secboot: ignoring unsigned line after the signed payloads: %.*s\n",
               (int)min((size_t)(nl - p), (size_t)64), p);
        p = nl + 1;
    }
    return p;
#else
    return p;
#endif
}

bool secboot_host_proxy_allowed(void)
{
#ifdef SECBOOT_SECURE_FLAVOUR
    return false;
#else
    return !secboot_enforcing();
#endif
}

const char *secboot_flavour(void)
{
    return flavour_name;
}

bool secboot_allow_unverified(const char *what)
{
    all_verified = false;
    if (secboot_enforcing()) {
        printf("secboot: %s cannot be verified; refusing it under policy enforce\n", what);
        return false;
    }
    UNUSED(what);
    return true;
}

/*
 * A signed image ends with its aura_sig record and a 16-byte footer: the record's offset (u64 LE)
 * and "AURSIGT1". The record covers every byte before it.
 */
int secboot_check_chainload(void *image, size_t *size, char **vars, size_t *var_cnt,
                            size_t var_capacity)
{
    static char var[sizeof("chosen.aurora,measure-0=") + 160];
    static char flavour_var[] = "chosen.aurora,stage1-flavour=" FLAVOUR_STR;
    u8 *img = image;
    size_t covered = *size;
    struct measurement *m = add("next-stage", SECBOOT_UNSIGNED);
    if (!m)
        return -1;

    if (*size >= 16 && !memcmp(img + *size - 8, TRAILER_MAGIC, 8)) {
        u64 off;
        memcpy(&off, img + *size - 16, 8);
        if (off < *size - 16 && !memcmp(img + off, SIG_TAG, SIG_TAG_LEN)) {
            covered = off;
#ifdef SECBOOT_CHECKS
            m->verdict = check_record(img + off, *size - 16 - off, img, covered, m);
#else
            m->verdict = SECBOOT_MEASURED;
#endif
        } else {
            m->verdict = SECBOOT_BAD_FORMAT;
        }
    }
    if (m->verdict == SECBOOT_UNSIGNED || m->verdict == SECBOOT_MEASURED)
        aura_sha256(img, covered, m->sha256);
    int ret = settle(m, "next stage");
    if (ret)
        return ret;
    *size = covered;

    /* The next stage records it as its first entry (secboot_export_fdt). */
    int n = 0;
    for (size_t i = 0; i < *var_cnt; i++)
        if (!strncmp(vars[i], "chosen.aurora,measure-", 22))
            n++;
    if (*var_cnt >= var_capacity || n > 9) {
        printf("secboot: no room to pass the measurement on\n");
        return 0;
    }
    char h[65], k[17];
    hex(h, m->sha256, 32);
    hex(k, m->key_id, 8);
    snprintf(var, sizeof(var), "chosen.aurora,measure-%d=%s;%s;%s;%s;%s;%u;%s", n, stage_name,
             policy_name(), "next-stage", verdict_name(m->verdict), k, m->security_version, h);
    vars[(*var_cnt)++] = var;
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
    /* Which Stage 1 flavour started the chain; the next stage copies it into the record. */
    if (*var_cnt < var_capacity)
        vars[(*var_cnt)++] = flavour_var;
#else
    UNUSED(flavour_var);
#endif
    return 0;
}

/* iBoot's account of the boot policy, copied as is (names prefixed apple,). */
static const char *const adt_chosen_props[] = {
    "secure-boot",
    "effective-security-mode-ap",
    "effective-production-status-ap",
    "security-domain",
    "crypto-hash-method",
    "boot-manifest-hash",
    "trusted-boot-policy-measurement",
    NULL,
};

static void copy_adt(void *dt, int node)
{
    if (!adt)
        return;
    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen < 0)
        return;
    char name[48];
    for (const char *const *p = adt_chosen_props; *p; p++) {
        u32 len = 0;
        const void *val = adt_getprop(adt, chosen, *p, &len);
        if (!val || len > 256)
            continue;
        snprintf(name, sizeof(name), "apple,%s", *p);
        fdt_setprop(dt, node, name, val, len);
    }
    int asmb = adt_path_offset(adt, "/chosen/asmb");
    if (asmb < 0)
        return;
    ADT_FOREACH_PROPERTY(adt, asmb, prop)
    {
        if (strncmp(prop->name, "lp-", 3) || prop->size > 256)
            continue;
        snprintf(name, sizeof(name), "apple,%s", prop->name);
        fdt_setprop(dt, node, name, prop->value, prop->size);
    }
}

static int add_entry(void *dt, int node, int idx, const char *stage, const char *policy,
                     const struct measurement *m)
{
    char nm[24];
    snprintf(nm, sizeof(nm), "measurement@%d", idx);
    int e = fdt_add_subnode(dt, node, nm);
    if (e < 0)
        return e;
    fdt_setprop_u32(dt, e, "reg", idx);
    fdt_setprop_string(dt, e, "stage", stage);
    fdt_setprop_string(dt, e, "policy", policy);
    fdt_setprop_string(dt, e, "component", m->component);
    fdt_setprop_string(dt, e, "verdict", verdict_name(m->verdict));
    fdt_setprop(dt, e, "sha256", m->sha256, sizeof(m->sha256));
    if (m->signed_record) {
        fdt_setprop(dt, e, "key-id", m->key_id, sizeof(m->key_id));
        fdt_setprop_u32(dt, e, "security-version", m->security_version);
    }
    return 0;
}

static bool unhex(const char *s, size_t len, u8 *out)
{
    for (size_t i = 0; i < len; i++) {
        u8 v = 0;
        for (int j = 0; j < 2; j++) {
            char c = s[2 * i + j];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= c - '0';
            else if (c >= 'a' && c <= 'f')
                v |= c - 'a' + 10;
            else
                return false;
        }
        out[i] = v;
    }
    return true;
}

/* chosen/aurora,measure-N = stage;policy;component;verdict;key-id;security-version;sha256 */
static int import_previous(void *dt, int chosen, int node, int *idx, bool *prev_verified)
{
    char nm[24];
    int found = 0;
    for (int n = 0; n < 10; n++) {
        snprintf(nm, sizeof(nm), "aurora,measure-%d", n);
        int len;
        const char *s = fdt_getprop(dt, chosen, nm, &len);
        if (!s || len <= 0 || len > 200)
            break;
        char buf[200], *f[7];
        memcpy(buf, s, len);
        buf[len - 1] = 0;
        char *c = buf;
        int k = 0;
        for (; k < 7 && c; k++) {
            f[k] = c;
            c = strchr(c, ';');
            if (c)
                *c++ = 0;
        }
        if (k != 7 || strlen(f[6]) != 64)
            continue;
        struct measurement m = {0};
        strncpy(m.component, f[2], sizeof(m.component) - 1);
        m.verdict = !strcmp(f[3], "verified") ? SECBOOT_VERIFIED : SECBOOT_UNSIGNED;
        if (!unhex(f[6], 32, m.sha256))
            continue;
        if (strlen(f[4]) == 16 && unhex(f[4], 8, m.key_id) && strcmp(f[3], "unsigned"))
            m.signed_record = true;
        m.security_version = atol(f[5]);
        /* Only an enforcing stage's "verified" means the next one could not have run otherwise. */
        if (m.verdict != SECBOOT_VERIFIED || strcmp(f[1], "enforce"))
            *prev_verified = false;
        add_entry(dt, node, (*idx)++, f[0], f[1], &m);
        fdt_delprop(dt, chosen, nm);
        found++;
        chosen = fdt_path_offset(dt, "/chosen");
        node = fdt_subnode_offset(dt, chosen, "aurora,boot-security");
    }
    return found;
}

void secboot_export_fdt(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return;
    int node = fdt_add_subnode(dt, chosen, "aurora,boot-security");
    if (node < 0) {
        printf("secboot: couldn't add /chosen/aurora,boot-security (%d)\n", node);
        return;
    }
    fdt_setprop_string(dt, node, "compatible", "aurora,boot-security-v1");
    fdt_setprop_u32(dt, node, "#address-cells", 1);
    fdt_setprop_u32(dt, node, "#size-cells", 0);
    copy_adt(dt, node);

    int idx = 0;
    bool prev_verified = true;
    chosen = fdt_path_offset(dt, "/chosen");
    node = fdt_subnode_offset(dt, chosen, "aurora,boot-security");
    if (!import_previous(dt, chosen, node, &idx, &prev_verified))
        prev_verified = false; /* the stage before this one measured nothing */
    chosen = fdt_path_offset(dt, "/chosen");
    node = fdt_subnode_offset(dt, chosen, "aurora,boot-security");
    for (size_t i = 0; i < log_cnt; i++) {
        if (add_entry(dt, node, idx++, stage_name, policy_name(), &mlog[i]) < 0)
            break;
        chosen = fdt_path_offset(dt, "/chosen");
        node = fdt_subnode_offset(dt, chosen, "aurora,boot-security");
    }

    /*
     * verified: every earlier stage enforced and verified this one, and this stage enforced and
     * verified everything it hands over. partial: this stage verified what it hands over, but an
     * earlier link was not verified. unverified: anything else.
     */
    bool mine = all_verified && any_verified && secboot_enforcing();
    const char *state = mine && prev_verified ? "verified" : mine ? "partial" : "unverified";
    fdt_setprop_string(dt, node, "state", state);
    fdt_setprop_string(dt, node, "policy", policy_name());
    fdt_setprop_string(dt, node, "stage", stage_name);

    /* The Stage 1 flavour: from the Stage 1 that chained this stage, or this stage's own. */
    int flen = 0;
    const char *fl = fdt_getprop(dt, chosen, "aurora,stage1-flavour", &flen);
    char fbuf[8] = {0};
    if (fl && flen > 0 && flen <= (int)sizeof(fbuf) &&
        (!strcmp(fl, "dev") || !strcmp(fl, "secure"))) {
        memcpy(fbuf, fl, flen);
        fdt_delprop(dt, chosen, "aurora,stage1-flavour");
        chosen = fdt_path_offset(dt, "/chosen");
        node = fdt_subnode_offset(dt, chosen, "aurora,boot-security");
        fdt_setprop_string(dt, node, "stage1-flavour", fbuf);
    } else if (!strcmp(stage_name, "stage1") || !strcmp(stage_name, "aura")) {
        fdt_setprop_string(dt, node, "stage1-flavour", flavour_name);
    }
    fdt_setprop_string(dt, node, "flavour", flavour_name);
    printf("secboot: hand-off record: %d measurements, state %s, policy %s, flavour %s\n", idx,
           state, policy_name(), flavour_name);
}
