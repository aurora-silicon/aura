/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "types.h"
typedef struct asc_dev asc_dev_t;
typedef struct rtkit_dev rtkit_dev_t;
typedef struct sart_dev sart_dev_t;
struct rtkit_message {
    u8 ep;
    u64 msg;
};
asc_dev_t *asc_init(const char *path);
void asc_free(asc_dev_t *asc);
void asc_cpu_stop(asc_dev_t *asc);
bool asc_cpu_running(asc_dev_t *asc);
rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, void *dart, void *ops, sart_dev_t *sart,
                        bool sram);
bool rtkit_boot(rtkit_dev_t *rtk);
bool rtkit_quiesce(rtkit_dev_t *rtk);
bool rtkit_quiesce_ap(rtkit_dev_t *rtk);
bool rtkit_sleep(rtkit_dev_t *rtk);
void rtkit_stop(rtkit_dev_t *rtk);
void rtkit_free(rtkit_dev_t *rtk);
int rtkit_recv(rtkit_dev_t *rtk, struct rtkit_message *msg);
struct asc_message {
    u64 msg0;
    u32 msg1;
};
bool asc_cpu_idle(asc_dev_t *asc);
void asc_i2a_enable(asc_dev_t *asc, bool on);
u32 asc_cpu_status(asc_dev_t *asc);
bool asc_recv(asc_dev_t *asc, struct asc_message *msg);
bool rtkit_idle(rtkit_dev_t *rtk);
int rtkit_probe_hello(asc_dev_t *asc);
bool rtkit_boot_warm(rtkit_dev_t *rtk, u32 last_mask);
