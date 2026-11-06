/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors), 2023.
 *
 * Filename: hv_psci.c
 *
 * Description: Implements PSCI services for a running guest when the hypervisor is in use.
 *
 * Note: for bare metal booting, we use a higher level firmware running in GL2 to provide PSCI, so
 * this file does not account for that case.
 *
 *
 *
 * Implementation is basically a straight port of the implementation in ARM Trusted Firmware-A.
 * (https://github.com/ARM-software/arm-trusted-firmware) as reinventing the wheel here is not very
 * fun.
 *
 * License: SPDX-License-Identifier: MIT OR BSD-3-Clause
 */

#include "hv.h"
#include "adt.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "exception.h"
#include "heapblock.h"
#include "hv_psci.h"
#include "malloc.h"
#include "memory.h"
#include "pcie.h"
#include "pmgr.h"
#include "smp.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "usb.h"
#include "utils.h"

//
// PSCI variables.
//

uint32_t psci_capabilities;

unsigned int psci_num_cores, psci_num_clusters;

//
// Make the arrays the size they'd need to be on an M2 Ultra.
// Yes I realize that this is probably pretty dumb, but this is just a temporary thing for now.
//
cpu_power_domain_node_t psci_cpu_nodes[MAX_CPUS];
non_cpu_power_domain_node_t psci_non_cpu_nodes[MAX_CPUS + NUM_SYSTEMS_ACTIVE];
static platform_local_state_t psci_requested_local_power_states[PSCI_MAX_POWER_LEVEL][MAX_CPUS];
spinlock_t *psci_locks;
psci_per_cpu_data_t psci_cpu_data_array[MAX_CPUS];
static int adt_cpu_nodes[MAX_CPUS];
/*
 * Apple physical CPU IDs are not necessarily dense (J414s omits 7 and 11).
 * PSCI's topology walk is dense, while all hardware-facing arrays remain
 * indexed by the physical ADT cpu-id.  These maps keep the two namespaces
 * explicit instead of assuming that cpu-id == topology index.
 */
static uint8_t psci_active_cpu_ids[MAX_CPUS];
static bool psci_cpu_present[MAX_CPUS];
static unsigned char psci_power_domain_tree[MAX_CPUS + 2];

//
// A table of valid idle states. Anything else is considered invalid.
// The states will be listed as this: (system state, cluster state, core state)
//
const unsigned int valid_idle_states[] = {
    // (On, On, Idle Standby/WFI) - core is in standby mode.
    apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_ON_STATE, PSCI_IDLE_STANDBY_STATE,
                             PSCI_CPU_POWER_LEVEL, PSCI_POWER_STATE_TYPE_STANDBY),
    // (On, On, Poweroff/Deep Sleep/S2R) - level 0 is powered down
    // turned off for testing purposes.
    // apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_ON_STATE, PSCI_OFF_STATE, PSCI_CPU_POWER_LEVEL,
    // PSCI_POWER_STATE_TYPE_POWERDOWN),
    // (On, Idle Retention, Idle Retention/Deep WFI.) - level 1 is in standby
    apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_IDLE_STANDBY_STATE, PSCI_IDLE_STANDBY_STATE,
                             PSCI_CLUSTER_POWER_LEVEL, PSCI_POWER_STATE_TYPE_STANDBY),
    // (On, Off, Off) - level 1/cluster off
    // Not supported, pending an understanding of how to trigger this power state.

    // (Retention, Off, Off) - level 2 standby
    // Not supported, pending an understanding of how to trigger this power state.

    // (Off, Off, Off) - system off.
    apple_make_pwrstate_lvl2(PSCI_OFF_STATE, PSCI_OFF_STATE, PSCI_OFF_STATE, PSCI_MAX_POWER_LEVEL,
                             PSCI_POWER_STATE_TYPE_POWERDOWN),
    0};
/**
 * Aside: Apple core topology will be defined as follows (NOTE: only for cores/clusters/system,
 * pmgr peripherals are not accounted for) per ARM Trusted Firmware
 * requirements for PSCI.
 *
 * Max power level - MPIDR (Aff2) (0 is core, 1 is cluster, 2 is system)
 *
 * Number of nodes in power domain tree (aka clusters + cores):
 *
 * <6-24> (number of cores) + <2-6> (number of clusters) + 1 (system power domain)
 *
 * deepest power down state: OFF
 *
 * Low power sleep states:
 * - Idle WFI (aka "shallow") - when core is in WFI but not deep sleeping
 * - Deep WFI (aka deep sleep)
 *
 *
 * MPIDR syntax for Apple SoCs from M1 onwards:
 *
 * bits 31:24 - bit 31, RES1, not a hyperthreading system
 *
 * bits 23:16 - aff2, 0x1 for P-cores, 0x0 for E-cores.
 *
 * bits 15:8 - aff1, (die_num * 8) + local_cluster_number, indicates what cluster we are on.
 *
 * bits 7:0 - aff0, core_num on the local cluster.
 *
 */

//
// Apple CPU suspend notes:
//
// - can suspend cores in a "deep WFI" or "shallow WFI" state (the former clock gates the cores)
// - power gating all cores in a cluster will put the cluster into a retention state (general
// purpose regs are lost but cluster uncore remains powered.)
// - separate mechanism to power off a cluster completely (to start cores from RVBAR)
//
// Possible states for power domain nodes:
// CPU: can be in an on state, a shallow WFI state, a deep WFI state (aka clock gated), and OFF.
// Cluster: can be on (when any core in the cluster is on or in retention), in retention (when all
// cores in a cluster are OFF), or off (when all cores are in retention and turned off) System: ON
// or OFF. easy.
//
// Still in the process of being documented.
//

//
// Description:
// Initializes PSCI module. Only expected to be called once when the hypervisor is initially
// started. (Note: this code assumes that the hypervisor will only be started up once, as shutting
// down the hypervisor is not a supported scenario, albeit doable.)
//
// Return value:
// None.
//

typedef struct psci_adt_cpu {
    uint8_t cpu_id;
    uint32_t cluster_key;
    uint32_t local_core_id;
} psci_adt_cpu_t;

static bool hv_psci_cpu_precedes(const psci_adt_cpu_t *left, const psci_adt_cpu_t *right)
{
    if (left->cluster_key != right->cluster_key)
        return left->cluster_key < right->cluster_key;
    if (left->local_core_id != right->local_core_id)
        return left->local_core_id < right->local_core_id;
    return left->cpu_id < right->cpu_id;
}

