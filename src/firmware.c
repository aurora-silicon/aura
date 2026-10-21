/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "firmware.h"
#include "adt.h"
#include "exception.h"
#include "gfx_fw_image_info.h"
#include "mcc.h"
#include "memory.h"
#include "stdint.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"
#include "libfdt/libfdt_env.h"

struct fw_version_info os_firmware;
struct fw_version_info system_firmware;

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

const struct fw_version_info fw_versions[NUM_FW_VERSIONS] = {
    // clang-format off
    [V_UNKNOWN]  = {V_UNKNOWN,  "unknown",     {0, 0, 0},      3, "unknown"},
    [V11_0B1]    = {V11_0B1,    "11.0 beta",   {10, 98, 1},    3, "iBoot-6603.110.6.3"},
    [V11_0B3]    = {V11_0B3,    "11.0 beta3",  {10, 98, 3},    3, "iBoot-6671.0.0.0.6"},
    [V11_0B4]    = {V11_0B4,    "11.0 beta4",  {10, 98, 4},    3, "iBoot-6723.0.0.141.6"},
    [V11_0B5]    = {V11_0B5,    "11.0 beta5",  {10, 98, 5},    3, "iBoot-6723.40.26.151.1"},
    [V11_0B6]    = {V11_0B6,    "11.0 beta6",  {10, 98, 6},    3, "iBoot-6723.40.67.0.1"},
    [V11_0B7]    = {V11_0B7,    "11.0 beta7",  {10, 98, 7},    3, "iBoot-6723.40.86.0.5"},
    [V11_0B9]    = {V11_0B9,    "11.0 beta9",  {10, 98, 9},    3, "iBoot-6723.40.120.0.2"},
    [V11_0B10]   = {V11_0B10,   "11.0 beta10", {10, 98, 10},   3, "iBoot-6723.40.151"},
    [V11_0]      = {V11_0,      "11.0",        {11, 0, 0},     3, "iBoot-6723.41.11"},
    [V11_0_1]    = {V11_0_1,    "11.0.1",      {11, 0, 1},     3, "iBoot-6723.50.2"},
    [V11_1B1]    = {V11_1B1,    "11.1 beta1",  {11, 0, 98, 1}, 4, "iBoot-6723.60.62"},
    [V11_1]      = {V11_1,      "11.1",        {11, 1, 0},     3, "iBoot-6723.61.3"},
    [V11_2B1]    = {V11_2B1,    "11.2 beta1",  {11, 1, 98, 1}, 4, "iBoot-6723.80.14"},
    [V11_2B2]    = {V11_2B2,    "11.2 beta2",  {11, 1, 98, 2}, 4, "iBoot-6723.80.17"},
    [V11_2]      = {V11_2,      "11.2",        {11, 2, 0},     3, "iBoot-6723.81.1"},
    [V11_3B1]    = {V11_3B1,    "11.3 beta1",  {11, 2, 98, 1}, 4, "iBoot-6723.100.321"},
    [V11_3B2]    = {V11_3B2,    "11.3 beta2",  {11, 2, 98, 2}, 4, "iBoot-6723.100.337.121.1"},
    [V11_3B3]    = {V11_3B3,    "11.3 beta3",  {11, 2, 98, 3}, 4, "iBoot-6723.100.360"},
    [V11_3B4]    = {V11_3B4,    "11.3 beta4",  {11, 2, 98, 4}, 4, "iBoot-6723.101.3"},
    [V11_3]      = {V11_3,      "11.3",        {11, 3, 0},     3, "iBoot-6723.101.4"},
    [V11_4B1]    = {V11_4B1,    "11.4 beta1",  {11, 3, 98, 1}, 4, "iBoot-6723.120.33.0.1"},
    [V11_4B2]    = {V11_4B2,    "11.4 beta2",  {11, 3, 98, 2}, 4, "iBoot-6723.120.35"},
    [V11_4]      = {V11_4,      "11.4",        {11, 4, 0},     3, "iBoot-6723.120.36"},
    [V11_5B3]    = {V11_5B3,    "11.5 beta3",  {11, 4, 98, 3}, 4, "iBoot-6723.120.36.131.1"},
    [V11_5B4]    = {V11_5B4,    "11.5 beta4",  {11, 4, 98, 4}, 4, "iBoot-6723.140.1"},
    [V11_5]      = {V11_5,      "11.5",        {11, 5, 0},     3, "iBoot-6723.140.2"},
    [V12_0B1]    = {V12_0B1,    "12.0 beta",   {11, 98, 1},    3, "iBoot-7429.0.72.0.3"},
    [V12_0B2]    = {V12_0B2,    "12.0 beta2",  {11, 98, 2},    3, "iBoot-7429.0.133.121.1"},
    [V12_0B3]    = {V12_0B3,    "12.0 beta3",  {11, 98, 3},    3, "iBoot-7429.0.181.131.1"},
    [V12_0B4]    = {V12_0B4,    "12.0 beta4",  {11, 98, 4},    3, "iBoot-7429.0.207.141.1"},
    [V12_0B5]    = {V12_0B5,    "12.0 beta5",  {11, 98, 5},    3, "iBoot-7429.30.8.0.4"},
    [V12_0B6]    = {V12_0B6,    "12.0 beta6",  {11, 98, 6},    3, "iBoot-7429.40.38"},
    [V12_0B7]    = {V12_0B7,    "12.0 beta7",  {11, 98, 7},    3, "iBoot-7429.40.68"},
    [V12_0B8]    = {V12_0B8,    "12.0 beta8",  {11, 98, 8},    3, "iBoot-7429.40.84.181.1"},
    [V12_0B9]    = {V12_0B9,    "12.0 beta9",  {11, 98, 9},    3, "iBoot-7429.40.92.0.5"},
    [V12_0B10]   = {V12_0B10,   "12.0 beta10", {11, 98, 10},   3, "iBoot-7429.41.4"},
    [V12_0]      = {V12_0,      "12.0",        {12, 0, 0},     3, "iBoot-7429.30.65"},
    [V12_0_1RC1] = {V12_0_1RC1, "12.0.1 RC",   {12, 0, 0, 1},  4, "iBoot-7429.41.4"},
    [V12_0_1]    = {V12_0_1,    "12.0.1",      {12, 0, 1},     3, "iBoot-7429.41.5"},
    [V12_1B1]    = {V12_1B1,    "12.1 beta",   {12, 0, 98, 1}, 4, "iBoot-7429.60.27.0.1"},
    [V12_1B2]    = {V12_1B2,    "12.1 beta2",  {12, 0, 98, 2}, 4, "iBoot-7429.60.41"},
    [V12_1B3]    = {V12_1B3,    "12.1 beta3",  {12, 0, 98, 3}, 4, "iBoot-7429.60.47"},
    [V12_1]      = {V12_1,      "12.1",        {12, 1, 0},     3, "iBoot-7429.61.2"},
    [V12_2B1]    = {V12_2B1,    "12.2 beta",   {12, 1, 98, 1}, 4, "iBoot-7429.80.17"},
    [V12_2B2]    = {V12_2B2,    "12.2 beta2",  {12, 1, 98, 2}, 4, "iBoot-7429.80.33"},
    [V12_2]      = {V12_2,      "12.2",        {12, 2, 0},     3, "iBoot-7429.81.3"},
    [V12_3B1]    = {V12_3B1,    "12.3 beta",   {12, 2, 98, 1}, 4, "iBoot-7459.100.487.115.1"},
    [V12_3B2]    = {V12_3B2,    "12.3 beta2",  {12, 2, 98, 2}, 4, "iBoot-7459.100.494.121.1"},
    [V12_3B3]    = {V12_3B3,    "12.3 beta3",  {12, 2, 98, 3}, 4, "iBoot-7459.100.504.0.1"},
    [V12_3]      = {V12_3,      "12.3",        {12, 3, 0},     3, "iBoot-7459.101.2"},
    [V12_3_1]    = {V12_3_1,    "12.3.1",      {12, 3, 1},     3, "iBoot-7459.101.3"},
    [V12_4B1]    = {V12_4B1,    "12.4 beta",   {12, 3, 98, 1}, 4, "iBoot-7459.120.39.111.1"},
    [V12_4B2]    = {V12_4B2,    "12.4 beta2",  {12, 3, 98, 2}, 4, "iBoot-7459.120.45.0.4"},
    [V12_4B3]    = {V12_4B3,    "12.4 beta3",  {12, 3, 98, 3}, 4, "iBoot-7459.120.56.0.2"},
    [V12_4B4]    = {V12_4B4,    "12.4 beta4",  {12, 3, 98, 4}, 4, "iBoot-7459.121.2"},
    [V12_4]      = {V12_4,      "12.4",        {12, 4, 0},     3, "iBoot-7459.121.3"},
    [V12_5B1]    = {V12_5B1,    "12.5 beta",   {12, 4, 98, 1}, 4, "iBoot-7459.140.8"},
    [V12_5B2]    = {V12_5B2,    "12.5 beta2",  {12, 4, 98, 2}, 4, "iBoot-7459.140.10"},
    [V12_5B3]    = {V12_5B3,    "12.5 beta3",  {12, 4, 98, 3}, 4, "iBoot-7459.140.12.0.1"},
    [V12_5B4]    = {V12_5B4,    "12.5 beta4",  {12, 4, 98, 4}, 4, "iBoot-7459.140.15"},
    [V12_5]      = {V12_5,      "12.5",        {12, 5, 0},     3, "iBoot-7459.141.1"},
    // Same as 12.5
    // {V12_6, "12.6", {12, 6, 0}, 3, "iBoot-7459.141.1"},
    [V12_6_8]    = {V12_6_8,    "12.6.8",      {12, 6, 8},     3, "iBoot-7459.141.1.700.1"},
    [V12_7_1]    = {V12_7_1,    "12.7.1",      {12, 7, 1},     3, "iBoot-7459.141.1.701.1"},
    [V13_0B1]    = {V13_0B1,    "13.0 beta",   {12, 98, 1},    3, "iBoot-8419.0.42.111.3"},
    [V13_0B2]    = {V13_0B2,    "13.0 beta2",  {12, 98, 2},    3, "iBoot-8419.0.79.0.2"},
    [V13_0B3]    = {V13_0B3,    "13.0 beta3",  {12, 98, 3},    3, "iBoot-8419.0.113.0.1"},
    [V13_0B4]    = {V13_0B4,    "13.0 beta4",  {12, 98, 4},    3, "iBoot-8419.0.151.0.1"},
    [V13_0B5]    = {V13_0B5,    "13.0 beta5",  {12, 98, 5},    3, "iBoot-8419.40.2.0.5"},
    [V13_0B6]    = {V13_0B6,    "13.0 beta6",  {12, 98, 6},    3, "iBoot-8419.40.33.0.1"},
    [V13_0B7]    = {V13_0B7,    "13.0 beta7",  {12, 98, 7},    3, "iBoot-8419.40.75"},
    [V13_0B8]    = {V13_0B8,    "13.0 beta8",  {12, 98, 8},    3, "iBoot-8419.40.95.0.1"},
    [V13_0B9]    = {V13_0B9,    "13.0 beta9",  {12, 98, 9},    3, "iBoot-8419.40.102.0.2"},
    [V13_0B10]   = {V13_0B10,   "13.0 beta10", {12, 98, 10},   3, "iBoot-8419.41.6"},
    [V13_0]      = {V13_0,      "13.0",        {13, 0, 0},     3, "iBoot-8419.41.10"},
    [V13_1B1]    = {V13_1B1,    "13.1 beta",   {13, 0, 98, 1}, 4, "iBoot-8419.60.31"},
    [V13_1B2]    = {V13_1B2,    "13.1 beta2",  {13, 0, 98, 2}, 4, "iBoot-8419.60.38.0.1"},
    [V13_1]      = {V13_1,      "13.1",        {13, 1, 0},     3, "iBoot-8419.60.44"},
    [V13_2B1]    = {V13_2B1,    "13.2 beta",   {13, 1, 98, 1}, 4, "iBoot-8419.80.3"},
    [V13_2B2]    = {V13_2B2,    "13.2 beta2",  {13, 1, 98, 2}, 4, "iBoot-8419.80.4.0.1"},
    [V13_2]      = {V13_2,      "13.2",        {13, 2, 0},     3, "iBoot-8419.80.7"},
    [V13_3B1]    = {V13_3B1,    "13.3 beta",   {13, 2, 98, 1}, 4, "iBoot-8422.100.610.0.1"},
    [V13_3B2]    = {V13_3B2,    "13.3 beta2",  {13, 2, 98, 2}, 4, "iBoot-8422.100.640.505.1"},
    [V13_3B3]    = {V13_3B3,    "13.3 beta3",  {13, 2, 98, 3}, 4, "iBoot-8422.100.645.0.1"},
    [V13_3]      = {V13_3,      "13.3",        {13, 3, 0},     3, "iBoot-8422.100.650"},
    [V13_4B1]    = {V13_4B1,    "13.4 beta",   {13, 3, 98, 1}, 4, "iBoot-8422.120.33"},
    [V13_4B2]    = {V13_4B2,    "13.4 beta2",  {13, 3, 98, 2}, 4, "iBoot-8422.120.55"},
    [V13_4B3]    = {V13_4B3,    "13.4 beta3",  {13, 3, 98, 3}, 4, "iBoot-8422.120.65"},
    [V13_4]      = {V13_4,      "13.4",        {13, 4, 0},     3, "iBoot-8422.121.1"},
    [V13_4_1]    = {V13_4_1,    "13.4.1",      {13, 4, 1},     3, "iBoot-8422.121.3.1.1"},
    [V13_5B1]    = {V13_5B1,    "13.5 beta",   {13, 4, 98, 1}, 4, "iBoot-8422.140.18.0.2"},
    [V13_5B2]    = {V13_5B2,    "13.5 beta2",  {13, 4, 98, 2}, 4, "iBoot-8422.140.32"},
    [V13_5B3]    = {V13_5B3,    "13.5 beta3",  {13, 4, 98, 3}, 4, "iBoot-8422.140.46.505.1"},
    [V13_5B4]    = {V13_5B4,    "13.5 beta4",  {13, 4, 98, 4}, 4, "iBoot-8422.140.50.0.2"},
    [V13_5B5]    = {V13_5B5,    "13.5 beta5",  {13, 4, 98, 5}, 4, "iBoot-8422.141.1"},
    [V13_5]      = {V13_5,      "13.5",        {13, 5, 0},     3, "iBoot-8422.141.2"},
    [V13_6_1]    = {V13_6_1,    "13.6.1",      {13, 6, 1},     3, "iBoot-8422.141.2.700.1"},
    [V14_0B1]    = {V14_0B1,    "14.0 beta",   {13, 98, 1},    3, "iBoot-8419.0.42.111.3"},
    [V14_0B2]    = {V14_0B2,    "14.0 beta2",  {13, 98, 2},    3, "iBoot-10151.0.156.505.1"},
    [V14_0B3]    = {V14_0B3,    "14.0 beta3",  {13, 98, 3},    3, "iBoot-10151.0.172.0.3"},
    [V14_0B4]    = {V14_0B4,    "14.0 beta4",  {13, 98, 4},    3, "iBoot-10151.0.255.0.4"},
    [V14_0B5]    = {V14_0B5,    "14.0 beta5",  {13, 98, 5},    3, "iBoot-10151.0.305.0.1"},
    [V14_0]      = {V14_0,      "14.0",        {14, 0, 0},     3, "iBoot-10151.1.1"},
    [V14_1B1]    = {V14_1B1,    "14.1 beta",   {14, 0, 98, 1}, 4, "iBoot-10151.40.132"},
    [V14_1B2]    = {V14_1B2,    "14.1 beta2",  {14, 0, 98, 2}, 4, "iBoot-10151.40.171.501.2"},
    [V14_1B3]    = {V14_1B3,    "14.1 beta3",  {14, 0, 98, 3}, 4, "iBoot-10151.41.10"},
    [V14_1]      = {V14_1,      "14.1",        {14, 1, 0},     3, "iBoot-10151.41.12"},
    [V14_2B1]    = {V14_2B1,    "14.2 beta",   {14, 1, 98, 1}, 4, "iBoot-10151.60.43"},
    [V14_2B2]    = {V14_2B2,    "14.2 beta2",  {14, 1, 98, 2}, 4, "iBoot-10151.60.55"},
    [V14_2B3]    = {V14_2B3,    "14.2 beta3",  {14, 1, 98, 3}, 4, "iBoot-10151.60.56"},
    [V14_2]      = {V14_2,      "14.2",        {14, 2, 0},     3, "iBoot-10151.61.4"},
    [V14_3B1]    = {V14_3B1,    "14.3 beta",   {14, 2, 98, 1}, 4, "iBoot-10151.80.6"},
    [V14_3B2]    = {V14_3B2,    "14.3 beta2",  {14, 2, 98, 2}, 4, "iBoot-10151.80.20"},
    [V14_3]      = {V14_3,      "14.3",        {14, 3, 0},     3, "iBoot-10151.81.1"},
    [V14_4B1]    = {V14_4B1,    "14.4 beta",   {14, 3, 98, 1}, 4, "iBoot-10151.100.738.0.1"},
    [V14_4B2]    = {V14_4B2,    "14.4 beta2",  {14, 3, 98, 2}, 4, "iBoot-10151.100.753"},
    [V14_4B3]    = {V14_4B3,    "14.4 beta3",  {14, 3, 98, 3}, 4, "iBoot-10151.100.756"},
    [V14_4B4]    = {V14_4B4,    "14.4 beta4",  {14, 3, 98, 4}, 4, "iBoot-10151.101.2.501.1"},
    [V14_4]      = {V14_4,      "14.4",        {14, 4, 0},     3, "iBoot-10151.101.3"},
    [V14_5B1]    = {V14_5B1,    "14.5 beta",   {14, 4, 98, 1}, 4, "iBoot-10151.120.115.0.4"},
    [V14_5B2]    = {V14_5B2,    "14.5 beta2",  {14, 4, 98, 2}, 4, "iBoot-10151.120.136"},
    [V14_5B3]    = {V14_5B3,    "14.5 beta3",  {14, 4, 98, 3}, 4, "iBoot-10151.120.136.501.1"},
    [V14_5]      = {V14_5,      "14.5",        {14, 5, 0},     3, "iBoot-10151.121.1"},
    [V14_6B1]    = {V14_6B1,    "14.6 beta",   {14, 5, 98, 1}, 4, "iBoot-10151.140.18"},
    [V14_6]      = {V14_6,      "14.6",        {14, 6, 0},     3, "iBoot-10151.140.19"},
    [V14_7]      = {V14_7,      "14.7",        {14, 7, 0},     3, "iBoot-10151.140.19.700.2"},
    // Same iBoot string as 14.7: detect_firmware() always picks V14_7 (first match).
    // V14_8_3 is only selected from the loaded GPU firmware, see firmware_gfx_version().
    [V14_8_3]    = {V14_8_3,    "14.8.3",      {14, 8, 3},     3, "iBoot-10151.140.19.700.2"},
    [V15_0B1]    = {V15_0B1,    "15.0 beta",   {14, 98, 1},    3, "iBoot-11881.0.80.0.2"},
    [V15_0B2]    = {V15_0B2,    "15.0 beta2",  {14, 98, 2},    3, "iBoot-11881.0.193.501.1"},
    [V15_0B3]    = {V15_0B3,    "15.0 beta3",  {14, 98, 3},    3, "iBoot-11881.0.224.0.2"},
    [V15_0B4]    = {V15_0B4,    "15.0 beta4",  {14, 98, 4},    3, "iBoot-11881.0.285.0.3"},
    [V15_0B5]    = {V15_0B5,    "15.0 beta5",  {14, 98, 5},    3, "iBoot-11881.0.344.0.5"},
    [V15_0]      = {V15_0,      "15.0",        {15, 0, 0},     3, "iBoot-11881.1.1"},
    [V15_1B1]    = {V15_1B1,    "15.1 beta ",  {15, 0, 98, 1}, 4, "iBoot-11881.0.285.0.3"},
    [V15_1B2]    = {V15_1B2,    "15.1 beta2",  {15, 0, 98, 2}, 4, "iBoot-11881.40.88"},
    [V15_1B4]    = {V15_1B4,    "15.1 beta4",  {15, 0, 98, 4}, 4, "iBoot-11881.40.128"},
    [V15_1B5]    = {V15_1B5,    "15.1 beta5",  {15, 0, 98, 5}, 4, "iBoot-11881.40.153"},
    [V15_1B6]    = {V15_1B6,    "15.1 beta6",  {15, 0, 98, 6}, 4, "iBoot-11881.41.3"},
    [V15_1]      = {V15_1,      "15.1",        {15, 1, 0},     3, "iBoot-11881.41.5"},
    [V15_2B1]    = {V15_2B1,    "15.2 beta",   {15, 1, 98, 1}, 4, "iBoot-11881.60.577.0.2"},
    [V15_2B2]    = {V15_2B2,    "15.2 beta2",  {15, 1, 98, 2}, 4, "iBoot-11881.60.606"},
    [V15_2B3]    = {V15_2B3,    "15.2 beta3",  {15, 1, 98, 3}, 4, "iBoot-11881.60.608.501.1"},
    [V15_2B4]    = {V15_2B4,    "15.2 beta4",  {15, 1, 98, 4}, 4, "iBoot-11881.61.2.0.1"},
    [V15_2]      = {V15_2,      "15.2",        {15, 2, 0},     3, "iBoot-11881.61.3"},
    [V15_3B1]    = {V15_3B1,    "15.3 beta",   {15, 2, 98, 1}, 4, "iBoot-11881.80.44"},
    [V15_3B2]    = {V15_3B2,    "15.3 beta2",  {15, 2, 98, 2}, 4, "iBoot-11881.80.54.0.2"},
    [V15_3]      = {V15_3,      "15.3",        {15, 3, 0},     3, "iBoot-11881.81.2"},
    [V15_3_1]    = {V15_3_1,    "15.3.1",      {15, 3, 1},     3, "iBoot-11881.81.4"},
    [V15_4B1]    = {V15_4B1,    "15.4 beta",   {15, 3, 98, 1}, 4, "iBoot-11881.100.964.0.1"},
    [V15_4B2]    = {V15_4B2,    "15.4 beta2",  {15, 3, 98, 2}, 4, "iBoot-11881.100.991.505.1"},
    [V15_4B3]    = {V15_4B3,    "15.4 beta3",  {15, 3, 98, 3}, 4, "iBoot-11881.100.993"},
    [V15_4]      = {V15_4,      "15.4",        {15, 4, 0},     3, "iBoot-11881.101.1"},
    [V15_5B1]    = {V15_5B1,    "15.5 beta",   {15, 4, 98, 1}, 4, "iBoot-11881.120.91.0.1"},
    [V15_5B2]    = {V15_5B2,    "15.5 beta2",  {15, 4, 98, 2}, 4, "iBoot-11881.120.111.0.1"},
    [V15_5B3]    = {V15_5B3,    "15.5 beta3",  {15, 4, 98, 3}, 4, "iBoot-11881.120.116.501.1"},
    [V15_5]      = {V15_5,      "15.5",        {15, 5, 0},     3, "iBoot-11881.121.1"},
    [V15_6B1]    = {V15_6B1,    "15.6 beta",   {15, 5, 98, 1}, 4, "iBoot-11881.140.89"},
    [V15_6B2]    = {V15_6B2,    "15.6 beta2",  {15, 5, 98, 2}, 4, "iBoot-11881.140.90.0.1"},
    [V15_6B3]    = {V15_6B3,    "15.6 beta3",  {15, 5, 98, 3}, 4, "iBoot-11881.140.95"},
    [V15_6]      = {V15_6,      "15.6",        {15, 6, 0},     3, "iBoot-11881.140.96"},
    [V26_0B1]    = {V26_0B1,    "26.0 beta ",  {15, 98, 1},    3, "iBoot-13822.0.88.511.1"},
    [V26_0B2]    = {V26_0B2,    "26.0 beta2",  {15, 98, 2},    3, "iBoot-13822.0.166.0.1"},
    [V26_0B3]    = {V26_0B3,    "26.0 beta3",  {15, 98, 3},    3, "iBoot-13822.0.194.0.3"},
    [V26_0B4]    = {V26_0B4,    "26.0 beta4",  {15, 98, 4},    3, "iBoot-13822.0.233"},
    [V26_0B5]    = {V26_0B5,    "26.0 beta5",  {15, 98, 5},    3, "iBoot-13822.0.277.0.3"},
    [V26_0]      = {V26_0,      "26.0",        {26, 0, 0},     3, "iBoot-13822.1.2"},
    [V26_1B1]    = {V26_1B1,    "26.1 beta",   {26, 0, 98, 1}, 4, "iBoot-13822.40.85"},
    [V26_1B2]    = {V26_1B2,    "26.1 beta2",  {26, 0, 98, 2}, 4, "iBoot-13822.40.107.0.1"},
    [V26_1B3]    = {V26_1B3,    "26.1 beta3",  {26, 0, 98, 3}, 4, "iBoot-13822.40.110"},
    [V26_1]      = {V26_1,      "26.1",        {26, 1, 0},     3, "iBoot-13822.41.1"},
    [V26_2B1]    = {V26_2B1,    "26.2 beta",   {26, 1, 98, 1}, 4, "iBoot-13822.60.24.501.1"},
    [V26_2B2]    = {V26_2B2,    "26.2 beta2",  {26, 1, 98, 2}, 4, "iBoot-13822.60.24.0.4"},
    [V26_2]      = {V26_2,      "26.2",        {26, 2, 0},     3, "iBoot-13822.61.10"},
    [V26_3B1]    = {V26_3B1,    "26.3 beta",   {26, 2, 98, 1}, 4, "iBoot-13822.80.393"},
    [V26_3B2]    = {V26_3B2,    "26.3 beta2",  {26, 2, 98, 2}, 4, "iBoot-13822.80.406"},
    [V26_3]      = {V26_3,      "26.3",        {26, 3, 0},     3, "iBoot-13822.81.10"},
    [V26_4]      = {V26_4,      "26.4",        {26, 4, 0},     3, "mBoot-18000.101.7"},
    [V26_6_2]    = {V26_6_2,    "26.6.2",      {26, 6, 2},     3, "mBoot-18000.161.10"},
    // clang-format on
};

