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
  `aop*`, `admac*`, `i2c*`, `*audio*`, `gpio*`, `nub-*`, `dp2hdmi*`, `wdt`, `usb-drd*`,
  `pmgr*`, `*spmi*`, `dart*`, `sart*`, `ans`, `smc`, `mtp*`, `spi*`, `dcp*`, `sio*`,
  `atc-phy*` (usb-drd1..3 reg 0 goes in as two entries, the xHCI window and the rest, so that
  D4 can lift only the first; both are denied unless a port is attached); a fixed
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

## D5: the AHCI disk on an internal NVMe partition

Status: host-tested only; nothing of it has run on the M6 yet. Reasoning and the failure
list: `windows-m6/D5-NVME-DESIGN.md` (owner notes, not in this tree). Windows keeps the D1
AHCI disk (inbox storahci, no NVMe driver, the ANS never mapped to the guest); only its backend
changes: a dedicated GPT partition of `nvme0n1`, read and written by aura's EL2 NVMe driver.
Persistent, no upload, and guest RAM is the whole `--windows-guest-ram` window again.

- **Guest view**: the D1 HBA, IDENTIFY 512e (512-byte logical, 4096-byte physical, word 106 =
  0x6003, word 209 = 0x4000), strings `AURA-AHCI-NVME000001` / `D5.0` / `Aura carrier NVMe
  disk`, sectors = partition bytes / 512. AHCI sector L is partition byte L x 512, i.e. NVMe
  block `first + L / 8`. Mu gets the 24-byte `/chosen/aura,windows-ahci-v1` (no store).
- **Choosing the partition**: never by number (macOS 27.2 renumbered this disk). The launcher
  sends disk GUID, unique partition GUID, type GUID, name and first/last LBA from the private
  config; the resident (`gpt_check.c`) reads the live primary and backup GPT through Identify's
  namespace size, checks both headers and arrays (CRCs, positions, agreement), and accepts
  exactly one entry matching all five values, inside the usable range, 1 MiB aligned, of no
  well-known type (EFI, APFS, Apple boot/iSC/Recovery, HFS+, Linux types, Microsoft data/MSR/
  recovery), overlapping no other entry (all pairs) and with no duplicate GUID or name. Then it
  checks the inner disk: a protective MBR and a 512-byte-sector GPT that fits the partition.
- **Guards**: the model's range check, the backend's (`ahci_nvme.c`), and an I/O fence in
  `nvme.c` set at attach that refuses every read or write outside the partition until the
  shutdown. Linux and macOS partitions are unreachable, for reads too.
- **Data path**: the model calls the backend per PRD; it stages data in a 528 KiB bounce pool
  (carrier heap, denied to stage 2) and issues whole 4 KiB-block commands of up to 64 blocks.
  A write that covers a block in part reads it first (read-modify-write); RMW is atomic
  against other AHCI commands because the model runs one command at a time under `ahci_lock`.
  A command completes only after every NVMe write it caused completed: the carrier never holds
  acknowledged data. FLUSH CACHE is an NVMe flush; with the write cache disabled (SET FEATURES
  0x82) every write command is followed by one; `--windows-ahci-nvme-flush every-write` forces
  that. No FUA.
- **Which CPU**: the trapping CPU, synchronously, polled (the ANS interrupt stays unaliased),
  under `bhl` and `ahci_lock`, then the owner-tagged NVMe lock (`nvme_lock_acquire`). The poll
  loop pets the HV watchdog. Writes wait up to 5 s and flushes 10 s for their completion
  (reads 1 s): the only earlier aura write on this SoC lost its completion within 1 s.
  Multi-block writes (PRP2 or a PRP list) are used only after this session's multi-block reads
  passed their self-check; otherwise every write is one 4 KiB block per command.
- **Durability**: on the M6 only a controller shutdown (CC.SHN normal, SHST done) is trusted
  (a flush is not, across a hard reset). An ATA FLUSH CACHE completes after an NVMe Flush, so
  NTFS's write barrier holds only as long as the session ends with that shutdown: after a hard
  reset or power loss writes Windows believes committed may be lost, possibly out of order, and
  NTFS can be damaged structurally, not just miss recent data. Every exit the carrier controls
  closes the disk with that shutdown before anything can reset:
  - every `hv_start()` return (PSCI SYSTEM_OFF/RESET, ^C then `exit`, refusals before entry):
    `hv_ahci_session_end()`;
  - `flush_and_reboot()` (panic, assert, EL2 exceptions, SError, the HV watchdog, `P_REBOOT`):
    `hv_ahci_nvme_reset_close()`. It stops the lock owner issuing further I/O commands, waits
    up to 12 s for the NVMe lock (longer than any one command's timeout), then the normal
    shutdown; only if a CPU is stuck holding the lock it writes CC.SHN directly and polls CSTS
    alone, leaving the RTKit mailbox to that CPU. It resets only after SHST done (or if
    nothing was written, including a carrier whose first NVMe wake failed); otherwise it
    parks, and the proxy is gone (a park is followed by a power hold, a hard reset).
  - `P_REBOOT` in the Windows personality goes through `flush_and_reboot()`, and is refused
    while a guest session owns the disk or after a FAILED close. `P_REBOOT` with args[0] =
    `"FORCE"` after a FAILED close makes one more shutdown attempt and then resets even without
    its confirmation (the partition then needs a restore or chkdsk). `P_NVME_*` refuse while
    the disk owns the controller, and after a close that had to shut down without the lock,
    until that CPU lets go.
  - The carrier resets J804 by the watchdog sequence that has worked through the proxy
    (control = reset enable, then alarm 0, count 0); gate R0 ends by checking that `P_REBOOT`
    brings Linux back.
  The state is `DETACHED -> ATTACHED -> RUNNING -> CLEAN | FAILED`; FAILED means written and
  not confirmed.
- **Not covered**: a hard reset (`macvdmtool reboot`, a direct watchdog write through the
  proxy), a power-button hold or power loss gives the controller no shutdown. After any session
  that wrote and did not close CLEAN, do not trust NTFS journal replay: restore the partition
  from the image, or check it with chkdsk from WinPE (a read-only session first, then a
  read-write one) before the next full read-write session.

`P_HV_WINDOWS_CAPS2` bit 5. `P_HV_WINDOWS_AHCI` op 5 ATTACH_NVME (a1 = address of the 192-byte
`hv_ahci_nvme_req`: "AWNVME01", version 1, flags RW/FLUSH_EVERY/GATE/SCRATCH, the three GUIDs
in GPT byte order, first, last, image bytes, the UTF-16LE name), 6 DETACH (close, idempotent,
refused while the guest runs), 7 NVME_STATUS (0 state, 1 flags, 2 commands, 3 NVMe commands, 4
longest command us, 5 flushes, 6 RMW blocks, 7 guard hits, 8/9 first/last LBA, 10 wrote, 11
failed commands, 12/13 blocks read/written, 14 inner disk bytes, 15 longest flush us), 8
NVME_TEST (gate attaches only: write/verify/flush/CRC of seeded blocks; writes only read-write,
and only on a SCRATCH attach or past the inner disk's end (from the first 4 KiB block holding
no byte of it), the tail an installed image leaves free until its backup GPT is moved to the
partition's end or C: is extended; never inside the image). A SCRATCH attach is refused unless
the partition's first 4 KiB are zero (an installed image is never scratch).

Launcher (`run_guest.py`):

| Option | Meaning |
|---|---|
| `--windows-ahci-backend {ram,nvme}` | `ram` is D1 (default); `nvme` is this |
| `--windows-ahci-partition GUID\|NAME` | the Windows partition, resolved only through the private config; a number (`6`, `p6`, `nvme0n1p6`) is refused |
| `--windows-ahci-nvme-config PATH` | default `$J804_PRIVATE_DATA/d5/windows-partition.env` |
| `--windows-ahci-nvme-access {ro,rw}` | required, no default |
| `--windows-ahci-nvme-flush {normal,every-write}` | default `normal` |

Refused before `hv.init()`: any RAM-store option with `nvme`; a missing or incomplete config
(`J804_WIN_PART_TYPE/_UUID/_NAME`, `J804_WIN_DISK_GUID`, `J804_WIN_PART_FIRST_LBA/_LAST_LBA`,
`J804_WIN_LBA_BYTES` = 4096, `J804_WIN_IMAGE_BYTES`; optional `J804_WIN_OTHER_PARTS` =
`first-last,...` for an overlap check), a well-known type, an unaligned start, a partition
smaller than the image, a key matching neither GUID nor name; CAPS2 without bit 5, the D1
contract and the AHCI FD marker; `rw` with a WinPE ramdisk boot. After ATTACH the launcher
compares the resident's first/last LBA with the config. At exit it prints the counters and
`CLEAN` or `FAILED`; `--windows-reset-on-exit` resets only after CLEAN.

Runbook rules: end every session from Windows (Shut down / Restart) or with ^C then `exit`;
leave the proxy only through the carrier after the launcher printed CLEAN:
`tools/windows_nvme_gate.py exit` (refuses unless the disk is DETACHED or CLEAN, then
`P_REBOOT`) or `--windows-reset-on-exit`. Only `P_REBOOT` goes through `flush_and_reboot()`; a
Stage 1 exit that writes the watchdog registers directly through the proxy is a hard reset,
like `macvdmtool reboot`, and is acceptable only after CLEAN. After FAILED the launcher says
so and gives no exit advice: retry DETACH (`p.hv_windows_ahci(6)`); if it stays FAILED,
`windows_nvme_gate.py exit --force`, then restore or chkdsk the partition. Keep the image as
the restore point. Gates before Windows writes: `tools/windows_nvme_gate.py r0` (two read-only
attaches, no guest, then `exit`, which checks the carrier's own reset brings Linux back),
`w0` (seeded writes in the free tail past the installed image, or anywhere with
`--partition-is-empty` on an empty partition; any read-back mismatch fails it; `exit`, then
compare the blocks from Linux with `windows_nvme_gate.py expect`), then a read-only session,
then rw. The install itself is a Linux-side recipe in the owner's private data.

Host tests (`test_windows_t8152_ahci_nvme.py`): aura's real `nvme.c` and the carrier files are
built unchanged with ASan/UBSan against `tests/e2e/windows_nvme_stubs/` and a simulated T8152
controller (`tests/e2e/nvme_sim.c`) whose media has a volatile and a durable layer (only a
normal shutdown commits). `windows_nvme_host.c` drives ATTACH, the guest's ABAR accesses, the
exits and the reset path, cuts power and checks that everything Windows was told survived and
nothing outside the partition changed: 512e IDENTIFY, RMW data against a reference, a 10^5
command guard fuzz, the fence behind a corrupted backend, flush mapping, write-through, read
only, PSCI OFF/RESET, abort, refusal, reset from another CPU mid-command, from inside the lock,
after a lost completion, with SHST stuck (parks, never resets), a FAILED close retried, a
failed quiesce after the commit (still CLEAN), a slow write completion, an error status, three
wakes in one boot, the proxy and reboot rules and the W0 test. GPT fixtures come from the test's own writer with seeded GUIDs (the M6 shape, entries
renumbered, 256-byte entries, 1024 entries) and 19 refusal variants, each leaving the media
unchanged, the fence unset and the controller shut down. `AURA_NVME_TRANSCRIPT=<file>` saves
the case transcript.

Not known yet (gates on the M6): why aura's only earlier write on T8152 lost its completion
(the longer write timeout is the first candidate, W0 decides); whether aura can wake the ANS a
second time in one boot (R0); whether CC.SHN normal plus RTKit quiesce is enough for writes to
survive a WD1 reset (W0); flush latency and the time `bhl` is held under a running Windows (R1,
D5a; the launcher prints the longest command and flush); the RTKit syslog rate during I/O (its
lines are printed from the poll loop, not rate limited yet).

## D3: EL2 SError context and the tick's guest-PC walk

The first long Windows run on the AHCI disk (D3 run 1, NT on all 12 CPUs, storahci, ~42k
commands) ended 388 s after entry with an SError taken at EL2 on the boot CPU, ESR 0xbe000000
(EC 0x2f, ISS 0: no architectural classification), at the instruction after `at s12e0r` in
`hv_translate()`. The call came from the host tick: `hv_recover_t8142_sysreg_undef()`
translated the guest's interrupted EL0 PC through the current process's TTBR0. FAR held a
stale AHCI PxCI abort and x12-x16 were the framebuffer converter's leftovers. SErrors are
asynchronous on these cores, so the PC alone could not separate the walk from the earlier
converter stores. The cause is not isolated. The walk is the weaker suspect: it used the same
TTBR0/TCR/VTTBR the guest's own fetch of that PC had just used, so it could reach no PA the
guest's walk had not (barring a concurrent page-table change), and an SError pending from
earlier in the tick, such as one from the converter's Normal-NC scanout stores on the same
CPU, is taken at the next context-synchronising point, which was that AT. Changes, all
T8152-only:

- The wedge recovery skips a tick that interrupted guest EL0 (the wedge is an EL1 handler
  spinning on itself), so the tick no longer walks arbitrary user PCs. This is hygiene, not
  the fix for run 1: it also stops a stale ESR_EL12 plus a user-mode branch-to-self from
  looking like a wedge. A clean run 2 does not show the walk was the cause.
- Kernel VAs (bit 55 set) read by the absent-PMU recovery are translated with the EL1 regime
  (`hv_translate_el()`); before, a tick that interrupted EL0 walked them as EL0 and silently
  skipped the vector re-verify and the module scan.
- SError fences (`dsb sy; isb`): before every AT in `hv_translate_el()` (every translation on
  every CPU, including the ELR/FAR translations of each guest data abort, so each AHCI MMIO
  trap), before and after each converter band (tick and idle bands, every CPU), and at every
  `hv_tick()` stage boundary (about 11 per tick on the boot CPU), so the next SError is taken
  in the stage that caused it. These fences, not the converter band alone, are what makes a
  converter-caused error show up as "fb rows ... IN PROGRESS"; no separate converter-off build
  is needed to tell the converter from the walk.
- `HCR_EL2.PTW` is set and checked before entry: a stage-1 walk that reaches the AIC Device
  leaf (the only one) faults instead of reading AIC registers.
- `hv_fatal.c`: every guest exit records kind, ESR, PC, SPSR, FAR and HPFAR per CPU (seqlocked,
  one barrier); `hv_tick()` records its stage, the converter its band, `hv_ahci.c` its last
  access and last command with the FIS, disk range and the last 8 guest spans. The EL2
  "Unhandled exception" paths (SError, synchronous) and the unhandled-guest-exception reset
  print all of it, then reboot as before: PAR_EL1 (decoded; read on entry by the EL2 handlers,
  labelled "before the proxy's translations" on the guest-exception path), ESR, ISR_EL1, FAR
  and HPFAR, then each CPU's records and breadcrumbs and the AHCI records, then a bounded EL2
  frame-pointer walk (rel addresses for `llvm-symbolizer`), and last the reads most likely to
  trap (DISR_EL1 when FEAT_RAS exists, the guest `_EL12` registers). IRQ exits are recorded at
  the top of `hv_exc_irq()`, so the shipped `ENABLE_VGIC_MODULE` build records them. Nothing is contained: the syndrome carries no
  classification and the Apple error registers are locked at this EL.

Host tests: `tests/e2e/windows_fatal_host.c` replays `hv_fatal.c` unchanged under ASan/UBSan
(gating, re-entry, the frame walk's bounds, a seqlock race against a writer thread, the D3
state report, DISR only with RAS); the AHCI trace is the `fatal_trace` case of
`windows_ahci_store_host.c`; PTW is in the policy harness; the source rules are in
`test_windows_t8152_serror.py`. `AURA_FATAL_TRANSCRIPT=<file>` saves the replay transcript.
The replay's register values are synthetic (a PA inside the synthetic window
`0x10100000000:0x10561600000`, made-up kernel and user VAs, a FAR that keeps the PxCI offset
0x138); none is copied from the run log.