/*
 * Build the PSCI hierarchy from Apple's live device tree.  Family-wide core
 * maxima are not topology: binned parts such as J414s legitimately have
 * sparse cpu-id values and fewer active cores in a cluster.  Linux makes the
 * same decision from the enabled CPU nodes rather than a SoC maximum.
 */
static const unsigned char *hv_psci_build_adt_topology(void)
{
    psci_adt_cpu_t cpus[MAX_CPUS];
    unsigned int cpu_count = 0;
    int cpus_node = adt_path_offset(adt, "/cpus");
    int node = cpus_node;

    if (cpus_node < 0)
        panic("PSCI setup fatal error: /cpus is missing from ADT\n");

    memset(adt_cpu_nodes, 0, sizeof(adt_cpu_nodes));
    memset(psci_active_cpu_ids, 0xff, sizeof(psci_active_cpu_ids));
    memset(psci_cpu_present, 0, sizeof(psci_cpu_present));
    memset(psci_cpu_nodes, 0, sizeof(psci_cpu_nodes));
    memset(psci_cpu_data_array, 0, sizeof(psci_cpu_data_array));
    memset(psci_power_domain_tree, 0, sizeof(psci_power_domain_tree));

    ADT_FOREACH_CHILD(adt, node)
    {
        unsigned int cpu_id;
        unsigned int reg;
        unsigned int cluster_id;
        unsigned int local_core_id;
        unsigned int die_id = 0;
        unsigned short cluster_type;
        uint64_t mpidr = BIT(31);

        if (ADT_GETPROP(adt, node, "cpu-id", &cpu_id) < 0)
            continue;
        if (cpu_id >= MAX_CPUS)
            panic("PSCI setup fatal error: ADT cpu-id %u exceeds MAX_CPUS\n", cpu_id);
        if (psci_cpu_present[cpu_id])
            panic("PSCI setup fatal error: duplicate ADT cpu-id %u\n", cpu_id);
        if (cpu_count >= MAX_CPUS)
            panic("PSCI setup fatal error: too many ADT CPUs\n");
        if (ADT_GETPROP(adt, node, "reg", &reg) < 0)
            panic("PSCI setup fatal error: CPU%u has no reg property\n", cpu_id);
        if (ADT_GETPROP(adt, node, "die-cluster-id", &cluster_id) < 0)
            cluster_id = (reg >> 8) & 0xff;
        if (ADT_GETPROP(adt, node, "die-id", &die_id) < 0)
            die_id = 0;
        if (ADT_GETPROP(adt, node, "cluster-core-id", &local_core_id) < 0)
            local_core_id = reg & 0xff;
        if (ADT_GETPROP(adt, node, "cluster-type", &cluster_type) < 0)
            panic("PSCI setup fatal error: CPU%u has no cluster-type property\n", cpu_id);

        /* Apple ADT reg omits Aff2=1 on P cores; MPIDR_EL1 and Linux include it. */
        mpidr |= reg;
        if (smp_is_alive(cpu_id))
            mpidr = smp_get_mpidr(cpu_id);
        else if (cluster_type == 'P')
            mpidr |= BIT(16);
        /* Unstarted CPUs remain enumerable, but CPU_ON refuses them. */

        adt_cpu_nodes[cpu_id] = node;
        psci_cpu_present[cpu_id] = true;
        psci_cpu_nodes[cpu_id].mpidr = mpidr;
        psci_cpu_data_array[cpu_id].cpu_index = cpu_id;
        psci_cpu_data_array[cpu_id].reg_value = reg;
        psci_cpu_data_array[cpu_id].cluster_index = cluster_id;
        psci_cpu_data_array[cpu_id].die_index = die_id;
        psci_cpu_data_array[cpu_id].local_core_number = local_core_id;
        cpus[cpu_count++] = (psci_adt_cpu_t){
            .cpu_id = (uint8_t)cpu_id,
            .cluster_key = (die_id << 16) | (cluster_id & 0xffff),
            .local_core_id = local_core_id,
        };
    }

    if (!cpu_count)
        panic("PSCI setup fatal error: ADT contains no usable CPU nodes\n");

    for (unsigned int i = 1; i < cpu_count; i++) {
        psci_adt_cpu_t current = cpus[i];
        unsigned int j = i;
        while (j && hv_psci_cpu_precedes(&current, &cpus[j - 1])) {
            cpus[j] = cpus[j - 1];
            j--;
        }
        cpus[j] = current;
    }

    psci_power_domain_tree[0] = NUM_SYSTEMS_ACTIVE;
    uint32_t previous_cluster = ~0u;
    unsigned int cluster_count = 0;
    for (unsigned int dense = 0; dense < cpu_count; dense++) {
        unsigned int cpu_id = cpus[dense].cpu_id;
        if (cpus[dense].cluster_key != previous_cluster) {
            previous_cluster = cpus[dense].cluster_key;
            if (++cluster_count >= MAX_CPUS)
                panic("PSCI setup fatal error: too many ADT clusters\n");
        }
        psci_power_domain_tree[cluster_count + 1]++;
        psci_active_cpu_ids[dense] = (uint8_t)cpu_id;
        psci_cpu_data_array[cpu_id].topology_index = dense;
    }
    psci_power_domain_tree[1] = cluster_count;
    psci_num_cores = cpu_count;
    psci_num_clusters = cluster_count;

    printf("PSCI: ADT topology has %u active CPUs in %u clusters:", psci_num_cores,
           psci_num_clusters);
    for (unsigned int dense = 0; dense < psci_num_cores; dense++)
        printf(" %u", psci_active_cpu_ids[dense]);
    printf("\n");

    return psci_power_domain_tree;
}

void hv_psci_init(void)
{
    const unsigned char *topology_tree = hv_psci_build_adt_topology();

    printf("PSCI DEBUG: ADT topology tree built\n");
    printf("PSCI DEBUG: allocating RAM for locks\n");
    psci_locks = malloc(((psci_num_clusters + NUM_SYSTEMS_ACTIVE) * sizeof(spinlock_t)));
    if (!psci_locks)
        panic("PSCI setup failed to allocate domain locks\n");
    printf("PSCI DEBUG: populating power domain tree\n");
    unsigned int populated_cores = hv_psci_populate_power_domain_tree(topology_tree);
    if (populated_cores != psci_num_cores)
        panic("PSCI setup fatal error: topology populated %u of %u ADT CPUs\n", populated_cores,
              psci_num_cores);
    printf("PSCI DEBUG: updating power level limits\n");
    hv_psci_update_power_level_limits();

    printf("PSCI DEBUG: Number of cores for PSCI nodes is %d\n", psci_num_cores);

    printf("PSCI DEBUG: Number of clusters for PSCI nodes is %d\n", psci_num_clusters);
    printf("PSCI DEBUG: Total number of nodes in power domain tree is %d (%d cores, %d clusters, "
           "%d system)\n",
           (psci_num_clusters + psci_num_cores + NUM_SYSTEMS_ACTIVE), psci_num_cores,
           psci_num_clusters, NUM_SYSTEMS_ACTIVE);

    /* Public Windows bring-up exposes only implemented virtual CPU services.
     * Physical CPU_OFF/SUSPEND and memory protection are not implemented. */
    psci_capabilities = PSCI_GENERIC_CAPABILITY | define_psci_cap(PSCI_CPU_ON_ARM64_FUNCTION_ID) |
                        define_psci_cap(PSCI_SYSTEM_POWEROFF_FUNCTION_ID) |
                        define_psci_cap(PSCI_SYSTEM_RESET_FUNCTION_ID);

    hv_psci_init_requested_local_power_states();

    hv_psci_set_power_domains_to_on_state(PSCI_MAX_POWER_LEVEL);

    return;
}