int firmware_set_fdt(void *fdt, int node, const char *prop, const struct fw_version_info *ver)
{
    fdt32_t data[ARRAY_SIZE(ver->num)];

    for (size_t i = 0; i < ver->num_length; i++) {
        data[i] = cpu_to_fdt32(ver->num[i]);
    }

    if (fdt_setprop(fdt, node, prop, data, ver->num_length * sizeof(u32)))
        bail("FDT: couldn't set %s property to firmware info", prop);
    return 0;
}

void firmware_parse_version(const char *s, u32 *out)
{
    memset(out, 0, sizeof(*out) * IBOOT_VER_COMP);

    for (int i = 0; i < IBOOT_VER_COMP; i++) {
        while (*s && !(*s >= '0' && *s <= '9'))
            s++;
        if (!*s)
            break;
        out[i] = atol(s);
        while (*s >= '0' && *s <= '9')
            s++;
    }
}

static void detect_firmware(struct fw_version_info *info, const char *ver)
{
    for (size_t i = 0; i < ARRAY_SIZE(fw_versions); i++) {
        if (fw_versions[i].iboot && !strcmp(fw_versions[i].iboot, ver)) {
            *info = fw_versions[i];
            return;
        }
    }

    *info = fw_versions[V_UNKNOWN];
    info->iboot = ver;
}

