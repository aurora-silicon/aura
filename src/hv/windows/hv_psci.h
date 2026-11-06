/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors), 2023.
 *
 * Filename: hv_psci.c
 *
 * Description: PSCI definition header file.
 *
 * Note: for bare metal booting, we use a higher level firmware running in GL2 to provide PSCI, so
 * this file does not account for that case.
 *
 * Implementation is based off the version in ARM Trusted Firmware-A.
 * (https://github.com/ARM-software/arm-trusted-firmware)
 *
 * License: SPDX-License-Identifier: MIT OR BSD-3-Clause
 */

#ifndef HV_PSCI_H
#define HV_PSCI_H

#define PSCI_MAJOR_VER_1 (1U << 16)
#define PSCI_MINOR_VER_0 0
#define PSCI_MINOR_VER_1 1

//
// PSCI definition macros.
//
//
// A max power level of 2 corresponds to MPIDR Aff2 (this is how ARM Trusted Firmware defines it, it
// permits control of core, cluster, and system power states.)
//
#define PSCI_CPU_POWER_LEVEL     0U
#define PSCI_CLUSTER_POWER_LEVEL 1U
#define PSCI_MAX_POWER_LEVEL     2U
#define CLUSTER_NUMBER_MASK      (0xff << 8)
#define CORE_NUMBER_MASK         0xff
#define NUM_SYSTEMS_ACTIVE       1
#define PSCI_ON_STATE            0U
//
// Clusters do not support retention/standby power state.
//
#define PSCI_IDLE_STANDBY_STATE 1U
#define PSCI_OFF_STATE          2U
//
// cannot power manage above level 2, aka the system itself.
//
#define PSCI_INVALID_LEVEL 3U

#define PSCI_MAX_RETENTION_STATE        1U
#define PSCI_MAX_OFF_STATE              2U
#define PSCI_STATE_VALID_MASK           0xB0000000U
#define PSCI_STATE_TYPE_MASK            0x1U
#define PSCI_STATE_TYPE_SHIFT           30U
#define PSCI_STATE_ID_MASK              0xFFFFFFFU
#define PSCI_STATE_ID_SHIFT             0U
#define PSCI_POWER_STATE_TYPE_STANDBY   0x0U
#define PSCI_POWER_STATE_TYPE_POWERDOWN 0x1U
//
// Next two macros from Trusted Firmware-A verbatim, from the QEMU SBSA platform.
//
#define PLAT_LOCAL_PSTATE_WIDTH 4
#define PLAT_LOCAL_PSTATE_MASK  ((1 << PLAT_LOCAL_PSTATE_WIDTH) - 1)

//
// PSCI return values.
//
#define PSCI_STATUS_SUCCESS            0
#define PSCI_STATUS_NOT_SUPPORTED      -1
#define PSCI_STATUS_INVALID_PARAMETERS -2
#define PSCI_STATUS_OPERATION_DENIED   -3
#define PSCI_STATUS_ALREADY_ON         -4
#define PSCI_STATUS_ON_PENDING         -5
#define PSCI_STATUS_INTERNAL_FAILURE   -6
#define PSCI_STATUS_NOT_PRESENT        -7
#define PSCI_STATUS_DISABLED           -8
#define PSCI_STATUS_INVALID_ADDRESS    -9

//
// PSCI function IDs.
//
#define PSCI_GET_VERSION_FUNCTION_ID           0x84000000
#define PSCI_SUSPEND_CPU_ARM32_FUNCTION_ID     0x84000001
#define PSCI_CPU_OFF_FUNCTION_ID               0x84000002
#define PSCI_CPU_ON_ARM32_FUNCTION_ID          0x84000003
#define PSCI_SYSTEM_POWEROFF_FUNCTION_ID       0x84000008
#define PSCI_SYSTEM_RESET_FUNCTION_ID          0x84000009
#define PSCI_FEATURES_FUNCTION_ID              0x8400000A
#define PSCI_MEM_PROTECT_FUNCTION_ID           0x84000013
#define PSCI_MEM_CHECK_RANGE_ARM32_FUNCTION_ID 0x84000014
#define PSCI_SUSPEND_CPU_ARM64_FUNCTION_ID     0xC4000001
#define PSCI_MEM_CHECK_RANGE_ARM64_FUNCTION_ID 0xC4000014
#define PSCI_CPU_ON_ARM64_FUNCTION_ID          0xC4000003
#define PSCI_AFFINITY_INFO_ARM32_FUNCTION_ID   0x84000004
#define PSCI_AFFINITY_INFO_ARM64_FUNCTION_ID   0xc4000004
#define PSCI_MIG_ARM64_FUNCTION_ID             0xC4000005
#define PSCI_MIG_INFO_UP_CPU_ARM64_FUNCTION_ID 0xC4000007
#define PSCI_NODE_HW_STATE_ARM64_FUNCTION_ID   0xC400000D
#define PSCI_STAT_RESIDENCY_ARM64_FUNCTION_ID  0xC4000010
#define PSCI_STAT_COUNT_ARM64_FUNCTION_ID      0xC4000011
#define PSCI_SYSTEM_RESET2_ARM64_FUNCTION_ID   0xC4000012
#define SMCCC_VERSION                          0x80000000

static inline unsigned int define_psci_cap(unsigned int x)
{
    return 1U << (x & 0x1fU);
}

#define PSCI_VERSION    (PSCI_MAJOR_VER_1 | PSCI_MINOR_VER_1)
#define SMC_64_FUNCTION BIT(30)
#define PSCI_GENERIC_CAPABILITY                                                                    \
    (define_psci_cap(PSCI_GET_VERSION_FUNCTION_ID) |                                               \
     define_psci_cap(PSCI_AFFINITY_INFO_ARM64_FUNCTION_ID) |                                       \
     define_psci_cap(PSCI_FEATURES_FUNCTION_ID))
#define PSCI_CAP_64BIT_MASK                                                                        \
    (define_psci_cap(PSCI_SUSPEND_CPU_ARM64_FUNCTION_ID) |                                         \
     define_psci_cap(PSCI_CPU_ON_ARM64_FUNCTION_ID) |                                              \
     define_psci_cap(PSCI_AFFINITY_INFO_ARM64_FUNCTION_ID) |                                       \
     define_psci_cap(PSCI_MIG_ARM64_FUNCTION_ID) |                                                 \
     define_psci_cap(PSCI_MIG_INFO_UP_CPU_ARM64_FUNCTION_ID) |                                     \
     define_psci_cap(PSCI_NODE_HW_STATE_ARM64_FUNCTION_ID) |                                       \
     define_psci_cap(PSCI_SUSPEND_CPU_ARM64_FUNCTION_ID) |                                         \
     define_psci_cap(PSCI_STAT_RESIDENCY_ARM64_FUNCTION_ID) |                                      \
     define_psci_cap(PSCI_STAT_COUNT_ARM64_FUNCTION_ID) |                                          \
     define_psci_cap(PSCI_SYSTEM_RESET2_ARM64_FUNCTION_ID) |                                       \
     define_psci_cap(PSCI_MEM_CHECK_RANGE_ARM64_FUNCTION_ID))

//
// PSCI enums.
//

typedef enum {
    AFFINITY_STATE_ON = 0U,
    AFFINITY_STATE_OFF = 1U,
    AFFINITY_STATE_ON_PENDING = 2U
} affinity_info_state_t;

//
// PSCI typedefs and structs.
//

//
// A typedef indicating a platform local state.
//
typedef uint8_t platform_local_state_t;

//
// Struct/typedef storing current CPU's desired power state.
// Taken from ARM Trusted Firmware-A, include\lib\psci\psci.h
//
typedef struct psci_power_state_status {
    //
    // There are a maximum of three power levels (core, cluster, system).
    // This will store the state for each level of the CPU.
    //
    unsigned char power_domain_state[3];

    //
    // highest power level at which current CPU is the last running one.
    //
    unsigned int last_cpu_at_power_level;

} psci_power_state_status_t;

//
// Struct/typedef indicating a CPU power domain node.
//

typedef struct psci_cpu_power_domain_node {
    unsigned long mpidr;
    unsigned int parent_node;
    spinlock_t lock_for_cpu;
} cpu_power_domain_node_t;

typedef enum platform_local_state_type {
    STATE_TYPE_RUN = 0,
    STATE_TYPE_RETN,
    STATE_TYPE_OFF
} platform_local_state_type_t;

//
// Struct describing a non-CPU power domain node.
// Implementation is the one used in Trusted Firmware-A.
//
typedef struct psci_non_cpu_power_domain_node {
    //
    // The first CPU which has this node as its parent.
    //
    unsigned int first_cpu_idx;

    //
    // Sibling nodes of the first CPU.
    //
    unsigned int num_cpu_siblings;

    //
    // Index of the parent of this node.
    //
    unsigned int parent_node;

    //
    // The local power state.
    //
    platform_local_state_t local_power_state;

    //
    // current power level (on/retention/off)
    //
    unsigned char power_level;

    uint16_t lock_index;

} non_cpu_power_domain_node_t;

//
// Struct for holding per CPU information for PSCI code.
//
typedef struct psci_per_cpu_data {
    affinity_info_state_t affinity_state;
    unsigned int target_power_level;
    platform_local_state_t local_cpu_state;
    //
    // To know which CPU we are, according to how Apple hardware understands core position.
    //
    unsigned int cpu_index;
    // Dense position in the PSCI topology (physical ADT cpu-id may be sparse).
    unsigned int topology_index;
    unsigned int cluster_index;
    unsigned int die_index;
    //
    // The "reg" value of a core, aka the lower two bytes of it's MPIDR index.
    //
    unsigned int reg_value;
    //
    // The index of the core *locally* within a cluster.
    //
    unsigned int local_core_number;
} psci_per_cpu_data_t;

typedef struct parameter_header {
    uint8_t type;
    uint8_t version;
    uint16_t size;
    uint32_t attributes;
} parameter_header_t;

typedef struct aarch64_syscall_args {
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
    uint64_t arg6;
    uint64_t arg7;
} aarch64_syscall_args_t;

typedef struct entry_point_info {
    parameter_header_t header;
    unsigned long long pc;
    uint32_t spsr;
    aarch64_syscall_args_t arguments;
} entry_point_info_t;

//
// PSCI function prototypes.
//

unsigned int hv_psci_get_core_position(void);
unsigned int hv_psci_populate_power_domain_tree(const unsigned char *power_domain_tree_map);
void hv_psci_update_power_level_limits(void);
void hv_psci_get_parent_nodes(unsigned int cpu_index, unsigned int end_power_level,
                              unsigned int *node_index);
void hv_psci_init_requested_local_power_states(void);
uint64_t hv_psci_mem_protect(unsigned int enable_mem_protect);
int hv_psci_mem_protect_check_range(unsigned long long base, unsigned long length);
int hv_psci_features(unsigned int psci_function_id);
void hv_psci_turn_off_system(void);
void hv_psci_reset_system(void);
int hv_psci_suspend_cpu(uint64_t power_state, uint64_t cpu_reentry_addr, uint64_t context);
int hv_psci_start_cpu_suspend(const entry_point_info_t *entry_point, unsigned int end_power_level,
                              psci_power_state_status_t *power_state_info,
                              unsigned int is_power_down_state);
void hv_psci_finish_cpu_suspend(unsigned int cpu_index, unsigned int end_power_level);
int hv_psci_validate_suspend_request(const psci_power_state_status_t *power_state_info,
                                     unsigned int is_power_down_state);
void hv_psci_set_power_domains_to_on_state(unsigned int end_power_level);
void hv_psci_start_suspend_to_power_down(unsigned int end_power_level,
                                         const entry_point_info_t *entry_point,
                                         const psci_power_state_status_t *power_state_info);
void hv_psci_build_saved_cpu_context(const entry_point_info_t *entry_point);
int hv_psci_turn_on_cpu(uint64_t target_cpu, uint64_t entry_point, uint64_t context_id);
int hv_psci_validate_mpidr_exists(uint64_t mpidr);
int hv_psci_validate_entry_point(entry_point_info_t *entry_point,
                                 unsigned long long cpu_reentry_addr, uint64_t context);
unsigned int hv_psci_find_target_suspend_level(const psci_power_state_status_t *power_state_info);
int hv_psci_turn_off_cpu(void);
void hv_psci_acquire_power_domain_tree_locks(unsigned int end_power_level,
                                             const unsigned int *parent_nodes);
void hv_psci_release_power_domain_tree_locks(unsigned int end_power_level,
                                             const unsigned int *parent_nodes);
void hv_psci_set_affinity_info_state(affinity_info_state_t state);
void hv_psci_get_target_local_power_states(unsigned int end_power_level,
                                           psci_power_state_status_t *target_state);
platform_local_state_type_t hv_psci_power_state_categorize_type(platform_local_state_t state);
void hv_psci_construct_poweroff_state(psci_power_state_status_t *state_info);
void hv_psci_power_down_cpu_maintenance(unsigned int power_level);
void hv_psci_get_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node);
void hv_psci_release_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node);
void hv_psci_coordinate_power_states(unsigned int end_power_level,
                                     psci_power_state_status_t *current_state_info);