## Guest (lower-EL) SErrors

i0 session 2 (USB drd1 + GPU admit + input host, AHCI disk) ended about 500 s after entry, at
the logon disk burst, on an SError the guest took on the boot CPU (cpu 6, the CPU that runs the
host tick, the proxy link, the vUART and input polls). ESR ISS 0: no class, no address. Before
this change the carrier printed nothing and the host exited the guest without reading the
frame. The cause is not known. The carrier now prints a report before the host decides:

- The SError line: ESR_EL2 with its class, guest PC, mode and SPSR, FAR_EL2 and HPFAR_EL2
  (both marked stale: an SError writes neither), AFSR1_EL2 raw, ISR_EL1 now.
- The previous exit of that CPU (saved before the SError's own record overwrites it): kind,
  ESR, PC, IPA for a data abort, ISR_EL1 at the exit, and its return: every return to the
  guest goes through `_hv_return`, which calls `hv_fatal_note_return()`. That function runs
  `dsb sy` with PSTATE.A masked and then records ELR_EL2, SPSR_EL2 and ISR_EL1. An error
  that the exit's own EL2 device accesses raised is therefore pending there and recorded.
- A verdict line, `SError vs previous exit: verdict <n>: <text>`:
  - 1: it was already pending when the guest exited. A guest access or earlier EL2 work raised
    it.
  - 2: it was not pending at the exit but was pending after the return fence, and it was taken
    at the return point. The EL2 work of that exit raised it. The exit kind and ESR name the
    path:
    - EC 0x01: the WFI idle path. "SError pending after that band" means the converter.
    - EC 0x18 or 0x3f: sysreg/PMU emulation.
    - EC 0x16 or 0x17: HVC/SMC (PSCI).
    - F: the tick, with its stage.
    - EC 0x24: an emulated page, named by its IPA.
  - 3: it was taken at the return point but was not pending at the fence, and the guest ran
    nothing in between. This is most likely EL2 work whose error arrived late.
  - 4: the guest ran on. Its own access raised it, or something completed late.
  - 5: it was pending at the return and taken later, with the guest running with A masked.
  - 6: it was taken at the return point but long after it, so it cannot be attributed.
  - 0: no exit was recorded.
- Every CPU's records and breadcrumbs, then the AHCI, USB, GPU and input records:
  - USB: the xHCI window, whether it is mapped, and when the carrier last read the controller
    and last saw its line.
  - GPU: state, ranges, handoff page and pool, the logging-hook hits and the last access.
  - Input: the counters, guest page accesses with the last one, and the last host-feed poll.
  - With the D5 NVMe disk, AHCI NVMe: the backend state (ATTACHED, RUNNING, CLEAN, FAILED),
    the partition and access mode, the controller lock owner, the AHCI and NVMe command
    counts, the closing/fence/session flags, and the last NVMe command the backend issued
    (read, write or flush; namespace block, partition offset, block count, CPU, age) with
    "IN FLIGHT" while it has no completion, or its duration and result.
- The guest x0-x30, SP_EL0 and SP_EL1.
- Last, the attribution (`hv_serror.c`). It is circumstantial. It walks the guest's stage 1 in
  software (TTBR0/1_EL12 and TCR_EL12, descriptors read only from the guest RAM window, never
  AT). It lists:
  - the loads and stores in the eight instructions before the PC and the one at it, with the
    address each one names;
  - every register whose value translates to a non-RAM IPA, or is itself the address of a
    passthrough, identity or denied region.

  Each entry gets what stage 2 and the policy lists say the IPA is. Only a passthrough (AIC,
  xHCI) or a mapped identity exception reaches hardware, so only those are counted as
  CANDIDATEs. An emulated page or an unmapped IPA traps synchronously first. The closing line
  gives the count.

The carrier then forwards the SError to the host as before. The host logs the frame (ESR, ELR
and its PA, SPSR, FAR, SP, x0-x30, the code around ELR). `run_guest.py --windows-serror
{abort,continue}` sets what happens next:
- `abort` (the default, unchanged) exits the guest.
- `continue` resumes the guest where the SError was taken, at most 8 times per session, and
  then aborts.

The SError is consumed by being taken, and the access that raised it is lost, so a continued
session only collects more reports. The proxyclient must be from this branch for the host half.

With the D5 NVMe disk (`j804-windows-nvme-serror`), an aborted session ends like any other
`hv_start` return: the carrier joins the CPUs, tears USB down and closes the disk (CLOSE_PROXY:
CLEAN or FAILED); an SError the host does not answer with EXIT_GUEST or HANDLED resets through
`flush_and_reboot()`, which closes NVMe first or parks. The launcher side:

- It keeps the carrier's `HV: fatal:` lines and, after the abort, prints the abort reason and a
  summary of the report (the SError line, the verdict, the AHCI NVMe records, the attribution's
  conclusion and CANDIDATEs), then the NVMe close report and its verdict: CLEAN, FAILED, or
  UNKNOWN when the resident does not answer (with the last NVMe verdict line the carrier
  printed on its console, if any).
- It never waits unbounded after EXIT_GUEST. 2026-11-17, carrier 353b3cf5: after the forwarded
  guest SError the carrier printed nothing for about 13 minutes, until ^C sent the proxy break;
  then every CPU exited and the disk closed CLEAN. From the exit request on, the link timeout
  is 60 s of silence (the NVMe close itself is bounded by a 12 s lock wait, a 10 s flush and
  the shutdown, and prints as it goes). On silence the launcher sends the same break, at most
  twice, and then gives up with "Hypervisor exit did not finish" and the UNKNOWN verdict. A
  link that drops (a carrier reset after its NVMe close) ends the same way. The close report's
  DETACH retry is bounded to 90 s. Why the carrier stalled until the break is not known.
- `--windows-serror` and the D5 options (`--windows-ahci-backend nvme`,
  `--windows-ahci-partition`, `--windows-ahci-nvme-access`) are independent and combine.

Capabilities are unchanged by the SError work: `P_HV_WINDOWS_CAPS` = `0x4157494e0001ffdf`
('AWIN', ABI 1, bits 0-4 and 6-15; bit 5 clear), `P_HV_WINDOWS_CAPS2` = `0x415749320000002f`
('AWI2'; bit 0 D1 AHCI RAM store, 1 D4 USB, 2 G0 GPU, 3 I0 input, 5 D5 NVMe disk; bit 4
reserved for GPU wave 2). The G5b merge (D5 + I1/I2 + tick CPU + G1/G2) reports
`0x415749320000007f`: bit 4 G1 GPU power-on, bit 5 D5 NVMe disk, bit 6 G2 (bit 5 on gpu3).

Host tests: the `report_guest` and `report_return` cases of `windows_fatal_host.c` (the return
fence comes before the ISR read, an exit's note clears the old return record, each verdict),
`tests/e2e/windows_serror_host.c` (the stage-1 walk for 4 and 16 KiB granules, blocks, faults
and reads that leave RAM; the load/store decode against assembler encodings; the
classification; the report on a synthetic xHCI load), and `test_windows_t8152_serror_attr.py`
for the source rules and the launcher policy. With D5: the `fatal_record_mid_command` case of
`windows_nvme_host.c` (the AHCI NVMe record taken by another CPU in the middle of a write, then
after it, then a session ended as after an aborted SError, closing CLEAN and surviving a power
cut), and `test_windows_t8152_nvme_serror.py`: the real UartInterface, AuraProxy and HV
exception handling against a fake carrier on the wire (the 2026-11-17 stall with the EXIT
answered or not, a dead carrier, a dropped link, a FAILED close, a normal exit), and run_guest's
close and summary functions run unchanged.

## D4: USB keyboard and mouse (usb-drd1 xHCI passthrough)

Status: built and host-tested only (`tests/python/test_windows_t8152_usb.py`). Nothing of it
has run on the M6. Contract and reasoning: `windows-m6/D4-USB-DESIGN.md` (owner notes, not in
this tree); the Mu side is `J804_WINDOWS_USB` in Mu `j804-windows`: `\_SB.XHC1` (`_HID
PNP0D10`, `_CCA 1`) lives in its own SSDT (OEM table ID `J804XHC1`) that `J804XhciAcpiDxe`
installs only when the launcher's `/chosen/aura,windows-usb-v1` handshake is present and valid
(the DSDT names no USB controller), and the FD carries the marker `J804USB1` at 0x120. Off
unless `run_guest.py --windows-usb` asks for it.

What the guest gets, and nothing more:

- **MMIO**: `[0x40a430000, 0x40a438000)`, the dwc3 xHCI register file (two 16 KiB pages),
  identity, Device-nGnRnE, mapped by the resident at ATTACH (`hv_t8152_usb_allow()` lifts
  only the denylist entry split off usb-drd1's ADT reg 0 and refuses if anything else on the
  denylist overlaps it). The rest of reg 0, every other usb-drd reg (dwc3 globals at
  +0xc100, device/OTG, PIPE, evt), `atc-phy*`, `dart-usb*`, the TLBI aperture, hpm, SPMI,
  pmgr and usb-drd0 stay denied. Teardown unmaps the window and denies it again.
- **Interrupt**: AIC 1692 (usb-drd1 `interrupts[0]`, level) published as **SPI 995**;
  usb-drd3 1834 as 997. The alias exists only while the port is attached
  (`hv_aic_alias_t8152_usb()`), so with USB off a guest enable of 995 cannot unmask 1692.
  On launches with USB configured (BIT(9)), the T8152 IRQ path pends a hardware line only
  through an owned alias (`hv_aic_alias_lookup_physical()`); any other line is masked,
  counted and logged once. Launches without USB keep the D3-validated path unchanged (the
  line is pended under `hv_aic_alias_to_published()`, its own number when not aliased). The
  DART's own line (read from the ADT) is masked and never aliased; DART faults are polled.
- **DMA**: both dart-usb1 instances, streams (instance 0, SID 0), (0, 1) and (1, 1), TCR
  **0x9** (TRANSLATE_ENABLE | FOUR_LEVEL, ias 42: what Linux runs on this port), ENABLE_STREAMS
  0x3 / 0x2. One shared identity table set (`hv_usb_dart.c`) maps exactly guest RAM
  `[ram_start, ram_end)`, DVA == IPA == PA; the AHCI store, the BGRA shadow, carrier memory,
  the tables and all MMIO fault. 4.5 GiB of guest RAM (with the AHCI store) is 146 tables,
  2.28 MiB; the whole 17.52 GiB window would be 563 (cap 640). The tables come from aura's
  heap (refused unless it has room below its limit), are cleaned to the PoC and joined to the
  stage-2 denylist (`hv_t8152_reserve_carrier()`), and are walked back before use (first and
  last page, the first 32 MiB boundary, both neighbours outside, the tables themselves).
  TLBI is aura's `dart_gen3_tlb_invalidate` (aperture slot + `tlbi vale1os`, route read from
  +0x4604) through `dart_gen3_tlbi_adt()`, at attach and teardown only: the map is static
  while the guest runs, and a Windows HCRST needs no DART action.
- **Controller state**: as `j804_usb_prep()` left it (host mode, eUSB2 PHY, VBUS from hpm1),
  verified, never re-initialised. The only write outside the DARTs and the AIC masks is
  clearing GUSB2PHYCFG0/GUSB3PIPECTL0.SUSPHY if set. Windows cannot power the controller off:
  every power lever is outside stage 2 and the XHC1 SSDT has no `_PSx`/`_PRx`/`_PRW`/`_DSW`/
  `_S0W`.

ATTACH (`P_HV_WINDOWS_USB` 0xc87, op 1, port mask bit 1 = drd1, bit 3 = drd3) runs after the
guest RAM window and the AHCI store are set and before `hv_start()`. It refuses, writing
nothing, unless: configure BIT(9) was given (T8152, soft GIC, board 0x22); the mask names only
drd1/drd3 (drd0 is refused by mask and by address); `j804_usb_port_state()` says the port is
up and the UTMI event counter runs; the ADT names the same controller, line 1692 and Gen3
DARTs; both DART instances read PARAMS1 16 KiB pages, PARAMS3 PA/VA width 42 and version 3.0
(field layout from Linux apple-dart.c), at least 2 SIDs,
PROTECT/LOCK 0, ENABLE_STREAMS 0, and each used stream has no valid TTBR and TCR 0 or 0x1; the
xHCI is halted (HCH 1, CNR 0, RS 0) and its operational, runtime and doorbell registers and
every extended capability (no Debug Capability) lie below 0x8000; GCTL is in host mode; the
tables fit and pass the walk-back. Results: 0, or -1 state, -2 ports, -3 port not up, -4 ADT,
-5 DART, -6 xHCI, -7 tables, -8 commit (TCR 0x9 did not read back, TLBI, stage 2; torn down
again). Each refusal prints its reason (`HV: USB ...`). ATTACH is also refused (-1) once the
guest session has started (an in-session proxy request) and after an earlier ATTACH in the
same carrier session reached the table build, even if teardown has since cleared it: one
ATTACH per carrier load. STATUS (op 2, port, index) returns USBSTS, PORTSC1/2, the interrupt
count, the DART exception bitmaps seen, GCTL, the unowned-line count and the storm flag
(recorded at teardown, so they survive the session); indices 10 and 11 return the live DART
identity window `[base, top)` (2**64 - 1 when nothing is attached), which the launcher puts in
the Mu handshake. DETACH (op 3) is the teardown before entry; while a started guest session
holds the port it is refused (`hv_start()`'s own exit tears down), and after the session it is
an idempotent no-op. CAPS2 bit 1 advertises all of it.

Teardown (`hv_usb_teardown()`) runs on every way out of `hv_start()` (it wraps the session:
PSCI SYSTEM_OFF/RESET, a failed join, a refusal before entry): withdraw the alias, mask 1692,
USBCMD.RS = 0 and wait up to 20 ms for HCH, then per stream DISABLE_STREAMS, TTBR 0, TCR 0x1,
TLBI, then unmap and deny the window, and free the tables last. `flush_and_reboot()` (panic,
assert, fatal EL2 exceptions, `hv_safe_reset()`) first runs `hv_usb_emergency_stop()`: the same
hardware steps without locks, stage-2 or heap work. The EL2 fatal report prints the recorded
USB state (no MMIO).

While the guest runs, the 1 Hz host report reads USBSTS and PORTSC (read only), polls both
DART exception bitmaps (+0x4000) and ERROR (+0x100); on a hit it logs the SIDs and the raw
records at +0x8000 and acknowledges both, the bitmap then ERROR (W1C, as our linux-j804
`apple-dart-t8152-gen3-irqdump.py` handler does), compares the dwc3 globals with the attach snapshot (logged, not applied),
and logs one line per change (or every 10 s while interrupts arrive). More than 50,000
interrupts per second on the line withdraws the alias and masks it for the rest of the session
(the guest keeps running without USB).

Launcher (`run_guest.py --windows-usb {off,drd1,drd1+drd3,drd3}`, default off), before
anything is uploaded: refuses any spelling of drd0, a chip other than T8152, a board other than
0x22, a resident without CAPS2 bit 1, and an FD/flag mismatch in either direction (a
`J804USB1`/`J804USB3` marker with the port off: Windows would touch a denied page; the port on
without its marker: no `XHC1` to bind). ATTACH follows the AHCI attach and precedes
`hv.start()`; a refusal stops the launch (the session is used up). Only after ATTACH returned
0 does the launcher read the DART window (STATUS 10/11), check it equals the guest RAM window
it configured, is 16 KiB aligned, non-empty and clear of the xHCI window and the AHCI store,
and add Mu's handshake `/chosen/aura,windows-usb-v1` to the guest ADT: 40 bytes little-endian,
`<IIQQQQ>` = port mask 0x2, GSIV 995, xHCI base 0x40a430000, size 0x8000, DMA window base,
DMA window top (exclusive). Any problem there detaches again and stops the launch. Without
that property Mu installs no XHC1 SSDT and Windows never touches the xHCI window, so a USB FD
under a launch with USB off is safe as well (the marker check still refuses it). The guest ADT
still loses every `usb-drd*`/`dart-usb*` node; `/chosen` also gets `aura,windows-usb` (the
port mask, diagnostic) instead of `aura,windows-usb-disabled`. STATUS is printed after the
guest exits. The JSON profile path takes `usb` disabled/off only.

Host tests: `tests/e2e/windows_usb_dart_host.c` replays `hv_usb_dart.c` unchanged under
ASan/UBSan (counts, encoders, a full 4.5 GiB sweep, faults, refusals) and dumps three arenas
that an independent Python model must match byte for byte; `tests/e2e/windows_usb_host.c`
drives `hv_usb.c` unchanged over a fake register file (every refusal writes nothing; the exact
success and teardown write sequences; F7 and refused stage-2 maps tear down; the storm guard;
the reset-path stop; DART exception and ERROR acknowledgement; one ATTACH per session, none
after entry, no DETACH under a running guest; the DMA window STATUS); the stage-2 rules run
`hv_t8152.c` unchanged with a synthetic ADT; the alias table runs `hv_aic_alias.c` unchanged;
the launcher's handshake bytes are packed and decoded and, with `AURA_MU_TREE` pointing at a
Mu `j804-windows` tree, compared with `J804Windows.h` (name, size, port bit) and fed through
Mu's own parser (`J804UsbHandshake.h`). `AURA_USB_TRANSCRIPT=<file>` saves the glue replay.
All windows and MMIO addresses in the tests are synthetic or the repo's j873.c constants; the
fake ADT's DART and secondary interrupt numbers are synthetic.

Not verified (each needs a hardware run): TCR 0x9 accepted on dart-usb1 from a carrier-built
table; delivery of a physical AIC line to a Windows guest on T8152 (which CPU takes it, after
Mu cleared the AIC enable; GICD_IROUTER is not honoured for hardware lines); Windows usbxhci on
aura-only dwc3 global state (GCTL, GUCTL and GUCTL1 differ from Linux's, logged at attach); its
64-bit register write order; USB2 LPM; a bare keyboard with nothing servicing hpm1; Mu's check
that every RAM range in its memory map lies inside the handshake's DMA window (if PrePi
describes RAM outside it, Mu leaves USB off for that boot). The design document's worked leaf
vector has a nibble slip in the SUBPAGE_END field ([51:40]); the tests check the format on a
synthetic page (`0x10100000000` -> `0x000fff1010000001`).

## G0: GPU admission and logging hooks (WINDOWS-GPU-PLAN H2/H3)

Status: built and host-tested only (`tests/python/test_windows_t8152_gpu.py`, plus a G0 block
in `test_windows_t8152_policy.py`). Nothing of it has run on the M6. Contract:
`windows-m6/G0-CARRIER-CONTRACT.md` (owner notes, not in this tree; it holds this machine's
addresses). Off unless `run_guest.py --windows-gpu admit|log` asks for it; with it off no op is
sent and the guest sees no change.

- **Admission port** (`src/hv/windows/gpu_t8152_admit.c`): the checks of
  `src/kboot_gpu_t8152.c` against the live ADT only (no FDT): chip and board, the sgx node, the
  eight firmware regions, the gfx-asc/gfx1-asc ascwrap-v8 shape, both firmware UUIDs, the
  patchbay TLVs against this boot's ADT and the TEXT SHA-256 against the J804 profile (the
  same private build header kboot uses, through `gpu_t8152_j804_profiles()`). On top of kboot:
  both wrappers and iop-vbar words inside the sgx window, one post-office bank for both roles
  outside it, the UAT regions above boot_args RAM, the firmware segments below it, two ASC
  interrupt lines each in `[32, 1020)` and outside the carrier's `[980, 1000)` block, and the
  perf-state table. A refusal zeroes the result. Every address is lifted from the ADT; the
  carrier and the admission carry none.
- **Range table**: the sgx window page by page (16 KiB), where any other `/arm-io` node's
  register (pmgr-child, error-handler, ...) makes the page a hole, rounded outward and clipped
  to the window; the page with CPU_CONTROL/STATUS (+0x44/+0x48 of each wrapper) and the pages
  with the iop-vbar words get their own classes ahead of holes. Then the post-office page, the
  four UAT regions (class fw-rw), the four DATA regions (fw-ro), both `__TEXT` (fw-text), the
  pool and the handoff page. Classes are `enum gpu_class` in `gpu_t8152_admit.h`.
- **Stage-2 policy** (`hv_t8152.c`): `T8152_MAX_ALLOW` 8 -> 24 and `T8152_MAX_MMIO` 4 -> 32
  (room for the eight firmware entries, the handoff page and G1's measured sgx sub-ranges).
  New class `hv_t8152_gpu_fw_allow()`: the only identity exception outside boot_args RAM,
  inside `/chosen` DRAM, clear of CTXR_A-D, CTRR_C-D, the denylist and guest RAM, RW or RO;
  registered, never mapped in G0. `hv_t8152_gpu_deny()` puts both `__TEXT` on the denylist.
- **ADMIT** (`hv_gpu.c`, `P_HV_WINDOWS_GPU` op 1, CAPS2 bit 2): once per carrier session,
  after guest RAM, before entry. Validates the pool first (32 MiB aligned, inside guest RAM, at
  most half of it; a bad pool spends nothing), then admits, builds the table, denies TEXT,
  registers the firmware classes, builds the handoff page in carrier memory and maps it
  read-only at IPA = PA. Logs `HV: GPU: admitted J804 <receipt>`.
- **HOOK** (op 2, `--windows-gpu log`): carrier C hooks over the whole sgx window and the
  post-office page, read-as-zero and write-ignored, counted per class (STATUS op 3), the first
  32 accesses printed with class, range and whether they fall in a GFX mailbox window. No
  access reaches hardware; no host (proxy) hook is involved.
- **Alias slots** (`hv_aic_alias.c`): four GPU slots after the two USB ones, published under
  the lines' own numbers by `hv_aic_alias_t8152_gpu()`, refusing lines outside `[32, 1020)`, in
  the carrier block, on a USB line or repeated. Nothing calls it with `live` in G0.
- **Handoff page and Mu**: 16 KiB, magic `AGH1`, version 1, layout in `gpu_t8152_admit.h`,
  CRC-32 (zlib) over the used bytes. The launcher writes `/chosen/aura,windows-gpu-v1` (16
  bytes: handoff IPA, size) only after ADMIT returned 0; Mu's GPU0 node is built from the page.

G0 itself never powers or starts anything; "once per carrier session" was enough for it. G1
(below) adds what powers the GPU, and with it the boot-wide marker.

A failed ADMIT rolls back every stage-2 exception it added (`hv_t8152_allow_rollback()`); the
`__TEXT` denylist entries stay, since they only forbid more. The handoff page is cleaned to the
point of coherency after each build, because Mu's PrePi reads it with the MMU off. The
launcher detaches USB before every GPU refusal, and refuses a pool that overlaps the loaded
image (Mu FD, appended payloads, SEPFW, preoslog, boot_args) or, with an AHCI store, leaves
less than 4 GiB of guest RAM below the pool.

## G1: GPU power-on, boot marker and mediated forwarding (`--windows-gpu probe`)

Status: built and host-tested only (`test_g1_*` and `test_launcher_probe_*` in
`tests/python/test_windows_t8152_gpu.py`, drivers `tests/e2e/windows_gpu_probe_host.c` and
`windows_gpu_hw_model.h`). Nothing of it has run on the M6. For a UEFI probe (`g18probe.efi`,
private repo) that runs under Mu before Windows, or instead of it. CAPS2 bit 4.

- **Power recipe** (`gpu_t8152_power.c`, `gpu_t8152_power_recipe.c`): PMGR PSD requests, one per
  GPU service, walked from a recipe: for each step, in the recipe's order, write the request,
  poll the acknowledge, then read the state and compare it with the step's target. The order, the
  state each service must read before its request, and the order in which a sample reads the
  acknowledge and state registers (`read_order`) are recipe data; the code refuses to write a
  service that is not in the recipe's precondition state. A failure stops there; nothing is
  powered off or retried. The recipe is not in this tree: the Makefile copies
  `$(J804_PRIVATE_DATA)/aura/g18g-j804-gpu-power.h` into
  `build/j804_gpu_power.h`, and without that file the carrier builds with power-on disabled
  (`STATUS 13` reads 0, `POWER` returns -7). Every PSD block must lie inside a `pmgr*` register
  range of the live ADT before it is touched.
- **Boot marker** (`HV_GPU_BOOT_MARKER`, `hv_gpu.h`): once the last service is on the GPU
  firmware is running (RUN comes up set), and the plan forbids a second firmware start in one
  boot (WINDOWS-GPU-PLAN section 7, a design rule, not something we tested), so POWER is once
  per boot, not per carrier session. Before its first PSD request it claims
  `/chosen/memory-map/MemoryMapReserved-15` in the live ADT (a 16-byte slot iBoot leaves zero) for
  a 16 KiB carrier page holding a CRC-protected record (`struct gpu_boot_marker`: claimed /
  powered / failed / found-on / forwarding / ended, the PSD samples, the pool, counters), and
  `MemoryMapReserved-14` for the pool. A carrier reloaded in place sees both (chainload keeps
  the live ADT and never reuses the old heap); a reboot clears them (iBoot builds a new ADT).
  POWER is refused when the marker slot is set (even if the page was overwritten), when the
  pool slot is taken, or when any service is already on (that also claims the marker). The
  guest-RAM checks of `hv_t8152.c` and `windows.py` already refuse memory-map ranges, so the
  pool, which the GPU may still reach, stays out of every later guest this boot. kboot on
  T8152 (`dt_prepare_t8152()`, `kboot_t8152_gpu_session_check()`) refuses to boot a kernel
  while either slot is set: the Linux GPU path would start the firmware a second time, and the
  pool would be ordinary RAM to Linux.
- **POWER** (op 4): after ADMIT, not with the G0 hooks, once per session. After the sequence it
  reads the GPU ID words and both ASCs' CPU_CONTROL / CPU_STATUS (guarded reads) into the
  handoff page (offsets 0xc0, 0xdc, 0xe4; version stays 1) and logs them.
- **PROBE** (op 5, `gpu_t8152_probe.c`): C hooks over the sgx window and the post-office page,
  as in G0, but each access is decided: the six GPU ID words and the core-mask word that macOS
  reads at bring-up (32-bit reads), CPU_STATUS reads, CPU_CONTROL reads, and the RX / TX /
  status registers of the two GFX post-office windows are performed on the hardware; a
  CPU_CONTROL write is never performed (a rewrite of the held value with RUN set is accepted as a
  no-op, anything else is refused and counted); everything else, the pmgr-child and
  error-handler holes, the iop-vbar pages, the rest of the wrappers and the other coprocessors'
  windows, reads as zero, ignores writes and never reaches the hardware. The four UAT regions are
  mapped RW and the four DATA regions RO at IPA = PA. Every access goes into an access map (one
  entry per address, width, direction and outcome; STATUS 0x1000 + 4 i), the first sight of each
  is printed with its offset, and the first 128 GFX messages are printed. The GPU interrupts are
  not published: G1 polls.
- **STOP / teardown** (op 6; `hv_start()` runs it on every exit, `flush_and_reboot()` turns
  forwarding off first): forwarding off, firmware regions unmapped, the access map printed, the
  marker marked ended. The GPU stays powered and its firmware running for the rest of the boot;
  nothing ever stops, powers off or restarts it.

The launcher (`--windows-gpu probe`) checks CAPS2 bit 4, the recipe and a free marker before the
upload, then runs ADMIT, POWER, PROBE and writes `/chosen/aura,windows-gpu-v1`; a POWER or PROBE
refusal, or a later input refusal, runs STOP and detaches USB. At exit, a PSCI exit or a host
stop (Ctrl-C, "Windows guest aborted"), it prints the counters and the access map.

## G2: guards, owner grants and the UAT audit (`--windows-gpu g2`)

Status: built and host-tested only (`test_g2_*` and `test_launcher_g2_*` in
`tests/python/test_windows_t8152_gpu.py`, driver `g2_tests()` in
`tests/e2e/windows_gpu_probe_host.c`). Nothing of it has run on the M6. For a UEFI probe that
publishes the GPU firmware's init data, opens its queues and replays one compute job. CAPS2 bit 6 (bit 5 on gpu3 `22261c1c`; the G5b
merge moved it because D5 holds bit 5).
G2 forwards **no register beyond G1's**: every doorbell is a message on the two GFX post-office
windows G1 already forwards.

- **G2** (op 7, `gpu_t8152_g2.c`): after ADMIT, before POWER, once, not with the G0 hooks. Arms
  the guards below for the PROBE that follows, with the launcher's grants (args[1]); a refusal
  touches nothing. The private recipe (`gpu_t8152_g2_recipe.c`) is not in this tree: the
  Makefile copies `$(J804_PRIVATE_DATA)/gpu/carrier/g18g-j804-gpu-g2.h` into
  `build/j804_gpu_g2.h`, and without it G2 is refused (`STATUS 14` reads 0, op 7 returns -11).
- **Grant `fw-mmio`** (owner decision G2-D1): the GPU firmware's page tables may map the
  recipe's register windows. Each window is anchored at G2 in the live ADT (an `/arm-io` node and
  a register range of it; the window must stay inside the hull of those registers), must not
  touch DRAM or guest RAM, and its 16 KiB pages must touch no class-table range other than the
  sgx window and its holes (no ASC wrapper, CPU_CONTROL page, iop-vbar page, post-office page,
  firmware memory, pool or handoff page). Accepted windows are published after the class table
  as handoff ranges of class 12 (`GPU_C_FW_MMIO`, tag = the firmware's slot, flag
  `GPU_RF_DECLARED`, plus read-only where the firmware only reads). They are never mapped to the
  guest CPU and never part of the stage-2 policy.
