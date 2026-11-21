# Boot handoff contract

What each aura stage receives from the one before it, and what it hands on. Every Mac boots
iBoot → Stage 1 → Stage 2 → the OS (Linux through kboot, or U-Boot). A Stage 1 or Stage 2 can
also run a one-shot `aura-try` image in place of the configured Stage 2, or stop in the proxy.

## Rules

- **One owner per block.** A block is either left exactly as iBoot handed it, or brought up by
  one stage. That stage hands it on in one documented state (the table below). A later stage
  never guesses the state from the chip: it reads it from the hardware or from what the earlier
  stage passed on.
- **Selectors.** Behaviour is chosen by what the hardware or firmware presents: an ADT node or
  property and its value, a device's own compatible, a firmware version or endpoint, or register
  state. A table keyed by a device compatible is a device match. A board list or a chip-id
  switch is not.
- **Fail closed only where continuing is unsafe**: secure boot, memory protection (carveouts,
  CTRR, TZ), or handing a block over in an unknown state. Optional setup that meets an ADT it
  does not expect (a binned part, a missing optional node, other counts) logs and leaves the
  block as iBoot set it up. An ADT that contradicts itself fails.
- **No runtime A/B switches** in shipped code. A `chosen.*` line that selects behaviour is a
  bring-up aid, and it goes once the behaviour is the default.

## Stage 1 → Stage 2 (chainload.c)

Stage 1 loads the next image from the ESP and enters it with the boot arguments it was given.
Additions:

| What | Where | Meaning |
|---|---|---|
| `chosen.aurora,stage1-log=0x…` | config var | Stage 1's console log; kboot exports it as the `aura_stage1.log` MTD |
| `nvme.sart=…` | config var | post-M4 ANS was powered off: the SART entries the next stage starts it with |
| `aura-trial`, `chosen.aurora,trial=<path>` | boot args, config var | this boot runs a one-shot trial image |
| `aura.host-window=done` | config var | DEBUGUSB_PROXY builds: the Stage 1 host window was offered; the next stage does not offer it again |
| `aura-console` | boot args | a DebugUSB host is reading the console; the OS keeps it |
| `top_of_kernel_data` | boot args | the previous stage's live heap high-water: memory that a block handed on live may still use |

An older Stage 1 passes what its version knew: 1.0.0b9 passed `nvme.adopt=live-rtkit-v1` for a
live ANS session. Later stages take it only as a sign that an aura stage ran before them
(`payload.c`) and find the running session from the controller registers (`nvme_init()`). Which
installed Stage 1 versions may chainload which images is not checked or recorded today (open
question 1).

## Per block

| Block | Owner | Handed to the next aura stage | Handed to the OS | Selector today |
|---|---|---|---|---|
| ANS / NVMe | the stage that reads the ESP | M1–M3: shut down, IOP asleep, reset. Post-M4 (`nvme-secure-bar` or `nvme-secure-reg-layout` in the ADT): per chip (see below) | M1–M3: shut down. Post-M4: left running for Linux (`boot=`) and the embedded U-Boot, started first if an earlier stage powered it off; otherwise as found, or off with `apple,sart-entries` | chip id (below) |
| DCP / display | iBoot | left running; framebuffer kept | reserved firmware and mappings, per SoC profile (`kboot_display_handoff.c`) or T8140/T8132 code | root compatible + OS firmware version |
| Display DARTs | iBoot | locked tables kept | inherited tables reserved | DART state |
| PCIe | first stage that needs it | link up, as trained | link state in the DT; T6050 GP ports adopted as trained | chip id / ADT |
| MCC | iBoot (cache), aura (M1–M5 cache enable, TZ) | unchanged | TZ carveouts unmapped from aura's map | `mcc,*` compatible + ADT counts |
| CPUs | iBoot / Stage 1 | secondaries parked as `smp.c` leaves them (spin table, RVBAR) | spin table; S2R mailbox where RVBAR is locked | chip id (CPU start offsets, T8142 spin block, T8152 RVBAR) |
| USB | the stage that runs the proxy | CDC shut down, DMA released or reserved | controllers as Linux expects; retained DMA reserved | ADT + T8140/T8142/T8152 gates |
| PMU panic counter | every stage | cleared | cleared | ADT `info-leg_scrpad` |
| SEP, AOP, ISP, SIO, AVE | iBoot | unchanged | firmware segments reserved, tables passed | ADT nodes + some board gates (#293, #310, #418) |

### ANS today

Post-M4 ANS is handed between aura stages in one of four ways, chosen by chip id in
`src/nvme.c` (`nvme_post_m4_keeps_live()`, `nvme_ans_storage_gated()`, `nvme_will_hand_on_live()`):

- T8132, T8140: controller disabled, firmware idled, ANS and its PCIe block powered off; SART
  entries passed on (`nvme.sart=`); the next stage starts it from off.
- T6040, T6041: handed on running, I/O queues cleared; the next stage adopts it. A cold start
  after power-off gets no RTKit HELLO there.
- T6050, T8142: powered through the storage gates (`pcie_ans_ps()`), adopted when healthy.
- T8152 (`nvme-secure-reg-layout`): controller disabled, RTKit quiesced, ASC left running.

Each variant fixed a measured failure. One stopped-ANS handoff for every post-M4 chip is aura#380
(draft); until it has hardware rows on every family, the variants stay.

## Open questions

1. **Version skew.** Nothing states, checks or records which installed Stage 1 versions can
   chainload which Stage 1 or Stage 2 images (1.0.0b9 → 1.0.0b14 resets on the J616C were an MCC
   panic in the new image, not a contract break, but nothing told anyone that).
2. **A failed trial leaves no record.** An `aura-try` image that resets early loses its log: only
   the installed Stage 1's `chosen.aurora,stage1-log` survives. The next boot cannot tell that a
   trial ran or how it ended.