void hv_psci_init_requested_local_power_states(void)
{
    unsigned int power_level, core;
    for (power_level = 0; power_level < PSCI_MAX_POWER_LEVEL; power_level++) {
        for (core = 0; core < psci_num_cores; core++) {
            psci_requested_local_power_states[power_level][core] = PSCI_MAX_OFF_STATE;
        }
    }
}

void hv_psci_update_power_level_limits(void)
{
    unsigned int dense_index;
    int j;
    unsigned int nodes_index[PSCI_MAX_POWER_LEVEL];
    unsigned int temp_index[PSCI_MAX_POWER_LEVEL];

    for (unsigned int index = 0; index < PSCI_MAX_POWER_LEVEL; index++)
        nodes_index[index] = ~0u;

    for (dense_index = 0; dense_index < psci_num_cores; dense_index++) {
        unsigned int cpu_index = psci_active_cpu_ids[dense_index];
        hv_psci_get_parent_nodes(cpu_index, PSCI_MAX_POWER_LEVEL, temp_index);
        for (j = PSCI_MAX_POWER_LEVEL - 1; j >= 0; j--) {
            if (temp_index[j] != nodes_index[j]) {
                nodes_index[j] = temp_index[j];
                psci_non_cpu_nodes[nodes_index[j]].first_cpu_idx = dense_index;
            }
            psci_non_cpu_nodes[nodes_index[j]].num_cpu_siblings++;
        }
    }
}

void hv_psci_initialize_power_domain_node(uint16_t node_index, unsigned int parent_index,
                                          uint8_t level)
{
    if (level > PSCI_CPU_POWER_LEVEL) {
        // printf("PSCI DEBUG: setting non CPU node to level %d\n", level);
        psci_non_cpu_nodes[node_index].power_level = level;
        // printf("PSCI DEBUG: initializing lock on node %d\n", node_index);
        hv_psci_lock_init(psci_non_cpu_nodes, node_index);
        // printf("PSCI DEBUG: setting non CPU node's parent to %d\n", parent_index);
        psci_non_cpu_nodes[node_index].parent_node = parent_index;
        // printf("PSCI DEBUG: setting non CPU node's power state to OFF\n");
        psci_non_cpu_nodes[node_index].local_power_state = PSCI_MAX_OFF_STATE;
    } else {
        psci_per_cpu_data_t *cpu_data;
        // printf("PSCI DEBUG: setting CPU parent node to %d\n", parent_index);
        psci_cpu_nodes[node_index].parent_node = parent_index;
        cpu_data = &psci_cpu_data_array[node_index];
        // printf("PSCI DEBUG: setting CPU node affinity state to OFF\n");
        cpu_data->affinity_state = AFFINITY_STATE_OFF;
        // printf("PSCI DEBUG: setting CPU node targeted suspend level to default level\n");
        cpu_data->target_power_level = PSCI_INVALID_LEVEL;
        // printf("PSCI DEBUG: setting CPU node power state to OFF\n");
        cpu_data->local_cpu_state = PSCI_MAX_OFF_STATE;
        // printf("PSCI DEBUG: invalidating data cache/flushing\n");
        dc_civac_range((void *)cpu_data, sizeof(*cpu_data));
    }
}

unsigned int hv_psci_populate_power_domain_tree(const unsigned char *power_domain_tree_map)
{
    unsigned int i, j = 0U;
    unsigned int number_of_nodes_at_level = 1U;
    unsigned int number_of_nodes_at_next_level;
    unsigned int node_index = 0U;
    unsigned int num_children;
    unsigned int parent_node_index = 0U;
    int level = (int)PSCI_MAX_POWER_LEVEL;

    while (level >= (int)PSCI_CPU_POWER_LEVEL) {
        printf("PSCI DEBUG: current level is %d\n", level);
        number_of_nodes_at_next_level = 0U;
        for (i = 0; i < number_of_nodes_at_level; i++) {
            assert(parent_node_index <= psci_num_clusters + 1);
            num_children = power_domain_tree_map[parent_node_index];
            printf("PSCI DEBUG: number of children in level %d is %d\n", level, num_children);
            for (j = node_index; j < (node_index + num_children); j++) {
                unsigned int storage_index = j;
                if (level == (int)PSCI_CPU_POWER_LEVEL) {
                    if (j >= psci_num_cores)
                        panic("PSCI setup fatal error: topology CPU index %u is out of range\n", j);
                    storage_index = psci_active_cpu_ids[j];
                }
                printf("PSCI DEBUG: initializing power domain node %d (storage %d) with parent %d, "
                       "at level %d\n",
                       j, storage_index, (parent_node_index - 1), level);
                hv_psci_initialize_power_domain_node((uint16_t)storage_index,
                                                     parent_node_index - 1U, (unsigned char)level);
            }
            node_index = j;
            number_of_nodes_at_next_level += num_children;
            parent_node_index++;
        }
        number_of_nodes_at_level = number_of_nodes_at_next_level;
        level--;
        if (level == (int)PSCI_CPU_POWER_LEVEL) {
            node_index = 0;
        }
    }
    return j;
}

//
// Helpers to get CPU specific data.
//

static inline void hv_psci_set_suspend_power_level(unsigned int target_level)
{
    unsigned int cpu_identifier = hv_psci_get_core_position();
    psci_cpu_data_array[cpu_identifier].target_power_level = target_level;
}

void hv_psci_set_cpu_local_state(platform_local_state_t state)
{
    unsigned int cpu_identifier = hv_psci_get_core_position();
    psci_cpu_data_array[cpu_identifier].local_cpu_state = state;
}

