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
  (2026-11-16) had EL1PTEN set (`CNTHCTL_EL2=0xc00`): readiness never latched and the guest's
  serial stopped mid line after TimerDxe armed CNTP (10 ms period). An EL2 FIQ storm from that
  timer is the inferred cause, not observed; run 1 has no per-chunk serial timestamps, so the
  stop time is not known. Not yet run with the trap.
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
- The guest's EL1 virtual timer is handed over disabled (`CNTV_CTL_EL0` = 0) on the boot CPU
  and on every CPU the guest starts (console: `HV: T8152: guest CNTV_CTL 0x7 -> 0 at entry`).
  aura's own startup leaves it ENABLE|IMASK and long expired. M3 run 1 measured, at the Mu
  shell, ~126k/s each of `smc`, `da`, `sr` and `refl` (~380k exits/s) with `cntv=7`. The
  inferred cause: with no Apple VM timer FIQ gate the carrier gates timers with the guest
  IMASK, so each NTAIC completion unmasked a CNTV that Mu never programs and the next exit
  reflected it again (EVENT abort + trapped `CNTP_CTL` read + SMC). That the quiesce removes
  the loop is confirmed only when run 2's g6 samples show it. `CNTV_CTL.ENABLE` is UNKNOWN at
  reset, so no guest may rely on the old value.
- ECV is not used until measurement (e); the architectural PMU stays trapped to the
  synthetic EL2 backend.
- There is no architectural PMUv3 bank, as on T8142: an EL1/EL0 PMU access UNDEFs at EL1
  (`ESR_EL1` 0x02000000) before `MDCR_EL2.TPM` can trap it (M3 run 1: bootmgr's
  `mrs x9, PMCCNTR_EL0`, 0xd53b9d09, faulted with `ESR_EL1` 0x02000000 and `MDCR_EL2` 0x60). `hv_absent_pmuv3()` (T8142, T8152) opens the T8142 recovery:
  - always: Mu's forwarded `hvc #0` with x0 = "NTSRG" is answered from the software PMU
    (`PMCCNTR_EL0` = the 1 GHz counter, the control registers read 0 and drop writes);
    console once: `HV: T8152: handled forwarded EL1 sysreg instruction 0x… at EL2`;
  - with `guest_compatibility: t8152-pmu-undef` (`run_guest.py
    --windows-guest-compatibility t8152-pmu-undef`, resident bit 0): the sync-vector
    redirect (`HVC #0x4d31`), the access-site replacement in kernel images, the module walk
    and the `b .` wedge recovery, as on T8142. On T8152 these read and write guest memory only
    inside the guest RAM window. The NT build-specific diagnostics (process names, the
    `KiBugCheckData` bugcheck and dead-process reports, both on the tick and in the stuck-PC
    path of the wedge recovery) and the Mu SP_EL0 repair stay T8142-only: their RVA and
    `EPROCESS` offsets come from one T8142 ntoskrnl build and are not checked.
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
  (`software` only, the default), `wfi` (`poll` only), `on_reset` (`stay`/`reboot`),
  `guest_compatibility` (`none`, the default, or `t8152-pmu-undef`).
- A resident must report capability bits 0x0800 (T8152 policy), 0x1000 (12-live-CPU
  refusal), 0x2000 (guest-WFE trap and code placement), 0x4000 (architectural
  `ACTLR_EL12` only) and 0x8000 (trapped software GIC CPU interface) for any of this.
- Before `hv_init`, the launcher reads `P_SMP_IS_ALIVE` for cpu0-11 (read-only) and stops
  with the list of CPUs that are not live; the resident checks the same again.
- The guest ADT drops `qspi`, `i2c*`, `aop*`, `admac*`, `*audio*` and every `nvram,nor`
  node, and gets `aura,windows-nvram-hidden`.
- `run_guest.py -r -E 0 --windows-guest-ram BASE:TOP MU.fd` drives the same path on a
  Windows resident on T8152; it never stops secondaries or sleeps the machine afterwards.

## D1: the carrier-emulated AHCI disk

Status: built and host-tested only (`tests/python/test_windows_t8152_ahci.py`). Nothing of it
has run on the M6. Contract and reasoning: `windows-m6/D1-AHCI-DESIGN.md` (owner notes, not in
this tree); the Mu side is `J804_WINDOWS_AHCI` in Mu `j804-windows`.

What the guest sees:

- one AHCI 1.3.1 HBA at IPA **0xF40000000** (16 KiB, hooked, never mapped), CAP 0x81341F00:
  64-bit addressing, CLO, Gen3, 32 slots, one port, **no NCQ**, no ALPM, no hot plug;
- one ATA disk: 512-byte sectors, LBA48, non-rotating, volatile write cache, no SMART, no
  security, no TRIM; IDENTIFY strings `AURA-AHCI-0000000001` / `D1.0` /
  `Aura carrier RAM disk`;
