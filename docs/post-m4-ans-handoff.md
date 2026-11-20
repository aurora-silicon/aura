# Post-M4 ANS between boot stages: STOPPED

Every aura stage that uses a post-M4 ANS (the NVMe coprocessor) leaves it in one state, for the
next aura stage and for the OS: **STOPPED**. Every stage starts ANS from that state the same way.

## STOPPED

- **Power.** ANS and its storage domains stay powered. ANS code never touches the shared APCIe
  PHY, the GP domains or any root port.
- **NVMe.** The controller gets a normal shutdown (CC.SHN) when the stage wrote, then CC.EN=0.
- **RTKit.** The firmware is put to SLEEP and acknowledges it (IDLE is the fallback). Then its
  CPU is stopped, and ANS alone is reset through PMGR, with its gate held on for the reset.
- **SART.** The stage removes its own entries. iBoot's entries stay in hardware, because nothing
  loses power.

XNU starts ANS cold from this state: it sets the run bit and the firmware says HELLO. This was
measured on T8140 (macOS 26) and T8132 (macOS 27.2) through the hypervisor's cold handoff
(`nvme_cold_handoff()`, [sptm-virt-ans-cold-handoff.md](sptm-virt-ans-cold-handoff.md)).

## Which chips

| SoC | `nvme_shutdown()` | `nvme_init()` |
|---|---|---|
| T8132, T8140, T6040, T6041, T8142 | STOPPED | from iBoot's session, STOPPED, or legacy OFF (below) |
| T6050 | unchanged: suspend, idle, storage-only power-off (measured, runs 24-28) | unchanged: adopt, or warm start from off |
| T8152 (secure register layout) | unchanged: RTKit quiesced, ASC left running | unchanged |
| M1-M3 | unchanged: sleep and reset (the same shape as STOPPED) | unchanged |

## Starting ANS: the three states `nvme_init()` meets

`nvme_init()` reads PMGR's ANS power state first. It reads no ANS, NVMe or SART register on a
powered-off ANS.