bool firmware_iboot_in_range(u32 min[IBOOT_VER_COMP], u32 max[IBOOT_VER_COMP],
                             u32 this[IBOOT_VER_COMP])
{
    int lower = 0;
    int upper = 0;

    for (int i = 0; i < IBOOT_VER_COMP; i++) {
        if (!lower && this[i] != min[i])
            lower = this[i] < min[i] ? -1 : 1;
        if (!upper && this[i] != max[i])
            upper = this[i] < max[i] ? -1 : 1;
    }

    return lower >= 0 && upper < 0;
}

// Note: semi-open range
bool firmware_sfw_in_range(enum fw_version lower_bound, enum fw_version upper_bound)
{
    u32 min[IBOOT_VER_COMP] = {0};
    u32 max[IBOOT_VER_COMP] = {UINT32_MAX};
    u32 this[IBOOT_VER_COMP] = {0};

    if (lower_bound > V_UNKNOWN && lower_bound < NUM_FW_VERSIONS)
        firmware_parse_version(fw_versions[lower_bound].iboot, min);

    if (upper_bound > V_UNKNOWN && upper_bound < NUM_FW_VERSIONS)
        firmware_parse_version(fw_versions[upper_bound].iboot, max);

    firmware_parse_version(system_firmware.iboot, this);

    return firmware_iboot_in_range(min, max, this);
}