void hv_psci_set_target_local_power_states(unsigned int end_power_level,
                                           const psci_power_state_status_t *target_state);
unsigned int hv_psci_find_max_off_level(const psci_power_state_status_t *state_info);
void hv_psci_set_cpu_local_state(platform_local_state_t desired_state);
void hv_psci_set_non_cpu_power_domain_node_local_state(unsigned int parent_index,
                                                       platform_local_state_t state);
platform_local_state_t hv_psci_get_non_cpu_power_domain_local_state(unsigned int parent_index);
platform_local_state_t hv_psci_get_target_power_state(unsigned int level,
                                                      const platform_local_state_t *states,
                                                      unsigned int num_cpu_siblings);
platform_local_state_t *hv_psci_get_requested_local_power_states(unsigned int power_level,
                                                                 unsigned int cpu_index);
void hv_psci_set_requested_local_power_state(unsigned int power_level, unsigned int cpu_index,
                                             platform_local_state_t requested_power_state);
int hv_psci_validate_power_state(unsigned int power_state,
                                 psci_power_state_status_t *power_state_info);
unsigned int hv_psci_translate_mpidr_to_cpu(uint64_t mpidr);

//
// The following macros are taken from Trusted Firmware-A to support extended state IDs and sanitize
// for valid idle power states.
//