static inline platform_local_state_t hv_psci_get_cpu_local_state(void)
{
    unsigned int cpu_identifier = hv_psci_get_core_position();
    return psci_cpu_data_array[cpu_identifier].local_cpu_state;
}

//
// Description:
// PSCI power state helper function to sanity check the power state.
//
// Return value:
//    None.
//
static inline unsigned int hv_psci_power_state_sanity_check(unsigned int power_state)
{
    return ((power_state)&PSCI_STATE_VALID_MASK);
}

//
// Description:
// PSCI power state helper function to get the power state type.
//
// Return value:
//    None.
//

//
// Description:
// PSCI power state helper function to get the power state ID.
//
// Return value:
//    None.
//
static inline unsigned int hv_psci_power_state_get_id(unsigned int power_state)
{
    return ((power_state)&PSCI_STATE_ID_MASK);
}

//
// Description:
// Validate that the power state is good.
//
// Return value:
//    PSCI_SUCCESS - the power state is good.
//    PSCI_INVALID_PARAMETERS - the power state is NOT valid.
//
int hv_psci_validate_power_state(unsigned int power_state,
                                 psci_power_state_status_t *power_state_info)
{
    unsigned int power_state_id;
    int i;
    if (hv_psci_power_state_sanity_check(power_state) != 0) {
        printf("PSCI DEBUG: power state sanity check failed or code buggy\n");
        return PSCI_STATUS_INVALID_PARAMETERS;
    }
    for (i = 0; !!valid_idle_states[i]; i++) {
        if (power_state == valid_idle_states[i]) {
            break;
        }
    }

    if (!valid_idle_states[i]) {
        return PSCI_STATUS_INVALID_PARAMETERS;
    }
    i = 0;

    power_state_id = hv_psci_power_state_get_id(power_state);

    for (i = (int)PSCI_CPU_POWER_LEVEL; i <= (int)PSCI_MAX_POWER_LEVEL; i++) {
        power_state_info->power_domain_state[i] = power_state_id & PLAT_LOCAL_PSTATE_MASK;
        power_state_id = power_state_id >> PLAT_LOCAL_PSTATE_WIDTH;
    }
    return PSCI_STATUS_SUCCESS;
}

//
// Description:
// Helper function to calculate "core position" for PSCI code.
//
// Return value:
//    Core position as an integer.
//
unsigned int hv_psci_get_core_position(void)
{
    unsigned int mpidr_calculated = mrs(MPIDR_EL1);
    unsigned int reg_value_calculated = mpidr_calculated & GENMASK(15, 0);
    unsigned int core_position = 0xfe;
    for (int i = 0; i < MAX_CPUS; i++) {
        int current_node = adt_cpu_nodes[i];

        if (!current_node) {
            continue;
        }
        unsigned int cpu_identifier;
        if (ADT_GETPROP(adt, current_node, "cpu-id", &cpu_identifier) < 0) {
            continue;
        }
        if (psci_cpu_data_array[cpu_identifier].reg_value == reg_value_calculated) {
            core_position = cpu_identifier;
            break;
        }
    }
    if (core_position == 0xfe) {
        printf("Core position was not found! (Or there's a bug in the code.)\n");
    }
    return core_position;
}

//
// Description:
// Sets the local power state array to the desired/requested state.
// (Note: does not apply to CPU power levels as those aren't stored in the array.)
//
// Return value:
//    None.
//
void hv_psci_set_requested_local_power_state(unsigned int power_level, unsigned int cpu_index,
                                             platform_local_state_t requested_power_state)
{
    //
    // Do not allow access to CPU power level (as this array does not store the requested state for
    // that.)
    //
    assert(power_level > 0);
    if ((power_level > 0) && (power_level <= PSCI_MAX_POWER_LEVEL) && (cpu_index < MAX_CPUS) &&
        psci_cpu_present[cpu_index]) {
        unsigned int dense_index = psci_cpu_data_array[cpu_index].topology_index;
        psci_requested_local_power_states[power_level - 1][dense_index] = requested_power_state;
    }
}

//
// Description:
// Returns a pointer to local power states requested by CPUs for a given power domain tree node.
// (Note: CPU power levels not part of this array, assertion to prevent this kind of access.)
//
// Return value:
//    A pointer inside the array containing local power states requested by a given CPU.
//
platform_local_state_t *hv_psci_get_requested_local_power_states(unsigned int power_level,
                                                                 unsigned int cpu_index)
{
    assert(power_level > 0);
    if ((power_level > 0) && (power_level <= PSCI_MAX_POWER_LEVEL) &&
        (cpu_index < psci_num_cores)) {
        return &psci_requested_local_power_states[power_level - 1U][cpu_index];
    } else {
        return NULL;
    }
}

platform_local_state_t hv_psci_get_target_power_state(unsigned int level,
                                                      const platform_local_state_t *states,
                                                      unsigned int num_cpu_siblings)
{
    platform_local_state_t target_state = PSCI_OFF_STATE, temp;
    const platform_local_state_t *state1 = states;
    unsigned int siblings = num_cpu_siblings;
    assert(num_cpu_siblings > 0);
    UNUSED(level);

    do {
        temp = *state1;
        state1++;
        if (temp < target_state) {
            target_state = temp;
        }
        siblings--;
    } while (siblings > 0U);

    return target_state;
}

//
// Description:
// Get non-CPU power domain local state.
//
// Return value:
//    Local state of requested non-CPU power domain node.
//
platform_local_state_t hv_psci_get_non_cpu_power_domain_local_state(unsigned int parent_index)
{
    dc_civac_range((void *)&psci_non_cpu_nodes[parent_index],
                   sizeof(psci_non_cpu_nodes[parent_index]));
    return psci_non_cpu_nodes[parent_index].local_power_state;
}

//
// Description:
// Update non-CPU power domain local state.
//
// Return value:
//    None.
//
void hv_psci_set_non_cpu_power_domain_node_local_state(unsigned int parent_index,
                                                       platform_local_state_t state)
{
    psci_non_cpu_nodes[parent_index].local_power_state = state;
    //
    // Flush and invalidate caches here for safety in case we're not cache coherent.
    //
    dc_civac_range((void *)&psci_non_cpu_nodes[parent_index],
                   sizeof(psci_non_cpu_nodes[parent_index]));
}

//
// Description:
// Helper function to find the highest power level that will be turned off.
//
// Return value:
//    Highest power level to be powered down.
//
unsigned int hv_psci_find_max_off_level(const psci_power_state_status_t *state_info)
{
    for (int i = (int)PSCI_MAX_POWER_LEVEL; i >= (int)PSCI_CPU_POWER_LEVEL; i--) {
        int result = (((state_info->power_domain_state[i]) > PSCI_MAX_RETENTION_STATE) &&
                      ((state_info->power_domain_state[i]) <= PSCI_MAX_OFF_STATE))
                         ? 1
                         : 0;
        if (result != 0) {
            return (unsigned int)i;
        }
    }
    return PSCI_INVALID_LEVEL;
}