int firmware_init(void)
{
    int node = adt_path_offset(adt, "/chosen");

    if (node < 0) {
        printf("ADT: no /chosen found\n");
        return -1;
    }

    u32 len;
    const char *p = adt_getprop(adt, node, "firmware-version", &len);
    if (p && len && p[len - 1] == 0) {
        detect_firmware(&os_firmware, p);
        printf("OS FW version: %s (%s)\n", os_firmware.string, os_firmware.iboot);
    } else {
        printf("ADT: failed to find firmware-version\n");
        return -1;
    }

    p = adt_getprop(adt, node, "system-firmware-version", &len);
    if (p && len && p[len - 1] == 0) {
        detect_firmware(&system_firmware, p);
        printf("System FW version: %s (%s)\n", system_firmware.string, system_firmware.iboot);
    } else {
        printf("ADT: failed to find system-firmware-version\n");
        system_firmware.string = "unknown";
        system_firmware.iboot = "unknown";
        return -1;
    }

    return 0;
}

// clang-format off
/*
 * GPU (gfx-asc) firmware identification.
 *
 * The 14.8.3 firmware reports the same iBoot string as 14.7, so detect_firmware() cannot tell
 * them apart. The GPU firmware ABI is fixed by the image iBoot loaded into the gfx-asc text segment,
 * so identify that image by content instead. The image has an image-info header at text+0x4200
 * that carries its UUID. The code reads these header fields and accepts only the table below:
 *   +0x00 u32 branch over the header (b +0x44 = 0x14000011)
 *   +0x04 u32 magic 'uuid' (0x64697575 LE)
 *   +0x08 u32 header version (5)
 *   +0x0c u32 text offset of the RTKit version string
 *   +0x10 u32 TEXT offset of the client version pointer
 *   +0x14 u8[16] image UUID
 *   +0x24 u32[2] zero
 *   +0x2c u32 patchbay offset, +0x30 u32 patchbay size, +0x34 u32 text size
 *
 * Only an exact match of the whole identifying part of the header (branch, magic, version, UUID,
 * patchbay offset/size, text size) against the table below counts as a positive identification.
 * Anything else (no header, unknown image, read fault, odd ADT) makes firmware_gfx_version() return
 * NULL, and the caller must then leave the GPU disabled.
 */