- **Grant `boot-control`** (owner decision G2-D2): POWER does one checked write to the recipe's
  sgx boot-control register right after the PSD sequence and the ID reads, before any guest:
  the register must lie in a pmgr sgx hole of the live ADT (checked at G2); read, refuse unless
  the value is one the recipe accepts, set the recipe's bits, read back the expected value. A
  wrong value is reported (`-10`), never retried; the GPU stays powered. Handoff state bit 7 and
  header word 0xec (the value read back); boot marker bit 6 and its before/after words.
- **Guards** (PROBE with G2 armed): the GFX windows take whole 128-bit messages only (the 64-bit
  halves are denied, so every forwarded message is seen whole); a second init-data message
  (EP 0x20 type 0x81) to one role, the recipe's refused types and an RTKit IOP power state
  other than on are refused (not sent, counted, boot marker bit 8); once init data went out, the
  guest's sgx and CPU_CONTROL/STATUS accesses read as zero and are not performed (the firmware
  power-gates sgx after init); every forwarded message is counted per role, direction and kind
  (`STATUS 0x400`). The access map gains the dispositions `locked` and `blocked`.
- **UAT audit** (`gpu_t8152_uat.c`, every PROBE, G1 too): the GPU reaches memory only through
  its page tables, which the guest writes (pool tables, the UAT regions mapped RW), and stage 2
  does not apply to it. The carrier walks every context-table root and the recipe's firmware
  roots: at PROBE before the guest (the baseline: what the boot chain and the firmware mapped in
  their own tables and the shared-L2 table, sorted for lookup, sized for every entry of those
  tables), before every init-data message (any entry outside the pool, the UAT and DATA
  regions and the declared windows that is not in the baseline refuses it), before every other
  message on EP 0x20/0x21 (such an entry in any guest-writable table refuses it: a pool table,
  a context-table root, and every table in an fw-rw region, since PROBE maps all four RW for the
  guest; a change the firmware makes to its own tables after init data therefore also refuses
  the next message), and at teardown (reported, boot marker bit 8). Outliers are classified
  (firmware TEXT, other DRAM, MMIO, malformed descriptor or a page reached as a table at two
  different levels, table outside the readable regions); `STATUS 0x480` and `0x4c0` give the last
  walk. Pool pages are read after a guarded probe of the EL2 mapping and a clean+invalidate.