#define apple_make_pwrstate_lvl0(lvl0_state, pwr_lvl, type)                                        \
    (((lvl0_state) << PSCI_STATE_ID_SHIFT) | ((type) << PSCI_STATE_TYPE_SHIFT))

#define apple_make_pwrstate_lvl1(lvl1_state, lvl0_state, pwr_lvl, type)                            \
    (((lvl1_state) << PLAT_LOCAL_PSTATE_WIDTH) |                                                   \
     apple_make_pwrstate_lvl0(lvl0_state, pwr_lvl, type))

#define apple_make_pwrstate_lvl2(lvl2_state, lvl1_state, lvl0_state, pwr_lvl, type)                \
    (((lvl2_state) << (PLAT_LOCAL_PSTATE_WIDTH * 2)) |                                             \
     apple_make_pwrstate_lvl1(lvl1_state, lvl0_state, pwr_lvl, type))

//
// Helper functions to test for a particular power state. Taken verbatim from Trusted Firmware-A.
//

static inline int hv_psci_is_local_state_run(unsigned int plat_local_state)
{
    return (plat_local_state == PSCI_ON_STATE) ? 1 : 0;
}

static inline int hv_psci_is_local_state_retn(unsigned int plat_local_state)
{
    return ((plat_local_state > PSCI_ON_STATE) && (plat_local_state <= PSCI_IDLE_STANDBY_STATE))
               ? 1
               : 0;
}