// clang-format on
struct gfx_fw_image {
    u32 chip; /* the SoC whose GPU runs this image (chip_id) */
    enum fw_version version;
    const char *image;
    u8 uuid[16];
    u32 patchbay_off;
    u32 patchbay_size;
    u32 text_size;
    bool experimental; /* identified only by firmware_gfx_version_ex(chip, true) */
};

// clang-format off
static const struct gfx_fw_image gfx_fw_images[] = {
    /* 14.8.3 G15S firmware (InitData ver_info 0x0c08e21e83800490) */
    {T6030, V14_8_3, "g15s build a0", {0x0c, 0xbb, 0x17, 0xaf, 0x78, 0x3d, 0x36, 0x7b,
                                       0x9c, 0xed, 0xd5, 0x91, 0x09, 0x44, 0x73, 0x1e},
     0x58e88, 0x1e8, 0x5c000},
    {T6030, V14_8_3, "g15s build b0", {0xdb, 0xf3, 0x7c, 0x40, 0xea, 0xd5, 0x37, 0x60,
                                       0x94, 0x41, 0x99, 0x50, 0x18, 0x78, 0x29, 0x55},
     0x58e88, 0x1e8, 0x5c000},
    /*
     * T8122 (M3, G15G): the image iBoot loads on the M3 Air (J613, chip revision C0, RTKit
     * 2419.140.12). Experimental: its ABI is taken to be the 14.8.3 G15S one and is not validated,
     * so firmware_gfx_version(T8122) still identifies nothing and only the opt-in T8122 experiment
     * accepts it.
     */
    {T8122, V14_8_3, "g15g build c0", {0xdf, 0x69, 0x7f, 0x05, 0xf6, 0xb5, 0x33, 0xef,
                                       0xa1, 0x37, 0x61, 0xc4, 0xa7, 0x3d, 0x66, 0x6a},
     0x5d54a, 0x1e8, 0x64000, true},
};
// clang-format on

