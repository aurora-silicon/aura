# Virtualized SPTM under aura (T8140, T8132)

aura can run macOS's own stock SPTM, TXM, the cL4 secure kernel with its exclaves, and XNU as a
FEAT_NV2 guest at virtual EL2, with their guarded world (GXF and SPRR) running natively one
level below aura's own GL2. macOS boots to the desktop this way on all six cores of a J700, and
it suspends to RAM and wakes as it does natively. It is a development tool: aura sees every
power-management access macOS makes, which is how the sleep sequence for Linux was recorded.

It is built only with `SPTM_VIRT=1` (which needs `CHAINLOADING=1`) and only does anything on
T8140 or T8132 booted as a kcbz=0 Mach-O boot object (`docs/macho-boot-object.md`): iBoot then skips its
own SPTM loader, leaves GXF and SPRR writable, and jumps to aura at EL2. aura's NV2 support is
built only with `SPTM_VIRT=1` as well: other images, the Linux boot included, carry none of it.
The host side is `proxyclient/tools/run_guest_sptm.py`.

The boot object has to be enrolled on the volume group of the macOS it boots. iBoot brings the SEP
up for the volume group it booted: from another one (a Linux stub's), XNU mounts the sealed root
only with that macOS's `/chosen` values, and then panics in `SEPEpoch::getFullEpochs` when the SEP
does not answer for its xART.

### T8132 (J713, MacBook Air M4)

25G83 boots the same way on a J713 to a working desktop, on all ten cores. Its build manifest has no
`Ap,ExclaveOS` or `Ap,cL4`, so the guest is SPTM, TXM and XNU only (no `AURA_KEEP_EXCLAVES`).
TXM and the static trust cache are the J700's files; the kernel is `kernelcache.release.mac16g`.
`sptm.t8132.release` is the T8140 build with `init_pre_late_io_reservation` 0xc4 later, so the
host shifts only the four AMCC patches. The debug port is hpm0 behind `usb-drd0`/`dart-usb0`/
`atc-phy0`, which also carries Thunderbolt (`acio0`, `apciec0`), and all of it is hidden from
XNU. The sleep path (`sptm_virt_s2r.c`) stays T8140-only: its S2R flags are ignored on T8132.

The code:

| File | What |
|---|---|
| `src/sptm_virt.c` | carve-out layout, host entry points, guest entry, the reset path |
| `src/sptm_virt_gl.c` | SPTM's traps into aura's GL2: the GL2 register shadow, CTRR/CTXR, guarded ERET and GEXIT, AMX |
| `src/sptm_virt_cl4.c` | the cL4 world switch |
| `src/sptm_virt_s2.c` | the stage 2 aura gives XNU: vuart, reset guard, aura's own memory unmapped |
| `src/sptm_virt_s2r.c` | suspend to RAM and its trace |
| `src/nv2.c`, `src/nv2_asm.S` | FEAT_NV2: the guest at virtual EL2, its EL2 setup and entry, the lower-EL ERET gate (`_v_lower_sync`); `nv2_guarded` for a guest with its own GXF/SPRR world |
| `proxyclient/aura/sptm_virt_plan.py` | the iBoot-to-SPTM handoff: blob layout, device tree, boot_args |
| `proxyclient/aura/exclave_bundle.py` | the ExclaveCore bundle's cL4 regions |

## Why NV2, and who owns GXF

On SPTM-class SoCs the secure monitor runs in Apple's guarded levels, entered with GENTER and left with
GEXIT, and polices page tables with SPRR and CTRR. In kcbz=0 mode iBoot leaves the secondary
cores' CTXR windows locked, so at EL2 a secondary can only execute inside the boot object: aura
has to stay resident at EL2 and run everything else at EL1. To host SPTM, which believes it runs
at EL2, aura uses FEAT_NV2: the guest runs at physical EL1 with `HCR_EL2.NV|NV2`, its EL2 register
accesses go to a per-CPU VNCR page, and its ERETs trap.

SPTM's `__start` writes GL-only SPRR registers that UNDEF at non-guarded EL1, and HACR cannot trap
them. So aura brings up its own GL2 (SPRR, then GXF; the permission maps are loaded at boot so
aura's own pages stay accessible) before every entry, and SPTM's GENTER from virtual EL2 lands in
GL1. An SPTM_VIRT build sets `mmu_sprr` on M4 for this. `apple_sysregs_unlocked` stays false
there: it gates CYC_OVRD, which is locked, and nothing on this path touches CYC_OVRD. What SPTM
then does that needs GL2 traps to aura's GL2 (`sptm_virt_gl_sync`):

- GL2 register accesses: kept per CPU in a shadow (`sptm_virt_gl2_slot`), so reads see earlier
  writes. AMX_CTL_EL2 is applied, since aura is the real EL2.
- CTRR/CTXR registers: the real value is read and writes are dropped, except the bounds SPTM
  checks against what it stashed for its own text, which answer with SPTM's expected values
  (stage 1 locked the hardware for aura).
- Its guarded ERET: the target is in the guest's GL1 bank, reachable from GL2 as `_GL12`.
- Its GEXIT to its own normal world (virtual EL2), which physical GL1 cannot reach.

The host also patches five checks in SPTM's text that compare the locked hardware against SPTM's
own placement (`SPTM_PATCHES` in run_guest_sptm.py). The patch offsets, the CTRR stash layout and
the GEXIT offset that marks XNU's entry are specific to the 25G83 SPTM; the host refuses an SPTM
whose instructions don't match.

## Memory

- aura's image stays at stage 1's base, the only place a secondary may execute after a reset.
  iBoot puts the display coprocessors' `__OS_LOG` buffers right after it, so aura's `.bss` and
  stacks must end below them; the host refuses to enter otherwise. The large tables (the GL2
  shadow, the trace ring) live on the heap.
- aura's heap, per-CPU pages and stage-2 tables live in a 512 MiB carve-out at the top of the
  managed window, below the fixed high-firmware regions (SAPT, the GPU handoff). SPTM's
  `region-id-24` is shortened to end below it and the carve-out is declared as a reserved region
  of its own (`sptm_virt_compute_layout`, unit-tested).
- SPTM's window starts above everything aura and iBoot left low (`sptm_virt_low_floor`). This is
  the native layout: physBase is the window's base, the images follow a 32 MiB phys slide that
  XNU frees to its VM, and iBoot's data regions (RTBuddySeg, SEPFW, SEPPatches, uStuff, preoslog)
  are copied to the end of the blob, with the coprocessors' `__OS_LOG` ranges pointed into the
  RTBuddySeg copy, so nothing SPTM walks lies below physBase.
- SPTM's window stays mapped in aura's EL2 map, which the CTRR answers read.

## The handoff

SPTM's `__start` takes, in x0, the physical address of an XNU boot_args (revision 2, version 2,
0x480 bytes plus 8 readable zero bytes): virtBase +8, physBase +0x10, memSize +0x18, the device
tree's VA and length at +0x60/+0x68, the command line at +0x6c. It is entered with its MMU off.
Everything else is in the device tree and one contiguous blob in iBoot's order, each piece
recorded under `/chosen/memory-map`: TXM, the trust cache, the kernelcache, the cL4 groups, the
device tree, SPTM, the writable and linkedit groups, BootArgs, the exclave catalogs. The planner
(`plan_boot`) builds both, and also:

- keeps TXM off physBase and sets SPTM's offset from the kernelcache's low 36 virtual bits (SPTM
  places TXM and the kernelcache one and two `0x10000000` strides below itself);
- adds `/chosen/hibernation` (SPTM requires non-zero CTRR keys there), `/chosen/exclave-memory-map`
  and `/chosen/exclave-misc`;
- lays out cL4 from the ExclaveCore bundle, with the read-only tail placed twice: after CL4-rx for
  SPTM, and after CL4-le as the data window cL4 itself reads;
- hides the debug link's nodes (DockChannel, the ATC0 port's PD controller, USB controller and
  PHY), the ANE, and without exclaves their display nodes; and moves XNU's console to uart0.

