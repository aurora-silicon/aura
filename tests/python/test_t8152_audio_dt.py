# SPDX-License-Identifier: MIT
"""The T8152 DT path runs speaker safety, amp configuration, and speaker ID before fdt_pack().

Extracts the real dt_set_speaker_safety() and dt_prepare_t8152() from src/kboot.c
and runs them against the in-tree libfdt covering:
- both loader modes (with and without aurora,minimal-boot);
- no CPMS background values in ADT /product/audio (stale node scrubbed, boot continues);
- a complete CPMS profile in ADT /product/audio (node published with all fields);
- a partial CPMS profile in ADT /product/audio (dt_prepare_t8152 fails closed);
- amp configuration and speaker ID running before fdt_pack() while the tree still has room;
- missing /chosen failing closed.
"""
from fdt_harness import PRELUDE, REPO, definition, run

HARNESS = PRELUDE + r'''
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void *adt = (void *)0xad7;
static bool dt_ready;
static char calls[512];
static int amp_written;
static int speaker_id_called;

enum cpms_mode {
    CPMS_NONE,
    CPMS_COMPLETE,
    CPMS_PARTIAL_MISSING_LONG,
    CPMS_PARTIAL_MISSING_ACOUSTIC_ID,
};
static enum cpms_mode cpms_mode;

static int adt_path_offset(const void *tree, const char *path)
{
    assert(tree == adt);
    assert(!strcmp(path, "/product/audio"));
    return 1;
}

static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    static const u32 acoustic_id = 48;
    static const u32 word_val = 0x1234;
    static const u64 long_val = 0x1122334455667788ULL;

    assert(tree == adt && node == 1);
    if (!strcmp(name, "acoustic-id")) {
        if (cpms_mode == CPMS_PARTIAL_MISSING_ACOUSTIC_ID)
            return NULL;
        if (len)
            *len = sizeof(acoustic_id);
        return &acoustic_id;
    }
    if (!strcmp(name, "enabledChannels") || !strcmp(name, "historyChannels") ||
        !strcmp(name, "supportedChannels") || !strcmp(name, "safety-offset-seed-us")) {
        if (len)
            *len = sizeof(word_val);
        return &word_val;
    }
    if (!strcmp(name, "speaker-cpms-bgd_100ms") || !strcmp(name, "speaker-cpms-bgd_1s")) {
        if (cpms_mode == CPMS_NONE)
            return NULL;
        if (len)
            *len = sizeof(long_val);
        return &long_val;
    }
    if (!strcmp(name, "speaker-cpms-bgd_inst")) {
        if (cpms_mode == CPMS_NONE || cpms_mode == CPMS_PARTIAL_MISSING_LONG)
            return NULL;
        if (len)
            *len = sizeof(long_val);
        return &long_val;
    }
    return NULL;
}

#define ADT_GETPROP(tree, node, name, out)                                                        \
    ({                                                                                            \
        u32 _l = 0;                                                                               \
        const void *_p = adt_getprop((tree), (node), (name), &_l);                                \
        if (_p && _l >= sizeof(*(out)))                                                           \
            memcpy((out), _p, sizeof(*(out)));                                                    \
        _p ? (int)_l : -1;                                                                        \
    })

static int leaf(const char *name)
{
    strcat(calls, name);
    strcat(calls, " ");
    return 0;
}

static int dt_set_serial_number(void) { return leaf("serial"); }
static int dt_set_display_t8152(void) { return leaf("display"); }
static int dt_set_live_afk_t8152(void) { return leaf("afk"); }
static int dt_set_gpu(void *tree) { assert(tree == dt); return leaf("gpu"); }
static void dt_set_dcpext_t8152(void) { leaf("dcpext"); }
static int dt_set_dcp_state_t8152(void) { return leaf("dcp_state"); }
static int dt_set_cpus(void) { return leaf("cpus"); }
static int dt_set_mac_addresses(void) { return leaf("mac"); }
static int kboot_setup_atc(void *tree) { assert(tree == dt); return leaf("atc"); }
static int dt_set_acio_tunables(void) { return leaf("acio"); }
static int dt_set_pcie_tunables(void) { return leaf("pcie"); }
static void dt_copy_dart_tunables(const char *const *darts)
{
    assert(darts[0] && !strcmp(darts[0], "dart-acio"));
    assert(darts[1] && !strcmp(darts[1], "dart-apciec"));
    assert(darts[2] == NULL);
    leaf("dart");
}
static int dt_set_fb2_t8152(void) { return leaf("fb2"); }
static int dt_set_memory(void) { return leaf("memory"); }
static void dt_reserve_rtbuddyseg_logs(void) { leaf("logs"); }

static int kboot_audio_amp_config(void *tree, const void *from)
{
    static const u8 row[4] = {0x00, 0x0f, 0x00, 0x02};
    int node = fdt_node_offset_by_compatible(tree, -1, "ti,sn012776");
    assert(tree == dt && from == adt && node >= 0);
    /* Must succeed before fdt_pack() removes free space from the tree. */
    amp_written = (fdt_setprop(tree, node, "apple,speaker-config", row, sizeof(row)) == 0);
    leaf("amp");
    return 0;
}

static int kboot_audio_speaker_id(void *tree, const void *from)
{
    assert(tree == dt && from == adt);
    speaker_id_called = 1;
    leaf("speaker_id");
    return 0;
}

@BODY@

static void build_tree(bool minimal, bool with_chosen, bool with_stale_safety)
{
    static const char aic_compat[] = "apple,t8152-aic3\0apple,t8122-aic3";
    fdt32_t reg38 = cpu_to_fdt32(0x38);

    fresh();
    dt_ready = false;
    calls[0] = '\0';
    amp_written = 0;
    speaker_id_called = 0;

    if (minimal)
        assert(!fdt_setprop(dt, 0, "aurora,minimal-boot", NULL, 0));

    if (with_chosen) {
        int chosen = node_at("/", "chosen");
        if (with_stale_safety) {
            int stale = fdt_add_subnode(dt, chosen, "asahi-speaker-safety");
            assert(stale >= 0);
            assert(!fdt_setprop_u32(dt, stale, "asahi,adt-acoustic-id", 99));
        }
    }

    int soc = node_at("/", "soc");
    int aic = fdt_add_subnode(dt, soc, "interrupt-controller@301000000");
    assert(aic >= 0);
    assert(!fdt_setprop(dt, aic, "compatible", aic_compat, sizeof(aic_compat)));

    int i2c = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "i2c@33101c000");
    assert(i2c >= 0);
    int amp = fdt_add_subnode(dt, i2c, "codec@38");
    assert(amp >= 0);
    assert(!fdt_setprop_string(dt, amp, "compatible", "ti,sn012776"));
    assert(!fdt_setprop(dt, amp, "reg", &reg38, sizeof(reg38)));
}

int main(void)
{
    /* 1. No CPMS profile (J873g 27.x ADT), minimal-boot mode: stale safety scrubbed, amp set. */
    cpms_mode = CPMS_NONE;
    build_tree(true, true, true);
    assert(dt_prepare_t8152() == 0);
    assert(dt_ready);
    assert(amp_written == 1);
    assert(speaker_id_called == 1);
    assert(fdt_path_offset(dt, "/chosen/asahi-speaker-safety") < 0);
    int amp = fdt_node_offset_by_compatible(dt, -1, "ti,sn012776");
    int len = 0;
    const u8 *cfg = fdt_getprop(dt, amp, "apple,speaker-config", &len);
    assert(cfg && len == 4 && cfg[1] == 0x0f);
    assert(!strcmp(calls,
                   "amp speaker_id serial display afk gpu dcpext dcp_state cpus mac "
                   "atc acio pcie dart fb2 memory logs "));

    /* 2. No CPMS profile, non-minimal mode: same audio setup, skips minimal-only steps. */
    cpms_mode = CPMS_NONE;
    build_tree(false, true, true);
    assert(dt_prepare_t8152() == 0);
    assert(dt_ready);
    assert(amp_written == 1);
    assert(speaker_id_called == 1);
    assert(fdt_path_offset(dt, "/chosen/asahi-speaker-safety") < 0);
    assert(!strcmp(calls, "amp speaker_id dcp_state cpus mac atc acio pcie dart fb2 memory logs "));

    /* 3. Complete CPMS profile: replaces stale node and publishes all profile properties. */
    cpms_mode = CPMS_COMPLETE;
    build_tree(true, true, true);
    assert(dt_prepare_t8152() == 0);
    assert(dt_ready);
    assert(amp_written == 1);
    assert(speaker_id_called == 1);
    int safety = fdt_path_offset(dt, "/chosen/asahi-speaker-safety");
    assert(safety >= 0);
    const fdt32_t *aid = fdt_getprop(dt, safety, "asahi,adt-acoustic-id", &len);
    assert(aid && len == 4 && fdt32_to_cpu(*aid) == 48);
    assert(fdt_getprop(dt, safety, "asahi,adt-speaker-cpms-instant-raw", &len) && len == 8);

    /* 4. Partial CPMS profiles: fail closed before touching amps or marking dt_ready. */
    cpms_mode = CPMS_PARTIAL_MISSING_LONG;
    build_tree(true, true, false);
    assert(dt_prepare_t8152() == -1);
    assert(!dt_ready && amp_written == 0 && speaker_id_called == 0);

    cpms_mode = CPMS_PARTIAL_MISSING_ACOUSTIC_ID;
    build_tree(false, true, false);
    assert(dt_prepare_t8152() == -1);
    assert(!dt_ready && amp_written == 0 && speaker_id_called == 0);

    /* 5. Missing /chosen: fails closed. */
    cpms_mode = CPMS_NONE;
    build_tree(true, false, false);
    assert(dt_prepare_t8152() == -1);
    assert(!dt_ready && amp_written == 0 && speaker_id_called == 0);

    puts("t8152-audio-dt ok");
    return 0;
}
'''


def test_t8152_audio_dt(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    body = "\n".join((
        definition(source, "static int dt_set_speaker_safety(void)"),
        definition(source, "static int dt_prepare_t8152(void)"),
    ))
    run(tmp_path, "t8152_audio_dt", HARNESS.replace("@BODY@", body), "t8152-audio-dt ok")