static bool gfx_fw_range_is_carveout(u64 base, u64 size)
{
    for (size_t i = 0; i < mcc_carveout_count; i++) {
        u64 cb = mcc_carveouts[i].base, ce = cb + mcc_carveouts[i].size;
        if (base < ce && (base + size) > cb)
            return true;
    }
    return false;
}

static bool gfx_fw_range_is_dram(u64 base, u64 size)
{
    if (!ram_base || !mem_size_actual)
        return false;
    if (base + size < base)
        return false;
    return base >= ram_base && (base + size) <= (ram_base + mem_size_actual);
}

/*
 * Read the header with synchronous aborts and SErrors trapped. The dsb/isb before dropping the
 * guard makes sure an asynchronous SError from these loads is taken while the guard is still armed.
 */
static int gfx_fw_read_header(u64 addr, u32 *hdr)
{
    int exc_before = exc_count;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    for (int i = 0; i < GFX_FW_HDR_SIZE / 4; i++)
        hdr[i] = read32(addr + 4 * i);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;

    return exc_count == exc_before ? 0 : -1;
}

/*
 * Return only the fixed image-info identity of the already-loaded GFX image.
 * This diagnostic is not an ABI allowlist and does not start the GPU. The
 * address and length come from iBoot's ADT; reject MMIO, carveouts, malformed
 * extents and any read that faults before copying fields to the caller.
 */
