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

Not in G0 yet, each needing an owner-present session: PSD power-on (3, 82, 81), the
CPU_CONTROL guard (our J804 Linux logs and the macOS traces show RUN already set once power-on
completes, so G1 refuses writes that clear or toggle RUN instead of granting one RUN), iop-vbar pinned by value, mailbox forwarding, measured sgx passthrough, live GPU aliases,
mapping the firmware classes, and a boot-wide (not per-carrier-session) admission marker.
"Once per carrier session" is all G0 has: a carrier reload resets it. That is harmless while
nothing powers or starts the firmware; the GPU host test fails as soon as `hv_gpu.c` sets
`GPU_HS_POWERED` or `GPU_HS_ASC_RUN` without a `HV_GPU_BOOT_MARKER`.

A failed ADMIT rolls back every stage-2 exception it added (`hv_t8152_allow_rollback()`); the
`__TEXT` denylist entries stay, since they only forbid more. The handoff page is cleaned to the
point of coherency after each build, because Mu's PrePi reads it with the MMU off. The
launcher detaches USB before every GPU refusal, and refuses a pool that overlaps the loaded
image (Mu FD, appended payloads, SEPFW, preoslog, boot_args) or, with an AHCI store, leaves
less than 4 GiB of guest RAM below the pool.