The launcher (`--windows-gpu g2 --windows-gpu-g2-grant fw-mmio,boot-control`) checks CAPS2 bits 4
and 6, both recipes and a free marker before the upload, then runs ADMIT, G2, POWER, PROBE and
writes the Mu handshake; a refusal at any step runs STOP and detaches USB. At exit it prints the
G1 counters and access map, the UAT audit and the G2 guards and message counts. Mu needs a G2 FD
(state bit 7, word 0xec and class-12 ranges accepted); the G1 FD publishes no GPU0 for a G2 page.

## I0: Mac-fed keyboard and pointer (carrier input device)

Status: built and host-tested only (`tests/python/test_windows_t8152_input.py`). Nothing of it
has run on the M6. All three pieces now exist but have never run together: this carrier
(`j804-windows-next`), Mu `j804-windows-next` (`J804_WINDOWS_INPUT`: SSDT `J804INP0`,
`J804InputAcpiDxe`, the `J804INP1` marker at 0x140; FD in `out/j804-windows-next`), and the
`m6input` HID minidriver staged in a test-signed image (`out/d5/win11-m6-input.img`; the D3
image has no driver, so with it INP0 stays unbound). The driver has run only in QEMU on its
self-test source, under an SSDT without `_CRS`; its hardware mode (Mu's page and GSIV 991) is
untested. Contract and reasoning:
`windows-m6/INPUT-DESIGN.md` (owner notes, not in this tree). Off unless `run_guest.py
--windows-input host` asks for it; a launch without it runs exactly the D1/D4 code (the vSPI
gate is per INTID, `vdev_spi_live()`).

What the guest gets:

- **Page**: IPA `0xF50000000`, 16 KiB, a C hook (`hv_input.c`), never mapped. Only aligned
  32- and 64-bit accesses act (a 64-bit access is two 32-bit ones, low word first); 8/16-bit,
  unaligned and LDP/STP-pair accesses read 0 and write nothing (logged once, counted).

  | Off | Name | Access | Meaning |
  |---|---|---|---|
  | 0x000 | ID | RO | 0x4E49364D ("M6IN") |
  | 0x004 | VERSION | RO | 0x00010000 |
  | 0x008 | FEATURES | RO | bits 0-3 keyboard / absolute / relative / consumer reports; bit 16 host source |
  | 0x00C | CTRL | RW | bit 0 ENABLE, bit 1 IRQ_EN, bit 2 PTP_MODE (stored only); bit 31 RESET (write: flush, clear OVERFLOW, re-emit state) |
  | 0x010 | STATUS | RO | bit 0 PENDING, bit 1 OVERFLOW (sticky until RESET or ENABLE 0->1), bits 16-31 depth |
  | 0x014 | IRQ_STATUS | RO | bit 0 = the line (ENABLE && IRQ_EN && PENDING) |
  | 0x018 | SEQ | RO | sequence number of the latched report |
  | 0x01C | DROPPED | RO | reports dropped since ENABLE |
  | 0x020 | POP | WO | write 1: retire the latched report |
  | 0x024 | REPORT_LEN | RO | bits 7:0 length (0 = none), bits 15:8 report ID |
  | 0x040 | OUT_LEN | WO | bits 7:0 length, 15:8 ID, bit 31 feature; submits OUT_DATA |
  | 0x080 | OUT_DATA | WO | 64 bytes, byte 0 = report ID (v1 takes the keyboard LED report: ID 1, 2 bytes) |
  | 0x100 | REPORT | RO | 64 bytes, the latched report, byte 0 = report ID |
  | 0x180 | STATS | RO | 16 counters (`enum input_stat`) |

  Reports (the driver's own descriptor, INPUT-DESIGN.md 1.5): ID 1 keyboard, 22 bytes (id,
  modifier bits, a 160-bit bitmap of usages 0x00-0x9F); ID 2 absolute pointer, 8 bytes
  (buttons 1-5, X/Y 0..32767, wheel, AC pan); ID 3 relative pointer, 8 bytes (dX/dY
  -32767..32767); ID 4 consumer, 3 bytes (one usage 0..0x3FF). Every report carries its
  collection's full state; motion and wheel are deltas. Nothing is queued before ENABLE;
  ENABLE 0->1 and RESET emit the current state once (keyboard and consumer always, absolute
  once a position was given, relative while a button is held). Identical states are not
  re-queued. The queue holds 256 reports; when full, the oldest report whose collection has a
  later state queued (pointer reports first) is dropped, never the latched one, so every
  collection's last state survives (a press and release inside one dropped stretch is lost;
  relative motion in a dropped report is lost; both counted).
