# HvXfer on Aura's Windows personality

This is a port of m1n1 `j813`'s HvXfer bulk host<->guest channel to
`src/hv/windows`. It costs one VM exit per buffer, not one per byte. The guest
writes a 32-bit command to a trapped 16 KiB doorbell. EL2 raises one
`HV_XFER` proxy event, and the tethered host moves the whole buffer with
REQ_MEMWRITE/REQ_MEMREAD. The guest ABI is unchanged: the parity check against
`AppleHvXferCore.h` passes. Nothing is mapped unless a profile asks for it.

No hardware was touched for any of this. Nothing is committed (base `fc1c198a`).

## Files

| File | Purpose |
|---|---|
| `src/hv/windows/hv_xfer.h` | Register/command/status ABI and `struct hv_xfer_exc_info`, same as m1n1 plus `TAG_WRITE`/`TAG_ID_MASK`. |
| `src/hv/windows/hv_xfer.c` | The EL2 device: a stage-2 hook on the locked slow path, plus `hv_map_xfer()` with Aura-specific refusals. |
| `src/proxy.h`, `src/proxy.c` | `P_HV_MAP_XFER = 0x1700` (was free in Aura), compiled only under `AURA_WINDOWS`. Caps go `0x...11fff` -> `0x4157494e00013fff`, new bit 13 = `0x2000`, no renumbering. |
| `Makefile` | Adds `hv_xfer.o` to the `WINDOWS=1` objects only. |
| `proxyclient/aura/hv/xfer.py` | The host engine: `XferExcInfo`, `ProxyTransport` (gzip+`P_GZDEC`), and `XferHostDevice` with the serve_dir/inbox policy. |
| `proxyclient/aura/hv/__init__.py` | `attach_xfer()`, `handle_xfer()`, doorbell conflict check / `auto` pick. The `START.HV/HV_EVENT.XFER` handler is always registered. |
| `proxyclient/aura/hv/types.py` | `HV_EVENT` gains `TPM=8, XFER=9, SYSREG_ASSIST_READY=10`, matching the C enum. |
| `proxyclient/aura/proxy.py` | `P_HV_MAP_XFER` and `hv_map_xfer()`. |
| `proxyclient/aura/windows.py` | `XFER` cap bit, the strict optional `"xfer"` profile key, `doorbell_conflicts()`, attach before guest entry, and `launch.json` `"xfer"` receipt. |
| `proxyclient/tools/boot_windows.py` | `validate` prints the xfer plan. |
| `tests/hv_xfer/*` | Failure modes, offline E2E replay, mutation controls, build evidence (below). |

