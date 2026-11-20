# ANS for a macOS guest: the cold handoff (post-M4)

On a native boot iBoot hands ANS to XNU running, with its RTKit session quiesced, and XNU wakes
that session with one INIT. Under aura, a stage that read NVMe first (Stage 1 for the ESP's
`aura-proxy` marker, for one) has already changed that state, and XNU's warm start fails.
`sptm_virt_ans_cold()`, which `run_guest_sptm.py` calls before it builds the guest, ends whatever
session is there and stops ANS so that XNU starts it cold. The runner then drops `quiesced`
from `/arm-io/ans/iop-ans-nub`. XNU then maps the iBoot-loaded firmware and sets the run bit
itself, as after its own sleep.

Measured on the MacBook Neo (J700), macOS 26 (25G83), on both routes:
- through a blessed Aurora stub;
- through a one-shot bless of the macOS volume.

Both reached the desktop. On the M4 Mac mini (J773g, macOS 27.2) XNU started ANS cold from this
state too (`RTBuddy(ANS2): start`, its endpoint found).

The state this handoff leaves, STOPPED, is the one every aura stage now leaves a post-M4 ANS in
([post-m4-ans-handoff.md](post-m4-ans-handoff.md)). After such a stage the handoff only checks
it: nothing is running, no aura SART entry is left, and ANS is reset and probed.

`AURA_ANS_COLD` selects the mode:
- `sleep`, the default on every SoC whose stages leave ANS STOPPED (T8132, T6040, T6041, T8140,
  T8142);
- `idle`, a diagnostic;
- `off`, XNU's warm wake of iBoot's quiesced session, which only an ANS no stage touched still
  has; the default elsewhere (T6050).

`AURA_ANS_PROBE=0` skips the check below.

## The four traps

1. **A live session can't be taken over.** Stage 1 (up to 1.0.0b8) leaves ANS running with the
   RTKit session and log buffers it negotiated. XNU's INIT to that session gets log messages for
   endpoints it never started, and XNU panics in RTBuddy(ANS2). The firmware has to be put to
   SLEEP, and its CPU stopped, before XNU.
2. **A quiesced session answers nothing until it's woken.** When an earlier stage shut the NVMe
   controller down (CC.SHN normal), its session was quiesced with it, and an RTKit SLEEP times
   out. A firmware stopped without an acknowledged sleep doesn't start again from the run bit,
   reset or not. So such a session is woken first, as XNU does: INIT, the handshake and its
   buffers through the SART. Only then is it put to sleep.
3. **ANS's ADT power gate isn't ANS's.** `/arm-io/ans`'s gate is the virtual `APCIE_SYS_ST-V`,
   which sits idle under PMGR auto-enable and can't be gated or reset. The real `ANS` gate is
   listed under `/arm-io/apcie`. Gating it would also wipe the SART, and XNU's SPTM registers the
   entries live at its first mapper activation as the bootloader's, so iBoot's would be lost to
   XNU. ANS is reset instead: the real gate is held on for the reset, then left as iBoot set it.
   ANS's CPU status reads 0x6a right after a reset.
4. **Not every SART entry is iBoot's firmware.** iBoot's entries include an MMIO page and a
   carve-out above the OS's DRAM. Only entries inside the OS's DRAM (boot_args) and outside the
   firmware's segments are an aura stage's buffers. Only those are cleared, and any entry the
   reset takes is written back.

Before XNU, the handoff starts ANS once and waits for its HELLO, then stops and resets it again.
Without a HELLO it fails, and the host stops before XNU rather than letting XNU panic 20 s in.

## An ANS an older stage powered off

Stages from 1.0.0b13 until the STOPPED handoff powered a post-M4 ANS off with the whole APCIe
block once they had read the disk, and passed the SART entries they held as `nvme.sart=` to an
image they chainloaded themselves. The SART loses its entries with ANS's power, iBoot's included.

- The handoff powers back only the gates that depend on ANS: on T8132 and T8140 these are ANS,
  APCIE_ST, APCIE_SYS_ST and APCIE_PHY_SW.
- It brings the shared PHY up as `pcie_init()` does, and touches no root port.
- It puts iBoot's entries back from `nvme.sart=`, leaving out the older stage's own buffers.
- It then goes on as for a stopped ANS.

On T8132, XNU read the empty port 1's window after a full re-init that included the root ports,
and took an LLC SLVERR. Whether XNU takes the GP side as the older stage left it is still to be
seen on hardware. An image a host chainloads from such a stage's proxy has no `nvme.sart=`, and
the handoff refuses it.
