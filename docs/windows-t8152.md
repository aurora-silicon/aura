# Windows personality on T8152 (J804, MacBook Pro M6)

Status: built and host-tested only. Nothing here has run on a T8152 machine.

The Windows personality (`gmake WINDOWS=1`) refuses to start a guest on T8152 unless every
safety precondition below holds, and prints which one failed. The policy lives in
`src/hv/windows/hv_t8152.c`; the host side is in `proxyclient/aura/windows.py`,
`proxyclient/aura/hv/__init__.py` and `proxyclient/tools/run_guest.py`.

## What the resident checks

At `hv_init()` (`hv_t8152_preflight()`), before anything is mapped:

- board 0x22 (J804) and boot CPU MPIDR 0x10100;
- CTXR_D is armed and aura runs inside it (chainloaded in place, never with `--relocate`),
  with all of its code (`[_base, _rodata_end)`, rounded up to 16 KiB) ending at or below
  the CTXR exec window base (`ALIGN_UP(_base + 16 MiB, 2 MiB)`) and below the 32 MiB block
  that holds CTXR_D's last 16 KiB page. Measured on J804 (Phase 3 (b)): EL2 on the
  secondaries ran aura's image and the window's first pages, but faulted on the last 16 KiB
  page of the 128 MiB exec window (not the last page of CTXR_D) with ESR_EL2 0x86000002, an
  address size fault at level 2 (Arm ARM ESR_ELx IFSC 0b000010). The cause is unknown and
  one page was probed, so the rule keeps code out of everything not shown to execute;
- all 12 CPUs (cpu0-11) are live in aura's dispatch loop (`smp_is_alive()`, the boot CPU
  counted as the vGIC counts it) and no spin-table entry past cpu11 is. The J804 Mu MADT is
  static (12 GICCs, all enabled) and has no live mask to patch, so a partial machine is
  refused, not launched. Start the secondaries once, in a fresh carrier session, before
  launching. After a failed start (`SMP: T8152 started ... failed` nonzero) another
  `smp_start_secondaries()` does nothing (`smp_start_fail_mask`): reload the carrier. The
  resident never starts them itself;
- AIC3 initialized (the J804 proxy path does not do it; `hv_init()` calls `aic_init()`);
- `HCR_EL2.FWB` clear, `CNTFRQ_EL0` non-zero, boot_args RAM non-empty;
- a denylist read from the live ADT: every top-level `/arm-io` node named `qspi`, `sep`,
  `aop*`, `admac*`, `i2c*`, `*audio*`, `gpio*`, `nub-*`, `dp2hdmi*`, `wdt`, `usb-drd0`,
  `pmgr*`, `*spmi*`, `dart*`, `sart*`, `ans`, `smc`, `mtp*`, `spi*`, `dcp*`, `sio*`; a fixed
  second copy of the J804 QSPI controller pages (0x331118000, 32 KiB); the MTE tag storage
  `region-id-218` and `region-id-4` above RAM. Missing `qspi`, `sep` or `wdt` nodes refuse.
  Every `reg` entry of a denied node is taken (J804 `pmgr-child` has 64); a node with more
  than 1024 entries refuses rather than truncating.

