#undef printf
static unsigned checks;
static void need(bool value,const char *name){if(!value){fprintf(stderr,"FAIL %s\n",name);exit(1);}checks++;}
static u64 expected_clock(void){return timer_windows_ready?(u64)(((__uint128_t)alias_value*125)/3)+timer_windows_offset:physical_value;}
static bool eligible(u32 insn){return chip_id==T8142&&!cpu_features->apple_sysregs_unlocked&&(insn&0xffffffe0U)==0xd53b9d00U;}
static u64 fnv(const void *p,size_t n){u64 h=1469598103934665603ULL;const u8 *b=p;for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;}return h;}
static void reset_effects(void){clock_reads=generic_entries=gic_calls=diagnostics=0;pending=41;active=1023;ack_count=eoi_count=0;carrier_x18=0xaaaaaaaaaaaaaaaaULL;}
static struct exc_info context(void){struct exc_info c;memset(&c,0x5a,sizeof c);for(unsigned i=0;i<31;i++)c.regs[i]=0xabc00000+i;c.elr=0x200004;c.spsr=5;c.esr=HV_T8142_SITE_HVC_IMM_BASE;return c;}
static void trace(const char *name,u64 value,const struct exc_info *c){printf("TRACE {\"case\":\"%s\",\"value\":%llu,\"context_hash\":%llu,\"clock_reads\":%llu,\"gic_calls\":%llu,\"pending\":%u,\"active\":%u,\"ack\":%u,\"eoi\":%u}\n",name,(unsigned long long)value,(unsigned long long)(c?fnv(c,sizeof *c):0),(unsigned long long)clock_reads,(unsigned long long)gic_calls,pending,active,ack_count,eoi_count);}
static void dispatch_equal(u32 insn,u64 input){
 reset_effects();u64 actual=0xdeadbeefcafef00dULL;bool got=hv_handle_t8142_sysreg_assist(insn,input,&actual);u64 reads=clock_reads,gics=gic_calls,gen=generic_entries,carrier=carrier_x18;u32 p=pending,a=active,ack=ack_count,eoi=eoi_count;
 need(gen==((EXPECT_FAST&&eligible(insn))?0:1),"exact fast-branch admission");
 reset_effects();u64 expected=0xdeadbeefcafef00dULL;bool want=hv_t8142_sysreg_assist_impl(insn,input,&expected,false);
 need(got==want&&actual==expected,"generic result/output equivalence");need(reads==clock_reads&&gics==gic_calls&&p==pending&&a==active&&ack==ack_count&&eoi==eoi_count&&carrier==carrier_x18,"generic side-effect equivalence");
}
int main(void){
 static const u64 ticks[]={0,1,2,3,4,10000000,0xffffffffULL,0xfffffffffffffff0ULL,UINT64_MAX};
 static const u64 offsets[]={0,1,0x1234567890123456ULL,UINT64_MAX};
 for(unsigned ready=0;ready<2;ready++)for(unsigned t=0;t<sizeof(ticks)/sizeof(ticks[0]);t++)for(unsigned o=0;o<4;o++)for(unsigned rt=0;rt<32;rt++){
  timer_windows_ready=ready;alias_value=ticks[t];physical_value=ticks[t];timer_windows_offset=offsets[o];reset_effects();u64 out=0x55;
  need(hv_handle_t8142_sysreg_assist(0xd53b9d00U|rt,0x1234,&out),"all Rt counter accepted");need(out==expected_clock()&&clock_reads==1&&gic_calls==0,"exact scaled clock/offset/wrap and one read");need(generic_entries==(EXPECT_FAST?0:1),"PMCCNTR bypasses only generic scratch");
  need(pending==41&&active==1023&&ack_count==0&&eoi_count==0,"PMCCNTR leaves interrupt state");
 }
 printf("PASS scaled clock matrix all32Rt/zero/wrap/offset/alias-and-physical\n");
 timer_windows_ready=true;alias_value=400;timer_windows_offset=1000;reset_effects();need(hv_handle_t8142_sysreg_assist(0xd53b9d1f,0,NULL)&&clock_reads==1,"null output still one counter read");trace("null-output",0,NULL);
 for(unsigned chip=0;chip<2;chip++)for(unsigned unlocked=0;unlocked<2;unlocked++)for(unsigned rt=0;rt<32;rt++){
  chip_id=chip?T8142:0x8112;cpu_features->apple_sysregs_unlocked=unlocked;dispatch_equal(0xd53b9d00U|rt,0x1234);
  for(unsigned bit=5;bit<32;bit++)dispatch_equal((0xd53b9d00U|rt)^(1U<<bit),0x99887766);
  dispatch_equal(0xd51b9d00U|rt,0x1234);dispatch_equal(0xd53b9c00U|rt,0xabcdef);dispatch_equal(0xd51b9c00U|rt,0xabcdef);dispatch_equal(0xd538cc00U|rt,41);dispatch_equal(0xd518cc20U|rt,41);
 }
 printf("PASS exact opcode chip feature guards and generic write/otherPMU/GIC fallback\n");
 chip_id=T8142;cpu_features->apple_sysregs_unlocked=false;
 for(unsigned rt=0;rt<32;rt++){
  reset_effects();struct exc_info c=context(),expected=c;t8142_pmu_site_count=1;t8142_pmu_site[0].va=0x200000;t8142_pmu_site[0].orig=0xd53b9d00U|rt;
  if(rt<31)expected.regs[rt]=expected_clock();need(hv_handle_t8142_undef_trampoline(&c),"indexed HVC accepted");need(!memcmp(&c,&expected,sizeof c),"indexed PC/allGPR/SP/PSTATE preserved except Rt;XZR discarded");need(clock_reads==1,"indexed one counter");trace("indexed",rt,&c);
  c=context();c.elr=0x900004;expected=c;if(rt<31)expected.regs[rt]=expected_clock();need(hv_handle_t8142_undef_trampoline(&c)&&!memcmp(&c,&expected,sizeof c),"relocated indexed site semantics");
  for(unsigned mode=0;mode<3;mode++){
   reset_effects();model_pstate=mode==0?0:mode==1?4:5;model_insn=0xd53b9d00U|rt;model_esr=0x02000000;translate_ok=true;t8142_undef_vec_count=1;c=context();c.esr=HV_T8142_UNDEF_HVC_IMM;expected=c;expected.elr=fault_pc+4;expected.spsr=model_pstate;if(rt<31)expected.regs[rt]=expected_clock();need(hv_handle_t8142_undef_trampoline(&c),"pending undef accepted");need(!memcmp(&c,&expected,sizeof c),"pending undef returnPC/PSTATE/allRt/SP");need(clock_reads==1,"pending one clock read");trace("pending-undef",rt,&c);
  }
 }
 printf("PASS actual indexed-site relocated-site and pending-UNDEF full context flows\n");
 reset_effects();u64 out=0;need(hv_handle_t8142_sysreg_assist(0xd538cc09,0,&out)&&out==41&&ack_count==1&&active==41&&carrier_x18==0,"GIC IAR x9 preserves zeroed carrier x18 scratch");
 need(hv_handle_t8142_sysreg_assist(0xd53b9d08,0,&out)&&out==expected_clock()&&active==41&&ack_count==1,"interleaved counter does not acknowledge twice");
 need(hv_handle_t8142_sysreg_assist(0xd518cc22,41,&out)&&active==1023&&eoi_count==1,"GIC EOI preserved");trace("irq-counter-eoi",out,NULL);
 reset_effects();need(hv_t8142_insn_is_absent_reg(0xd538cc09)&&pending==41&&active==1023&&ack_count==0,"image probe cannot acknowledge");need(generic_entries==1,"probe retains generic path");
 need(hv_t8142_insn_is_absent_reg(0xd53b9d08)&&clock_reads==1,"counter probe unchanged");trace("generic-probe",0,NULL);
 printf("PASS GIC interleave scratch and side-effect-free image probe\n");
 struct exc_info c=context(),old=c;compat=false;need(!hv_handle_t8142_undef_trampoline(&c)&&!memcmp(&c,&old,sizeof c),"compatibility guard");compat=true;chip_id=0x8112;need(!hv_handle_t8142_undef_trampoline(&c)&&!memcmp(&c,&old,sizeof c),"trampoline chip guard");chip_id=T8142;
 c.esr=HV_T8142_SITE_HVC_IMM_BASE+1;old=c;need(!hv_handle_t8142_undef_trampoline(&c)&&!memcmp(&c,&old,sizeof c),"invalid indexed site");c.esr=1;old=c;need(!hv_handle_t8142_undef_trampoline(&c)&&!memcmp(&c,&old,sizeof c),"unrecognized HVC immediate");
 c=context();old=c;translate_ok=false;u32 insn=0x55;u64 pc=0x66;need(!hv_emulate_t8142_pending_undef(&c,&insn,&pc)&&!memcmp(&c,&old,sizeof c)&&insn==0x55&&pc==0x66,"untranslated pending fault leaves outputs");translate_ok=true;model_esr=0;need(!hv_emulate_t8142_pending_undef(&c,&insn,&pc)&&!memcmp(&c,&old,sizeof c),"non-UNDEF leaves context");
 printf("PASS unknown/disabled/unmapped context refusal\n");printf("CHECKS %u\n",checks);return 0;
}
