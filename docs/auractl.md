# auractl

auractl is a small paravirtual control interface between aura and the OS it runs as its
SPTM-virt guest. From inside the guest, a privileged process can list the MMIO ranges aura can
trace, arm and disarm aura's stage-2 trace hooks on them, start, stop and clear aura's trace
ring, read the ring back, write markers into it, read a few plain power-manager registers, and
read the tail of aura's console. Without it, all of this needs the debug cable and the proxy on
the host.

It exists only in builds made with `AURACTL=1`, which requires `SPTM_VIRT=1`, so it is never in
Stage 1. Hooks and the ring exist only when the boot's trace flags include `SPTM_VIRT_S2R`;
`auractl status` says whether this boot has a ring.

auractl does not change the guest's kernel. A guest reaches the call through an *entry*: a small
guest-side piece that carries a request to aura. This tree ships two for macOS: the `AuractlDevice`
IOKit kext, which issues the hvc from the kernel, and a kext-free one that reaches aura through an
EL0 debug register (`ENTRY=dcc`, below). Both drive the same ABI and the same handler.

## The call

The ABI is in `src/auractl_abi.h`, shared by aura and the tool. The guest's kernel, at virtual
EL2, issues

    w0 = 0x41435431 ('ACT1')
    x1 = the calling process's address of its struct auractl_req
    hvc #0xac71

and gets 0 or a Darwin errno back in x0. Any other hvc, or this immediate without the magic, goes
back to the guest as before. aura's handler is `src/sptm_virt_ctl.c`, dispatched from
`nv2_handle_sync` in `src/nv2.c`.

aura reads the request block and every buffer it names through the calling process's own EL0
translation (AT S1E0R/S1E0W), as that process itself would. It touches only Normal memory inside
the RAM SPTM manages, never aura's own image, through a private one-page window that it unmaps
and invalidates before it returns. A call moves at most 256 KiB, and one ARM or DISARM of a name
or a range changes at most 64 hooks. Calls are serialized.

Which of its processes may make the call is the guest's decision.

## Transports

The call reaches aura two ways, both into the same handler:

- **hvc** (`ENTRY=iokit`): the `AuractlDevice` kext issues `hvc #0xac71` on the calling thread, so
  the request is read in that process's EL0 context. It needs the kext loaded, which needs root
  and SIP off — and a loadable kext, which under SPTM-virt is not a given (below).
- **DCC** (`ENTRY=dcc`): a plain, unprivileged process writes its request's own address, tagged in
  the top 16 bits (a macOS user VA is 47-bit), to the debug-communications-channel register
  `DBGDTR_EL0`. aura traps every EL0/EL1 DCC access to EL2 with `MDCR_EL2.TDCC` (`src/nv2.c`), sees
  the tag, and reads and runs the request exactly as the hvc path does; the errno is read back from
  `DBGDTR_EL0`. No kext, no entitlement, no root: `DBGDTR_EL0` is an architectural EL0 register.
  For the EL0 trap to reach aura rather than the guest's own EL1, aura also forces the hardware
  `MDSCR_EL1.TDCC` clear (the guest's `MDSCR_EL1` is VNCR-banked, so what it reads back is
  unchanged). This is the transport to use when no kext can load.

## Hooks

A hook is a pass-through stage-2 trap, the same as the sleep-path trace's own
(`src/sptm_virt_s2r.c`): every guest access to the page still goes to the hardware, and aura
appends a record of it to the ring. aura hooks a page only if all of the following hold:

- it is MMIO, below DRAM;
- it is not the interrupt controller's or the DockChannel's, and not on the chip's never-read
  list;
- its 32 MiB stage-2 block was split when aura set up the guest's stage 2, because a catalogue
  range lives there. Changing one 16 KiB leaf of an already-split block is safe with the guest
  running on other cores. Replacing a whole block is not, so a page in a block that was never
  split is refused;
- it is a plain hardware page, not one any other aura hook or guard owns.

After a change aura invalidates the guest's stage-2 TLBs and records the new tree as the one each
core checks when it next enters the guest's kernel. That check and the change take the same lock.

