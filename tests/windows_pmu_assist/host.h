/* Hardware/OS boundary only. Production clock and assist bodies are extracted verbatim. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define TYPES_H
#define sys_reg(op0,op1,crn,crm,op2) , _S, op0, op1, crn, crm, op2
#define BIT(x) (1ULL << (x))
#define GENMASK(h,l) (((~0ULL) << (l)) & ((~0ULL) >> (63-(h))))
typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
#include "hv/windows/windows_regs.h"
#define T8142 0x8142
#define T8152 0x8152
#define HV_T8142_UNDEF_HVC_IMM 0x4d31
#define HV_T8142_SITE_HVC_IMM_BASE 0x4e00
/* ACTUAL_EXC_INFO */
static struct {bool apple_sysregs_unlocked;} feature_value,*cpu_features=&feature_value;
static u32 chip_id=T8142;
static bool compat=true,timer_windows_ready=true,translate_ok=true;
static u64 timer_windows_offset,alias_value=123,physical_value=456,clock_reads,generic_entries,gic_calls,diagnostics;
static u64 fault_pc=0x100000,model_esr=0x02000000,model_pstate=4;static u32 model_insn;
static u32 pending=41,active=1023,ack_count,eoi_count;static u64 carrier_x18;
static u64 el0_sync_pass,el0_sync_emul,el1_sync_pass,el1_sync_emul;
static struct {u64 va,pa;u32 orig;} t8142_pmu_site[1024];static u32 t8142_pmu_site_count;
static struct {u64 va,pa;u32 orig;bool refused;} t8142_undef_vec[16];static u32 t8142_undef_vec_count;
static bool hv_windows_guest_compat_enabled(void){return compat;}
static bool hv_absent_pmuv3(void){return chip_id==T8142||chip_id==T8152;}
static const char *hv_absent_pmu_chip(void){return chip_id==T8152?"T8152":"T8142";}
static struct hv_guest_diag { u64 pmc_reads, pmc_last; } hv_guest_diag[16];
static unsigned smp_id(void){return 0;}
static u64 model_mrs(const char *reg){
 if(!strcmp(reg,"SYS_IMP_APL_CNTVCT_ALIAS_EL0")){clock_reads++;return alias_value;}
 if(!strcmp(reg,"CNTPCT_EL0")){clock_reads++;return physical_value;}
 if(!strcmp(reg,"ESR_EL12"))return model_esr;
 if(!strcmp(reg,"ELR_EL12"))return fault_pc;
 if(!strcmp(reg,"SPSR_EL12"))return model_pstate;
 fprintf(stderr,"FAIL unexpected modeled register %s\n",reg);exit(1);
}
#define mrs(reg) model_mrs(#reg)
static u64 hv_translate(u64 va,bool write,bool stage1,void *unused){(void)write;(void)stage1;(void)unused;return translate_ok&&va==fault_pc?(u64)(uintptr_t)&model_insn:0;}
static u64 hv_absent_pmu_translate(u64 va,bool write,u64 size){(void)size;return hv_translate(va,write,true,NULL);}
static u32 read32(u64 pa){return *(u32 *)(uintptr_t)pa;}
static bool hv_replay_vector_insn(struct exc_info *ctx,u64 va,u32 orig,bool consume){(void)ctx;(void)va;(void)orig;(void)consume;return false;}
static bool hv_unpatch_t8142_undef_vector(u64 va){(void)va;return false;}
static int production_printf(const char *fmt,...){(void)fmt;diagnostics++;return 0;}
#define printf production_printf
/* EL2 accounting classification is exercised by tests/windows_el2_acct. */
#define HV_EL2_VEC 3
static unsigned el2_classified;
static void hv_el2_cause(u32 cause){(void)cause;el2_classified++;}
static void hv_el2_site_cause(u32 insn){(void)insn;el2_classified++;}
/* Unchanged production GIC body is source-bound separately. This boundary
 * models observable acknowledgement/completion and the real carrier x18 read. */
static bool hv_handle_t8142_gic_cpuif(u64 reg,bool is_read,u64 rt,u64 regs[32],bool probe){
 gic_calls++;if(chip_id!=T8142)return false;
 if(reg==SYSREG_ISS(ICC_IAR1_EL1)){
  if(is_read){regs[rt]=pending;if(!probe&&pending!=1023){active=pending;pending=1023;ack_count++;carrier_x18=regs[18];}}
  return true;
 }
 if(reg==SYSREG_ISS(ICC_EOIR1_EL1)){
  if(!is_read&&!probe&&regs[rt]==active){active=1023;eoi_count++;}return true;
 }
 return false;
}
