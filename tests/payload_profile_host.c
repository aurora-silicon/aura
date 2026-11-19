/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include "payload_profile.h"
#include "t8122_25g83.h"
int main(void)
{
    char legacy[2048], profile[4096];
    assert(!fdt_create_empty_tree(legacy, sizeof(legacy)));
    assert(!fdt_setprop_string(legacy, 0, "compatible", "apple,j613"));
    assert(!fdt_open_into(legacy, profile, sizeof(profile)));
    int dcp = fdt_add_subnode(profile, 0, "dcp");
    assert(dcp >= 0);
    assert(!fdt_setprop_u32(profile, dcp, "apple,j613-25g83-profile", 1));
    fdt32_t fw[] = {cpu_to_fdt32(26), cpu_to_fdt32(6), cpu_to_fdt32(2)};
    assert(!fdt_setprop(profile, dcp, "apple,firmware-compat", fw, sizeof(fw)));
    int aliases = fdt_add_subnode(profile, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(profile, aliases, "dcp", "/dcp"));
    assert(payload_profile_rank(legacy, false) == 1);
    assert(payload_profile_rank(profile, false) == 0);
    assert(payload_profile_rank(legacy, true) == 1);
    assert(payload_profile_rank(profile, true) == 2);
    /* Exact profile wins both archive orders, while legacy remains fallback. */
    for (int order = 0; order < 2; order++) {
        const void *trees[] = {order ? profile : legacy, order ? legacy : profile};
        int rank = 0; const void *selected = NULL;
        for (int i = 0; i < 2; i++) {
            int candidate = payload_profile_rank(trees[i], true);
            if (candidate && candidate >= rank) rank = candidate, selected = trees[i];
        }
        assert(selected == profile);
    }
    dcp = fdt_path_offset(profile, "dcp");
    assert(!fdt_delprop(profile, dcp, "apple,firmware-compat"));
    assert(payload_profile_rank(profile, true) == 0);
    assert(!fdt_setprop(profile, dcp, "apple,firmware-compat", fw, sizeof(fw)));
    assert(!fdt_setprop_u32(profile, dcp, "apple,j613-25g83-profile", 2));
    assert(payload_profile_rank(profile, true) == 0);
    assert(!fdt_setprop_u32(profile, dcp, "apple,j613-25g83-profile", 1));
    /* The J615 25G83 tree (experimental) ranks like the J613's; other boards do not. */
    assert(!fdt_setprop_string(profile, 0, "compatible", "apple,j615"));
    assert(payload_profile_rank(profile, true) == 2);
    assert(payload_profile_rank(profile, false) == 0);
    assert(!fdt_setprop_string(profile, 0, "compatible", "apple,j504"));
    assert(payload_profile_rank(profile, true) == 0);
    /* Handoffs: a J613, or a J615 only with chosen.aurora,j615-25g83-experimental=1. */
    assert(!fdt_setprop_string(profile, 0, "compatible", "apple,j613"));
    assert(t8122_25g83_board(profile));
    static const char ambiguous[] = "apple,j613\0apple,j615\0";
    assert(!fdt_setprop(profile, 0, "compatible", ambiguous, sizeof(ambiguous)));
    assert(!t8122_25g83_board(profile));
    assert(payload_profile_rank(profile, true) == 0);
    assert(!fdt_setprop_string(profile, 0, "compatible", "apple,j615"));
    assert(!t8122_25g83_board(profile));
    int chosen = fdt_add_subnode(profile, 0, "chosen");
    assert(chosen >= 0);
    assert(!t8122_25g83_board(profile));
    assert(!fdt_setprop_string(profile, chosen, T8122_25G83_J615_OPT_IN, "0"));
    assert(!t8122_25g83_board(profile));
    assert(!fdt_setprop_string(profile, chosen, T8122_25G83_J615_OPT_IN, "11"));
    assert(!t8122_25g83_board(profile));
    static const char invalid[][4] = {{'1'}, {'1',0,0}, {0,0,0,1}, {'1','1',0}, {'0',0}};
    static const int invalid_lengths[] = {1,3,4,3,2};
    for (unsigned i = 0; i < sizeof(invalid_lengths)/sizeof(*invalid_lengths); i++) {
        assert(!fdt_setprop(profile, chosen, T8122_25G83_J615_OPT_IN, invalid[i], invalid_lengths[i]));
        assert(!t8122_25g83_board(profile));
    }
    assert(!fdt_setprop_string(profile, chosen, T8122_25G83_J615_OPT_IN, "1"));
    assert(t8122_25g83_board(profile));
    assert(!fdt_setprop(profile, 0, "compatible", ambiguous, sizeof(ambiguous)));
    assert(!t8122_25g83_board(profile));
    assert(payload_profile_rank(profile, true) == 0);
    assert(!fdt_setprop_string(profile, 0, "compatible", "apple,j504"));
    assert(!t8122_25g83_board(profile));
    return 0;
}