bool firmware_gfx_image_info(struct gfx_fw_image_info *out)
{
    if (!out)
        return false;
    int node = adt_path_offset(adt, "/arm-io/gfx-asc");
    if (node < 0)
        return false;

    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len != 2 * sizeof(*seg))
        return false;
    const char *names = adt_getprop(adt, node, "segment-names", &len);
    if (!names || len < sizeof("__TEXT;") - 1 ||
        memcmp(names, "__TEXT;", sizeof("__TEXT;") - 1))
        return false;

    u64 phys = seg[0].phys;
    u32 size = seg[0].size;
    if (!phys || size < GFX_FW_HDR_OFFSET + GFX_FW_HDR_SIZE || (phys & 3) ||
        phys + GFX_FW_HDR_OFFSET < phys)
        return false;
    u64 addr = phys + GFX_FW_HDR_OFFSET;
    if (!gfx_fw_range_is_dram(addr, GFX_FW_HDR_SIZE) ||
        gfx_fw_range_is_carveout(addr, GFX_FW_HDR_SIZE))
        return false;

    u32 hdr[GFX_FW_HDR_SIZE / 4];
    if (gfx_fw_read_header(addr, hdr))
        return false;
    return gfx_fw_parse_image_info(hdr, size, out);
}

bool firmware_gfx_rtkit_version(char out[GFX_FW_VERSION_MAX])
{
    if (!out)
        return false;
    struct gfx_fw_image_info image;
    if (!firmware_gfx_image_info(&image))
        return false;
    if (image.rtkit_version_offset > image.text_size ||
        GFX_FW_VERSION_MAX > image.text_size - image.rtkit_version_offset)
        return false;

    int node = adt_path_offset(adt, "/arm-io/gfx-asc");
    if (node < 0)
        return false;
    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len != 2 * sizeof(*seg) ||
        seg[0].phys + image.rtkit_version_offset < seg[0].phys)
        return false;
    u64 addr = seg[0].phys + image.rtkit_version_offset;
    if (!gfx_fw_range_is_dram(addr, GFX_FW_VERSION_MAX) ||
        gfx_fw_range_is_carveout(addr, GFX_FW_VERSION_MAX))
        return false;

    u8 bytes[GFX_FW_VERSION_MAX];
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = read8(addr + i);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before && gfx_fw_parse_rtkit_version(bytes, out);
}

