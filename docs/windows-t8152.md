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
  with all of its code (`[_base, _rodata_end)`) clear of the last 16 KiB page of CTXR_D:
  measured on J804, EL2 on the secondaries cannot execute that page;
- all 12 CPUs (cpu0-11) are live in aura's dispatch loop (`smp_is_alive()`, the boot CPU
  counted as the vGIC counts it) and no spin-table entry past cpu11 is. The J804 Mu MADT is
  static (12 GICCs, all enabled) and has no live mask to patch, so a partial machine is
  refused, not launched: start the secondaries (`p.smp_start_secondaries()`) first. The
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
reads its own copy of the filtered guest ADT. `hv_write_hcr()` keeps `HCR_EL2.TWI` set and `FWB` clear.

Before entry, `hv_t8152_verify()` walks the whole stage-2 table again and checks the entry
point and boot_args are in the window, TWI is set for the boot CPU and the secondaries,
FWB is clear, stage 2 and the SMC trap are on, `framebuffer=bgra-shadow` is selected and all
12 CPUs are still live.

## Other T8152 behaviour

- PSCI CPU_ON is a spin-table release (`hv_start_secondary()`); CPU_OFF parks the core back
  in aura's spin table and is never a power-off. The boot CPU's CPU_OFF is DENIED.
- Guest WFI is always trapped (`HCR_EL2.TWI`); the EL2 wait wakes at 1 kHz. Phase 3
  measurement (a) found WFI_WIPES on T8152 (a timer-woken WFI clears GPRs, NZCV and SIMD on
  E, P and M cores), so carrier idle (trapped WFI, PSCI CPU_SUSPEND) runs no WFI: it spins on
  `ISR_EL1`, bounded at 1 ms. The real-WFI opt-in (config bit 7, `wfi: wfi`,
  `--windows-wfi wfi`) is refused by the resident and the launcher. aura's own dispatch loop
  is WFE-only on T8152.
- IPIs: polling by default (1 kHz secondary tick drains queued guest IPIs and vSGIs).
  Apple Fast IPIs only with the launcher's `ipi: fast`, after Phase 3 measurement (c).
- The software GIC CPU interface (as T8142) only with `gic_cpuif: software`, after (f).
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
  (`carrier`/`software`), `wfi` (`poll` only), `on_reset` (`stay`/`reboot`).
- A resident must report capability bits 0x0800 (T8152 policy) and 0x1000 (12-live-CPU and
  CTXR_D last-page refusal) for any of this.
- Before `hv_init`, the launcher reads `P_SMP_IS_ALIVE` for cpu0-11 (read-only) and stops
  with the list of CPUs that are not live; the resident checks the same again.
- The guest ADT drops `qspi`, `i2c*`, `aop*`, `admac*`, `*audio*` and every `nvram,nor`
  node, and gets `aura,windows-nvram-hidden`.
- `run_guest.py -r -E 0 --windows-guest-ram BASE:TOP MU.fd` drives the same path on a
  Windows resident on T8152; it never stops secondaries or sleeps the machine afterwards.