1. **A session is running** (the CPU's run bit is set). Either it's iBoot's, or an older aura
   stage handed its own session on running.
   - The session is woken first, as XNU wakes iBoot's: INIT, then the handshake. This happens when
     its controller was shut down (CC.SHN set), or when it doesn't acknowledge SLEEP.
   - Then it is put to SLEEP and its CPU stopped.
   - An older stage's buffers are cleared from the SART. They are the entries in the OS's DRAM
     outside the firmware's segments.
   - ANS is reset. It is now STOPPED.
2. **STOPPED** (powered, run bit clear). The stage's SART instance protects the entries already
   there, the mailbox receive side is turned on (as macOS does before the run bit), and the normal
   RTKit boot runs: run bit, INIT, HELLO, endpoint map, the stage's buffers through the SART.
3. **Legacy OFF.** An older stage (1.0.0b13 lineage) powered ANS off and passed the SART entries
   it held as `nvme.sart=`. This is a repair path:
   - Power on the gates that depend on ANS: on T8132 that is APCIE_PHY_SW, APCIE_SYS_ST, APCIE_ST
     and ANS. On T8142 it is the storage gate table.
   - Bring the shared PHY up as `pcie_init_controller()` does, with no root port.
   - Restore iBoot's entries from `nvme.sart=`, leaving out the older stage's buffers.
   - Reset ANS. It is now STOPPED.

## What a stage tells the next one

- `nvme.ans=stopped`: this stage left ANS STOPPED. It is a hint. The next stage still reads the
  hardware, and it logs a mismatch.
- `nvme.sart=`: ANS is off, with the entries to restore. Only T6050 still produces it. Every stage
  still accepts it from older stages.
- A stage that doesn't use NVMe passes on what it got.

## Payloads

- **XNU** under the hypervisor (`run_guest_sptm.py`): cold handoff by default on every SoC that
  follows the contract. The runner drops `quiesced` from the guest's `iop-ans-nub`. `AURA_ANS_COLD=off`
  keeps XNU's warm wake of an untouched iBoot session.
- **U-Boot** (the one aura embeds) adopts only a running session: it needs the run bit and
  BOOT_STATUS OK. It can't start from STOPPED or from off. So for every payload, the stage starts
  ANS itself and hands it on running, as `boot=` already did for Linux.
- **Linux** started from the bootloader directly (the proxy's kboot) gets STOPPED. By its code,
  apple-nvme starts an ANS whose run bit is clear on every post-M4 compatible:
  - it sets RUN and runs a fresh RTKit boot, relying on the bootloader's reset;
  - on T8132 it resets ANS again itself.

  This is untested on hardware. Before, that route got ANS off plus `apple,sart-entries`, which
  Linux never reads.

## Ways this can fail (written before the code)

1. **SLEEP is never acknowledged.** A quiesced session answers nothing until it is woken. A
   firmware stopped without an acknowledged SLEEP doesn't start again from the run bit, reset or
   not (T8140).
   - Wake it first: INIT, then the handshake. Retry SLEEP once, then try IDLE.
   - If all of that fails, the CPU is stopped anyway, the log says so, and the next start fails
     loudly.
2. **The ANS reset raises an SError, or isn't taken.** An early J700 Linux-side reset assert did
   this, and an idle, auto-gated ANS refuses a plain reset.
   - Reset only after the CPU is stopped and idle.
   - Hold the real ANS gate on for the reset, as the hypervisor does (measured on T8140 and T8132).
3. **The reset takes SART entries with it.** The entries are read before the reset and written back
   if they changed. None changed on T8132.
4. **An iBoot entry is taken for an aura stage's and cleared.** The firmware then can't run.
   - A stage removes only its own buffers, by exact address (`rtkit_free()`).
   - Address classification runs only where an older stage's buffers can be left: its running
     session, the legacy `nvme.sart=` list, and the hypervisor handoff. It is measured on T8132
     and T8140, and not on T6040, T6041 or T8142.
5. **No HELLO after the run bit when starting from STOPPED.** Possible causes: the mailbox receive
   side is off, the firmware expects a warm endpoint-map reply, or a SART entry is missing.
   - The receive side is turned on before the run bit.
   - On failure the stage keeps its buffers and asks for a reboot (`nvme_reboot_required`), as
     every post-M4 setup failure does.
6. **The state is read wrong.** Reading ANS, NVMe or SART registers on a powered-off ANS hangs the
   bus. So PMGR's ANS power state is read first, and the hints (`nvme.ans=`, `nvme.sart=`) are
   checked against it.
7. **The first stage pays a second firmware boot.** iBoot's session goes through handshake, SLEEP,
   reset and a cold start. That adds time on every boot, and a new failure point before any NVMe
   read.
8. **Mixed versions.**
   - A new Stage 1 with an older Stage 2: the older Stage 2 refuses a stopped ANS ("ANS is off and
     no earlier stage passed its SART entries"). So Stage 2 must be updated first, or with Stage 1.
   - An older Stage 1 (1.0.0b13) with a new Stage 2: the legacy OFF repair. That repair is the
     least proven path (item 9).
9. **Legacy OFF: the GP side.** 1.0.0b13 took the shared PHY down while the GP domains stayed up.
   - After a full re-init, XNU read the empty port 1's window and took an LLC SLVERR (T8132, hv3
     and hv5).
   - The storage-only repair leaves the GP ports alone. Whether XNU accepts them as 1.0.0b13 left
     them is open.
   - On a Linux route, kboot's later `pcie_init()` brings the root ports up as before.
10. **Legacy OFF: no `nvme.sart=`.** This happens for an image a host chainloads from an older
    Stage 1's proxy. The firmware's own SART entry can't be rebuilt, so the stage refuses.
11. **U-Boot can't take STOPPED.** If Stage 2's start fails, no payload boots. The refusal is logged
    before the handoff, not in U-Boot.
12. **A start that never reaches IOP power ON.** `rtkit_boot()` waits for IOP ON without a time
    limit. That was already true of the first stage's handshake.
13. **The SART runs out of slots.** It has 16. iBoot uses 6 on T8132, and a stage adds 3 to 4.
14. **Chips not measured with this contract:** T6040, T6041 and T8142. The PMGR gate is named
    "ANS" there too, but their resets haven't run.

## End-to-end plan

These are the rows each change must pass on its hardware, each under that machine's own lock.
Each row produces its console logs and a summary line per boot.

- **M4 Mac mini (T8132):**
  - Stage 2 to a Linux disk boot, three times in a row.
  - The hypervisor to the macOS desktop, three times in a row.
  - Both routes with a 1.0.0b13 Stage 1 (legacy OFF), and with a new stage chained from its proxy
    (iBoot's session, then STOPPED).
- **MacBook Neo (T8140):** the same two routes, three times each.
- **M5 Pro (T6050):** regression only. The same routes, with the same log lines as before this change.

What to look for in the logs:
- `nvme: ANS entry state ...`, and the handoff line of each stage.
- `nvme: stopped (SLEEP acknowledged), ANS reset`.
- `nvme: started from STOPPED`.
- No `reboot required`.
- XNU: `RTBuddy(ANS2): start`, `Found the ANS2Endpoint1`, the desktop, and no LLC SLVERR.
- Linux: `nvme nvme0: ... adopted` (U-Boot route), or a fresh RTKit boot on the kboot route.