## Cores

Every core enters the guest through a hardware reset, as natively: XNU starts a core with PMGR
CPUSTART, and a core that power-gates in idle or sleep comes back the same way. Its locked RVBAR
lands in aura (`_cpu_reset_c`, MMU off, on a stack published per MPIDR), which rebuilds its GL2 and
EL2 state and enters SPTM's reset vector at virtual EL2 (`sptm_virt_cpu_reset`). SPTM decides cold
start or resume itself. Until a core's first GEXIT into XNU, its WFEs trap: SPTM's panic halt is
`wfe; b .`, which nothing would ever end under aura, so aura reports it once and steps over.

## cL4

cL4 runs as SPTM's guarded child, entered by SPTM's guarded ERET to EL1 and left by any exception
it takes to the level above, which aura delivers to SPTM's GL vector as the hardware would. Both
EL1 children (cL4, and XNU's own VMs) share the one physical EL1 register file with the host, so
aura switches Apple's per-level registers with the world (APCTL, GXF and SPRR configuration, the
PAC keys, VMSA_LOCK, AGTCNTRDIR, AMX_CONFIG): see the comment at the top of `sptm_virt_cl4.c`. The
child's view survives a warm reset; reseeding it there broke cL4's PAC after a sleep.

## XNU's stage 2

