/* SPDX-License-Identifier: GPL-2.0-only */
/* Native, read-only scope proof for explicit whole-N1 acquisition. No captured
 * phandle values or raw ADT data are embedded here. Included by windows_n1.c. */
#define N1_SCOPE_NODES 4096
#define N1_SCOPE_PMGR  2048
/* Qualified J813 bound, not an ADT format limit: the complete retained tree
 * has unrelated nodes with 226 and 152 properties. */
#define N1_SCOPE_PROPS 256
struct n1_scope_node {
    u32 offset;
    int parent;
};
static struct n1_scope_node n1_nodes[N1_SCOPE_NODES];
static unsigned n1_node_count;
/* A node finishes duplicate checks before recursing, so this scratch can be
 * reused by children without increasing the bounded recursive stack frame. */
static u32 n1_scope_previous[N1_SCOPE_PROPS];
static struct pmgr_gate n1_gates[13], n1_protected[3];
static u32 n1_protected_state[3];
static const struct {
    u16 id, parent[2];
    bool virt;
    const char *name;
} n1_scope_gates[] = {
    {74, {60, 0}, false, "APCIE_GP"},         {109, {74, 0}, false, "APCIE_SYS_GP"},
    {110, {76, 0}, false, "APCIE_ST"},        {144, {76, 110}, false, "APCIE_SYS_ST"},
    {152, {144, 109}, false, "APCIE_PHY_SW"}, {155, {152, 0}, false, "ST0_AUS5_AON"},
    {156, {155, 0}, false, "ST0_AUS5"},       {143, {109, 0}, false, "SYS_GP_OCLA"},
    {153, {144, 0}, false, "SYS_ST_OCLA"},    {338, {74, 0}, true, "APCIE-GP-V"},
    {339, {109, 0}, true, "APCIE-SYS-GP-V"},  {340, {110, 0}, true, "APCIE-ST-V"},
    {341, {144, 0}, true, "APCIE-SYS-ST-V"},
};

/* Validate byte bounds before any path/property API walks this tree. Bound
 * nodes, properties, nesting and property lengths; reject ambiguous properties. */