static inline int hv_psci_is_local_state_off(unsigned int plat_local_state)
{
    return ((plat_local_state > PSCI_IDLE_STANDBY_STATE) && (plat_local_state <= PSCI_OFF_STATE))
               ? 1
               : 0;
}

static inline bool hv_psci_is_cpu_standby_requested(unsigned int is_power_down_state,
                                                    unsigned int retention_lvl)
{
    return (is_power_down_state == 0U) && (retention_lvl == 0U);
}

static inline void hv_psci_lock_init(non_cpu_power_domain_node_t *non_cpu_pd_node, uint16_t index)
{
    non_cpu_pd_node[index].lock_index = index;
}

//
// Miscellaneous defines.
//

#define SPSR_MODE_RW_SHIFT               0x4U
#define SPSR_MODE_EL_MASK                0x3U
#define SPSR_MODE_EL_SHIFT               0x2U
#define SPSR_MODE_SP_SHIFT               0x0U
#define SPSR_MODE_SP_MASK                0x1U
#define SPSR_MODE_SP_EL0                 0x0U
#define SPSR_MODE_SP_ELX                 0x1U
#define SPSR_DAIF_MASK                   0xFU
#define SPSR_DAIF_SHIFT                  0x6U
#define SPSR_SSBS_BIT_AARCH64            BIT(12)
#define SPSR_FIQ_BIT                     BIT(0)
#define SPSR_IRQ_BIT                     BIT(1)
#define SPSR_ABT_BIT                     BIT(2)
#define SPSR_DAIF_DISABLE_ALL_EXCEPTIONS (SPSR_FIQ_BIT | SPSR_FIQ_BIT | SPSR_ABT_BIT)