The guest RAM window (`P_HV_WINDOWS_GUEST_RAM`, `hv_t8152_set_guest_ram()`) must be 64 KiB
aligned, at least 256 MiB, inside boot_args RAM, and clear of every carve-out except the
usable-RAM one, every `/chosen/memory-map` entry, CTXR_A-D and CTRR_C/D, the live ADT,
the scanout, the denylist, and `[_base, max(heap high water, heap limit, launcher heap top))`:
aura, the room its heap may still grow into, and the launcher's Python heap above that limit
(the launcher passes `ProxyUtils.heap_top` as the op's third argument). No heap limit, or no
launcher heap top, refuses.

Every stage-2 mapping (`hv_map()`, also the host's `P_HV_MAP`, which now reports a refusal)
goes through `hv_t8152_map_filter()`: hardware and software-map leaves may reach only the
guest RAM window (identity, normal memory), carrier-registered exceptions (the BGRA shadow)
or the AIC (identity, forced to Device-nGnRnE). Host proxy hooks are refused everywhere: the
Python handler would access PA = IPA, and stage 2 covers exactly the PA space. Carrier (C)
hooks (vUART, vGIC, AIC transitions) emulate and are allowed. The fixed Mu ADT alias at
0x5300000000 is not mapped on T8152: it would be aura's unfiltered live ADT, and the J804 Mu
reads its own copy of the filtered guest ADT. `hv_write_hcr()` keeps `HCR_EL2.TWI` and `TWE` set
and `FWB` clear.

Before entry, `hv_t8152_verify()` walks the whole stage-2 table again and checks the entry
point and boot_args are in the window, TWI and TWE are set for the boot CPU and the secondaries,
FWB is clear, stage 2 and the SMC trap are on, `framebuffer=bgra-shadow` is selected and all
12 CPUs are still live.

## Other T8152 behaviour

- PSCI CPU_ON is a spin-table release (`hv_start_secondary()`); CPU_OFF parks the core back
  in aura's spin table and is never a power-off. The boot CPU's CPU_OFF is DENIED.
- Guest WFI is always trapped (`HCR_EL2.TWI`); the EL2 wait wakes at 1 kHz. Phase 3
  measurement (a) found WFI_WIPES on T8152 (a timer-woken WFI clears GPRs, NZCV and SIMD on
  E, P and M cores), so carrier idle (trapped WFI, PSCI CPU_SUSPEND) runs no WFI: it spins on
  `ISR_EL1`, bounded at 1 ms. The real-WFI opt-in (config bit 7, `wfi: wfi`,
  `--windows-wfi wfi`) is refused by the resident and the launcher.
- Guest WFE is trapped too (`HCR_EL2.TWE`) and completes at once without sleeping
  (`hv_handle_wfx()`); WFE may complete at any time, so the guest just re-checks. Part A's
  WFE rows used SEVL first and never slept, so nothing shows yet that a WFE that sleeps keeps
  GPRs, NZCV and SIMD. So `spin_lock()` (taken on the hypervisor lock while a guest's
  SIMD/FP registers are live in hardware) waits without WFE on T8152 under `WINDOWS=1`: it
  re-reads the lock instead. Aura's dispatch loop still uses WFE at EL2 (no guest state
  live there); a measurement of a sleeping WFE (no SEVL) is still wanted before any WFE with
  guest state live. Capability bit 13 (0x2000) marks a resident with the WFE trap and the
  code-placement rule; the launcher requires it on T8152.
- The guest's ACTLR_EL1 is saved, restored and emulated only through the architectural
  `ACTLR_EL12` (`hv_t8152_guest_actlr_*`; the M6 CPU parts set `actlr_el2`). Phase 3
  measurement (h) read the EL1 view there on E, P and M cores, and a write to the
  implementation-defined alias UNDEFs at EL2 on all of them, so the carrier never accesses
  that alias on T8152. A restore of the value the register already holds issues no write,
  because an `ACTLR_EL12` write is not yet measured. Capability bit 14 (0x4000) marks such a
  resident; the launcher requires it on T8152.
- Guest serial (the vUART hook on uart0's first page) goes only to the USB CDC vUART
  interface and, when enabled, the framed capture. On T8152 the emulation never writes the
  physical uart0 or the AIC: the launcher passes no interrupt to `P_HV_MAP_VUART` and the
  resident drops one if it is passed anyway, so no UTRSTAT clear on uart0 and no AIC software
  line on any trapped access or poll tick, and the HVLOG console passthrough is off. Nothing
  shows uart0 is clocked while a guest runs on the J804 route. The guest polls the emulated
  UTRSTAT/UFSTAT. A resident from before this rule still gets no interrupt from the launcher.
- IPIs: polling by default (1 kHz secondary tick drains queued guest IPIs and vSGIs).
  Apple Fast IPIs only with the launcher's `ipi: fast`, after Phase 3 measurement (c).
- GIC CPU interface: always the software one, with every `ICC_*` access trapped
  (`hv_gic_cpuif.c`, glue `hv_handle_t8152_gic_cpuif()` in `hv_exc.c`). Phase 3
  measurement (f) found `ICC_IAR1_EL1` and `ICC_IGRPEN1_EL1` UNDEFINED at EL1 in the
  hardware-interface configuration (`ICH_HCR_EL2` 0, `HCR_EL2.IMO` clear), and the measured
  accesses (`ICC_PMR_EL1`, `ICC_CTLR_EL1`, `ICC_IAR1_EL1`, `ICC_IGRPEN1_EL1`) trapping to EL2
  (EC 0x18), with `ICC_SRE_EL1` not trapping, under `ICH_HCR_EL2` = En | TC | TALL0 | TALL1
  (0x1c01), `ICH_VMCR_EL2.VENG1` and IMO/FMO set, on E, P and M cores and the boot CPU. That
  the other `ICC_*` registers trap under the same bits follows from IHI 0069; it is not
  measured.
  - Mu keeps the measured Mu configuration (`ICH_HCR_EL2` 0, IMO clear): the J804 FD is an
    AIC build (AppleAicDxe, no ArmGicDxe) and executes no `ICC_*` instruction.
  - The guest OS owns the GIC from Mu's ExitBootServices (AIC CONFIG cleared) or, failing
    that, from its first write to the emulated distributor or a redistributor. From then on
    every CPU (and every CPU the guest starts later) runs the trap-all configuration above.
    A trapped `ICC_*` access never claims the GIC: before the claim only the SGI generation
    registers can trap (`HCR_EL2.FMO` is set in the Mu phase, IHI 0069), and such a write is
    ignored so Mu keeps its AIC interrupts. A CPU that loses the claim race waits until the
    winner has published ownership, so no CPU falls back to a Mu-phase or T8142 setup.
  - EL2 emulates PMR, CTLR (EOImode, CBPR), BPR0/1, IGRPEN0/1, IAR1, EOIR1, DIR, HPPIR1, RPR,
    AP0R0/AP1R0 (5 priority and preemption bits, as `ICH_VTR_EL2` reports), SGI1R; Group 0
    reads spurious, `ICC_NMIAR1_EL1` reads spurious (no NMIs) and SGI0R/ASGI1R generate
    nothing (Group 0 would be FIQ). Only CRn 12, CRm 8-12 encodings and PMR are taken. Running
    priority and nested preemption follow IHI 0069.
  - Delivery is `HCR_EL2.VI` from the model; no list register is written (they stay zero),
    and no `ICH_MISR_EL2`/`ICH_EISR_EL2` read happens on T8152. Device SPIs: the AIC EVENT
    read marks the INTID pending on the target CPU's interface; the guest's EOI unmasks the
    line. Timers: the Apple timer FIQ is masked and GTDT PPI 17/18 pended, as on T8142.
    SGIs: `ICC_SGI1R_EL1` pends on each target and kicks it.
  - `ID_AA64PFR0_EL1.GIC` reads 1 to the guest (TID3; the hardware reads 0), from the first
    read, Mu phase included. Assumed, not verified: no client before ExitBootServices (Mu,
    the Windows boot applications) executes a Group 1 `ICC_*` instruction, which would UNDEF
    at EL1 there. On T8152 the rest of the TID3 space (AArch32 ID block, unallocated
    encodings, `ID_AA64FPFR0_EL1`) reads 0 at EL2 instead of going to the proxy, but only if
    `ID_AA64PFR0_EL1.EL0` and `.EL1` both read 0b0001 (AArch64 only), where the AArch32 ID
    registers are UNKNOWN; otherwise those reads keep the proxy path.
  - Not modelled: the redistributor/distributor enable bits. A pending PPI or SGI is
    delivered even if the guest disabled it (as on T8142).
  - The native AIC handoff (an AIC HAL extension's ready call) is ignored once the guest
    owns the GIC: J804 publishes no AIC CSRT.
  - Capability bit 15 (0x8000) marks such a resident; the launcher requires it on T8152 and
    refuses `gic_cpuif: carrier` there, and the resident's configure step refuses a T8152
    configuration without the software-interface bit.
- Mu-phase timer: EL0/EL1 CNTP accesses trap (`CNTHCTL_EL2` = EL1PCTEN only, so
  EL1PTEN is clear; under `HCR_EL2.E2H` that bit controls CNTP_CTL/CVAL/TVAL, Arm ARM
  CNTHCTL_EL2), as on T8142 but with the direct counter kept. The trapped accesses use the
  EL02 bank; TimerDxe's final `CNTP_CTL` write (ENABLE, IMASK clear) latches
  `Mu timer handler ready`, and from then on an expired CNTP is masked and posted as AIC
  software line `nr_irq - 48 + cpu` (CNTV: `nr_irq - 24 + cpu`), the 24-per-bank layout of
  Mu's `AIC_TIMER_REFLECT_CPU_SLOTS`. Before any route owns the guest timers,
  `hv_update_fiq()` masks an asserted one (the guest-visible IMASK on T8152, which has no
  Apple VM timer FIQ gate) instead of leaving it to re-enter EL2 forever. The first M2 run
  (2026-11-16) had EL1PTEN set (`CNTHCTL_EL2=0xc00`): readiness never latched and the guest
  stopped about 10 ms after TimerDxe armed CNTP. Not yet run with the trap.
- Once a second, next to `HV: fb: … lines/s`, the host tick prints one `HV: g<cpu> …` line
  for each CPU that took a guest exit since the previous one: route
  (`wait-aic`/`wait-timer`/`mu`/`gic`/`win-carrier`/`win`), the guest PC at its last exit
  (`(same)` if unchanged), mode and DAIF from `SPSR_EL2`, `ELR_EL1`/`ESR_EL1` (EL12 views),
  exit counts since the last line (`ex` all, `sr` sysreg, `da` data aborts, `fb` aborts on
  the BGRA shadow, `vu` vUART reads/writes, `tx` bytes, `full` TX-full answers, `wfx`,
  `hvc`, `smc`, `o` other, `fiq`, `gt` FIQs with a guest timer asserted and unmasked, `irq`,
  `refl` timer reflections, `hold` unrouted-timer masks, `vi` exits with a virtual
  interrupt asserted) and the guest `CNTP_CTL`/`CNTV_CTL` with how late (or early) CNTP's
  compare value is. Counters are per CPU, written only by their owner, read without a lock.
  vUART TX-full and recovery edges are recorded in the trap path and printed from the same
  tick (`HV: vuart: TX full …`, `HV: vuart: TX has space again …`).
- ECV is not used until measurement (e); the architectural PMU stays trapped to the
  synthetic EL2 backend.
- `usb_hpm_restore_irqs()` is skipped: nothing writes to the USB-C port controllers.
- The vGIC carrier answers at 0xF00000000 / 0xF10000000 (Mu J873 PCDs) for live CPUs. As on
  T8142, the guest's first SPI enable turns on the AIC master enable (`hv_vgic.c`).
- No AIC aliases yet (`hv_aic_alias.c`): none is needed before NVMe (M4).
- Resets: PSCI SYSTEM_RESET returns 2 to the host, whose launcher runs `nvme_shutdown` and
  only then `reboot` (opt-in). Every resident reset (HV panics, unhandled guest exceptions,
  watchdog barks via `hv_safe_reset()`, and the generic `panic()`, `assert` and unhandled
  EL2 exception paths) ends in `flush_and_reboot()`, which under `WINDOWS=1` shuts NVMe down
  first and otherwise parks, also when re-entered.
- The N1 handoff (`windows_n1.c`) is J813-only; T8152 profiles take no N1 and the
  launcher refuses one.

## Host side

- Profiles for chip 0x8152 must bind `guest_ram {base, top}` and use `bgra-shadow`; they
  take no `reserved_memory`, N1 or MTP. Optional: `ipi` (`poll`/`fast`), `gic_cpuif`
  (`software` only, the default), `wfi` (`poll` only), `on_reset` (`stay`/`reboot`).
- A resident must report capability bits 0x0800 (T8152 policy), 0x1000 (12-live-CPU
  refusal), 0x2000 (guest-WFE trap and code placement), 0x4000 (architectural
  `ACTLR_EL12` only) and 0x8000 (trapped software GIC CPU interface) for any of this.
- Before `hv_init`, the launcher reads `P_SMP_IS_ALIVE` for cpu0-11 (read-only) and stops
  with the list of CPUs that are not live; the resident checks the same again.
- The guest ADT drops `qspi`, `i2c*`, `aop*`, `admac*`, `*audio*` and every `nvram,nor`
  node, and gets `aura,windows-nvram-hidden`.
- `run_guest.py -r -E 0 --windows-guest-ram BASE:TOP MU.fd` drives the same path on a
  Windows resident on T8152; it never stops secondaries or sleeps the machine afterwards.