- one interrupt, **SPI/GSIV 990**, level, active-high, with no AIC line behind it. It is
  routed by the guest's GICD_IROUTER<990> (IRM 1: the CPU whose register access completed
  the command; otherwise the CPU with that affinity, else the boot CPU), gated by
  GICD_ISENABLER only (as the AIC-backed SPIs), cleared when the line drops and re-pended
  at deactivation while still high. It is pending on at most one CPU interface, and on none
  while it is active anywhere (one GICv3 state machine, `vdev_spi_plan`). Before the guest
  OS owns the GIC (Mu) the level is only remembered; Mu's driver polls. Without the AHCI
  configure flag none of this runs: a T8152 launch without D1 takes the pre-D1 GIC path.

Device model (`src/hv/windows/ahci_model.c`, pure, clean room from AHCI 1.3.1 and ACS-3):

- commands run synchronously inside the trapped PxCI write, lowest slot first;
- supported: IDENTIFY DEVICE, EXECUTE DEVICE DIAGNOSTIC, READ/WRITE DMA (EXT), READ/WRITE
  SECTORS (EXT), READ/WRITE MULTIPLE (EXT) with SET MULTIPLE MODE (1-16 sectors per DRQ
  block, default 16), READ VERIFY (EXT), FLUSH CACHE (EXT), SET FEATURES 02/82/03/55/AA/66/CC
  (03 records the selected DMA mode, shown in IDENTIFY words 63/88), READ LOG (DMA) EXT of
  the log directory, STANDBY/IDLE (IMMEDIATE), CHECK POWER MODE, RECALIBRATE; everything
  else (NOP, SMART, NCQ, DSM, security, ATAPI, the BUFFER and FUA commands) is aborted
  (ERR 0x04), logged once per opcode. COMRESET and GHC.HR restore the device defaults
  (UDMA 6, write cache on, 16 sectors per DRQ block);
- every guest span (command list, FIS area, command table, PRDT, data) is checked against
  the guest RAM window, wrap-safe, before it is touched. A span outside it is a host bus
  fatal error (PxIS.HBFS); a data-in PRDT shorter than the transfer is PxIS.OFS (fatal);
  both are checked for the whole PRDT before any data moves. LBA past the disk is IDNF;
  a misaligned PRD or a malformed FIS is ABRT;
- COMRESET (PxSCTL.DET), the AHCI software reset (two device-control FISes) and GHC.HR
  are modelled; none of them touches the disk contents. GHC.HR keeps PxCLB/PxCLBU/PxFB/PxFBU
  (AHCI 1.3.1 §10.4.3) and resets the other port fields.

The store (`src/hv/windows/hv_ahci.c`, `hv_t8152_reserve_store()`):

```
--windows-guest-ram BASE:TOP    (unchanged; region-id-218 starts at TOP)
desc        = [TOP - 64 KiB, TOP)                carrier-owned descriptor "AWAHCI01"
store_base  = align_down(desc - disk, 32 MiB)
disk        = [store_base, store_base + disk)
guest RAM   = [BASE, store_base)                 what Mu, boot_args and Windows see
```

Worked example with a synthetic window `0x10100000000:0x10561600000` (17.52 GiB; the real
one comes from `--windows-guest-ram` and is not recorded in this tree): a 12 GiB disk gives
store_base 0x10260000000 and exactly 5.5 GiB of guest RAM; the largest disk that leaves the
4 GiB minimum is 0x3615f0000 bytes (13.52 GiB). The resident refuses a store that does not start
at the guest RAM top, is not 32 MiB aligned, or meets a carve-out, a CTXR range, aura or its
heaps, the ADT or the scanout, and adds the whole `[store_base, TOP)` to the stage-2
denylist, so no later map and no leaf at entry (`hv_t8152_verify`) can reach it.
`hv_start()` additionally refuses unless the store is READY and every leaf of the ABAR page
is this hook (`hv_ahci_verify`).