Each core arms it at its first GEXIT into XNU, after checking that the tree is unchanged: an
identity map of the low 2 TiB with a few pages trapped. uart0 is aura's vuart, mirrored to the
proxy's iodev. The SoC watchdogs are guarded, so a restart parks the core in aura's proxy instead
of resetting the Mac. aura's image and carve-out are unmapped, so a stray guest access stops in
aura with the guest's frame chain. Blocks over the CTRR-class ranges are mapped with pages (those
ranges refuse a block translation that overlaps them).

## Suspend to RAM

With the proxy on CDC, aura keeps the debug link's power domains on (XNU lowers them about a second
into its boot). With `SPTM_VIRT_S2R`, pass-through hooks on the power-management blocks record
every access into an 8 MiB ring in the carve-out, and two of them time the sleep: each core's
self-disable in pmgr-soc-misc, and the boot core starting the S2R timer in pmgr-aon-pst, the
commit (`0x3082d4000 <- 1`).

- **Stop mode** stops in the proxy before the commit, with the whole entry in the ring.
- **Go mode** lets the sleep happen. What it takes, each rule learned from a failed attempt:
  - Let macOS program its sleep-time reconfig watchdog (about 5 s, reset enabled); the reset
    guard passes it through, still catching a real restart.
  - Print nothing once the first core disables itself: the boot core's last steps are short, and
    console output there delays them past that watchdog.
  - Move the console, the proxy and XNU's vuart to the always-on DockChannel first: uart0 and the
    CDC link are SoC-domain devices that are off at the wake.
  - At the commit, hand the link's domains the states XNU asked for and power the ATC0 port off
    (with `SPTM_VIRT_S2R_DROP_USB` after tearing the gadget down), or its USB wake source wakes the
    SoC halfway.
  - Run the exclaves: without them DCP never reaches its sleep power state.
- **The wake** is the first core reset after the commit: aura marks it, logs the first guest traps
  and hooked accesses, and with the full trace prints the wake's MMIO record once XNU stops the
  watchdog. `AURA_S2R_USB_RETAKE` takes the CDC link again on the running macOS, as a coroutine
  that runs in slices between XNU's traps. With the link dropped, what aura prints at the wake
  waits in the 8 KiB console buffer, which the wake's trail overruns, so once the link is up
  aura prints the `WAKE #N` line again.

What it found for a Linux port:

- macOS's entry, in order: secondaries disable themselves (`0x300734000`, a bit per core); WD2 is
  armed for 5 s; the CPU complexes are armed off (`0x300780008/0/4 <- 0`); the power gates, CLLT
  (`0x300768030/34/3c`) and clock muxes are parked; the commit; then the PMU RTC alarm over SPMI
  (`0xf80c`, `0xf808`, `0xf80e`, `0xf800 <- 0x41`), "go hib", and each core's deep WFI (bits 63
  and 0 of `S3_5_C15_C6_2`).
- With macOS's platform error handler hidden, as Linux has none, an enabled and pending
  memory-controller error at the commit wakes the SoC within 20 ms. Clearing AMCC error-enable word
  0 bit 25 (DSID_AGE_OUT) and word 1 bit 8 (UNEXP_RT_HIT_DIR) on all four planes before the commit
  (`AURA_S2R_QUIESCE=24`) stops it. Display power-off raises DSID_AGE_OUT.
- The wake is 83 writes on one core in about 18 ms, the same on an RTC and a Wi-Fi wake: both
  clusters and the power gates on, 22 domains to their targets with AUTO_ENABLE, unused domains
  lowered, WD2 stopped last. DCP, DCPEXT, ANS, SIO, PMP, GFX and GFX1 restart their firmware (RTKit
  HELLO); no coprocessor exchanges a message between the commit and the wake.

## Running it

```
make CHAINLOADING=1 SPTM_VIRT=1
python3 proxyclient/tools/chainload.py -r build/aura.bin
PYTHONPATH=proxyclient python3 proxyclient/tools/run_guest_sptm.py \
    --sptm sptm.bin --txm txm.bin --xnu kernelcache.decompressed --trustcache iboot.tc
```

`--dry-run` prints the plan and writes nothing. The desktop needs `AURA_KEEP_EXCLAVES=1` with
`AURA_EXCLAVE_FW` pointing at the extracted exclave firmware, `AURA_XNU_SERIAL=uart0` and
`AURA_HIDE_DOCKCHANNEL=1`. A sleep test adds `AURA_S2R_TRACE=1 AURA_S2R_GO=1 AURA_S2R_DROP_USB=1`,
and `AURA_KEEP_ERRH=1 AURA_AMCC_CLEAR=1` (or `AURA_S2R_QUIESCE=24` with the handler hidden). The
script's docstring lists every switch.
