# ANS for a macOS guest: the cold handoff (T8140)

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

Both reached the desktop.

`AURA_ANS_COLD` selects the mode:
- `sleep`, the default on T8140 and for an ANS an earlier stage powered off (below);
- `idle`, a diagnostic;
- `off`, the old handoff and the default elsewhere.

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

## An ANS an earlier stage powered off (post-M4)

From 1.0.0b13, a post-M4 stage that read NVMe powers ANS off when it is done
(`nvme_ans_power_off()`). On T8132 Stage 1 does that after its ESP read, also when the file it
looked for is missing and it falls back to its proxy. Then there is no session for XNU to wake,
and the SART lost its entries with the power, iBoot's included. They survive only in that stage,
which passes them on as `nvme.sart=` to an image it chainloads itself. Not tested on hardware yet.

- `run_guest_sptm.py` reads the state first (`sptm_virt_ans_state()`). With ANS off, the cold
  handoff is the default, `AURA_ANS_COLD=off` is refused, and so is a boot without the entries.
- The handoff powers ANS on again with its CPU stopped (`pcie_ans_power_on()`, as the next stage
  of a Linux boot does), puts the entries back, and goes on as for a stopped ANS. It clears the
  stage's own entries, resets ANS and probes the HELLO.
- An image the host chainloads from that stage's proxy has no entries. Before the chainload,
  `proxyclient/tools/ans_sart.py --start` has the stage start ANS again, reads its SART and prints
  the `nvme.sart=` value; `AURA_ANS_SART` passes it on. That start is the stage's own start from
  off. If it fails, the stage refuses to chainload anything (its NVMe needs a reboot).