//
// Description:
// Helper that sets the target local power state to be entered by power domains from current CPU to
// ancestor. Must be called after coordination of power states.
//
// Return value:
//    None.
//

void hv_psci_set_target_local_power_states(unsigned int end_power_level,
                                           const psci_power_state_status_t *target_state)
{
    unsigned int parent_index, level;
    const platform_local_state_t *power_domain_state = target_state->power_domain_state;

    hv_psci_set_cpu_local_state(power_domain_state[PSCI_CPU_POWER_LEVEL]);

    //
    // Flush data caches to prevent weirdness with cached local states.
    //
    dc_civac_range((void *)&psci_cpu_data_array[mrs(TPIDR_EL2)].local_cpu_state,
                   sizeof(psci_cpu_data_array[mrs(TPIDR_EL2)].local_cpu_state));

    parent_index = psci_cpu_nodes[hv_psci_get_core_position()].parent_node;

    //
    // Copy local state over from the state info array.
    //
    for (level = 1; level <= end_power_level; level++) {
        hv_psci_set_non_cpu_power_domain_node_local_state(parent_index, power_domain_state[level]);
        parent_index = psci_non_cpu_nodes[parent_index].parent_node;
    }
}

//
// Description:
// Coordinates the platform specific local power states requested by each CPU and returns
// the coordinated state.
// TODO: Check if we need to do platform specific things here.
//
// Return value:
//    Target power state after coordination.
//

void hv_psci_coordinate_power_states(unsigned int end_power_level,
                                     psci_power_state_status_t *current_state_info)
{
    unsigned int level, parent_index, cpu_index = hv_psci_get_core_position();
    unsigned int start_index;
    unsigned int num_cpu_siblings;
    platform_local_state_t target_state, *requested_states;

    //
    // Get the parent node of the current CPU node.
    //
    parent_index = psci_cpu_nodes[cpu_index].parent_node;

    for (level = 1; level <= end_power_level; level++) {
        //
        // Update requested power state.
        //
        hv_psci_set_requested_local_power_state(level, cpu_index,
                                                current_state_info->power_domain_state[level]);

        start_index = psci_non_cpu_nodes[parent_index].first_cpu_idx;
        requested_states = hv_psci_get_requested_local_power_states(level, start_index);

        //
        // Coordinate requested states at the power level, and return the target state.
        //
        num_cpu_siblings = psci_non_cpu_nodes->num_cpu_siblings;
        target_state = hv_psci_get_target_power_state(level, requested_states, num_cpu_siblings);
        current_state_info->power_domain_state[level] = target_state;

        //
        // If the coordinated state is normal running operaiton, break out early.
        //
        if ((current_state_info->power_domain_state[level]) == PSCI_ON_STATE) {
            printf("PSCI DEBUG: current state info says: %d, evaluated as PSCI on state\n",
                   (current_state_info->power_domain_state[level]));
            break;
        }

        parent_index = psci_non_cpu_nodes[parent_index].parent_node;
    }

    //
    // If the targeted power state is the run state, when power level is < than end power level,
    // update the requested power state and set that target state.
    //
    for (level = level + 1; level <= end_power_level; level++) {
        hv_psci_set_requested_local_power_state(level, cpu_index,
                                                current_state_info->power_domain_state[level]);
        current_state_info->power_domain_state[level] = PSCI_ON_STATE;
    }

    //
    // Finally, update target state in power domain nodes
    //
    hv_psci_set_target_local_power_states(end_power_level, current_state_info);
}

//
// Description:
//
// Releases a spinlock on a non-CPU power domain node in the tree. (CPU nodes do not need a
// spinlock)
//
// Return value:
//    None.
void hv_psci_release_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node)
{
    spin_unlock(&psci_locks[non_cpu_power_domain_node->lock_index]);
}

//
// Description:
//
// Gets a spinlock on a non-CPU power domain node in the tree. (CPU nodes do not need a spinlock)
//
// Return value:
//    None.
void hv_psci_get_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node)
{
    spin_lock(&psci_locks[non_cpu_power_domain_node->lock_index]);
}

//
// Description:
// This function does the architectural preparation to power down the CPU.
//
// Return value:
// None.
//

void hv_psci_power_down_cpu_maintenance(unsigned int power_level)
{
    UNUSED(power_level);
    unsigned int cpu_index = hv_psci_get_core_position();
    UNUSED(cpu_index);

    //
    // Disable data caching.
    //
    uint64_t sctlr_old = read_sctlr();
    uint64_t sctlr_cache_disable = sctlr_old & ~(SCTLR_C);
    write_sctlr(sctlr_cache_disable);

    dcsw_op_all(DCSW_OP_DCISW);

    return;
}

/**
 * Description:
 * This function constructs the PSCI power state to turn off at all levels.
 *
 * Return value:
 *    None.
 */

void hv_psci_construct_poweroff_state(psci_power_state_status_t *state_info)
{
    //
    // Iterate through every level, and set every state in the state_info struct
    // to the poweroff state (returned to caller which will then perform state coordination)
    //
    for (unsigned int level = 0; level <= PSCI_MAX_POWER_LEVEL; level++) {
        state_info->power_domain_state[level] = PSCI_OFF_STATE;
    }
}

platform_local_state_type_t hv_psci_power_state_categorize_type(platform_local_state_t state)
{
    if (state != PSCI_ON_STATE) {
        if (state > PSCI_IDLE_STANDBY_STATE) {
            return STATE_TYPE_OFF;
        } else {
            return STATE_TYPE_RETN;
        }
    } else {
        return STATE_TYPE_RUN;
    }
}

void hv_psci_get_target_local_power_states(unsigned int end_power_level,
                                           psci_power_state_status_t *target_state)
{
    unsigned int parent_index, level;
    platform_local_state_t *power_domain_current_state = target_state->power_domain_state;
    unsigned int cpu_index = hv_psci_get_core_position();
    power_domain_current_state[PSCI_CPU_POWER_LEVEL] = hv_psci_get_cpu_local_state();
    parent_index = psci_cpu_nodes[cpu_index].parent_node;

    for (level = PSCI_CPU_POWER_LEVEL + 1U; level <= end_power_level; level++) {
        power_domain_current_state[level] =
            hv_psci_get_non_cpu_power_domain_local_state(parent_index);
        parent_index = psci_non_cpu_nodes[parent_index].parent_node;
    }

    for (; level <= PSCI_MAX_POWER_LEVEL; level++) {
        target_state->power_domain_state[level] = PSCI_ON_STATE;
    }
}