- **Interrupt**: SPI **991**, level, active-high, purely virtual (no AIC line, never an
  alias), driven through the software GIC CPU interface like AHCI's 990, on line changes only.

Host feed: from ATTACH on, the RX direction of the vUART CDC pipe (`IODEV_USB_VUART`,
`/dev/cu.usbmodemP_03` on macOS) belongs to the input device. The guest's UART then reads an
empty receiver (no RXD, no RX count, URXH 0); its TX (COM0 output) is unchanged. The boot
CPU's 5 kHz tick, right after `hv_vuart_poll()`, reads at most 256 buffered bytes (never more
than are buffered: the CDC read would wait) and parses them under the input lock. Bytes the
Mac sends between ATTACH and guest entry are discarded at entry (at most the 1 MiB CDC ring;
the count is logged), and the parser and sequence start clean, so nothing sent before the
guest's session reaches it; frames during Mu only update state. Wire protocol v1,
little-endian:

    +0 'M' (0x4D)  +1 '6' (0x36)  +2 type  +3 len (0..64)  +4 seq u16  +6 payload[len]
    +6+len  CRC-32 (zlib) of bytes +2 .. +6+len-1

| type | name | payload |
|---|---|---|
| 0x01 | HELLO | u16 version 1, u16 flags 0: host state all up, the sequence restarts |
| 0x02 | KEYS | u8 modifiers, u8 n <= 16, u8 usage[n] in 0x04..0x9F: the full host keyboard state |
| 0x03 | ABS | u8 buttons (<= 0x1F), u16 x, u16 y (<= 32767), s8 wheel, s8 pan (>= -127) |
| 0x04 | REL | u8 buttons, s16 dx, s16 dy (>= -32767), s8 wheel, s8 pan |
| 0x05 | CONSUMER | u16 usage (<= 0x3FF, 0 = released) |
| 0x06 | KEEPALIVE | none: refreshes the held-key watchdog |
| 0x07 | RELEASE_ALL | none: every host key, button and consumer usage up |