Not ported: `enable_xfer_hvcall` (BRK/HVC front end; the guest driver doesn't use it).

## Enabling it

Add the following to an `aura.windows.boot.v1` profile. Leaving the key out
means disabled, and the old profiles validate exactly as before.

```json
"xfer": {
  "doorbell": "0x230000000",
  "window_bytes": 1048576,
  "serve_dir": "/abs/path/to/serve",
  "inbox": "/abs/path/to/inbox",
  "compress": true
}
```

- `doorbell` is required: a 16 KiB-aligned hex value, or `"auto"` (the first
  conflict-free `/arm-io` gap). `0x230000000` is a suggestion only. In the
  IPSW 25G76 J813 static device tree it sits in a 720 MiB unclaimed `/arm-io`
  gap (0x222000000-0x24f000000), with no device within 16 MiB. The live ADT is
  checked again at launch.
- `window_bytes` is the Aura-owned window. Default 1 MiB; it must be a 16 KiB
  multiple and at most 64 MiB.
- `serve_dir` is required: an existing absolute directory that isn't a
  symlink. `inbox` is optional. Without it, PUT is refused with NODEV. The two
  must not be equal, and neither may sit inside the other. `compress` is
  optional and defaults to true.

The launcher checks the doorbell three times:
1. Offline: alignment, IPA < 2^44, not in the vGIC synthetic windows.
2. At attach, against the live state: DRAM, every ADT `reg`, every host
   tracer except the `/arm-io` HW passthrough, and the resident's caps bit.
3. In C: it must be outside DRAM, inside the IPA space, and with **no existing
   stage-2 entry on any of its 4096 words**. That last test catches C-only
   hooks too.

The attach happens in `prepare()`, before `load`/`start`. The doorbell and
window get RESERVED tracers, so `start()`'s `pt_update()` leaves them alone.

The launcher prints the base, for example `HvXfer: doorbell 0x230000000 (pass
this to IOCTL_APPLE_HVXFER_ATTACH) ...`. It also records the base in
`launch.json` under `xfer` (`doorbell`, `aura_window`, `guest_attach`) before
the guest runs. Every command goes to `<out>/xfer-commands.jsonl`. When the
session ends, `launch.json` `xfer.engine.streams` holds the size and SHA-256 per
OPEN, plus the bytes and SHA-256 actually served or received.

Engine policy:
- **GET** looks the name up in `serve_dir` at OPEN, through a directory fd
  pinned at start, so files added mid-session work. The rules:
  - The name must match `^[A-Za-z0-9][A-Za-z0-9._+-]{0,62}$` and contain no
    `..`.
  - The file is opened `O_NOFOLLOW|O_NONBLOCK` and must be a regular file.
  - It is read once at OPEN. The guest gets that snapshot, so changing the file
    afterwards does not affect the transfer.
  - The cap is 256 MiB.
- **PUT** writes to `.<name>.<pid>.<n>.part`, opened `O_EXCL`. CLOSE publishes
  it with `link(2)`, which refuses an existing name. The SHA-256 is recorded at
  CLOSE. A crash leaves a visible `.part`.

## Differences from m1n1 (all refusals; ABI unchanged)

- SET_WINDOW bounds the guest IPA against `vaddr_bits` before walking. **In
  m1n1, a guest `UWIN_HI` past the IPA space hits
  `assert(idx < ENTRIES_PER_L1_TABLE)` in `hv_pt_walk()`: the guest can panic
  EL2.** The mutant `el2.no_ipa_bound` reproduces this.
- `hv_map_xfer()` now refuses a doorbell in DRAM, a doorbell beyond the IPA
  space, a doorbell on any existing stage-2 entry, and a window that is
  already guest-mapped.
- A host that claims RESULT > LEN for GET/PUT gets ST_IO, RESULT=0.
- Name sanitising no longer strips whitespace and refuses `..`. Files are
  served lazily instead of from blobs published at start. PUT is create-only.

## Build (reproduce: `tests/hv_xfer/build.sh`)

Same as the integration manifest: `gmake -j8 WINDOWS=1` and `gmake -j8`, with
`PATH=$HOME/.cargo/bin:/opt/homebrew/bin:...` and
`AURA_VERSION_TAG=hvxfer-compare`. Both builds run in throwaway copies (base =
`git archive HEAD`). The run writes its evidence to `out-build/build.json`
locally (not committed). The port's images are in `build/hvxfer/{windows,default}/`.

| Variant | File | Port SHA-256 | Base SHA-256 |
|---|---|---|---|
| windows | aura.bin | `1ef517977132ed2eb370822b19f28fb3039abc824a2565eb1ee862ad6589fe6b` | `4c0972d3...ba47404` |
| windows | aura.macho | `03a8b412fd6160f06c1f32fe24c36d4a76b398a4a1f78d10747c1ccac85d83af` | `489ccf3c...f988f430` |
| windows | aura-boot.macho | `0f465e0761145f7239250f23f90d34b9e0ca43452cfc35c6057c821fdc96efeb` | `f1d5647b...bd81a36` |
| default | aura.bin | `673853a0b896f4293a59b5d22cde0053e4b0fae4e9f2135efe9146723901fe6b` | identical |

- The default image is **byte-identical** to base.
- The warning sets are identical: Windows 33/33, default 12/12, nothing new.
- hv_xfer.c compiles under `-Werror` in the replay.
- clang-format is clean.
- **Cost:** base `.text` had 1,516 bytes of slack before the next 64 KiB
  section step, and the port adds 2,160 bytes. So the DCP file-part margin
  (`_file_vital_end` vs `_file_limit`) drops from **88,424 to 22,888 bytes**.
  The next branch that pushes `.text` across one more 64 KiB boundary fails
  the `aura-raw.ld` assert. That failure is safe (the link fails), but it is
  near.

## Tests

`PYTHON=<m1n1>/.venv/bin/python3 HVXFER_GUEST_DIR=<AuroraSilicon>/drivers/AppleSerial tests/hv_xfer/run.sh`
runs in about 2 s.

The failure modes were written first, in `FAILURE_MODES.md`. The replay
compiles the **real** `hv_xfer.c`, the **real** `hv_vm.c` (`hv_handle_dabort`,
instruction decoder, access splitting, stage-2 tables) and the **real** guest
`AppleHvXferCore.c` against the real Aura headers (`AURA_WINDOWS`). Guest
register accesses are real STR/LDR/STP instructions fed through
`hv_handle_dabort()`. A SIGILL shim emulates only ID_AA64MMFR0/SCTLR_EL12 reads
and VTCR/VTTBR writes. Each proxy event goes to the **real**
`HV.handle_xfer()`. The **real** `launch_profile()`/`plan_profile()`/
`attach_windows_xfer()`/`pt_update()` run against a simulated J813 memory map,
with DRAM at 0x100_0000_0000 mapped so PA == VA.

Covered:
- OPEN/GET-to-EOF/CLOSE of an 11.5 MiB+3 file in 1 MiB windows, byte-exact.
  SHA-256 `6fc009cd...` matches the OPEN record and the served bytes.
- Partial-window GETs at an offset through the gzdec path.
- A 2 MiB PUT, create-only.
- Window fence and cache-sync range on every GET/PUT.
- Refusals: bad base/alignment/overlap, SET_WINDOW onto unmapped,
  discontiguous, hooked, MMIO, DRAM-edge and beyond-IPA ranges, 8/16-bit,
  unaligned and 128-bit accesses, 64-bit CMD, bad names, symlinks, FIFOs, no
  host engine, a host that never answers, RESULT overclaim.

Results (each run writes these files locally; they are not committed):
- `out/result.json`: **122/122 pass**. The digest
  `43f3b1d47414ff68dc0c545ff2df899be25d40960dffe70730fc775305419921` was
  identical across reruns.
- `out-mutants/mutants.json` (`mutants.py`): **18/18 mutants killed**,
  including the unported m1n1 device. Each one plants a real bug in the EL2
  device, the host glue, the engine or the profile, and the named check fails.
- `out/abi-parity.txt`: `AppleSerial: hv_xfer ABI parity PASS (40 constants +
  2 feature bits + the window cap)`.

Not replayed: the rendezvous, CNTVOFF/stolen time, real link speed, real cache
coherency, and real Windows.

## Risks for the first hardware use

- **Xfer disabled, or wrong base.**
  - A doorbell IPA with no stage-2 entry makes `hv_handle_dabort()` fail
    ("Unmapped IPA"), which raises EXC_SYNC. The Windows launcher answers with
    `EXIT_GUEST`: **the Windows session ends**, with `windows_abort_reason` in
    the receipt. It does not park in a shell the way m1n1 did.
  - The same happens for any 8/16-bit, unaligned or 128-bit doorbell access.
  - A wrong base inside an `/arm-io` gap is HW passthrough. The driver's
    read-only probe then touches real address space, which most likely gives an
    SError and a dead session.
  - A wrong base on a real device reads its registers 0x00-0x1c. Those are
    read-only, but some registers have read side effects.
  - Copy the base from this boot's launcher output only.
- **Stall per window.**
  - Every command that reaches the host calls `hv_rendezvous()`, because time
    stealing is on: PING, OPEN, CLOSE, and in-range GET/PUT. NOP,
    SET_WINDOW, RESET_WINDOW and range-refused GET/PUT stay in EL2. **All
    vCPUs** wait in EL2 while the proxy round trip runs.
  - The stall is roughly LEN / link rate plus about 4 small proxy transactions
    plus the rendezvous (at most 20 ms). For 1 MiB that is about 50-100 ms at
    m1n1's measured 10-23 MB/s. On a slow serial carrier (about 150 KB/s) it is
    about **7 s**.
  - OPEN also reads and hashes the whole file inside the stall: about 0.1 s
    for 11.5 MB.
  - Start with LEN = 64 KiB and time it.
- **Rendezvous and secondary CPUs.** Each command is one rendezvous. **64
  consecutive misses call `hv_panic`.** 11.5 MB at 64 KiB per GET is about 185
  commands, so a CPU that systematically misses could panic the HV. At 1 MiB
  it is about 13 commands. Watch the console for "rendezvous" warnings.
- **Stolen time.** CNTVOFF_EL2 absorbs only the `uartproxy_run()` duration.
  The rendezvous time before it leaks, a few ms or less per command.
  - CNTVOFF does not cover **CNTPCT or CNTP-based timers**, and T8142 has no
    CNTPOFF.
  - Devices (NVMe, USB, display, the vUART RX side) see real time pass during
    the stall.
- **Window lifetime.** EL2 keeps the guest window's PA until RESET_WINDOW, and
  the driver resets it before freeing. Abnormal guest teardown that skips the
  driver's ReleaseHardware leaves EL2 pointing at reused memory until the next
  SET_WINDOW. A later GET would then write into it.
- **Concurrency.** The guest driver serialises commands through one queue.
  `hv_exc_proxy()` drops the HV lock only in its pinned-CPU and CPU-switch
  wait loops, which host debugging triggers. Using those mid-transfer could
  interleave two vCPUs' commands on the single register file.
- **Window trust.** Validation accepts any VALID stage-2 leaf in DRAM. No
  read-only leaves exist in the Windows personality (hv_sprr is inactive), but
  that would change if SPRR emulation were ever enabled.
- **Image size.** See the 22,888-byte DCP margin above.