//
// Description:
// This helper function sets the affinity info state for a given CPU.
//
// Return value:
//    None.
//
void hv_psci_set_affinity_info_state(affinity_info_state_t state)
{
    unsigned int cpu_identifier = hv_psci_get_core_position();
    psci_cpu_data_array[cpu_identifier].affinity_state = state;
}

//
// Description:
// This helper function releases locks for each power level in reverse order.
//
// Return value:
//    None.
//
void hv_psci_release_power_domain_tree_locks(unsigned int end_power_level,
                                             const unsigned int *parent_nodes)
{
    unsigned int parent_index;
    for (unsigned int level = end_power_level; level >= (PSCI_CPU_POWER_LEVEL + 1U); level--) {
        parent_index = parent_nodes[level - 1U];
        hv_psci_release_lock(&psci_non_cpu_nodes[parent_index]);
    }
}

//
// Description:
// This function acquires locks for the desired power level in the power domain tree.
//
// Return value:
//    None.
//
void hv_psci_acquire_power_domain_tree_locks(unsigned int end_power_level,
                                             const unsigned int *parent_nodes)
{
    unsigned int parent_index;

    //
    // Acquire the spinlock for levels above the CPU (clusters + system)
    //
    for (unsigned int level = 1U; level < end_power_level; level++) {
        printf("PSCI DEBUG: current level: %d, ending power level %d\n", level, end_power_level);
        parent_index = parent_nodes[level - 1U];
        hv_psci_get_lock(&psci_non_cpu_nodes[parent_index]);
    }
}

/**
 * Description:
 *
 * This function gets the parent nodes that are tied to a given CPU index.
 *
 * Return value:
 *    None.
 */

void hv_psci_get_parent_nodes(unsigned int cpu_index, unsigned int end_power_level,
                              unsigned int *node_index)
{
    unsigned int *node = node_index;
    unsigned int parent_node = psci_cpu_nodes[cpu_index].parent_node;
    //
    // Traverse the power domain tree backwards to find all the parent nodes of the current node.
    //
    for (unsigned int i = 1; i <= end_power_level; i++) {
        *node = parent_node;
        node++;
        parent_node = psci_non_cpu_nodes[parent_node].parent_node;
    }
}

/**
 * Description:
 *
 * This function powers off the desired node in the PSCI power domain tree hierarchy.
 *
 * Return value:
 * - PSCI_SUCCESS if CPU powered off successfully.
 * - PSCI_OPERATION_DENIED if an error occurred.
 */
int hv_psci_turn_off_cpu(void)
{
    /* The host owns physical power state; no guest CPU_OFF protocol yet. */
    return PSCI_STATUS_NOT_SUPPORTED;
}

//
// Description:
// Finds the highest power domain to be placed in low power state.
//
// Return value:
// Target power level to be suspended.
//
unsigned int hv_psci_find_target_suspend_level(const psci_power_state_status_t *power_state_info)
{
    for (int i = (int)PSCI_MAX_POWER_LEVEL; i >= (int)PSCI_CPU_POWER_LEVEL; i--) {
        if (hv_psci_is_local_state_run(power_state_info->power_domain_state[i]) == 0) {
            return (unsigned int)i;
        }
    }
    return PSCI_INVALID_LEVEL;
}
//
// Description:
// Validates and prepares the PSCI CPU suspend entry point.
//
// Return value:
//    PSCI_SUCCESS if validated, PSCI_STATUS_INVALID_ADDRESS otherwise.
//

int hv_psci_validate_entry_point(entry_point_info_t *entry_point,
                                 unsigned long long cpu_reentry_addr, uint64_t context)
{
    int retval = PSCI_STATUS_SUCCESS;
    uint64_t entry_point_attr;
    // unsigned int daif;
    const unsigned long el_mode = 0x2; // EL2 mode.
    // uint64_t hcr_el2 = mrs(HCR_EL2);
    // sctlr = read_sctlr();
    entry_point_attr = 0x1U; // corresponds to "not secure bit" set, which is unimportant on Apple
                             // platforms, but better safe than sorry since TF-A sets it.
    entry_point->header.type = (uint8_t)PARAMETER_ENTRY_POINT; // sets it as an entry point
    entry_point->header.version = (uint8_t)0x01U;
    entry_point->header.size = (uint16_t)sizeof(*(entry_point));
    entry_point->header.attributes = (uint32_t)entry_point_attr;

    entry_point->pc = cpu_reentry_addr;
    memset(&entry_point->arguments, 0, sizeof(entry_point->arguments));
    entry_point->arguments.arg0 = context;
    entry_point->spsr = SPSR_64(el_mode, SPSR_MODE_SP_ELX, SPSR_DAIF_DISABLE_ALL_EXCEPTIONS);
    return retval;
}

int hv_psci_validate_mpidr_exists(uint64_t mpidr)
{
    return hv_psci_translate_mpidr_to_cpu(mpidr) < MAX_CPUS ? PSCI_STATUS_SUCCESS
                                                            : PSCI_STATUS_INVALID_PARAMETERS;
}

unsigned int hv_psci_translate_mpidr_to_cpu(uint64_t target_cpu)
{
    const uint64_t affinity_mask = 0xff00ffffffULL;
    /* The U/MT bits are not affinity, but callers may pass a read MPIDR. */
    if (target_cpu & ~(affinity_mask | BIT(31) | BIT(24)))
        return MAX_CPUS;
    for (unsigned int dense = 0; dense < psci_num_cores; dense++) {
        unsigned int cpu = psci_active_cpu_ids[dense];
        if ((target_cpu & affinity_mask) == (psci_cpu_nodes[cpu].mpidr & affinity_mask))
            return cpu;
    }
    return MAX_CPUS;
}

/**
 * Description:
 *
 * This function powers on the core.
 *
 * Return value:
 * - PSCI_SUCCESS if CPU powered on successfully.
 * - PSCI_OPERATION_DENIED if an error occurred.
 */