The parser scans for the sync pair; a bad type or a length above 64 is refused at the header,
a bad CRC drops the frame, and either way the scan resumes at the byte after the failed sync
(a frame inside it is found). A good CRC with a payload invalid for its type is ignored. A
partial frame that has waited 50 ms since the last byte is given up the same way, so a
damaged length cannot hold the frames behind it. `seq` gaps are counted, not enforced. If any
host key, button or consumer usage is held and no valid frame arrived for 2 s, the carrier
releases them (a feeder holding keys sends KEEPALIVE every 0.5 s). At guest exit every host
key is released, state only: the vSPI is not driven once the guest has left. `proxyclient/aura/windows.py` has the encoder (`input_frame`, `input_keys`,
`input_abs`, `input_rel`, `input_consumer`, `input_hello`); a feeder must open only the guest
CDC port, never the proxy port (any byte there stops the guest).

The feed is not authenticated. The CRC only catches damage; it says nothing about who wrote
the bytes. The guest's COM0 output (TX) and the input feed (RX) share one Mac tty, so if that
tty echoes (a `cu` device left in its default cooked mode: ECHO, ICANON, IXON), every byte the
guest writes comes back as host input, and a guest that writes valid frames to COM0 injects
input. A feeder must put the port in raw mode before it writes (`m6type` does), and nothing
that echoes may hold the port while input is attached.

When the queue is full, each new report costs one pass over the queue (the drop choice) and
a move of at most 255 one-byte slot numbers; the reports themselves never move. So a Mac
stream into a driver that stopped popping costs at most a few hundred steps per report under
the input lock.

Resident ABI: configure **BIT(10)** (`HV_WINDOWS_T8152_INPUT`; T8152, software GIC, board
0x22), **CAPS2 bit 3**, **`P_HV_WINDOWS_INPUT` 0xc89** (0xc88 and CAPS2 bit 2 are
`j804-windows-gpu`'s; re-check both at merge): op 0 QUERY (0 version 1, 1 base, 2 size, 3
INTID, 4 features 0x1000F, 5 accepted sources 0x1), op 1 ATTACH (a1 sources: bit 0 host; bits
1/2, the internal keyboard and trackpad, are refused by this carrier; a2/a3 must be 0), op 2
STATUS (0-15 the counters, 16 attached sources, 17 CTRL, 18 depth, 19 DROPPED, 20 LEDs, 21
line, 22 RX owned), op 3 DETACH (before entry only; `hv_start()` then refuses). ATTACH
results: 0, -1 state (flag, chip, board, after entry, or a second ATTACH this carrier load),
-2 sources, -3 area, -4 hook. `hv_start()` refuses with BIT(10) set unless ATTACH succeeded
and the page is wholly this hook.

Launcher (`run_guest.py --windows-input {off,host}`, default off), before anything is
uploaded: refuses a chip other than T8152, a board other than 0x22, a resident without CAPS2
bit 3, `--windows-serial disabled`, `host+kbd`/`host+kbd+tp` (not in this carrier), and an
FD/flag mismatch in either direction (a `J804INP1` marker with input off, input on without the
marker, a marker with another page or GSIV). After the USB attach and before `hv.start()` it
checks QUERY against its contract, ATTACHes, and only after ATTACH returned 0 adds Mu's
handshake `/chosen/aura,windows-input-v1`: 24 bytes `<QIIII>` = page base 0xF50000000, size
0x4000, GSIV 991, feature mask (report bits 0-3, attached sources in bits 16-23; 0x1000F for
host), version 1. STATUS is printed after the guest exits.