static int n1_scope_walk(u32 *cursor, u32 size, int parent, unsigned depth)
{
    u32 at = *cursor;
    if (depth >= 64 || n1_node_count == N1_SCOPE_NODES || at > size || size - at < 8)
        return -1;
    const u8 *base = adt;
    u32 props = le32(base + at), children = le32(base + at + 4);
    if (props > N1_SCOPE_PROPS || props > (size - at - 8) / 36 || children > N1_SCOPE_NODES) {
        printf("Windows N1: ADT structure rejected at %#x (%u properties, %u children)\n", at,
               props, children);
        return -1;
    }
    int self = n1_node_count++;
    n1_nodes[self] = (struct n1_scope_node){at, parent};
    at += 8;
    for (unsigned i = 0; i < props; i++) {
        if (at > size || size - at < 36)
            return -1;
        const struct adt_property *prop = (const void *)(base + at);
        bool terminated = false;
        for (unsigned j = 0; j < 32; j++)
            if (!prop->name[j])
                terminated = true;
        if (!terminated)
            return -1;
        for (unsigned j = 0; j < i; j++)
            if (!strncmp(prop->name, (const char *)(base + n1_scope_previous[j]), 32))
                return -1;
        n1_scope_previous[i] = at;
        u32 length = le32(base + at + 32) & 0x7fffffff;
        if (length > 0x40000 || length > size - at - 36)
            return -1;
        u32 padded = (length + 3) & ~3U;
        if (padded > size - at - 36)
            return -1;
        at += 36 + padded;
    }
    *cursor = at;
    for (unsigned i = 0; i < children; i++)
        if (n1_scope_walk(cursor, size, self, depth + 1))
            return -1;
    return 0;
}
static int n1_scope_index(u16 id)
{
    for (unsigned i = 0; i < ARRAY_SIZE(n1_scope_gates); i++)
        if (n1_scope_gates[i].id == id)
            return i;
    return -1;
}
static int n1_scope_pm(void)
{
    int count = pmgr_device_count();
    if (count <= 0 || count > N1_SCOPE_PMGR)
        return -1;
    unsigned seen = 0;
    static u32 entries[N1_SCOPE_PMGR];
    for (int i = 0; i < count; i++) {
        struct pmgr_gate gate;
        if (pmgr_device_at(i, &gate) || !gate.found || !gate.id)
            return -1;
        for (int j = 0; j < i; j++)
            if (entries[j] == gate.entry)
                return -1;
        entries[i] = gate.entry;
        int index = gate.die ? -1 : n1_scope_index(gate.id);
        if (index < 0) {
            if (!gate.die &&
                (n1_scope_index(gate.parent[0]) >= 0 || n1_scope_index(gate.parent[1]) >= 0))
                return -1;
            continue;
        }
        if (gate.entry != gate.id || !gate.name ||
            strncmp(gate.name, n1_scope_gates[index].name, 16) ||
            gate.virt != n1_scope_gates[index].virt ||
            gate.parent[0] != n1_scope_gates[index].parent[0] ||
            gate.parent[1] != n1_scope_gates[index].parent[1] || (!gate.virt && !gate.psreg))
            return -1;
        n1_gates[index] = gate;
        seen |= 1U << index;
    }
    if (seen != ((1U << ARRAY_SIZE(n1_scope_gates)) - 1))
        return -1;
    /* Same seven write addresses and excluded OCLA addresses as the sanitized
     * J813 scope; identity alone cannot authorize a shifted PMGR register. */
    static const u64 expected_psreg[] = {0x3807002f0ULL, 0x380700408ULL, 0x380700410ULL,
                                         0x380700520ULL, 0x380700560ULL, 0x380700578ULL,
                                         0x380700580ULL, 0x380700518ULL, 0x380700568ULL};
    for (unsigned i = 0; i < ARRAY_SIZE(expected_psreg); i++)
        if (n1_gates[i].psreg != expected_psreg[i])
            return -1;
    /* No register alias, even through a PMGR entry outside the closure. */
    for (int i = 0; i < count; i++) {
        struct pmgr_gate gate;
        if (pmgr_device_at(i, &gate))
            return -1;
        if (gate.virt || !gate.psreg)
            continue;
        for (unsigned j = 0; j < ARRAY_SIZE(n1_gates); j++)
            if (!n1_gates[j].virt && gate.entry != n1_gates[j].entry &&
                gate.psreg == n1_gates[j].psreg)
                return -1;
    }
    static const u32 expected[] = {76, 74, 109, 110, 144, 152, 155, 156};
    int apcie = adt_path_offset(adt, "/arm-io/apcie");
    const char *props[] = {"clock-gates", "power-gates"};
    for (unsigned k = 0; k < 2; k++) {
        u32 list[8];
        if (n1_property(apcie, props[k], list, 8) || memcmp(list, expected, sizeof(list)))
            return -1;
    }
    for (unsigned i = 0; i < n1_node_count; i++)
        for (unsigned k = 0; k < 2; k++) {
            u32 length;
            const u8 *list = adt_getprop(adt, n1_nodes[i].offset, props[k], &length);
            if (!list)
                continue;
            if (length % 4 || length > 4096)
                return -1;
            for (unsigned j = 0; j < length; j += 4)
                if (n1_scope_index(le32(list + j) & 0xffff) >= 0 &&
                    (le32(list + j) > 0xffff || (int)n1_nodes[i].offset != apcie))
                    return -1;
        }
    if (pmgr_device_gate(0, 76, &n1_protected[0]) || !n1_protected[0].found ||
        n1_protected[0].virt || n1_protected[0].psreg != 0x380700300ULL || !n1_protected[0].name ||
        strncmp(n1_protected[0].name, "ANS", 16))
        return -1;
    n1_protected[1] = n1_gates[7];
    n1_protected[2] = n1_gates[8];
    return 0;
}
static int n1_scope_mappers(void)
{
    static const char *mapper_paths[] = {
        "/arm-io/dart-apcie0/mapper-apcie0-control", "/arm-io/dart-apcie0/mapper-apcie0-alpha",
        "/arm-io/dart-apcie0/mapper-apcie0-beta", "/arm-io/dart-apcie0/mapper-apcie0-piodma"};
    static const char *consumer_paths[] = {
        "/arm-io/apcie/pci-bridge0/centauri-control", "/arm-io/apcie/pci-bridge0/centauri-alpha",
        "/arm-io/apcie/pci-bridge0/centauri-beta", "/arm-io/apcie0-piodma"};
    static const char *functions[] = {"function-dart_force_active", "function-dart_release_sid",
                                      "function-dart_request_sid", "function-dart_self"};
    static const u32 operations[] = {0x46616374, 0x5352656c, 0x53526571, 0x53656c66};
    int dart = adt_path_offset(adt, "/arm-io/dart-apcie0"),
        bridge = adt_path_offset(adt, "/arm-io/apcie/pci-bridge0");
    int apcie = adt_path_offset(adt, "/arm-io/apcie"), mapper[4], consumer[4];
    u32 handles[6], word, reserved[3];
    if (n1_property(dart, "AAPL,phandle", &handles[0], 1) ||
        n1_property(dart, "sid-count", &word, 1) || word != 19 ||
        n1_property(dart, "sid", reserved, 3) || reserved[0] != 16 || reserved[1] != 17 ||
        reserved[2] != 18 || n1_property(apcie, "first-bypass-sid", &word, 1) || word != 16)
        return -1;
    for (unsigned i = 0; i < 4; i++) {
        mapper[i] = adt_path_offset(adt, mapper_paths[i]);
        consumer[i] = adt_path_offset(adt, consumer_paths[i]);
        if (mapper[i] < 0 || consumer[i] < 0 ||
            !adt_is_compatible(adt, mapper[i], "iommu-mapper") ||
            n1_property(mapper[i], "AAPL,phandle", &handles[i + 1], 1) ||
            n1_property(consumer[i], "iommu-parent", &word, 1) || word != handles[i + 1])
            return -1;
        u32 len;
        const void *reg = adt_getprop(adt, mapper[i], "reg", &len);
        if (i < 3 ? reg != NULL : (!reg || len != 4 || le32(reg) != 17))
            return -1;
    }
    if (!adt_is_compatible(adt, consumer[3], "pcie-apiodma,t6020") ||
        n1_property(consumer[3], "AAPL,phandle", &handles[5], 1) ||
        n1_property(bridge, "apcie-piodma", &word, 1) || word != handles[5] ||
        n1_property(bridge, "apcie-piodma-sid", &word, 1) || word != 17)
        return -1;
    for (unsigned i = 0; i < 6; i++) {
        if (!handles[i])
            return -1;
        for (unsigned j = 0; j < i; j++)
            if (handles[i] == handles[j])
                return -1;
    }
    unsigned handle_seen[6] = {0}, consumers_seen[4] = {0}, children = 0, functions_seen = 0;
    for (unsigned i = 0; i < n1_node_count; i++) {
        int node = n1_nodes[i].offset, parent = n1_nodes[i].parent;
        if (parent >= 0 && (int)n1_nodes[parent].offset == dart) {
            bool found = false;
            for (unsigned j = 0; j < 4; j++)
                if (node == mapper[j])
                    found = true;
            if (!found)
                return -1;
            children++;
        }
        if (parent >= 0)
            for (unsigned j = 0; j < 4; j++)
                if ((int)n1_nodes[parent].offset == mapper[j])
                    return -1;
        u32 len;
        const u8 *p = adt_getprop(adt, node, "AAPL,phandle", &len);
        if (p) {
            if (len != 4)
                return -1;
            for (unsigned j = 0; j < 6; j++)
                if (le32(p) == handles[j])
                    handle_seen[j]++;
        }
        p = adt_getprop(adt, node, "iommu-parent", &len);
        if (p) {
            if (len % 4 || len > 4096)
                return -1;
            for (unsigned k = 0; k < len; k += 4) {
                if (le32(p + k) == handles[0])
                    return -1;
                for (unsigned j = 0; j < 4; j++)
                    if (le32(p + k) == handles[j + 1]) {
                        if (len != 4 || node != consumer[j])
                            return -1;
                        consumers_seen[j]++;
                    }
            }
        }
        /* Every ADT function referencing this controller must be one of the
         * four observed bridge functions, with its complete operation tuple. */
        const u8 *base = adt;
        u32 at = n1_nodes[i].offset + 8;
        u32 props = le32(base + n1_nodes[i].offset);
        for (unsigned k = 0; k < props; k++) {
            const struct adt_property *prop = (const void *)(base + at);
            u32 length = le32(base + at + 32) & 0x7fffffff;
            if (!strncmp(prop->name, "function-", 9) && length >= 4 &&
                le32(prop->value) == handles[0]) {
                bool found = false;
                for (unsigned j = 0; j < 4; j++)
                    if (!strcmp(prop->name, functions[j]) && node == bridge && length == 8 &&
                        le32(prop->value + 4) == operations[j])
                        found = true;
                if (!found)
                    return -1;
                functions_seen++;
            }
            at += 36 + ((length + 3) & ~3U);
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        u32 value[2];
        if (n1_property(bridge, functions[i], value, 2) || value[0] != handles[0] ||
            value[1] != operations[i] || consumers_seen[i] != 1)
            return -1;
    }
    for (unsigned i = 0; i < 6; i++)
        if (handle_seen[i] != 1)
            return -1;
    return children == 4 && functions_seen == 4 ? 0 : -1;
}
static int n1_scope_tree(void)
{
    u32 size = adt_get_size(), cursor = 0;
    n1_node_count = 0;
    return !adt || !size || size > 8 * 1024 * 1024 || n1_scope_walk(&cursor, size, -1, 0) ? -1 : 0;
}
static int n1_scope_admit(void)
{
    if (n1_scope_pm()) {
        printf("Windows N1: PMGR scope rejected\n");
        return -1;
    }
    if (n1_scope_mappers()) {
        printf("Windows N1: mapper scope rejected\n");
        return -1;
    }
    for (unsigned i = 0; i < 3; i++)
        if (n1_read(n1_protected[i].psreg, 4, &n1_protected_state[i]))
            return -1;
    for (unsigned i = 0; i < 7; i++) {
        u32 state;
        if (n1_read(n1_gates[i].psreg, 4, &state) || (state & BIT(31)))
            return -1;
        unsigned target = state & 15, actual = (state >> 4) & 15;
        if ((target != 0 && target != 4 && target != 15) ||
            (actual != 0 && actual != 4 && actual != 15))
            return -1;
    }
    printf("Windows N1: admitted 13 PMGR descendants, sole gate consumer, four N1 mappers\n");
    return 0;
}
static int n1_scope_preserved(void)
{
    for (unsigned i = 0; i < 3; i++) {
        u32 state;
        if (n1_read(n1_protected[i].psreg, 4, &state) || state != n1_protected_state[i])
            return -1;
    }
    return 0;
}
static int n1_scope_power(void)
{
    for (unsigned i = 0; i < 7; i++) {
        enum exc_guard_t saved = exc_guard;
        int before = exc_count;
        exc_guard = GUARD_MARK | GUARD_SILENT;
        int result = pmgr_set_mode(n1_gates[i].psreg, 15);
        sysop("dsb sy");
        sysop("isb");
        exc_guard = saved;
        if (result || before != exc_count)
            return -1;
    }
    return n1_scope_preserved();
}