int hv_psci_turn_on_cpu(uint64_t target_cpu, uint64_t entry_point, uint64_t context_id)
{
    unsigned int cpu = hv_psci_translate_mpidr_to_cpu(target_cpu);
    if (cpu >= MAX_CPUS)
        return PSCI_STATUS_INVALID_PARAMETERS;
    if (!entry_point || (entry_point & 3) || !hv_pt_is_ram(entry_point))
        return PSCI_STATUS_INVALID_ADDRESS;
    if (!smp_is_alive(cpu))
        return PSCI_STATUS_INTERNAL_FAILURE;

    affinity_info_state_t expected = AFFINITY_STATE_OFF;
    if (!__atomic_compare_exchange_n(&psci_cpu_data_array[cpu].affinity_state, &expected,
                                     AFFINITY_STATE_ON_PENDING, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return expected == AFFINITY_STATE_ON ? PSCI_STATUS_ALREADY_ON : PSCI_STATUS_ON_PENDING;

    uint64_t regs[4] = {context_id, 0, 0, 0};
    int ret = hv_start_secondary(cpu, (void *)entry_point, regs);
    /* A timed-out handoff may enter late. Do not permit a second launch. */
    affinity_info_state_t state = !ret                 ? AFFINITY_STATE_ON
                                  : ret == SMP_TIMEOUT ? AFFINITY_STATE_ON_PENDING
                                                       : AFFINITY_STATE_OFF;
    __atomic_store_n(&psci_cpu_data_array[cpu].affinity_state, state, __ATOMIC_RELEASE);
    return ret ? PSCI_STATUS_INTERNAL_FAILURE : PSCI_STATUS_SUCCESS;
}

static int hv_psci_affinity_info(uint64_t target_cpu, uint64_t lowest_level)
{
    if (lowest_level != 0)
        return PSCI_STATUS_INVALID_PARAMETERS;
    unsigned int cpu = hv_psci_translate_mpidr_to_cpu(target_cpu);
    if (cpu >= MAX_CPUS)
        return PSCI_STATUS_INVALID_PARAMETERS;
    return __atomic_load_n(&psci_cpu_data_array[cpu].affinity_state, __ATOMIC_ACQUIRE);
}

//
// Description:
// Saves the context for re-entry from suspend.
//
// Return value:
// None.
//
void hv_psci_build_saved_cpu_context(const entry_point_info_t *entry_point)
{
    UNUSED(entry_point);
}

//
// Description:
// Does preparation to do a "power down" suspend.
//
// Return value:
// None.
//
void hv_psci_start_suspend_to_power_down(unsigned int end_power_level,
                                         const entry_point_info_t *entry_point,
                                         const psci_power_state_status_t *power_state_info)
{
    unsigned int max_off_level = hv_psci_find_max_off_level(power_state_info);
    unsigned int cpu_identifier = hv_psci_get_core_position();
    hv_psci_set_suspend_power_level(end_power_level);
    uint64_t ptr_of_target_power_level =
        (uint64_t)psci_cpu_data_array[cpu_identifier].target_power_level;
    dc_civac_range(((void *)ptr_of_target_power_level),
                   sizeof(((void *)ptr_of_target_power_level)));
    UNUSED(entry_point);
    //
    // ARM TF-A uses this, but do we need to save this?
    //
    // hv_psci_build_saved_cpu_context(entry_point);
    hv_psci_power_down_cpu_maintenance(max_off_level);
}

//
// TODO: add a description.
//
void hv_psci_set_power_domains_to_on_state(unsigned int end_power_level)
{
    unsigned int parent_index;
    unsigned int cpu_index = hv_psci_get_core_position();
    unsigned int level;
    parent_index = psci_cpu_nodes[cpu_index].parent_node;

    for (level = PSCI_CPU_POWER_LEVEL + 1U; level <= end_power_level; level++) {
        hv_psci_set_non_cpu_power_domain_node_local_state(parent_index, PSCI_ON_STATE);
        hv_psci_set_requested_local_power_state(level, cpu_index, PSCI_ON_STATE);
        parent_index = psci_non_cpu_nodes[parent_index].parent_node;
    }

    hv_psci_set_affinity_info_state(AFFINITY_STATE_ON);
    hv_psci_set_cpu_local_state(PSCI_ON_STATE);
    dc_civac_range((void *)&psci_cpu_data_array, sizeof(psci_cpu_data_array));
}

//
// Description:
// Validates the PSCI suspend request and makes sure no higher power level is turned off if
// the request is for a CPU to be put on standby.
//
// Return value:
//    PSCI_SUCCESS if validated successfully, PSCI_INVALID_PARAMETERS otherwise.
//
int hv_psci_validate_suspend_request(const psci_power_state_status_t *power_state_info,
                                     unsigned int is_power_down_state)
{
    unsigned int max_power_off_level, target_level, max_retention_level;
    platform_local_state_t platform_state;
    platform_local_state_type_t requested_state_type, lowest_state_type;

    target_level = hv_psci_find_target_suspend_level(power_state_info);
    if (target_level == PSCI_INVALID_LEVEL) {
        return PSCI_STATUS_INVALID_PARAMETERS;
    }
    lowest_state_type = PSCI_ON_STATE;
    for (int i = (int)target_level; i >= (int)PSCI_CPU_POWER_LEVEL; i--) {
        platform_state = power_state_info->power_domain_state[i];
        requested_state_type = hv_psci_power_state_categorize_type(platform_state);
        if (requested_state_type < lowest_state_type) {
            return PSCI_STATUS_INVALID_PARAMETERS;
        }
        lowest_state_type = requested_state_type;
    }
    max_power_off_level = hv_psci_find_max_off_level(power_state_info);

    max_retention_level = PSCI_INVALID_LEVEL;
    if (target_level != max_power_off_level) {
        max_retention_level = target_level;
    }

    if ((is_power_down_state == 0U) && ((max_power_off_level != PSCI_INVALID_LEVEL) ||
                                        (max_retention_level == PSCI_INVALID_LEVEL))) {
        return PSCI_STATUS_INVALID_PARAMETERS;
    }
    return PSCI_STATUS_SUCCESS;
}

//
// Description:
// Operations to be done after wake up from standby/s2idle state.
//
// Return value:
// None.
//
void hv_psci_finish_cpu_suspend(unsigned int cpu_index, unsigned int end_power_level)
{
    unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};
    psci_power_state_status_t power_state_info;

    hv_psci_get_parent_nodes(cpu_index, end_power_level, parent_nodes);

    hv_psci_acquire_power_domain_tree_locks(end_power_level, parent_nodes);

    hv_psci_get_target_local_power_states(end_power_level, &power_state_info);

    //
    // Set power domain state to ON state.
    //
    hv_psci_set_power_domains_to_on_state(end_power_level);

    hv_psci_release_power_domain_tree_locks(end_power_level, parent_nodes);
}

//
// Description:
// Suspends a power domain node in the PSCI power domiin tree.
//
// Return value:
// Status of the final suspend operation.
//
int hv_psci_start_cpu_suspend(const entry_point_info_t *entry_point, unsigned int end_power_level,
                              psci_power_state_status_t *power_state_info,
                              unsigned int is_power_down_state)
{
    int retval = PSCI_STATUS_SUCCESS;
    bool skip_wfi = false;
    unsigned int cpu_index = hv_psci_get_core_position();
    unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};

    hv_psci_get_parent_nodes(cpu_index, end_power_level, parent_nodes);

    //
    // Acquire power domain spinlocks to get a static snapshot to manage the states.
    //
    hv_psci_acquire_power_domain_tree_locks(end_power_level, parent_nodes);

    //
    // If there's any pending interrupt to be serviced, stop the suspend early.
    //
    uint64_t isr = mrs(ISR_EL1);
    if (isr != 0) {
        skip_wfi = true;
        goto exit_suspend;
    }

    hv_psci_coordinate_power_states(end_power_level, power_state_info);

    if (is_power_down_state != 0) {
        //
        // Do preparation for a "power down" suspend.
        //
        hv_psci_start_suspend_to_power_down(end_power_level, entry_point, power_state_info);
    }
    if (power_state_info->power_domain_state[PSCI_CPU_POWER_LEVEL] == PSCI_IDLE_STANDBY_STATE) {
        //
        // TODO: Deep sleep specific operations, only going to use "shallow sleep" for now.
        // For shallow sleep there's nothing to really do.
        //
    }
exit_suspend:
    hv_psci_release_power_domain_tree_locks(end_power_level, parent_nodes);
    if (skip_wfi == true) {
        return retval;
    }
    if (is_power_down_state != 0U) {
        //
        // We're going to be doing a deep sleep. Note this is not implemented yet, so this code path
        // right now will do nothing.
        //

        // cpu_sleep(true);
    }
    //
    // At this point we are about to execute a context-retaining/shallow WFI. Do so now.
    //
    sysop("isb");
    cpu_wfi_stateless();

    hv_psci_finish_cpu_suspend(cpu_index, end_power_level);
    return retval;
}

