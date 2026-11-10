/* Executes production WFI -> timer register -> full FIQ -> tick -> rearm flow. */
static unsigned checks;
static void check(bool ok, const char *name) { if (!ok) { fprintf(stderr,"FAIL %s\n",name); assert(ok); } ++checks; printf("PASS %s\n",name); }
static void reset(u64 start) {
    memset(hv_periodic_deadlines,0,sizeof(hv_periodic_deadlines)); memset(registers,0,sizeof(registers));
    memset(expires,0,sizeof(expires)); memset(pcpu,0,sizeof(pcpu)); memset(wfi_trap_count,0,sizeof(wfi_trap_count));
    cpu=0;chip_id=T8142;boot_cpu_idx=0;hv_pinned_cpu=-1;hv_want_cpu=-1;clock_now=start;
    hv_tick_interval=hv_secondary_tick_interval=1000000000;hv_wfi_wake_interval=1000000;
    full_ticks=serial_flushes=io_polls=vuart_polls=user_interrupts=0;
    guest_updates=ipi_retries=doorbells=wfi_calls=frame_idle=frame_tick=scans=vector_repairs=0;
    maybe_exits=rendezvous_entries=0;scan_delay=0;mu_ready=windows_ready=host_input=guest_tag=rendezvous=false;
    features=(struct features){0};wfi_trap_logged=true;
}
static void wake(struct exc_info *ctx) {
    u64 before=clock_now; unsigned ticks=full_ticks;
    assert(hv_handle_wfx(ctx)); hv_exc_fiq(ctx);
    printf("TRACE {\"cpu\":%u,\"before\":%llu,\"after\":%llu,\"deadline\":%llu,\"full\":%u}\n",
      cpu,(unsigned long long)before,(unsigned long long)clock_now,
      (unsigned long long)hv_periodic_deadlines[cpu].next,full_ticks-ticks);
}
int main(void) {
    struct exc_info ctx={0}; reset(0); hv_arm_tick(false);
    for(unsigned i=0;i<2500;i++) wake(&ctx);
    check(full_ticks==2 && scans==2 && vector_repairs==2 && frame_tick==2,"2500 actual idle wakes run only two genuine housekeeping ticks");
    check(serial_flushes==2500 && io_polls==2500 && !vuart_polls,"wake-only serial and host polling preserve early readiness guard");
    check(ipi_retries==2500 && guest_updates==2500 && maybe_exits==2500,"actual FIQ preserves every pending-timer IPI retry, guest update and exit check");
    check(hv_periodic_deadlines[0].next==3000000000 && clock_now==2500000000,"wake stream preserves periodic phase");
    windows_ready=true;host_input=true;guest_tag=true;wake(&ctx);
    check(vuart_polls==1 && user_interrupts==1 && doorbells==1,"wake-only input readiness, user interrupt and guest doorbell are serviced");
    reset(0);hv_arm_tick(false);
    for(unsigned i=1;i<10;i++){clock_now=i*100000000;hv_arm_tick(true);hv_arm_tick(false);}
    check(hv_periodic_deadlines[0].next==1000000000,"repeated primary true/false rearm does not postpone deadline");
    clock_now=1000000000;hv_arm_tick(true);
    check(hv_periodic_deadlines[0].next==1000000000 && registers[0][SYS_CNTHP_TVAL_EL2]==1,"misclassified primary rearm cannot consume a due tick");
    hv_tick(&ctx);hv_arm_tick(false);check(full_ticks==1 && hv_periodic_deadlines[0].next==2000000000,"due primary tick is consumed exactly once");
    reset(0);hv_arm_tick(false);clock_now=400000000;hv_arm_wfi_wake();clock_now=400500000;
    check(!hv_mask_pending_tick(),"unexpired timer does not become a tick");
    clock_now=400999999;check(!hv_mask_pending_tick(),"early FIQ does not consume periodic deadline");
    clock_now=401000000;assert(hv_mask_pending_tick());check(!hv_mask_pending_tick(),"same physical expiry is masked once");
    reset(0);hv_arm_tick(false);clock_now=3500000000;hv_exc_fiq(&ctx);
    check(full_ticks==1 && hv_periodic_deadlines[0].next==4000000000,"late overlapping periods coalesce without catch-up storm");
    reset(0);hv_arm_tick(false);clock_now=1000000000;scan_delay=2500000000;hv_exc_fiq(&ctx);
    check(full_ticks==1 && clock_now==3500000000 && hv_periodic_deadlines[0].next==4000000000,"long real housekeeping advances to first future phase");
    reset(0);hv_arm_tick(false);cpu=1;hv_arm_tick(true);clock_now=500000000;hv_pinned_cpu=1;
    for(unsigned i=0;i<500;i++)wake(&ctx);
    check(full_ticks==1 && hv_periodic_deadlines[1].next==2000000000,"pinned CPU promotion preserves its existing deadline");
    cpu=0;hv_arm_tick(false);check(hv_periodic_deadlines[0].next==2000000000,"demoted CPU consumes only its own overdue liveness deadline");
    hv_pinned_cpu=-1;hv_arm_tick(true);check(hv_periodic_deadlines[0].next==2000000000,"return to boot CPU does not rebase future deadline");
    reset(0);hv_arm_tick(false);cpu=1;hv_secondary_tick_interval=2000000000;hv_arm_tick(true);
    clock_now=500000000;hv_pinned_cpu=1;hv_arm_tick(false);
    check(hv_periodic_deadlines[1].next==2000000000 && hv_periodic_deadlines[1].interval==1000000000,"role interval change preserves already armed deadline");
    reset(0);for(cpu=0;cpu<MAX_CPUS;cpu++)hv_arm_tick(cpu!=0);
    for(cpu=1;cpu<MAX_CPUS;cpu++){clock_now=0;for(unsigned i=0;i<1000;i++)wake(&ctx);}
    check(full_ticks==0 && ipi_retries==9000 && guest_updates==9000,"nine secondary CPUs retain wake and IPI service without primary housekeeping");
    for(unsigned i=1;i<MAX_CPUS;i++)assert(hv_periodic_deadlines[i].next==2000000000);
    check(hv_periodic_deadlines[0].next==1000000000,"secondary state cannot consume primary deadline");
    reset(UINT64_MAX-500000000);u64 initial=clock_now;hv_arm_tick(false);for(unsigned i=0;i<1000;i++)wake(&ctx);
    check(full_ticks==1 && clock_now==initial+1000000000 && hv_periodic_deadlines[0].next==initial+2000000000,"clock wrap retains wake and deadline ordering");
    reset(0);hv_arm_tick(false);registers[0][HCR_EL2]=HCR_VI;assert(hv_handle_wfx(&ctx));
    check(!wfi_calls && clock_now==0 && expires[0][0]==1000000000,"pending virtual interrupt skips WFI and wake arming");
    registers[0][HCR_EL2]=0;registers[0][ISR_EL1]=1;assert(hv_handle_wfx(&ctx));
    check(!wfi_calls && clock_now==0,"pending physical interrupt skips WFI");
    reset(0);cpu=1;hv_arm_tick(true);rendezvous=true;wake(&ctx);
    check(rendezvous_entries==1 && guest_updates==1 && full_ticks==0,"secondary rendezvous keeps original slow exception path");
    reset(0);chip_id=0x8103;hv_arm_tick(false);check(registers[0][CNTP_TVAL_EL0]==1000000000,"non-T8142 periodic timer unchanged");
    hv_arm_wfi_wake();clock_now=1000000;hv_exc_fiq(&ctx);
    check(full_ticks==1 && registers[0][CNTP_TVAL_EL0]==1000000000,"non-T8142 wake dispatch keeps original full tick behavior");
    printf("CHECKS %u\n",checks);return 0;
}