/*
 * Returns the firmware version whose GPU ABI is loaded in gfx-asc memory, or NULL if the loaded
 * image could not be positively identified as one of `chip`'s known images (with `experimental`,
 * the experimental ones included). Never faults the boot.
 */
const struct fw_version_info *firmware_gfx_version_ex(u32 chip, bool experimental)
{
    int node = adt_path_offset(adt, "/arm-io/gfx-asc");
    if (node < 0) {
        printf("FW: GFX: /arm-io/gfx-asc not found\n");
        return NULL;
    }

    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len < sizeof(*seg) || len % sizeof(*seg)) {
        printf("FW: GFX: gfx-asc has no valid segment-ranges (len %u)\n", len);
        return NULL;
    }

    for (u32 i = 0; i < len / sizeof(*seg); i++) {
        u32 hdr[GFX_FW_HDR_SIZE / 4];
        u64 phys = seg[i].phys;
        u32 size = seg[i].size;
        u64 addr = phys + GFX_FW_HDR_OFFSET;

        if (!phys || size < GFX_FW_HDR_OFFSET + GFX_FW_HDR_SIZE || (phys & 3))
            continue;
        /* Never issue loads outside DRAM (MMIO reads can have side effects) or into a TZ carveout
         */
        if (!gfx_fw_range_is_dram(addr, GFX_FW_HDR_SIZE)) {
            printf("FW: GFX: segment %u at 0x%lx is not in DRAM, not reading it\n", i, phys);
            continue;
        }
        if (gfx_fw_range_is_carveout(addr, GFX_FW_HDR_SIZE)) {
            printf("FW: GFX: segment %u at 0x%lx is in a carveout, not reading it\n", i, phys);
            continue;
        }
        if (gfx_fw_read_header(addr, hdr)) {
            printf("FW: GFX: fault reading segment %u at 0x%lx\n", i, phys);
            continue;
        }
        if (hdr[1] != GFX_FW_HDR_MAGIC)
            continue;

        const u8 *uuid = (const u8 *)&hdr[5];
        printf("FW: GFX: image uuid "
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
               " hdr v%u pb 0x%x+0x%x text 0x%x (text at 0x%lx)\n",
               uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8],
               uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15], hdr[2], hdr[11],
               hdr[12], hdr[13], phys);

        if (hdr[0] != GFX_FW_HDR_BRANCH || hdr[2] != GFX_FW_HDR_VER || hdr[13] > size) {
            printf("FW: GFX: unexpected image-info header, GPU firmware not identified\n");
            return NULL;
        }

        for (size_t j = 0; j < ARRAY_SIZE(gfx_fw_images); j++) {
            const struct gfx_fw_image *img = &gfx_fw_images[j];

            if (img->chip != chip || (img->experimental && !experimental))
                continue;
            if (memcmp(uuid, img->uuid, sizeof(img->uuid)) || hdr[11] != img->patchbay_off ||
                hdr[12] != img->patchbay_size || hdr[13] != img->text_size)
                continue;

            const struct fw_version_info *ver = &fw_versions[img->version];
            printf("FW: GFX: identified image %s, firmware %s\n", img->image, ver->string);
            if (strcmp(ver->iboot, os_firmware.iboot))
                printf("FW: GFX: note: OS firmware iBoot is %s, the image belongs to %s\n",
                       os_firmware.iboot, ver->iboot);
            return ver;
        }

        printf("FW: GFX: unknown GPU firmware image, not identified\n");
        return NULL;
    }

    printf("FW: GFX: no image-info header found, GPU firmware not identified\n");
    return NULL;
}

const struct fw_version_info *firmware_gfx_version(u32 chip)
{
    return firmware_gfx_version_ex(chip, false);
}