#define SPSR_64(el, sp, daif)                                                                      \
    (((0 << SPSR_MODE_RW_SHIFT) | (((el) & SPSR_MODE_EL_MASK) << SPSR_MODE_EL_SHIFT) |             \
      (((sp) & SPSR_MODE_SP_MASK) << SPSR_MODE_SP_SHIFT) |                                         \
      (((daif) & SPSR_DAIF_MASK) << SPSR_DAIF_SHIFT)) &                                            \
     (~(SPSR_SSBS_BIT_AARCH64)))

#define PARAMETER_ENTRY_POINT 0x01U;

#define CPU_START_OFF_T8103 0x54000
#define CPU_START_OFF_T8112 0x34000
#define CPU_START_OFF_T6020 0x28000

#define CPU_REG_CORE    GENMASK(7, 0)
#define CPU_REG_CLUSTER GENMASK(10, 8)
#define CPU_REG_DIE     GENMASK(14, 11)

static inline u64 read_sctlr(void)
{
    sysop("isb");
    return mrs(SCTLR_EL1);
}

static inline void write_sctlr(u64 val)
{
    msr(SCTLR_EL1, val);
    sysop("isb");
}

#define T8103_NUM_CLUSTERS      2
#define T8112_NUM_CLUSTERS      2
#define T6000_NUM_CLUSTERS      3
#define T6001_NUM_CLUSTERS      T6000_NUM_CLUSTERS
#define T6002_NUM_CLUSTERS      T6001_NUM_CLUSTERS * 2
#define T6020_NUM_CLUSTERS      3
#define T6021_NUM_CLUSTERS      T6020_NUM_CLUSTERS
#define T8103_CORES_PER_CLUSTER 4
#define T8112_CORES_PER_CLUSTER 4
//
// T8142 (M5) is the first supported SoC with *asymmetric* cluster sizes, so it
// cannot use a single CORES_PER_CLUSTER like T8103/T8112. Ten cores total:
//   cluster 0 = cpu0..cpu5, 6x "sawtooth"  (E)
//   cluster 1 = cpu6..cpu9, 4x "everest"   (P)
// Read off this machine's own ADT (platform-data/t8142-inventory.txt), and it
// matches Apple's published M5 spec (10-core: 6 efficiency + 4 performance).
//
#define T8142_NUM_CLUSTERS         2
#define T8142_E_CLUSTER_CORE_COUNT 6
#define T8142_P_CLUSTER_CORE_COUNT 4
#define T600X_E_CLUSTER_CORE_COUNT 2
#define T602X_E_CLUSTER_CORE_COUNT 4
#define T600X_P_CLUSTER_CORE_COUNT 4
#define T602X_P_CLUSTER_CORE_COUNT 4

#endif // HV_PSCI_H