### The Mu side (contract; not in Mu yet)
With G0 in the same launch (`j804-windows-next`), the order is AHCI attach -> USB attach ->
GPU ADMIT (and HOOK) -> input ATTACH -> `hv.start()`, and every input refusal detaches USB
first, as the GPU refusals do (a failed input DETACH after a refused handshake does not skip
that). If `hv.start()` itself refuses before entry, the launcher detaches USB and input (the
vUART RX goes back to the console); a GPU admission has no release and lasts until the
carrier is reloaded. The four features do not share a number: proxy ops 0xc8e AHCI,
0xc87 USB, 0xc88 GPU, 0xc89 input; CAPS2 bits 0-3 in that order; configure BIT(8) AHCI, BIT(9)
USB, BIT(10) input (G0 has no configure flag); FD markers 0x110 `J804AHC1`, 0x120 `J804USB1`,
0x130 `J804USB3`, 0x140 `J804INP1` (G0 has none: Mu's GPU0 comes from the handoff page); SPIs
990 AHCI and 991 input (emulated), 995/997 USB (aliases), and the GPU alias slots refuse the
whole 980..999 carrier block.

### The Mu side (contract; built in Mu `j804-windows-next` b2287c9, not run on the M6)

Mu implements it following the D4 `J804_WINDOWS_USB` pattern (`J804XhciAcpiDxe`, SSDT
`J804XHC1`):

- `J804Windows.h`: `J804_INPUT_BASE 0xF50000000`, `J804_INPUT_SIZE 0x4000`,
  `J804_GSIV_INPUT 991`, `J804_INPUT_FD_MARKER_OFFSET 0x140`, `J804_INPUT_ADT_PROPERTY
  "aura,windows-input-v1"`, `J804_INPUT_ADT_PROPERTY_SIZE 24`, version 1.
- Build flag `J804_WINDOWS_INPUT` (default FALSE). With it, the FD carries 16 bytes at 0x140:
  `"J804INP1"`, u32 `0x00F50000` (base >> 12), u32 991. With it off, the default, AHCI and USB
  FDs rebuild byte-identical.
- An SSDT (OEM table ID `J804INP0`), not a DSDT change: `Device (INP0)` with `_HID
  "NTAS2040"`, `_UID 0`, `_CCA 1`, `_STA 0x0F`, `_CRS` = `QWordMemory (ResourceConsumer,
  PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite, 0, 0xF50000000, 0xF50003FFF, 0,
  0x4000)` + `Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 991 }`; no
  `_PSx`/`_PRW`/`_S0W`.
- `J804InputAcpiDxe` installs that SSDT only when the property is present, exactly 24 bytes,
  base/size/GSIV equal the header, version 1, feature bits 0-3 all set, bits 4-15 and 24-31
  clear, at least one source bit in 16-23, and the page is NonExistent in the GCD; otherwise
  nothing is installed (fail closed) and the reason is logged. Mu never touches the page and
  has no input driver: host frames during Mu only update carrier state.
- The `APPL8900` COM0 device stays as it is (its RX direction is now the input feed while
  `--windows-input host` is on; Windows binds no driver to it).
- `tests/python/test_windows_t8152_input.py::test_contract_matches_mu_header` compares the
  header with this launcher when `AURA_MU_TREE` names a Mu tree that has it.

Host tests: `tests/e2e/windows_input_host.c` replays `input_model.c` and `hv_vdev_spi.c`
unchanged under ASan/UBSan (21 cases plus a 10^6-step seeded fuzz: reset values, report
layout, ENABLE/RESET/POP and the line, overflow ordering and the latched report, every
malformed frame class, any split of the stream, the stall expiry, the watchdog, merging, bad
widths, output reports, the per-device SPI gate) and parses a corpus written by the
launcher's encoder; `tests/e2e/windows_input_glue_host.c` drives `hv_input.c` unchanged over a
fake vUART RX and hook (refusals attach nothing, one ATTACH per session, RX ownership, the
256-byte budget and no blocking read, SPI 991 on changes only, the watchdog on the system
counter, exit, verify). `AURA_INPUT_TRANSCRIPT=<file>` saves the model replay.

Not verified (each needs Windows or the M6): kbdhid with a bitmap keyboard on ARM64 26100;
SPI 991 free on the live machine (980..999 were free except 984 in the last ADT walk); the
CDC RX path under sustained host traffic while the guest runs; the cost of the trapped
accesses (about ten per report).

## I1: the M6's internal keyboard (carrier-owned MTP)

Status: built and host-tested only (`tests/python/test_windows_t8152_input_mtp.py`, plus the
I1 glue cases in `test_windows_t8152_input.py` and the pool-only scenario of
`tools/windows_rtkit_e2e.py`). Nothing of it has run on the M6. The launcher asks for it with
`--windows-input host+kbd` (I2 below adds `host+kbd+tp`). Plan and reasoning:
`windows-m6/INPUT-I1I2-BUILD.md` and `INPUT-DESIGN.md` section 3 (owner notes, not in this
tree). The guest ABI is unchanged: the internal keyboard arrives as the same ID 1 / ID 4
reports as the host feed, OR-merged with it; FEATURES bit 17 says it is live.

ATTACH (`P_HV_WINDOWS_INPUT` op 1) with `a1 = host | keyboard` (bit 0 | bit 1; bit 8 = Fn
default "fkeys"), `a2/a3` = an 8 MiB, 2 MiB aligned reserved area above guest RAM, which the
launcher carves off the top of the guest RAM window. The host source is required (the Mac-fed
path stays the fallback); the trackpad (bit 2) is I2 below. `QUERY Q_SOURCES` returns 7. The
carrier then, before guest entry (`hv_mtp_t8152.c`, `mtp_bringup.c`):

1. checks the live ADT: `/arm-io/mtp` `iop,ascwrap-v8` at 0x314600000 / 0x314700000, its
   `iommu-parent` is `dart-sbr/mapper-mtp` stream 4, `dart-sbr` at 0x318100000 with vm-base
   0x10040000000, the `keyboard` nub;
2. reserves the area (`hv_t8152_reserve_input_area()`: above guest RAM, clear of every
   carve-out, CTXR range, aura and its heaps, the AHCI store; denied to stage 2), remaps its
   device-written rings Normal-NC in every alias of its own map (both ring indices share one
   cache line), invalidates its own EL2&0 translations (`mmu_flush_own_tlb()`:
   `mmu_add_mapping()`'s `tlbi vmalle1is` reaches only EL1&0 once `hv_init()` cleared TGE),
   checks with `AT S1E2R` that every ring page now translates with attribute 0x44 (else the
   attach is refused before the MTP is touched), cleans the rings again and zeroes them and the
   table pages;
3. reads the 13 PMGR MTP states at 0x30828c000 + 0x20 i (PMGR is always powered). **All on:**
   reads `ASC_CPU_CONTROL`; **RUN set refuses with nothing written**. **Any off:** the MTP
   cannot be running, so the states whose target is not on are switched on (parent first, each
   write logged, each polled to on for at most 10 ms) and RUN is read after them (set: still
   refused). `ASC_CPU_CONTROL` is never read with an MTP domain off (an unpowered ASC faults or
   reads all ones). A reboot through iBoot gives a fresh MTP;
4. reads only dart-sbr `TTBR[4]` (valid, no bits outside 29:2) and `TCR[4]` (must be 9); the
   root must be carrier-mapped RAM outside guest RAM, the store and the area;
5. maps the TX ring (DVA 0x10040000000), RX ring (0x10040204000), wake ring (0x10040408000)
   and a 1 MiB RTKit pool (0x10040410000) with its own 3-level entries under that root
   (`mtp_dart_t8152.c`: refuses a foreign table or a valid leaf, checked by its own walk; no
   DART register write, no DART TLB invalidation);
6. boots RTKit in the new pool-only mode (`rtkit_set_buffer_pool_premapped()`; a buffer or
   oslog request with a preset address is answered as our Linux driver answers it: the preset
   is ignored, a pool slice is granted and its DVA replied, counted as "presets replaced"),
   which writes RUN, then starts endpoint 0x20;
7. SCM: power 2, waits for 0xfe, registers the three rings, 0xff; reads INIT, finds the
   interface named "keyboard" (complete only with its last packet **and** a descriptor: one
   whose last packet carried none is waited for until the 500 ms INIT deadline, as Linux waits
   on ring transports), maps its HID descriptor to a layout (`hid_desc.c`, `input_kbd.c`; an
   unmappable descriptor leaves the keyboard off; Fn is vendor usage 3 on page 0x00FF, 0xFF00
   or 0xFF0C), enables it (`{0xb4, i}`, acknowledged with return code 0) and waits for READY.
   A GPIO request is refused with 0xe000f00d and the request echoed; it never takes the
   command slot while a command is still reading its own acknowledgement. stm, actuator,
   tp-accel and mtp are never enabled. Frames up to the protocol's 64 KiB are buffered.

STATUS 23 steps: 1 identity, 2 power (domains on, RUN read clear), 3 dart (stream-4 entries),
4 rtkit, 5 scm, 6 init, 7 keyboard ready, 8 trackpad ready (`| 0x80` where it stopped).

Every step is bounded; a failure stops the MTP part with a step and a reason (STATUS 23/24)
and leaves the host source attached (ATTACH still returns 0). MMIO is limited to an
allow-list (read: ASC_CPU_CONTROL, TCR[4], TTBR[4], the 13 PMGR states; write: the PMGR
states only), each access under the exception guard; the mailbox goes through `asc.c`. No
NVRAM access in any of these files; the only SMC traffic is I2's AFE reset session below.

Run time: polled only (lines 1500/1501 stay masked), from the boot CPU's tick before the host
feed (1 kHz floor) and from the unlocked WFI path, one servicer at a time
(`hv_mtp_t8152_poll()`), never waiting: at most 4 RTKit messages and 32 frames per call; a
command doorbell that finds the mailbox full is retried on the next poll (latched as an SCM
send failure once it has been owed for 50 ms). The
WFI path never drives the SPI; the tick does. An RTKit failure, a ring index or framing error,
or three checksum errors in a row latch: the internal keyboard is released once and servicing
stops; host input continues. Entry and exit release the internal source (state only). Nothing
is sent to the MTP at exit: it keeps running and DMAing into the area, so a session that
brought it up must be left by a reboot. That is enforced on the host: every J804 launch
(`run_guest.py`) and every `chainload.py` first reads the 13 PMGR states and, only when all are
on, `ASC_CPU_CONTROL` (read-only, `aura/windows.py t8152_mtp_running()`), and refuses while RUN
is set. The internal keyboard has no held-key watchdog: its reports come only on change, so
silence cannot be told from a held key (the Mac-fed source has one because it sends
keep-alives); a latch releases it.

Keyboard translation: modifiers as reported (Command is GUI, Option is Alt); the Fn layer is
ours: by default the F-row sends brightness (F1/F2, 0x70/0x6F), AC Search (F4), previous /
play / next (F7-F9) and mute / volume down / up (F10-F12) as consumer usages and F1-F12 with
Fn ("fkeys" swaps that); Fn + arrows give Home/End/PgUp/PgDn, Fn + Backspace Delete, Fn +
Return Insert. A key keeps the translation it was pressed with until released. Fn alone
sends nothing; usages above 0x9F are dropped (counted); an ErrorRollOver report keeps the
previous state. The consumer usage the guest sees is the newest non-zero one of all sources.

`P_HV_WINDOWS_INPUT` op 4 `PROBE` (before entry, keyboard live): services for `a1` ms (at
most 10 s) and logs every frame and every translation (50 per second). The launcher runs it
with `--windows-input-probe SECONDS` (1-120, in 10 s slices) after ATTACH; with
`--windows-input-probe-only` it then stops without a guest (step A, carrier only: the NVMe
disk, if any, is closed with its verdict; leave by a reboot). ATTACH logs the steps'
timings, the register values, the PMGR states before, the INIT interface table and the
keyboard descriptor in hex. These logs carry the machine's own descriptor: keep them with the
owner's private data. STATUS 23-31: MTP state, reason, keyboard reports | bad << 32, trackpad
(0), RTKit app messages | doorbells << 32, longest service call (us), checksum | ring << 16 |
oversize << 32 | parse << 48 errors, SMC writes (0), GPIO requests refused. The 1 Hz report
adds `HV: input: mtp kbd ...` when its counters move.

Build: the I1 code pushes `.text` across a 64 KiB boundary, which moves `.data` and the vital
file end (0x12acd8) past the default 0x120000 limit, so the carrier is linked with
`FILE_LIMIT=0x200000`, the J804 value `aura-raw.ld` documents (the panel DCP's `__OS_LOG` is
0x234000 past the installed stage 1's base).

Not verified (each needs the M6): RUN clear when the carrier attaches on the Windows route;
which PMGR states are off then; TTBR/TCR/root as measured by Linux; the RTKit buffer requests
fitting a 1 MiB pool (and whether any carries a preset address); the keyboard descriptor
mapping; whether its INIT packets carry the descriptor with the last packet; the 1 kHz
service keeping the syslog backlog drained; GPIO requests; the next Linux boot getting its
keyboard back after a reboot.

## I2: the M6's trackpad (relative mouse over the same MTP)

Status: built and host-tested only (`windows_input_tp_host.c`, the P cases of
`windows_mtp_bringup_host.c`, the I2 glue cases, the launcher's area, container and parameter
page tests). Nothing of it has run on the M6. The guest ABI is unchanged: the trackpad arrives
as report ID 3 (relative pointer: buttons, dX, dY, wheel, AC Pan; the m6input descriptor has
all of them, so the Windows driver and image stay as they are); FEATURES bit 18 says it is
live. Precision Touchpad reports (IDs 0x10-0x14) are a later step (I2b).

Launcher: `--windows-input host+kbd+tp` (with `J804_PRIVATE_DATA` set, or
`--windows-input-tp-fw PATH`) reads the owner's trackpad firmware container (HIDF: magic,
version 1, header length, payload length, interface-byte offset; the owner's own file, lifted
from their recoveryOS, never in a repo or the Windows image), checks it, and before ATTACH
writes its payload at area + 0x700000 (at most 128 KiB) and a 64-byte parameter page at area
+ 0 (`mtp_bringup.h`: firmware size, CRC-32 and interface offset, the tunables, the page's own
CRC-32). Only the file's SHA-256 is printed. `QUERY Q_SOURCES` must have bit 2 (7). Options:
`--windows-input-tp-tap on|off`, `--windows-input-tp-scroll traditional|natural`,
`--windows-input-tp-gain` (counts per mm, 6), `--windows-input-tp-accel` (3: 4x when fast),
`--windows-input-tp-scroll-mm` (5), `--windows-input-require-mtp` (refuse to start unless
FEATURES shows every asked source after ATTACH). The handshake's feature mask is FEATURES as
read after ATTACH.

ATTACH with `a1 = host | keyboard | trackpad`: the carrier checks the ADT's
`iop-mtp-nub/multi-touch` `function-afe-reset` is exactly pKW8('pcIO', 0x10000, 0x1c0000),
checks the parameter page and the firmware's CRC (a bad page or CRC: no trackpad, nothing
mapped for it), maps the firmware's 128 KiB after the RTKit pool (DVA 0x10040514000, same
tables), and after the keyboard is READY (a trackpad failure never costs the keyboard):

1. enables "multi-touch" (`{0xb4, i}`, return code 0);
2. writes the interface number into the firmware at its offset and cleans the buffer to the
   PoC;