Pages the trace setup hooked are listed as hooked. auractl cannot release them.

## The catalogue

`auractl list` shows the named ranges, all taken from this machine's ADT:

- the power manager's blocks that the sleep path found (`src/sptm_virt_s2r_soc.c`): `pmgr-soc-ps`,
  `pmgr-pwrgate`, `pmgr-soc-misc`, `pmgr-cllt`, `pmgr-cluster-ctl`, `pmgr-aon`, `pmgr-aon-pst`,
  `nub-ap-ctrl`, `clkgen`, `pms-pmc`, `amcc`;
- on a chip whose sleep is the S2R timer's start alone, its PS groups, the cores' windows and the
  timer's page;
- the coprocessors' mailboxes (`<name>-mbox`, the ASC's page at +0x8000), `gpio0`, `spmi0`,
  `spmi-a0` and `dart-dcp`.

A block this ADT lacks is listed as absent. Arming a name arms every range of that name.

## PEEK

`auractl peek` reads up to 64 32-bit words from one `pmgr-*`, `clkgen` or `nub-ap-ctrl` range
whose pages aura would hook. It never reads a mailbox, a FIFO, the PMS or the AMCC. Some words
fault the SoC when read, so PEEK works only on a chip whose never-read list aura carries: T8140
for now. Any chip without one, the T6040/T6041 (J616s) included, returns `EACCES`: PEEK is not
enabled yet there. Enabling it on a chip means confirming its never-read list from our own
measured ADT (same way T8140's was built, no Apple firmware bytes) and adding it to
`src/sptm_virt_ctl.c`. The other commands do not depend on PEEK and work everywhere.

## Build

    make CHAINLOADING=1 SPTM_VIRT=1 AURACTL=1      # aura
    make -C tools/auractl ENTRY=dcc                # the tool, kext-free (no root, no SIP change)
    make -C tools/auractl ENTRY=iokit              # the tool, through the kext
    make -C tools/auractl kext                     # AuractlDevice.kext (needs Xcode's macOS SDK)

Run `make clean` between flag sets.

The tool hands each request to the guest's kernel through the entry it is built with
(`tools/auractl/entry.h`, `make ENTRY=<name>` for `entry_<name>.c`). `ENTRY=iokit` goes through
the `AuractlDevice` kext this tree ships (`tools/auractl/AuractlDevice`, built by `make kext`).
`ENTRY=none`, the default, refuses every call, so a tool built with it can only decode saved
files with `auractl print`. See `tools/auractl/README.md` for signing and loading the kext.

Loading the kext needs SIP off and root. Note that under SPTM-virt `kmutil load` cannot build an
AuxKC: the BootKC is fed by the host loader, not by the booted macOS, so `kmutil` has no writable
boot kext collection to extend (measured on the J616s). `kextload AuractlDevice.kext` fails the same
way, and SIP may be on regardless. When the kext cannot load, use `ENTRY=dcc`: it needs none of
this. `tools/auractl/README.md` has the details.

## Use

    sudo auractl status
    sudo auractl list
    sudo auractl arm pmgr-soc-ps           # a catalogue name
    sudo auractl arm 0x<base>+0x4000       # or a physical range in a split block
    sudo auractl mark before-sleep 1
    sudo auractl read /tmp/t.trc           # everything in the ring, as of now
    auractl print /tmp/t.trc
    sudo auractl peek 0x<pmgr address> 4
    sudo auractl disarm                    # everything auractl armed
    sudo auractl console                   # the tail of aura's console

`read` writes a file: a header (`AURATRC1`, the record size, the counter frequency, the first
sequence number, the record count and aura's version), then the records as aura stores them.

## Tests

    python3 tests/python/test_auractl_ctl_host.py   # or under pytest

This runs the handler against a fake guest (`tests/ctl_host`). It needs a host with 16 KiB pages:
an Apple Silicon Mac, or Linux on one. Guest RAM is a file, and aura's window is a real mapping
of that file, so a copy past what the handler mapped is a real fault. The SoC is a synthetic
layout. The mutation test removes each safety check in turn, and the harness must fail every
time.
