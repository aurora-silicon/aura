# macOS with SPTM under the classic hypervisor (M1 series)

macOS 27 boots M1-series Macs through SPTM and TXM too. On M4 and later aura runs those as a FEAT_NV2
guest at virtual EL2 (`docs/sptm-nv2-virt.md`). The M1 series has no NV2, but aura runs there at EL2
with unlocked Apple system registers, and its classic hypervisor can run SPTM, TXM and XNU one
level down: XNU at EL1, SPTM's guarded world in GL1 and TXM in GL0, with aura keeping EL2 and
GL2. That is how the hypervisor ran PPL. GENTER, GEXIT and SPTM's guarded ERETs then run in
hardware. Every MMIO access, sysreg trap and tracer the classic hypervisor offers works on the
guest as on older macOS.

macOS 27.2.1 (26B131) boots this way on a J313 (M1 MacBook Air) to the login window, on all eight
cores, from its own volume group's sealed root.

## What is rewritten

SPTM's bootstrap already checks CurrentEL and builds the SPSRs it returns with from it, so most
of it runs at EL1 as it is. What is left are accesses to registers that do not exist one level
down. Without NV2 such an access at EL1 is UNDEFINED and is taken at EL1, so the hypervisor never
sees it. `proxyclient/aura/sptm_el1_patch.py` rewrites them in the executable segments before the
upload, by instruction encoding:

| Access | Becomes | Why |
|---|---|---|
| `ELR`/`SPSR`/`ESR`/`FAR`/`AFSR1_EL2`, `CPTR_EL2` | the `_EL1` encoding (`CPACR_EL1`) | VHE redirects these, so natively both encodings reach the same register |
| `TPIDR_EL2` | `TPIDR_EL1` | SPTM's per-CPU pointer outside its guarded world, read only before XNU owns TPIDR_EL1 |
| `TPIDR_GL2` | `TPIDR_GL1` | the same pointer inside it; at GL1 that is SPTM's own bank |
| `HCR_EL2` | reads 0, writes ignored | SPTM asks it whether a VM took an exception; none runs |
| stage 2, GIC virtual interface, counter offset, `_EL12` aliases (also Apple's) | reads 0, writes ignored | used only to run VMs or to configure the level below from EL2 |
| two PMU counter configuration registers SPTM's bootstrap writes, one XNU's PMU setup writes | writes ignored | UNDEFINED at EL1 |

XNU checks CurrentEL before its own EL2 registers, so only that one PMU write is rewritten in it.
TXM uses no EL2 or GL2 register. Apple's own EL2 and GL2 registers (the CTRR ranges, AMX) are
left alone: from EL1 and GL1 they trap to the hypervisor, which shadows them. SPTM's VM context
switch still reads and writes `SP_EL1`; nothing starts a VM, so it never runs. SPTM's checks of
the hardware against its placement are patched as on M4 (`sptm_patch.py`); T8103's SPTM has only
the APSTS poll.

SPTM's early exception vector records an exception in its diagnostic record (boot_args revision
3) and halts in WFE, which loses the registers. Its 16 entries become HVCs, so the hypervisor
stops with the guest's state intact. With `--auto-fix` an UNDEFINED MRS or MSR found there is
rewritten in guest RAM, logged and stepped over: a bring-up aid, whose findings belong in
`sptm_el1_patch.py`.

## Running it

The Mac boots aura from the macOS volume group it is to boot (enrolled with `kmutil
configure-boot`), as for the NV2 path. Put an `aura-proxy` file at the ESP root and reboot; Stage 1
waits 15 s for a host. Then:

```
PYTHONPATH=proxyclient python3 proxyclient/tools/run_guest_sptm_hv.py --wait \
    --sptm sptm.bin --txm txm.bin --xnu kernelcache.macho --trustcache static.trustcache \
    --vuart-log guest.log
```

The images come from the Mac's own Preboot volume, decompressed: `sptm.<soc>.release.im4p`,
`txm.macosx.release.im4p`, the kernelcache and the static trust cache. The script patches the
images before it waits, so it is ready when the window opens. The guest's serial console is the
hypervisor's virtual UART, which is the proxy's second USB serial port (`--vuart-log`). Interrupt
the guest with SIGINT to the script. `diag()` and `panic()` print SPTM's diagnostic record and
panic text, and tracers load with `-m` as for any classic guest.

When the script exits, macOS keeps running under the hypervisor. To stop it, open the proxy port
again, send `!` and reboot through the proxy.