3. **SMC `pcIO` <- 0x001c0000_00010001**: the AFE reset asserted; services the MTP for 50 ms;
4. sends the firmware: `{0x95, 2, 0, i, u64 DVA, u32 size}`, return code 0;
5. power method 2 "on" announced: `{0x40, 2, i, 2, 0, 0, 0, 0, 0}`, return code 0;
6. **SMC `pcIO` <- 0x001c0000_00010000**: the AFE reset released; 50 ms;
7. "on" confirmed: `{0x40, 2, i, 2, 1, 0, 0, 0, 0}`; waits for multi-touch READY (1 s);
8. asks feature 0xD9 (geometry, 200 ms, optional: without it a 130 x 81 mm, 95 units/mm
   default is used).

SMC: the carrier's only SMC traffic is this one session in `op_afe_reset()`, only while a
trackpad ATTACH runs, through aura's own `smc.c` (the same init / write / shutdown pattern
main.c already runs on J804 for the N1 and SD reader pcIO lines). All of it, in order:
- `smc_init_timed(2 s)`: the RTKit wake (management IOP power state INIT); if the SMC answers
  HELLO, the version reply, the endpoint-map replies and the IOP and AP power states ON, else
  the session the boot left running is adopted; endpoint 0x20 started; `SMC_INITIALIZE` (its
  answer is the shared-memory address). Every wait is bounded (HELLO and each endpoint-map
  message 1 s, the power states and the `SMC_INITIALIZE` answer 2 s);
- exactly two key writes, each bounded at 200 ms (`smc_set_timeout()`): `pcIO` <-
  0x001c0000_00010001 (assert) and `pcIO` <- 0x001c0000_00010000 (release);
- `smc_shutdown()`: the RTKit AP power state, then the IOP power state, to QUIESCED.
No other SMC key is read or written: no audio, speaker or amp key, no NVRAM or power key,
ever. A failed write ends SMC access for the session. After a failure between the two writes
the AFE is left in reset (the safe state: Linux saw an MTP SError when the line was released
during an upload); STATUS 30 then says 1 write.

Translation (`input_tp.c`, integer only): report 0x75, a 38-byte header (byte 22 the record
count, at most 16; byte 23 bit 0 the physical click), 30-byte records (+4 X, +6 Y, Y up, +18
touch major; 0 = not touching). Contacts are matched to the previous frame by position. One
contact (or any while the button is down): the contact that moved most drives the pointer,
`gain` counts per mm, times an acceleration factor rising linearly from 1 at 15 mm/s to
1 + `accel` at 150 mm/s, remainders carried; a change in the number of contacts moves
nothing. Physical click: left, or right when it starts with two or more contacts, latched
until release; motion is ignored for 40 ms after it starts. Tap: one contact up within
180 ms, travel under 1.5 mm, no click, no scroll, no internal key in the last 300 ms: left
down and up; two contacts: right. Two contacts moving the same way: wheel / AC Pan detents
every 5 mm (dominant axis; traditional unless natural). Three or more: nothing. 500 ms
without frames drop held contacts and the button; while the pad has never ended a touch with
a zero-contact frame, a touch that ends in that silence is judged as a tap up to its last frame
(late, but not lost; "silent" taps are counted). A latch releases the pointer with the
keyboard; entry and exit release the trackpad source (state only).

STATUS: 23 reaches 8 (trackpad ready); 24 is reason | trackpad reason << 8 | trackpad step
<< 16 (reasons 21-28 in `mtp_bringup.h`); 26 trackpad frames | bad << 32; 30 SMC pcIO writes
| failed << 16. ATTACH logs the trackpad's step, firmware DVA and size, the SMC writes, the
geometry and the multi-touch descriptor in hex; PROBE logs its frames and translations.

Not verified (each needs the M6): the multi-touch descriptor and the 38 + 30n frame layout on
J804; the 0xD9 answer and the real resolution; whether the MTP asks for GPIO pulses on the
AFE line (refused); the SMC staying healthy after the two writes; the haptic click with the
actuator never enabled; the tunables (gain, acceleration, scroll travel, tap limits).

## G5b: the merged carrier (D5 + I1/I2 + tick CPU + G1/G2)

Status: built and host-tested only; nothing of the merge has run on the M6. One carrier carries
the NVMe-backed disk and the guest-SError report (D5), the internal keyboard and trackpad
(I1/I2), the tick-CPU choice (`P_HV_WINDOWS_TICK_CPU`) and the GPU G1/G2 path.
`P_HV_WINDOWS_CAPS2` = `0x415749320000007f`: bit 5 is the D5 NVMe disk and **G2 moved to bit 6**
(gpu3 had it on bit 5). `tests/python/test_windows_t8152_caps2.py` fails on any bit claimed twice.

Launch order (`run_guest.py`): the AHCI store or NVMe disk, USB, then **input ATTACH (the MTP
bring-up) and the optional input PROBE, then GPU ADMIT, G2, POWER and PROBE**, then `hv.start()`.
An input refusal therefore never spends the boot's GPU firmware session; a GPU refusal detaches
input again and says the MTP keeps running (leave by P_REBOOT). Refused before anything reaches
the M6: `--windows-input-probe-only` with any GPU mode; `--windows-gpu probe|g2` with the NVMe
backend (the GPU pool is not re-derived for that layout, G5-PLAN C-1); `--windows-serror
continue` with the GPU (D5). On Ctrl-C or a forwarded guest exception the launcher reads the
USB, GPU (access map, UAT audit) and input status before it detaches anything.

Teardown: `hv_start()` runs USB, then GPU teardown, then the AHCI/NVMe session end;
`flush_and_reboot()` stops USB DMA and GPU forwarding, then D5's bounded NVMe close.

The fatal report's GPU part describes the G1/G2 session from recorded values (no lock, no MMIO):
state, G1 counters and the boot marker, G2 grants, boot-control step, sgx lock, init data, the
five guards, the UAT audit summary, message counts and the last forwarded doorbell.

**Console.** The proxy CDC's console write no longer waits for the host (d5c froze cpu6 705 s in
the tick's printout while the Mac slept): it takes what fits in the 1 MiB ring and returns; what
the 8 KiB console buffer has to overwrite is dropped and counted, with stall count, longest and
current stall. Only the guest-SError report, the unhandled-exception report and `HV: All CPUs
exited` wait for the host, at most 2 s each. The proxy protocol itself stays blocking.

**Once a minute of guest run** the tick owner prints:

    HV: status t=<guest s> soc=<SoC s> tick cpu <t> boot cpu <b>
    HV: status exits/min: c0=<n> ... (live CPUs)
    HV: status ahci <commands> input <ok>/<bad>; console dropped <bytes> stalls <n> longest <us> us now <us> us write max <us> us; vuart dropped <bytes>
    HV: GPU SUM t=<s> st=0x<state> g2=<0|1> fwd <n> denied <n> runref <n> faults <n> db <n> aud <n> fresh_max <n> capped <0|1> tbl <n>/1024 guards <n> init 0x<roles> msgs tx/rx <gfx tx>/<gfx rx> <mgmt tx>/<mgmt rx> drop <bytes>

`GPU SUM` appears only while a G1/G2 session forwards. All values are relaxed reads of counters
other paths keep. The input line is unchanged (checked each second, printed on change).

## G6-S: the scanout service (virtual vblank and flips, `--windows-scanout`)

Status: built and host-tested only; nothing of it has run on the M6. The carrier gives the
WDDM driver a frame-based path to the internal panel without touching the DCP.

**How the panel is driven (unchanged).** iBoot leaves the DCP scanning one X2R10G10B10 surface
(the boot framebuffer, `boot_args.video`). The DCP is not the carrier's: its MMIO stays on the
stage-2 denylist and no swap or vblank comes from it. With `--windows-fb-shadow` the guest's
framebuffer is a BGRA shadow that the carrier repacks into that surface from idle CPUs
(`hv_fb_convert_slice`), a free-running sweep with no frames.

**What G6-S adds.** One hooked page and one level SPI, off unless the launcher passes
`--windows-scanout on|HZ|MHZ` (configure `BIT(11)`, CAPS2 bit 7, `P_HV_WINDOWS_SCANOUT 0xc8b`):

- page IPA `0xF60000000`, 16 KiB, never mapped (`hv_scanout.c`, model `scanout_model.c`);
- SPI GSIV 992, level, no AIC line, live only with the scanout flag (`hv_vdev_spi`);
- **vblank**: from the system counter at the attached rate (default 60 Hz: 400000 ticks of the
  24 MHz counter), epoch at guest entry, edges at `epoch + floor(n * F / mHz)` (no drift), run
  on the 5 kHz host tick (edges land within 200 µs), 64-bit count and guest-virtual edge time;
- **flip**: the driver names a buffer in guest RAM (or the shadow): BGRA8888 or the panel's own
  A2R10G10B10, 64-byte aligned base and stride, optional dirty rectangle, a nonzero cookie. The
  carrier copies it into the scanout in row bands: 160 rows on a CPU about to idle (unlocked),
  32 rows on every non-tick CPU's 1 kHz FIQ fast path (unlocked, ~75 µs), 16 rows on the tick
  (big lock) as a floor. Each band cleans and invalidates its source rows first and is fenced
  and recorded like the converter's (`hv_fatal_fb_begin/end`). The flip is reported done
  (DONE_COOKIE, STATUS.FLIP_DONE) at the first vblank after its last band. One flip in flight
  plus one waiting; a third is refused BUSY;
- while CTRL.ENABLE and CTRL.FLIP_MODE are set the shadow sweep stands down; clearing either
  (or guest exit) retires both slots and gives the panel back to the shadow.

Panel tearing: the copy is not synchronised with the DCP's real scan position (that needs the
DCP), so a fast-moving frame can still tear on the glass; the guest-visible flip/vblank
sequence is consistent. FEATURES bits 5 (hardware vblank) and 6 (scanout retarget) stay clear.

**Mu side** (`J804Windows.h`, Mu branch `j804-windows-g6-scanout`): FD marker `J804SCN1` at
0x150 (`u32 base >> 12, u32 GSIV`); `/chosen/aura,windows-scanout-v1`, 32 bytes `<QIIIII4x>`
= page base, size, GSIV, FEATURES, refresh mHz, version 1, written by the launcher only after
ATTACH returned 0. With it (and the GPU handshake) J804GpuAcpiDxe installs the GPU0 variant
whose `_CRS` adds resource 5 (the page) and resource 6 (`Interrupt` 992, level, active-high)
and whose `_DSD` names `aura,scanout-ipa`, `aura,scanout-gsiv`, `aura,scanout-refresh-mhz`.

The register map and buffer rules for the driver are in the G6 interface note kept with the
build outputs (out/g6-scanout/INTERFACE.md); `scanout_model.h` is the authority.