/**
 * Description:
 *
 * This function handles the PSCI function ID call to suspend a core.
 *
 * Arguments:
 *
 * - power_state - desired power state to set a CPU to
 * - cpu_reentry_addr - the address to resume a CPU's execution at
 * - context - CPU context.
 *
 * Note that "context" is only valid if the desired state is power down, per ARM document DEN0022
 */
int hv_psci_suspend_cpu(uint64_t power_state, uint64_t cpu_reentry_addr, uint64_t context)
{
    UNUSED(power_state);
    UNUSED(cpu_reentry_addr);
    UNUSED(context);
    return PSCI_STATUS_NOT_SUPPORTED;
}

//
// Description:
// Reboots the entire system. As simple as it sounds.
// This function should not return.
//
// Return value:
// N/A
//
void hv_psci_reset_system(void)
{
    iodev_console_flush();
    //
    // TODO: replace this placeholder reboot with a poweroff.
    //
    reboot();
}

//
// Description:
// Turns off the entire system. As simple as it sounds.
// This function should not return.
//
// Return value:
// N/A
//
void hv_psci_turn_off_system(void)
{
    flush_and_reboot();
}

//
// Description:
// Returns if the feature is supported.
//
// Return value:
// PSCI_STATUS_SUCCESS if supported, PSCI_STATUS_NOT_SUPPORTED otherwise.
//
int hv_psci_features(unsigned int function_id)
{
    /* Exact IDs avoid accepting unrelated SMCs that share their low 5 bits. */
    switch (function_id) {
        case PSCI_GET_VERSION_FUNCTION_ID:
        case PSCI_CPU_ON_ARM32_FUNCTION_ID:
        case PSCI_CPU_ON_ARM64_FUNCTION_ID:
        case PSCI_AFFINITY_INFO_ARM32_FUNCTION_ID:
        case PSCI_AFFINITY_INFO_ARM64_FUNCTION_ID:
        case PSCI_SYSTEM_POWEROFF_FUNCTION_ID:
        case PSCI_SYSTEM_RESET_FUNCTION_ID:
        case PSCI_FEATURES_FUNCTION_ID:
            return PSCI_STATUS_SUCCESS;
        default:
            return PSCI_STATUS_NOT_SUPPORTED;
    }
}

//
// Description:
// Checks the specified memory range to see if it's protected by PSCI_MEM_PROTECT.
// Currently a stub for now.
//
// Return value:
// PSCI_STATUS_SUCCESS if the range is protected, PSCI_STATUS_OPERATION_DENIED otherwise.
//
int hv_psci_mem_protect_check_range(unsigned long long base, unsigned long length)
{
    UNUSED(base);
    UNUSED(length);
    return PSCI_STATUS_NOT_SUPPORTED;
}

//
// Description:
// Returns the current status of PSCI memory protection and if asked for, enables it.
//
// Return value:
//    0 if disabled, nonzero if enabled.
//
uint64_t hv_psci_mem_protect(unsigned int enable_mem_protect)
{
    UNUSED(enable_mem_protect);
    return PSCI_STATUS_NOT_SUPPORTED;
}

bool hv_handle_psci_smc(struct exc_info *ctx)
{
    uint64_t function_id = ctx->regs[0];
    uint64_t a1 = ctx->regs[1], a2 = ctx->regs[2], a3 = ctx->regs[3];
    if (!(function_id & SMC_64_FUNCTION)) {
        a1 = (uint32_t)a1;
        a2 = (uint32_t)a2;
        a3 = (uint32_t)a3;
    }

    int result = PSCI_STATUS_NOT_SUPPORTED;
    switch (function_id) {
        case PSCI_GET_VERSION_FUNCTION_ID:
            result = PSCI_VERSION;
            break;
        case PSCI_FEATURES_FUNCTION_ID:
            result = hv_psci_features(a1);
            break;
        case PSCI_CPU_ON_ARM32_FUNCTION_ID:
        case PSCI_CPU_ON_ARM64_FUNCTION_ID:
            result = hv_psci_turn_on_cpu(a1, a2, a3);
            break;
        case PSCI_AFFINITY_INFO_ARM32_FUNCTION_ID:
        case PSCI_AFFINITY_INFO_ARM64_FUNCTION_ID:
            result = hv_psci_affinity_info(a1, a2);
            break;
        case PSCI_SYSTEM_POWEROFF_FUNCTION_ID:
        case PSCI_SYSTEM_RESET_FUNCTION_ID:
            hv_windows_system_exit(function_id == PSCI_SYSTEM_RESET_FUNCTION_ID);
    }
    ctx->regs[0] = result;
    return true;
}