Resident ABI: `P_HV_WINDOWS_CAPS` is unchanged (0x4157494e0001ffdf, ABI 1).
`P_HV_WINDOWS_CAPS2` (0xc8d) returns `'AWI2' << 32 | 1` (bit 0: AHCI RAM store v1).
`P_HV_WINDOWS_AHCI` (0xc8e): op 0 QUERY (0 version, 1 ABAR, 2 ABAR size, 3 INTID, 4 CAP,
5 minimum guest RAM, 6 store alignment, 7 descriptor size), 1 ATTACH(store_base, TOP,
disk_bytes, flags: 1 keep, 2 accept unsealed (with keep), 4 overwrite (fresh only)),
2 SEAL_UPLOAD(host CRC-32), 3 SEAL_EXIT, 4 STATUS. Configure flag BIT(8) (`CONFIG_T8152_AHCI`), only with the software GIC.
Descriptor states: EMPTY, LOADING (fresh, zeroed), READY (upload or kept store verified),
RUNNING (written just before guest entry), SEALED (after a PSCI exit, with the disk CRC-32
and generation + 1). `alive_cntpct` (+0x38) is the CNTPCT_EL0 at the last state change and,
while the guest runs, every 10 s; the counter restarts at a SoC reset, so a value above the
current counter shows the machine was reset since. A kept RUNNING store accepted unsealed is
recorded as READY with its CRC-32 at that moment, so a failed entry does not lock it out. A
fresh ATTACH refuses to zero a READY, RUNNING or SEALED store at the same TOP unless flag 4
is given. The launcher's host note at +0x200 (image SHA-256, size, upload time) is printed
on every keep and on a refused overwrite.

Launcher (`run_guest.py`, T8152 Windows path):

| Option | Meaning |
|---|---|
| `--windows-ahci-image PATH` | raw GPT image (sparse is fine): protective MBR and `EFI PART` at LBA 1 required, a missing backup header is a warning |
| `--windows-ahci-store-size SIZE` | disk size (`12G`, `13000M`, hex); default the image size rounded up to 256 MiB |
| `--windows-ahci-keep` | reuse the store a previous run sealed; the resident re-checks its CRC-32 |
| `--windows-ahci-keep-unsealed` | also accept a RUNNING store (no integrity check possible); unsafe after a hard reset or power cycle, refused when the system counter shows a reset since the store was last alive |
| `--windows-ahci-keep-expect SHA256` | with keep: refuse unless the store's host note names this image SHA-256 |
| `--windows-ahci-overwrite` | with `--windows-ahci-image`: allow zeroing a READY/RUNNING/SEALED store at the window top (refused otherwise) |
| `--windows-ahci-boot {auto,disk,ramdisk}` | Mu's first boot option; `auto` = the appended RAM disk if `-a` gives one, else the AHCI disk |

Order: CAPS2/QUERY and the FD marker (`J804AHC1` at 0x110) are checked before `hv.init()`,
together with the window split; `hv.init()` sets the configure flag and the guest ADT gets
`/chosen/aura,windows-ahci-v1` (40 bytes: ABAR, size, GSIV, boot, store base, TOP);
`load_raw` sets guest RAM to `[BASE, store_base)`; the payload must end below the 128 MiB
UEFI region at the guest RAM top and miss the PrePi hand-off copies; ATTACH; for a fresh
store the image is streamed in 16 MiB gzip chunks, holes and all-zero chunks skipped (the
store was zeroed), the host's zlib CRC-32 of the whole disk is sent with SEAL_UPLOAD and the
resident compares it with its own (Armv8 CRC32X); `hv.start()`; after a PSCI exit SEAL_EXIT.
`--windows-reset-on-exit` is refused with any AHCI option (a WD1 reset loses DRAM), so is an
AHCI FD without one, and `--windows-ahci-keep` with `--windows-ahci-image`.

Keeping the disk across a Windows restart: the guest's SYSTEM_RESET returns 2, the launcher
seals the store and prints its generation and CRC. Reload the carrier in place (no hard
reset, no power cycle) and launch again with `--windows-ahci-keep` and the same
`--windows-guest-ram`. Whether the in-place reload leaves the store untouched is not yet
measured; the CRC check refuses the disk if anything changed.

Host tests: the model and the routing are compiled unchanged with ASan/UBSan and replayed by
`tests/e2e/windows_ahci_host.c` (20 cases including a seeded 10^6-step fuzz with canaries
around guest RAM and the store); the carrier glue `hv_ahci.c` (attach, keep, seal, the
unsealed and reset rules, the overwrite guard) is compiled unchanged against stub headers
and driven by `tests/e2e/windows_ahci_store_host.c`; IDENTIFY is compared byte for byte with an independent
encoder, the CRC with zlib; the store reservation is in `test_windows_t8152_policy.py`; the
launcher planner, descriptor, FD marker, sparse upload and the HV lifecycle are in
`test_windows_t8152_ahci.py`. `AURA_AHCI_TRANSCRIPT=<file>` saves the replay transcript;
`AURA_MU_TREE` and `AURA_J804_AHCI_FD` enable the cross-checks against Mu's header and a
built AHCI FD.

Not known yet (D2 answers it on the M6): whether storahci accepts this HBA (ACPI-only, no
NCQ, an immediate completion interrupt), how long a 32 MiB command holds the hypervisor lock
under load (reported as "longest access" every 10 s while the disk is busy), and whether
Windows ever uses non-cacheable DMA buffers (the carrier does no cache maintenance).
