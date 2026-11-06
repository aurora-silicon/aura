/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define AURA_WINDOWS 1
#define BIT(n) (1ULL<<(n))
typedef uint64_t u64;
#define HV_WINDOWS_GUEST_COMPAT BIT(0)
#define HV_WINDOWS_FB_SHADOW BIT(1)
#define HV_WINDOWS_SERIAL_CAPTURE BIT(2)
#define HV_WINDOWS_SERIAL_CDC BIT(3)
#define HV_WINDOWS_MTP BIT(4)
#define HV_WINDOWS_CPUFREQ BIT(5)
#define IODEV_USB0 4
#define USB_IODEV_COUNT 2
#define T8142 0x8142
static bool hv_windows_initialized;
static u64 hv_windows_flags;
static unsigned chip_id=0x8142,board_id=0x24,uartproxy_iodev=4,writes;
static bool hv_windows_serial_available(void){return true;}
#include "cpufreq-native.inc"
static int cpufreq_init(void){writes++;return 7;}
static int cpufreq_set_cluster_pstate(unsigned c,unsigned s){(void)c;(void)s;writes++;return 8;}
static u64 cpufreq_get_cluster_hz(unsigned c){(void)c;return 9;}
enum {P_CPUFREQ_INIT,P_CPUFREQ_GET_CLUSTER_HZ,P_CPUFREQ_SET_CLUSTER_PSTATE};
struct req {unsigned opcode;u64 args[2];};struct rep {u64 retval;};
static void dispatch(struct req*request,struct rep*reply){switch(request->opcode){
#include "cpufreq-proxy.inc"
}}
int main(void){struct req q={P_CPUFREQ_INIT,{0,0}};struct rep r={0};
 dispatch(&q,&r);assert(writes==1&&r.retval==7);
 board_id=0x25;assert(hv_windows_configure(HV_WINDOWS_CPUFREQ)<0);assert(!hv_windows_cpufreq_owned());board_id=0x24;
 assert(!hv_windows_configure(HV_WINDOWS_CPUFREQ));assert(hv_windows_cpufreq_owned());assert(hv_windows_configure(0)<0);assert(hv_windows_cpufreq_owned());dispatch(&q,&r);assert(r.retval==(u64)-1&&writes==1);
 q.opcode=P_CPUFREQ_SET_CLUSTER_PSTATE;dispatch(&q,&r);assert(r.retval==(u64)-1&&writes==1);
 hv_windows_initialized=true;assert(hv_windows_configure(0)<0);assert(hv_windows_cpufreq_owned());dispatch(&q,&r);assert(writes==1);
 q.opcode=P_CPUFREQ_GET_CLUSTER_HZ;dispatch(&q,&r);assert(r.retval==9&&writes==1);
 hv_windows_initialized=false;hv_windows_flags=0;dispatch(&q,&r);q.opcode=P_CPUFREQ_SET_CLUSTER_PSTATE;dispatch(&q,&r);assert(r.retval==8&&writes==2);
 chip_id=0x8152;assert(hv_windows_configure(HV_WINDOWS_CPUFREQ)<0);puts("native configuration and proxy ownership cases passed");return 0;}
