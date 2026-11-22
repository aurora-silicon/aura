# J616s / T6040 proxy bring-up

Tested on J616sAP (Mac16,7), board 6, with the ADT supplied by macOS build
26A428. The branch is stacked on `j700` while the draft targets `aurora-wip`.
The M1/M2/M3 boot and proxyclient behavior is the implementation
standard; J700 is the feature-parity target. Linux RAM boot preparation is
now in scope. A bounded one-CPU native Linux test reaches PID 1 and the boot
framebuffer. Its USB gadget enumerates at high speed, but the host times out
when selecting its configuration. A separate hypervisor test reaches a
RAM-only BusyBox shell. Native USB shell access, persistent operation and
other Linux peripherals remain unqualified.

For implementation order, formats and ownership rules, start with the
[driver contracts](j616s-driver-contracts.md).

Linux driver preparation is tracked in
[draft PR #231](https://github.com/aurora-silicon/linux/pull/231), against
`aurora-wip`. It contains gated AOP/audio and ISP infrastructure plus a
read-only inherited USB2 PHY provider. Its source and build checks do not
qualify the combined kernel on J616s: camera/audio hardware admission remains
closed. The separate native test below establishes descriptor enumeration,
not working USB data or SuperSpeed.

## CPU clocks

`cpufreq_init()` runs the same cluster code as every other chip. On T6040/T6041
it checks each cluster block against the ADT CPU nodes' `cpm-impl-reg` and keeps
the clock states firmware left: there is no hold state, voltage, PLL, feature or
thermal-policy programming. Proxy operations `cpufreq_get_cluster_hz(cluster)` and
`cpufreq_set_cluster_pstate(cluster, state)` use cluster 0 for E, 1 for P0 and 2
for P1, bounded by the PMGR ladders `voltage-states1-sram`, `voltage-states5-sram`
and `voltage-states13-sram`. The getter reports the ADT frequency for the accepted
command state; it is not a frequency measurement.

Fixed-work measurements on running cores matched the ADT nominal transitions of
1020 -> 1404 MHz for E and 1260 -> 1512 MHz for both P clusters (states 1 and 2),
with execution-rate ratios of 1.376, 1.200 and 1.200. Each test restored state 1
and approximately its original execution time. Higher states have not been measured.

The request preserves other command bits, waits for the previous request, verifies
the accepted state and bounds each busy poll to 2 ms. A failed transition locks out
subsequent proxy writes for that cluster until the image is restarted.
A timeout does not imply that hardware reverted to the previous state.

## SMP and MMU

Existing startup code brought up all 13 secondary CPUs: IDs 0-3, 5-8 and 10-14;
CPU 4 is the boot CPU. The hardware has four E and ten P cores. CPU ID 9 is
absent from the ADT and must not be treated as a missing startup.

Every secondary executed scratch code, returned its expected MPIDR and ran
with EL1/EL2 MMU and caches enabled after `mmu_init_secondary()`. AT S1E1R
translations succeeded for shared scratch memory, and four-argument calls
returned the expected result. Concurrent exclusive-access increments across
all 13 secondaries produced exactly 130,000 updates to one shared counter.
Ten additional dispatches per secondary also passed. No new SMP startup code
was needed for this tested configuration. The boot CPU's `smp_is_alive()`
result is not a secondary-start indicator.

These checks cover startup, WFE/SEV dispatch and shared memory coherence.
A cold independent-workload probe also passed on all 13 secondaries. The E
cores checked repeated memory sums, P0 cores ran integer recurrences and P1
cores ran rotate/XOR loops in separate buffers. Every result matched its host
calculation, all execution intervals overlapped, and nominal cluster clock
settings were unchanged. The test initializes every secondary's MMU before
calling through the RX alias and keeps its scratch code unchanged until all
workers return. Earlier trials violated these assumptions and are invalid
qualification evidence. No new SMP/AMP driver code was needed.

Deep sleep, hotplug, multicore hypervisor operation and sustained thermal load remain
unqualified. Reproduce the cold independent-workload test with:

```sh
AURADEVICE=/dev/ttyACM0 python proxyclient/experiments/t6040_amp.py
```

The probe leaves secondaries running in WFE mode. Reboot before repeating it.

## Native USB RAM boot

A separate native diagnostic Image, SHA256
`06fe50711cc7ec2a58354e280fadc85a25518450368caa851d448c2cb2ad37d2`,
boots Linux 7.1.12+ on one CPU, registers the inherited framebuffer and runs
`/init`. The test uses an owned 128 MiB allocation and a validated 45-second
hardware watchdog. It mounts RAM and pseudo filesystems only. This is not
qualification of every driver in the integration kernel.

The host observed high-speed device `1d6b:0104`, product
`J616s RAM Linux ACM ECM`, serial `j616s-ram-linux`. The kernel reports UDC
`382280000.usb`, a configured gadget, `ttyGS0`, and ECM interface `usb0` at
`192.0.2.2/30`. The host subsequently reports configuration #1 timeout
`-110`; no host ACM shell or ECM data transfer is qualified. The composite
gadget code sets its configured state before completing function activation
and the control-request status stage; the kernel-side state alone does not
prove either completed.

Later RAM trials using both USB DART banks completed three fresh-nonce root
commands over physical high-speed ACM bulk traffic. Each returned UID0,
the exact kernel release and command line, a configured UDC, speed and uptime,
then passed independent watchdog recovery. The first two used notification
suppression, omitted empty-transfer priming and request deferral. The third
kept suppression and omitted priming but disabled request deferral, confirming
the ordinary queue/kick path for one command. Enabling notifications on that
setup still failed configuration. Sustained operation, normal notifications
and ECM remain unqualified; see the native USB handoff driver contract.

Two initramfs errors were corrected before enumeration. This kernel's
`gether_set_ifname()` requires a template containing exactly one `%d`; write
it literally with `printf 'usb%%d\n'`. Configfs resolves a symlink target
from the calling process's working directory at creation time. Use absolute
function targets when setup runs outside the gadget directory:

```sh
printf 'usb%%d\n' > "$GADGET/functions/ecm.usb0/ifname"
ln -s "$GADGET/functions/acm.gs0" "$GADGET/configs/c.1/acm.gs0"
ln -s "$GADGET/functions/ecm.usb0" "$GADGET/configs/c.1/ecm.usb0"
```

A host last-close with `HUPCL` can send a CDC control request during aura
retirement. The diagnostic client holds its original descriptor until the
source USB device disappears or a two-second deadline, then closes it without
further proxy operations. Native handoff remains intermittent; this comparison
is not proof that all USB shutdown failures are fixed. Failed retirement keeps
DMA memory and the original watchdog until cold recovery. Installed images,
boot policy, partitions and attached storage remain unchanged.

## Minimal Linux RAM boot

A guarded direct-entry test boots Linux 7.1.12+ through aura's hypervisor on
one CPU with an owned 64 MiB guest allocation. Image, FDT and initramfs are
read back twice and cache-published before entry. The guest maps only its RAM,
the physical AIC and an intercepted UART console on the second CDC interface.
Its CPU node uses the observed architectural affinity 0x10100.

The RAM initramfs reaches a BusyBox shell and accepts a diagnostic command:
kernel version, CPU online 0, uptime advancing across a two-second sleep,
kernel configuration hash and RAM-only mounts all return. Timer interrupts
advance and the interrupt error count remains zero. No block device is mounted.

An initial run exposed a host reply-ordering defect: register postchecks after
the exception proxy's EXIT command race the resumed guest's next notification.
Deferring those checks until the next suspended handler fixes the captured
opcode mismatch; byte-level regressions exercise the actual proxy framing.
Normal prechecks, pending records and write-ownership bounds remain enforced.

A later 60-second console window accepts four marked commands separated by
10-second waits. Every command returns status zero, uptime and timer interrupts
advance, and the interrupt error count remains zero. The queue rejects echoed,
unmatched or duplicate completion markers and never retries ambiguous input.
An absolute deadline bounds both proxy reads and console dispatch; a 120-second
hardware watchdog preserves recovery after the longer window.

The current aurora-wip kernel also boots with USB/configfs ACM/ECM and network
support compiled in, using the same RAM initramfs and minimal HV DT. Configfs
mounts successfully and its gadget directory is present. No UDC is exposed by
that DT, so this is kernel/configfs qualification, not Linux gadget operation.
One config-query command fails because the initramfs lacks a zcat symlink;
its BusyBox applet remains available by explicit invocation.

The exact kernel Image used for the native diagnostics also boots under
this HV setup with the same RAM initramfs and minimal DT. Its reported
configuration hash matches the frozen native build. On one CPU it completes
scheduler-clock initialization and delay calibration, reaches a BusyBox
shell, and advances uptime by two seconds across `sleep 2`. Timer IRQ 34
advances from 25 to 1278 over the initial five seconds. Native execution
context and peripheral nodes differ, so this comparison does not identify
the native boot failure or qualify physical USB.

The bounded captures end with a host timeout while the guest is running.
Mappings and guest memory remain retained until hardware-watchdog recovery
returns the installed proxy with its watchdog disabled. This is not a clean
guest shutdown or a persistent Linux development session. Native results are
reported above; Linux SMP and peripheral data transfer remain unqualified.

The following earlier checkpoints preceded the native full-boot results above;
they do not identify the current USB configuration failure. Separate native
RAM diagnostics reach the original Linux CPU setup, TTBR
loads, MMU enable and early kernel mapping. Nonce-bound markers survive a
warm return to the fully verified proxy. Fresh page-table walks verify the
diagnostic code, marker and FDT mappings, plus the virtual address of
`__primary_switched`. A further checkpoint reaches that virtual entry and
verifies the kernel stack, task pointer and exception vectors before
`finalise_el2`. These checkpoints alone do not qualify completed startup or
gadget traffic.
Diagnostic code uses owned linker padding with verified permissions; guest
tables and CPU context remain retained after the warm return.

A later checkpoint also completes the original `finalise_el2` transition to
VHE EL2 with the kernel MMU enabled. A separately compiled copy of Linux's
upstream reset routine runs through its verified physical identity mapping,
disables translation and returns to the verified proxy. This establishes the
early VHE transition and bounded recovery, not complete reset-register
qualification or completed kernel startup.

The next checkpoint reaches the original `start_kernel` prefix and records
the physical CPU identification and Linux version banner. The stopped
writer's bounded printk snapshot is cache-published before recovery; two
complete captures match and their record IDs, sequence and text bounds pass.
A later checkpoint completes the original `setup_arch`, including removal of
the initial identity map. Recovery calls Linux's original identity-map install
routine, uses reset code in the new map's executable page and preserves the
separate guest register bank. Two complete static log captures match and show
the machine model and reserved-memory ranges. Interrupt delivery and Linux
gadget traffic remain unqualified.

A subsequent diagnostic reaches the return from the original `init_IRQ`,
after memory, per-CPU, scheduler and RCU initialization. The marker verifies
the AIC IRQ/FIQ handler pointers and one possible CPU. The source reset path
returns to the proxy and two complete static log captures match. Offline
decoding validates 42 finalized records and one committed head record: the
last AIC message has no newline, so printk has not finalized it. The initial
decoder rejects this valid state; watchdog recovery passes. These results
establish controller initialization, not timer or interrupt delivery, completed
kernel startup or a Linux gadget shell.

A later diagnostic completes the original timer initialization, global IRQ
enable, console initialization and per-CPU page setup. Its marker records the
return from `setup_per_cpu_pageset`. The selected timer's IRQ 36 count advances
from zero to one over 3,724,788 counter ticks at the reported 1 GHz frequency
(3.724788 ms). The console is the dummy 80-by-25 console. Two complete log
captures match and validate 50 finalized records. Recovery returns to the
verified proxy and explicitly disables the watchdog. This establishes native
timer interrupt delivery and page setup; scheduler-clock initialization,
device initialization and a gadget shell remain unqualified.

Subsequent full native boot attempts did not enumerate a USB console.
An isolated build with jump labels disabled and a normal-kernel attempt
with the inherited framebuffer both returned through natural watchdog
recovery. Neither result identifies the kernel's stopping point. The
framebuffer driver probes during device initialization, so absence of
display output does not locate a failure before that stage.

## Memory controller

The ADT identifies the controller as `mcc,t6041`, with four AMCC apertures
and four planes per aperture. Initialization accepts the tested J616s layout,
checks the way-count status on every plane and preserves firmware cache
settings. A RAM-only candidate boot passed with zero CPU exceptions and
exported both carveout ranges matching the fresh ADT. Failed initialization
or unavailable carveout metadata stops startup on T6040.
An independent walk of the exported translation tables checked all 110,544
page/alias positions across both carveouts and the identity, RWX EL0, RW EL0
and RX EL1 aliases. Every position was unmapped. All 67 table snapshots
matched a second read, with zero CPU exceptions; replaying the archived
tables gave the same result. This verifies table contents, not independent
TTBR/TLB enforcement.
One guarded read of controller 0, plane 0 at offset `0x2800` returned
`0x8000000c`. A separate cold test read offset `0x2804` and returned
`0x0c000c00`, consistent with upstream PR664's proposed encoding of 12 data
ways and 12 tag ways. Both reads completed without CPU exceptions, followed
by successful watchdog recovery to the installed proxy. A further cold sweep
read `0x2804` once on each of the 16 controller/plane pairs. Every word
returned `0x0c000c00`, with no CPU exceptions.

These observations qualify status reads, not cache-control writes.
The ADT lock predicate uses offset `0x2800`, mask `0x1f` and expected value
zero; it must not be interpreted as the way-count status at `0x2804`.
PR664 defines the newer offsets but its enable routine still uses the older
`0x1c00`/`0x1c04` offsets. This implementation uses the physical ranges in
`/chosen/carveout-memory-map` rather than interpreting the unqualified TZ
registers. Cache-control writes and TZ register interpretation remain
unqualified.

## Native USB retirement

Generic DWC3 shutdown now ends every active endpoint, including control OUT,
using its recorded resource index and a forced ENDTRANSFER command. It waits
for the matching command-completion event before checking controller halt,
reset and DART invalidation. Constructor or retirement failures retain DMA
memory and tables, latch the failed owner and prevent another initialization
or native entry. The caller must provide recovery after an uncertain stop.

A guarded RAM test with this C implementation boots a candidate and vectors
back to a recovery image. Both new USB generations pass complete relocated
executable checks and preserve the caller's 45-second watchdog. The final
verified recovery explicitly disables it. The earlier checked implementation
failed this transition; adding the resource, force and completion handling
made it pass. Test-only watchdog startup and RAM diagnostics stay outside
the production changes. Separate native RAM bulk-command results are recorded
above; reliable normal Linux ACM and SuperSpeed remain unqualified.

A later console checkpoint records the post-retirement registers after the
original MMU-off reset and before the recovery copy. Two samples of all 46
selected registers match. DCTL reads `0x00f00000`, DSTS `0x00d20001`, and
DEVTEN, DALEPENA and event count read zero. The DART command register reads
`0x05000101` with BUSY clear; ERROR, PROTECT and LOCK read zero. Global stream
enable remains `0x2`; SID1 TCR reads `0x2` and its TTBR reads zero. Other
disabled streams retain nonzero TCRs and invalid-root address bits.

These are observations at a stopped pre-device checkpoint. The source writes
the SID1 bypass value `0x6`; that value must not be imposed as an exact
readback predicate. Successful retirement does not clear the entire bank or
qualify later driver activity, cache invalidation policy or a Linux gadget.

## DCP management

A guarded inherited-state test checks the current 25G76 firmware UUID, exact
clock ancestry, absence of a target-side DCP client and existing stream-23
translation before mailbox access. The CPU is running and both mailboxes are
healthy and empty; no unsolicited Hello is pending.

A separate bounded test sends one standard RTKit IOP power INIT request,
without starting/resetting the CPU or changing DART. It receives Hello,
negotiates protocol 12 and acknowledges the complete endpoint map, including
endpoint 0x23 used by the existing iBoot service. DART errors remain zero and
watchdog recovery passes. This establishes management communication.

A later bounded test starts endpoint 0x23 and sends the initial AFK request.
It receives INIT_ACK and GETBUF requesting 256 64-byte blocks (16 KiB), with
tag 0xcafe. No buffer address is acknowledged and no DMA allocation, ring
start or application query follows. Watchdog recovery passes. This establishes
initial AFK control, not shared-buffer ownership, a running application queue,
firmware-version queries or scanout. Preserve the inherited DART mode rather
than replacing it with another context based on SoC capability.

A later read-only capture verifies the complete inherited stream-23 table
closure: one root and five leaf tables, with 9,247 decoded mappings and no
physical aliases. Each table is read twice and checked again after the walk;
source, ADT, controller state and ownership checks remain around the reads.
The current parameter register reports DART version 2.2 (`0x2a2a0202`), and
TCR is 1. The source and metadata checks pass again before the watchdog is
explicitly disabled. This establishes a stable CPU-visible table capture,
not DART cache visibility, a publication sequence or permission to acknowledge
the AFK buffer address. Those remain separate bring-up steps.

The conditional publisher and bounded BUSY-poll helpers have now executed
against caller-owned Normal RAM. Actual CPU page-table walks verify the
identity and executable-alias mappings, attributes and permissions; current
translation registers and core identity remain stable around each operation.
Publication succeeds from zero, refuses a nonzero word and rejects invalid
arguments. The poll accepts clear, times out on permanent BUSY and rejects an
invalid mask. Full private-page readbacks, zero exceptions and cold watchdog
recovery pass. This qualifies helper execution only.

A checked publisher variant also passes seven private-RAM cases. It checks
the source exception counter before the first exclusive load and after each
exclusive load/store, stopping on an exception without another store or retry.
The tests include successful publication, mismatch, invalid arguments and
synthetic counter changes. Full target-page readbacks and cold recovery pass;
these synthetic cases do not qualify actual abort recovery on firmware memory.

The tested boot core (`MPIDR_EL1=0x80010100`) reports `CTR_EL0=0x9444c004`:
both its minimum data-cache line and writeback granule are 64 bytes. The CPU
register sample, mapping state and core identity remain stable under the same
guard. This is one core's observed geometry, not a guarantee for all CPUs.

A separate RAM-only test completes one version-2.2 range/STT command (`0xe117`)
for the vacant 16 KiB range at DVA `0x10010000000`. START and END are written
using the source's page encoding, `0x10010000`. START reads back exactly;
END reads back `0x10010003`, preserving the page field. The test requires that
specific END value; the low-bit semantics remain undocumented. The command's
BUSY bit clears within the bounded 2 ms poll, exceptions and DART errors remain
zero, and the complete inherited table closure and context remain unchanged.
Cold watchdog recovery passes. No table entry or AFK address is published.
This qualifies command completion for the tested empty range, not visibility
of a new mapping.

A later guarded experiment allocates one 16 KiB arena and one private leaf,
then conditionally publishes that leaf into vacant root slot 8. Actual CPU
mapping checks admit the inherited root and private pages as writable Normal
memory. The measured boot-core cache geometry bounds the clean to the formerly
zero 64-byte root cacheline. The selective range/STT command completes with
zero faults. Five complete raw table captures independently verify the only
change to the inherited tables is the expected eight-byte root entry; the
new leaf adds exactly one full-page mapping, raising the count to 9,248.
Cold watchdog recovery passes. This qualifies the tested CPU-visible publication
and command sequence. No AFK address is acknowledged, so hardware walker
visibility, buffer DMA, rings and scanout remain unqualified.

A subsequent bounded experiment obtains a fresh GETBUF request after that
mapping sequence and sends one address acknowledgement. Firmware returns a
tagged INIT_TX descriptor for an 8 KiB ring at offset zero. The host observation
deadline expires during the second ring receive; no arena headers are captured
and no START is sent. Cold recovery passes. This is initial address-exchange
evidence, not completed ring initialization or buffer-DMA qualification.

A later test compares all seven table pages exactly on the target against
retained, independently verified copies. This removes host readback latency
without extending the ACK deadline. The native comparator first passes private
equal and first/middle/last-byte mismatch cases. Firmware then returns both
INIT_TX and INIT_RX and initializes the mapped 16 KiB arena. Two cache-refreshed
full-page reads match. Each ring is 8 KiB, with 7,808 data bytes, a 128-byte
block stride, and zero read/write pointers; the regions do not overlap.
Table comparisons, controller state, zero exceptions and cold recovery pass.
This qualifies firmware DMA initialization of this owned arena and passive
ring headers. No START, service query, ring-pointer update or scanout follows.

A subsequent bounded test sends one START and receives START_ACK. Two stable
RX snapshots contain a valid AFKEPV2 publish for `disp0-service` on channel 3;
its OSSerialize properties report `interface-id: 3`. RX advances to byte 128,
while its read pointer remains zero. The RECV notification's low 48 bits are
opaque: both existing handlers use the actual ring header, not the payload as
a pointer. The first observer incorrectly validated that unused field and
stopped; the corrected observer completes with unchanged mapping/controller
state and cold recovery. No ring entry is acknowledged and no service query
or display change follows. This qualifies queue startup and the observed
announcement, not complete service enumeration or scanout.

Management startup also uses a qualified fixed-address reader to check all
five physical parents on the target before each controller word. Exceptions
are checked before and after every read; outer CPU, owner and deadline checks
remain in place. This removes repeated host round trips without caching power
state or extending the five-second discovery limit. The IOP-first continuation
then receives a fresh state-0x20 IOP acknowledgement and sends AP power-on.

Owned management startup prepares separate IOReport and syslog pages before
sending requests. One vacant root entry and exactly two private leaf entries
are published; the two-page selective command and complete CPU-visible table
verification pass. DCP requests a 12 KiB syslog buffer with 63 records of
160 bytes each. The reply preserves the requested three 4 KiB pages while
retaining a dedicated 16 KiB backing page. Notifications are acknowledged
under a current buffer lease. Both fresh IOP and AP power acknowledgements
are now observed; the startup return and table checks are described below.

The diagnostic retains 24 non-log pairs and permits at most 63 syslog
notifications after validated geometry, with an absolute cap of 87. The
original deadlines remain unchanged. Full checks bracket buffer grants,
destructive mailbox operations and closure verification; controller reads
retain their caller checks and fresh target-side parent checks.

One run reaches a preallocated OSLog buffer and then the unsupported word
`0x03010042100000a1`. It retains the word and stops without an ACK. Offline
analysis of the captured 25G76 firmware finds a type-3 producer followed by
three type-4 UUID chunks at offsets 4, 8 and 12. This binds the packet layout
to that firmware; the producer's endpoint binding and complete transport
semantics still require qualification. A qualified paired mailbox helper
reaches the candidate within the original deadline. Its single-follow-up
observer retains an interleaved syslog notification and stops before ACKing it.
A separately enabled EP8 observer handles only already-qualified interleaved
traffic, then captures `0x04000020d51e4021` on EP8 and stops without an ACK.
The type, source index and offset match the producer's first type-4 chunk at
offset 4; the remaining chunks and registration are not qualified.
A further bounded capture retains all three ordered chunks at offsets 4, 8
and 12, without ACKs or host registration. Continuing after that observed
sequence receives fresh IOP and AP state-0x20 acknowledgements. A later
post-receive parent check fails, so final management closure remains unproven.
The complete pair is retained and not replayed. Cold recovery passes.

An explicit startup-boundary test returns immediately after both guarded power
acknowledgements, matching the return condition of `rtkit_boot_timed()` and
`ASCManagementEndpoint.wait_boot()`. Required owned buffer replies and all three
OSLog chunks complete first. A fresh RAM-only run then compares all seven
published table pages exactly through CPU mappings, with the original source,
CPU, ownership and watchdog checks. No further DCP register or FIFO read occurs.
Watchdog recovery returns to the installed proxy. This qualifies startup
acknowledgements and CPU-visible table preservation; the last controller
observation remains prior evidence. Current controller access, firmware
coherence, OSLog registration, service queries and scanout remain unqualified.
DISP_CPU was observed automatically gated in the earlier continued-drain test;
its parent checks must remain in place for subsequent device access.

A separate native macOS observation retained the actual ASCWrapV6 and its
provider, verified the loaded register-access code, and read the inbox status
at `0x402e08110` once. It returned `0x20001` (empty). Both FAB6_SOC and
DISP_CPU sampled as automatic mode with actual state zero before and after
the read; AFI, DISP_SYS and DISP_FE were active. This establishes that single
native access under the sampled states, not continuous gating, absence of a
transient wake, or permission to access the FIFO or DART in that state.

An Aurora RAM image also returned `0x20001` from the same status register,
with all five parents active. Its executable, ADT and absent target-side
client owners were checked under the original watchdog. This early read
does not establish access after the startup acknowledgements.

A later Aurora RAM run completed the startup ACK boundary and CPU table
comparison, then read that inbox status once as `0x25501`. All five parent
domains sampled active before and after. This establishes a post-startup
status read in that state, not access with an automatically gated parent or
permission to consume FIFO traffic. The diagnostic used a bounded 20-second
host startup phase, still under its original 30-second root deadline and
45-second watchdog; individual native mailbox calls remained capped at
eight seconds. No service query or display change was attempted.

The paired helper checks fresh parents, translation context and exceptions
around each word, with data before endpoint commit. Partial results are
terminal; the pair is not atomic. Private refusal cases, code/data/stack CPU
mappings and positive management traffic pass live. Its deadlines use the
measured architectural counter: in this boot profile CNTFRQ reports 1 GHz,
confirmed against host timing brackets. The watchdog uses separate 24 MHz units. Keep those clocks
separate when converting timeouts.

## Other devices

* CDC proxy and bulk reads work at USB high speed (480 Mb/s). RAM-only
  controller halt/reset/reinitialization tests re-enumerate in the same image
  and pass an exact 64 KiB transfer. SS-capable controller settings also return
  through high-speed fallback when the ATC PHY is left in its initial state.
  Native mode-4 PHY preparation, the complete ten-write PIPE sequence and all
  immediate masked readbacks are qualified with high-speed operation. Tests
  retain the installed image and recover through the hardware watchdog.
  The current sequence completes RTKit startup and observes the firmware
  tunables-done indication before reset release. SS activation re-enumerates
  at 480 Mb/s and remains in RX.Detect. Neither the tunables indication nor
  the separate PCLK predicate establishes a physical SuperSpeed link. See the
  later SuperSpeed results and driver contracts for the remaining limits.
* SIO completes RTKit startup with four-level, full 42-bit mappings of its
  ADT-described external firmware. Firmware text is mapped read-only. The
  standard system endpoints start, AP/IOP both acknowledge state 0x20,
  IOReporting messages are handled and management ping receives a pong.
  Endpoint 0x20 is advertised but its application protocol and audio DMA are
  untested. AP/IOP acknowledge quiescence at state 0x10; CPU control, table
  pointer, TCR and stream mask return to their original values, with zero
  DART errors and a successful proxy NOP.
* AOP completes RTKit startup and answers a management ping with full 42-bit
  mappings, ADT DAPF ranges and its external-code boot argument set to the
  ADT remap address (1 TiB). Starting the advertised application endpoints
  announces SPUApp, wakehint, aop-audio, aop-voicetrigger, accelerometer,
  gyroscope, ambient-light and lid-angle services. All twelve application
  endpoints acknowledge queue startup, including the standard gyroscope queue.
  Only the external-code boot argument needs changing for this startup probe.
  ALS interval enable/disable, HID-descriptor and manufacturer queries return
  success, but no ALS samples were observed in a two-second capture. Periodic
  lid-angle reports from `las` now use the Asahi driver's angle byte and
  return a live raw value of 112. Response to lid movement remains untested.
  Reports from `cma` were captured; their format is unqualified.
  Fresh AFK shutdown responses are not received within the bounded wait.
  Earlier tests incorrectly counted the initially false `alive` flags as ACKs.
  IOP quiescence and sleep trials cause a firmware instruction abort. Mappings
  must remain installed until reboot on this
  failure. ALS and inertial measurements, audio paths and IOP shutdown remain
  unqualified.
  The native audio directory contains 33 devices and rejects index 33.
  `hpai`, `lpai` and `pdm0` attach successfully. Both input frontends return
  `idle` before and after attachment; their main-property replies list separate
  dependency chains. These control queries produce no DART fault.
  Inline PDM configuration queries are rejected. A shared-buffer command read
  returns 626 bytes for `pdm0` property 200; the legacy client structure expects
  631 bytes and is incompatible with the captured layout. Four trailing
  24-byte blocks match the separate property 206-209 replies. Earlier
  fields, timing units and setter ABI remain unqualified; no PDM configuration
  writes are made. The production shared-buffer command client also reads
  8,200 bytes for `pdm0` properties 211 and 212, with identical payloads in
  this capture. The existing high-frequency decimator decoder consumes and
  rebuilds both payloads exactly; property 212 also passes the typed response
  reader on a fresh live reply. The decoder reports latency 36, ratios
  10/5/3 and 386 coefficients. No new decoder is needed; setter behavior and
  changes to HP source configuration remain unqualified.
  It accepts command envelopes carried by subtype 0xa0 while preserving typed
  notification replies, validates request and response buffer bounds, performs
  cache maintenance and clears pending state on failure. The live command
  getters complete with both input frontends idle and zero DART faults.
  Typed property replies
  use the request's device and modifier when decoding their payload; live
  frontend-state queries and recovery after a rejected request build pass.
  Error replies contain only the firmware return code; the decoder preserves
  it without trying to parse a configuration payload. Successful payloads
  are bounded by the reported length. Live PDM queries return 0xe00002c2
  or 0xe00002f0, followed by a successful frontend-state query.
  `AudioPropertyFormat` uses the same reply envelope and returns `retcode`,
  `len` and raw `data`. Its old PCM field names incorrectly interpreted the
  length prefix as a format code. Live property 302 replies contain 24 bytes
  for `lpai`, four-byte `idle` for `a2px`/`adpx`, and four zero bytes for `dvpx`;
  their meanings remain unqualified.
  The J616s 25G76 AOP image matches the running ADT UUID and segment layout.
  Its metadata identifies the T604x audio driver; the main PDM configuration
  setter ABI remains unqualified. The 16-byte `lpai` property 301
  reads successfully; writing the same bytes returns 0xe00002cf (not writable).
  It cannot serve as a configuration-write template.
  A read-only input property scan also returns three words of value 7 from
  `lpai` property 300, matching the ADT channel masks. The matching firmware's
  initializer and validator identify a 12-byte structure ordered as supported,
  enabled and history channel masks. Its write handler requires that size and
  validates the values against the board configuration. An exact GET-byte SET
  returns zero; GET readback remains `(7, 7, 7)`, both input frontends remain
  idle and DART faults stay zero. Alternate masks are unqualified. All 105
  getter requests complete without DART faults.
  Matching firmware identifies `lpai` property 303 as latency in frames (320)
  and property 304 as safety offset in frames (672). Property 305 contains a
  48-byte `IOAudio2Status`. In 20 active captures, its second u64 advances as
  a frame counter and the last u64 equals `(counter + 1) * 12 % 768000`.
  Other fields and exact clock behavior remain unqualified.

  LP input capture works with an owned destination configured before startup.
  The ADT assigns the `adpx` buffer 768,000 bytes on DART stream 8. Zero a
  page-rounded allocation, install and verify four-level identity mappings,
  then SET `adpx` property 300 with a 16-byte address/capacity pair. Readback
  returns the owned address and normalized size 768,000. Public attachment of
  `lpai`, `pdm0` and `adpx`, followed by `lpai` `pw1 `, succeeds. Attach `lai `,
  then request `runn` on `lai ` and `lpai`; both return matching state readbacks.
  Earlier startup attempts without the buffer caused null-address aborts.

  Twenty native memory copies at half-second intervals produce contiguous
  observed update regions. Including an inferred initial 8,000-frame window,
  reconstruction produces a normalized ten-second mono preview at 16 kHz.
  The tester confirms recognizable speech; exact snapshot-boundary integrity
  remains unqualified. This qualifies the tested LP input path, not three
  independent microphones or
  exact sample-clock accuracy. Candidate lanes 0/2 largely duplicate each
  other; lane 1 has DC plus correlated signal. Allocation padding stays zero,
  DART reports no faults and CDC remains responsive.

  Stop requests `lpai` `pw1 ` and `lai` `idle` return success with matching
  states; the destination then becomes zero and stable. Copy data before stop.
  The buffer setter publishes its address before later validation, and public
  ownership release is not established. Retain the buffer, heap, AOP and all
  mappings on every attempted handoff until validated watchdog cold recovery.
  Recordings, firmware and private probe harnesses are not included here.

  HP microphone capture also contains user-recognized audio. With LP report
  readiness established, attach `hpai`, request `pw1 `, publish an owned
  stream-10 mapping and start NS RX0 before requesting `pwrd`. The tested
  channel uses the split RX bank and one non-repeating descriptor. Complete
  2 MiB and 6 MiB captures pass fresh full-range translation, matching report
  IDs, zero residue, zero DART faults and two identical full reads before RX
  stop. The 6 MiB recording contains 524,288 three-channel Float32 frames at
  the nominal 48 kHz rate, lasting 10.92 seconds; the tester confirms it works.

  Both captures begin with 7,211 zero frames, about 150 ms. Earlier 16 KiB
  captures were too short to cover that observed interval at this format.
  Longer capture windows produce samples without changing the DMA provider,
  clocks, coefficients or native stream-classification commands. Stop RX
  before returning the frontend to `idle`; retain AOP, buffers and mappings
  through watchdog recovery because ownership release remains unqualified.
  This qualifies cold captures, not continuous streaming, restart, calibration
  or measured sample-clock accuracy. The public `aop_capture.py` command now qualifies both cold extents;
  continuous streaming and restart remain outside its contract.

  Existing LEAP tools decode the pinned Apple program and identify three float
  outputs on port 41. The older Asahi driver supplies read-only execution
  counters; a routine-enable readback alone does not prove execution. Neo's
  tested capture provides useful timing and lifecycle evidence, but its DMA
  provider differs from the NS provider advertised by J616s and is not a
  transferable address assignment.
* Headset input `cin ` reaches `pwrd` with the exact 109-byte MCA profile.
  The working macOS profile matches these bytes, including `ms02`, `syn2`,
  mono 24-bit samples in 32-bit slots and a nominal 48 kHz rate. Native
  CS42L84 input uses a 32-bit TX word (`ASP_TX1_CTRL = 0x1f0001`).
  Matching that word alone still produces static. Initializing the documented
  `MIC_DET_CTL4.LATCH_TO_VP` bit before detection produces user-recognized
  phone music in the proxy capture. The cold latch changes from 0 to 2 with
  readback; other bits are preserved.
  The private harness maps an owned 2 MiB buffer on DART stream 8 while
  preserving stream 0 and submits one non-repeating RX2 descriptor. The
  report matches its ID with zero residue. Two full copies taken before RX
  stop match; all six RX2 configuration fields restore exactly. This
  qualifies one cold acoustic capture, not restart, calibration or a measured
  sample clock. The recording uses the nominal 48 kHz profile.
  Separate detection-mode and bias restoration succeeds after the full
  capture, followed by exact codec/controller restoration and frontend idle.
  Earlier I2C FIFO timeouts occur with a growing DMA checkpoint serialized
  between MMIO operations. Compact codec journals preserve pending-state
  evidence and pass the longer restoration trial. Transport timing and
  restart remain under qualification. AOP shutdown still needs watchdog
  recovery;
  installed images are preserved. Native macOS speaker traces supplied the
  protection and transport references for the separate playback tests below.
  The shared `CS42L84Input` class supplies the validated input-only codec
  lifecycle through bounded caller-provided register access. It requires
  receiver-start and receiver-stop gates, preserves unrelated register bits,
  and retains uncertain state without further access. The board power/reset,
  fresh MCA-profile check and owned DMA orchestration remain caller-owned.
  A live capture using this shared class passes exact cleanup as above.
* Headphone playback through `cout` is now user-confirmed. Its live 109-byte
  stereo MCA profile matches the native macOS profile. An owned, non-repeating
  TX2 descriptor plays a two-second sequence with a 440 Hz left-channel tone
  followed by an 880 Hz right-channel tone. The tester confirms audible output.
  The nominal format is 48 kHz, 24-bit samples in 32-bit slots.

  The native TX FIFO calculation requires the upper almost-full threshold to
  exceed the lower transfer threshold. The tested full-depth policy uses
  `0x03000180` for the owned 768-byte FIFO; the previous zero upper half stalled
  TX. The matching completion report, zero residue, empty rings and zero DART
  faults pass. TX configuration, codec controls, I2C controller/reset state and
  frontend power restore successfully before watchdog cold recovery.

  The test preserves voltage/impedance fields and uses a raw DAC gain of
  -60 dB. Compact per-operation I2C journals retain pending-state evidence
  while avoiding growing snapshots between FIFO bytes. Headset capture and
  headphone playback are qualified separately; simultaneous duplex,
  continuous playback, calibration and Linux audio remain unqualified.
* All six SN012776 speaker amplifiers pass native identity reads and a bounded
  protected configuration test while remaining in shutdown. The test completes
  534 byte writes, checks page 0, mode 0x82, identity 0x30 and digital volume
  0xc9 on each amp, and restores both I2C controllers and the shared reset GPIO.
  No output is enabled during that test. Native `spkr`/`ms00` playback uses six
  24-bit samples in 32-bit slots. A separate cold test checks its exact profile
  and power transitions, then completes one 1,152,000-byte zero-data TX0
  descriptor with all amps held in reset. Its report, zero residue/DART faults,
  configuration and frontend restoration, and watchdog recovery pass.
  A later bounded left-woofer pulse is user-confirmed audible: one second of
  300 Hz, peak 0.125 and DVC attenuation of 60 dB. Matching TX completion,
  zero residue/DART faults, muted stop, amplifier/controller/GPIO restoration
  and watchdog recovery pass. Its measured active interval is 1.219 seconds.
  A separate right-woofer-only pulse at the codec control midpoint (DVC 0x65,
  50.5 dB attenuation) also passes transport and cleanup and is user-confirmed
  audible. The other five amps remain in their verified initial shutdown.
  A subsequent paired first-woofer test is user-confirmed audible for five
  seconds. Its finite 5,760,000-byte TX0 descriptor completes with zero residue
  and DART faults; both amps mute before TX stops and restores its fields.
  An I2C timeout during the later amplifier shutdown wrapper prevents complete
  cleanup. Mappings remain retained until successful watchdog recovery.
  A subsequent observed-status-clear sequence completes all 534 shutdown
  writes with exact controller/GPIO restoration in 7.1 seconds. The second
  left/right woofers and both tweeters then each pass an individual one-second
  pulse, matching TX completion, zero residue/DART faults, muted stop, full
  amplifier/controller/GPIO restoration and watchdog recovery. The user
  confirms tones from every driver. Woofers use 300 Hz; tweeters use 2 kHz.
  The second woofers and left tweeter use DVC 0x65, while the right tweeter
  uses the requested lower setting, DVC 0x78 (60 dB attenuation). These tests
  qualify all six individually, not simultaneous full-array playback.

  Python I2C status clearing now writes the observed status word and refuses
  an active transfer. This follows native and upstream Linux handling;
  undefined status bits remain intact in the snapshot. Eight host regressions
  and direct live calls on both idle speaker buses pass. The bounded private
  amp trials keep their existing error and XEN/no-XIP completion gates.
  These successful runs do not prove that intermittent timeouts are cured.
  Native DMA format selection, feedback, calibration and continuous playback
  remain unqualified.
  See the driver contracts for initialization order and the tested AP policy.
* MTP completes RTKit startup, answers a management ping and acknowledges
  AP/IOP quiescence. The ADT selects DART stream 0, unlike the older MTP
  experiment's stream 1. Four-level mappings and ADT DAPF ranges are restored
  after shutdown with no DART fault. Passive DockChannel capture receives six
  checksum-validated initialization packets with keyboard and multitouch HID
  descriptors. The optional keyboard probe sends enable only to the announced
  keyboard interface 2; its zero-status command ACK and fresh DeviceReady
  response pass on repeated cold boots. The optional touchpad probe uploads the
  tested J616s 26A428 firmware and patches its interface slot. It first enables
  the announced actuator interface and requires its fresh DeviceReady response,
  then requires upload ACKs, four version-2 power-request replies and touchpad
  DeviceReady. A sensor-dimensions query returns sizes 15600/9600 and coordinate
  bounds X -7456..7976, Y -163..9283. Human movement tests produce report 0x75
  frames with changing coordinates; earlier runs without actuator enable
  produced only startup reports. Enabling keyboard only after touchpad readiness and dimensions gives a
  combined capture of 16 keyboard reports and 1791 touch frames, including
  two- and three-contact frames, button transitions and pressure changes.
  Enabling keyboard before touchpad startup produced neither input stream.
  Both final touchpad Off replies, whole-MTP AP/IOP quiescence and DART/DAPF restoration pass with the actuator enabled.
  The human tester confirms normal haptic clicks during touchpad-only and
  combined tests; no host actuator output reports are sent. Separate actuator
  output control remains unqualified.
* SEP's existing RNG routine returned zero bytes. Successful SEP communication
  is not established.
* ISP revision `0x100003` was read after powering the ADT parents and the
  ISP_CPU/ISP_FE global slots at offsets 0x4000/0x4008. All tested domains
  returned to their original states, including ISPSENS0 state 4. A separate
  readback trial confirms ISP_SYS and ISPSENS0 reach active mode through the
  existing ADT parent traversal; missing parent power is not the cause in this
  sequence. A guarded proxy trial now receives the first firmware handshake
  with full 42-bit SID0 mappings and native stream aliases: SIDs 1, 6, 7, 10
  and 11 use TCR `0x80` on all three DARTs, with enable mask 1. The previous
  trials left these streams in separate contexts with empty tables. The
  firmware announces seven channels, queue offset `0xef40`, descriptor flags
  2 and an extra-heap request of `0x6500000` bytes. Its bootstrap metadata
  changes, with zero DART faults and CPU exceptions. Watchdog recovery returns
  to macOS after a one-time aura boot, or to CDC after aura is selected as
  default. The modern `0x290`-byte boot block is accepted. All seven channel
  descriptors parse with 64-bit ring addresses in the owned IPC allocation,
  and the requested `0x6500000`-byte heap is mapped on all three DARTs.
  Type-0 ring initialization and the third boot handshake also pass, with
  zero faults. The owned allocation broker and CONFIG_GET pass. Native
  T604x DSID configuration, START, IMX958 identification and all 13 preset
  queries also pass. RAW receiver configuration and buffer submission are
  acknowledged, and CH_START produces fresh pixels in two owned surfaces.
  A later RAW trial acknowledges CH_STOP while servicing shared allocations,
  buffer returns and bounded terminal batches. Two complete 4,644,864-byte
  reads after stop match byte for byte and produce a diagnostic camera image.
  No completed RAW buffer report was received; stable memory after stop does
  not qualify frame completion or continuous capture. Per-module lens-shading
  queries return a valid 19-by-17, 8-bit grid. Its mapping to the RAW crop is
  unqualified. A private processed-preview trial enables the native local RAW
  allocation path and receives two completed pool-9 output reports. The native
  firmware exports zero-extended 32-bit plane addresses; each report matches
  one submitted surface uniquely by both addresses, plane count, pool and tag.
  The 1280-by-720 output uses 2560-byte rows and MSB-aligned 10-bit samples in
  two planes. Two complete reads of one returned surface match before reuse,
  and CH_STOP acknowledges. Video-range BT.709 rendering produces a coherent
  colour image. A bounded reuse trial returns six distinct frames across
  three generations of both output surfaces. Fresh tags identify each lease;
  completion acknowledgment precedes reads, and two matching full reads
  precede resubmission. Metadata refill, CH_STOP, zero DART faults and zero
  CPU exceptions also pass. The private capture consumes shared profile,
  boot-block, lease, mapping, startup, broker and command components. The
  mapper verifies 42-bit DART geometry, read-only TEXT, stream aliases and
  table readbacks before launch. Startup configures DAPF and GPIO, completes
  both handshakes, publishes the boot block and validates channel ownership.
  Phase order, fresh CONTROL/GPIO checks and attempt latches prevent repeated
  starts; failure retains resources. The broker rejects allocation flags
  before forwarding and retains retired storage. Command submission validates
  ownership and deadlines before publication, rejects late ACKs and captures
  raw replies before status checks. Shared sensor discovery identifies IMX958
  with 13 presets and bounds preset queries. A live capture through these
  components returns six full stable P010 frames with clean stop and fault
  checks. Shared T6040 preview configuration validates the IMX958 replies
  before issuing the tested P010 setup sequence and rejects retries after
  an ambiguous failure. A live capture through this component returns six
  full stable frames, clean stop, zero faults and a passing NOP.
  Power/residency qualification and capture pool/stream orchestration
  remain private. Capture wire builders validate pool/plane fields, 42-bit
  submitted addresses, opaque tags and report bounds. Their bytes match
  saved live submissions; they do not establish mapping or lease ownership.
  A later three-generation run returned four stable frames, then timed out
  with both output leases submitted and the current completion slot idle.
  Servicing control traffic during downloads received all six reports, but
  the host time limit interrupted the final copy. Neither run qualifies
  reliable streaming or a stall fix. A follow-up services at most one
  terminal message between pixel chunks while retaining the eight-message
  quota for normal polling. It completes six full stable frames, CH_STOP,
  zero DART faults and zero CPU exceptions, with NOP passing. Sustained
  reuse with this schedule also completes 80 frames: every frame has two
  matching 4096-byte sample reads, and the first/last surfaces have full
  matching double reads. Both rings wrap; CH_STOP, zero faults and NOP pass.
  This bounded run does not establish long-term reliability or sensor FPS. An earlier
  40-generation trial timed out at CH_START before returning frames; its
  command payloads and first 96 allocation requests match the successful
  bounded run. Timing of host allocation service remains under investigation.
  Watchdog recovery returned the installed proxy with NOP passing.
  A subsequent instrumented run completes 80 frames across 40 generations,
  wrapping both buffer rings. All 80 returned surfaces pass two matching
  4096-byte sample reads; the first and last generation of each surface also
  pass two matching full reads. CH_STOP, zero DART faults, zero exceptions
  and proxy NOP pass. Host request servicing consumes about 26 seconds of
  the 30-second startup deadline, plus about three seconds of terminal
  draining. Reliable startup timing, real-time frame rate, colour calibration
  and the complete production camera interface remain unqualified. A combined RAW/YUV trial exceeds
  the retained shared-memory aperture and refuses the allocation before
  mapping it. Watchdog recovery remains required after these private probes.
  The captured boot firmware is `mBoot-18000.161.9` from macOS 26.6.1 (25G76).
  Its ISP UUID and segment ranges match that restore image; macOS 26.6.2 and
  the installed macOS 27.0 ISP image have different UUIDs. The version table
  recognizes 26.6.1; other ISP firmware profiles remain unqualified.
  Host regressions cover initialization followed by shared-buffer allocation.
  Surface allocations now start beyond the fixed firmware mappings, preserving
  the configured IOVA bounds and a guard page. Captured J616s segment sizes
  combined with a synthetic legacy extra-heap request reproduce an overlap
  with the old allocator start. The live T6040 extra-heap request is larger
  than that synthetic request. The guarded boot-block trial qualifies its
  allocation and mapping; combined RAW/YUV capture remains unqualified.
  Required boot-argument and channel-message imports are explicit.
  Channel descriptors retain their 64-bit ring addresses at offset 0x50.
  The matching firmware writes that field with 64-bit stores in all seven
  0x100-byte records. Host tests cover legacy addresses and addresses above
  1 TB; live descriptor reads now confirm these high ring addresses.
  A subsequent cold 1920x1080 P010 trial queues one owned pool-9 surface
  while keeping native pool capacity two, the 256 MiB shared aperture and
  retained-allocation budgets unchanged. One uniquely matched completed
  report arrives; both full 6,242,304-byte reads match before CH_STOP.
  The stride is 3840, with aligned Y/UV extents 0x3f8000/0x1fc000. All three
  DART errors and CPU exceptions are zero; NOP and watchdog recovery pass.
  This qualifies one completed 1080p frame, not continuous 1080p streaming,
  restart, exposure/noise calibration or measured FPS.
* All six speakers have user-confirmed individual tone output. Feedback,
  calibration, simultaneous full-array and continuous playback remain
  unqualified; the separate headphone path also has finite output.

The experimental stage 2 was chainloaded into RAM. Installed boot images,
boot policy and partitions were not changed. Preserve a working CDC recovery
path before experimenting with further devices.

## Latest SuperSpeed and higher P-state boundaries

A RAM-only T6040 USB candidate completed the standard CIO80 RTKit sequence:
management version 12, the complete endpoint map, system endpoint 1, its
preallocated crashlog request, IOP/AP power acknowledgements at state `0x20`,
application endpoint 32 and the tunables transition from `0x881` to `0xc81`.
Reset release and the prepared PIPE-state checks passed. Moving the checked
PIPE switch before DWC3 USB3 PHY resume did not produce a 5 Gbit/s device. A
second diagnostic recorded all 15 controller reannouncement stages before the
link was lost: halt, reset, PIPE switch, USB3 resume, device configuration,
scratch and event setup, endpoint setup, `RUN_STOP`, and DSTS unhalt all
completed with return value zero and no CPU exception. No SuperSpeed device
enumerated on either xHCI bus before watchdog recovery. Three read-only status
samples over the following millisecond stayed at DSTS `0x00920004`: the
controller was running with its SuperSpeed field selected but the link stayed
in `SS_DISABLED`. The PIPE mux read `0x8`, its overrides were released and the
CIO tunables indication remained `0xc81`. Releasing the acknowledged PIPE lock
changed its readback from `1/1` to `0/0` but did not change DSTS or enumeration.
The installed proxy
returned with NOP passing and both watchdogs disabled. This rules out the
incomplete RTKit lifecycle and the tested DWC3 reannouncement sequence as the
remaining cause; investigation now belongs at the USB3 PHY link-training,
PLL, receiver-detect and lane state. Do not repeat either unchanged activation
or transplant the M5 Pro fixed-DisplayPort tables. SuperSpeed CDC remains
unqualified.

The same port, cable and host negotiate 10 Gbit/s when macOS exposes its native
USB device. A read-only native snapshot supplied exact DWC3, PIPE and PHY
steady-state references. Matching the native DWC3 power-down scale advanced
the RAM diagnostic from `SS_DISABLED` to Polling, but selecting the Gen 2
device-speed code did not complete training. A later candidate restored the
three Apple CIO timer values used by the upstream DWC3 glue after reset. All
three values retained, GCTL and GUSB3PIPECTL matched the native snapshot, and
DSTS remained in Polling without a SuperSpeed enumeration. Those unchanged
sequences are qualified negative results.

Offline source and ADT analysis bound the J616s default tables used by native
`AppleT8132USBXDCI::applyPlatformTunables`: 13 records for the DWC global/debug
windows, bulk fabric, PIPE handler, link registers and AUSBC debug block. A
RAM-only experiment explicitly applied all six present groups after validating
the complete tables, register layout and every masked readback. Host fault
injection covers all 39 table accesses and rejects missing, malformed or
misaddressed groups before any table writes. The experiment reached stage 15
without CPU exceptions; the observed global/debug/link values matched a fresh
native macOS snapshot at 10 Gbit/s, but SuperSpeed still did not enumerate.
The explicit table application therefore does not establish a functional fix.

A further diagnostic ran the ordinary USB event handler during a one-second
observation. DSTS stayed in Polling at nominal 50, 250 and 1000 ms; the lane
state remained `0x0600242a` and the sampled pending-event counts were zero.
The watchdog restored the installed 480 Mbit/s proxy with NOP passing, zero
CPU exceptions and both watchdogs disabled. The remaining link-training
prerequisite is unresolved. These experimental changes remain isolated.

T6050 fixed-DP PR #248 at `440f79f9` provides a useful structural comparison:
all 24 core-bank offsets match the J616s USB0 ADT, and its nine fixed common
tunable writes match the source-bound T6040 mode-4 helper. Its sleep/clamp
handshake, tunable order and failure handling corroborate existing work.
Its reset request uses wrapper offset 0x2c; the matching T6040 USB source uses
0x28. The DP lane/crossbar, AUX, link-rate and drive-training operations must
not be transplanted into USB. Its init sequence also lacks the USB-specific
CIO80 boot and tunables-readiness gate. The PR's host validation does not
establish working T6050 display hardware or T6040 USB training. Wrapper-power
ordering remains a source-review lead, not a qualified new register sequence.

Native wrapper-power dispatch reaches the PMGR clock-gate service. A cached
state match can skip the hardware transition; the observed active gate alone
does not establish that match or justify cycling the wrapper domain.

A separate RAM diagnostic kept USB3 SUSPHY set through PIPE routing, cleared
it before programming the native GCTL power-down scale, and completed stage 15
without CPU exceptions. DSTS then remained in RX.Detect at 50, 250 and 1000 ms,
with no SuperSpeed enumeration. CIO, debug and link-register readbacks were
zero, despite passing the earlier programming checks. This does not distinguish
lost register state from a change in register access. The watchdog restored the
installed high-speed proxy. The resume and clock-programming order needs further
source review; this experiment is not a production fix.

Programming the same GCTL scale before PIPE routing also did not establish
SuperSpeed. Guarded snapshots read LFPS offset `0x0f800f80` immediately before
and after USB3 resume, with the expected SUSPHY transition. The sequence then
timed out waiting for EP0 SETEPCONFIG to complete and stopped before RUN. No
post-failure register accesses were made, and watchdog recovery passed. This
run does not show an immediate LFPS register loss at resume; it leaves the later
controller command and register-access prerequisites unresolved.

The generic DWC3 endpoint-command path clears USB2 ENBLSLPM along with SUSPHY.
A guarded J616s experiment refused to change ENBLSLPM because it was already
clear; later snapshots confirmed that bit clear in `GUSB2PHYCFG=0x01002400`.
There is no qualified ENBLSLPM fix.

The own native controller checks RAM0_WR_ACTIVE at USBCTL offset 0, bit 15,
and USB31DRD_PIPE_PHY_READY at offset 0x28, bit 30, when the PIPE mux selects
USB3. The ADT maps this bank at `0x382200000`. A read-only diagnostic observed
RAM0 idle and PHY_READY initially clear, then asserted at nominal 50, 250 and
1000 ms. The link remained in Polling. A later candidate waited for PHY_READY
within the native 100 ms deadline before USB3 resume. That wait passed, but
EP0 configuration still timed out afterward. That run did not observe readiness
across resume and did not qualify a power, reset or command-timing fix.

A guarded follow-up observed PHY_READY asserted both immediately before and
after clearing USB3 SUSPHY, then stopped before endpoint commands. This rules
out loss of that readiness bit as the immediate cause of the EP0 command stall.
The relative ordering of USB3 resume and GCTL scale programming remains under
RAM-only qualification; no ordering is production support yet.

Waiting for PHY_READY, clearing USB3 SUSPHY and then programming the native
GCTL scale completes endpoint configuration and starts the controller. Reversing
the last two operations stalls the first EP0 command. With the command-completing
order, DSTS remains in RX.Detect for one second and no SuperSpeed target appears.
This narrows the remaining gap to link/electrical training after controller
startup; it does not qualify SuperSpeed CDC or a production ordering change.

Native `AppleT8132USBXDCI::controllerReset` additionally waits for its ADT
bank-4 offset-0x20 status word to become zero after PHY_READY. A guarded
observer found PHY_READY asserted and that word already zero both before and
after USB3 resume. The remaining RX.Detect failure is beyond these reset-status
checks. Native T6040 crossbar and lane-mode writers preserve core offset 0x6c
bit 20 instead of setting it, so its native steady-state difference is treated
as status until a producer is source-bound; direct writes remain unqualified.

A read-only ACE3 data-status query reports USB3 connection capability with
attachment and orientation unchanged. This is port-controller status, not
traffic proof. A further RAM candidate restores USB3 SUSPHY after endpoint
setup and before RUN, matching the native steady-state register value. It
still completes controller startup but remains in RX.Detect through one
second, with no SuperSpeed device or bulk traffic. Watchdog recovery returns
the installed proxy at 480 Mbps. Neither observation qualifies a PHY fix.

Timed RAM observations narrow a separate controller-state change. The three
legacy CIO timing words retain their programmed values for 10 ms after USB3
resume, but become zero within the observed interval after the native GCTL
scale update. DCFG remains unchanged and the observer stops before endpoint
commands. Native platform initialization supplies scale 13 as a fixed default.
Neither the timing loss nor the legacy timing values are established causes
or requirements for T6040 SuperSpeed; no production change follows from them.

The native USB3 power-on path routes PIPE before controller reset.
A guarded RAM comparison now completes that order and retains USB3 SUSPHY and
PIPE mux 8 after reset, then stops before reset-event cleanup. Holding the source
serial descriptor until USB disappearance avoids the last-close CDC request
seen in failed halt runs; the retained-descriptor runs reach the halt and reset
checks. This does not yet qualify SuperSpeed enumeration or traffic.

The full RAM continuation of that reset order reaches endpoint setup and RUN
with no CPU exception. It preserves USB3 SUSPHY and the legacy CIO values
through the sampled startup stages. The host still sees only a 480 Mbps RAM
epoch, followed by independently admitted watchdog recovery to the installed
proxy. This qualifies controller startup and observed CIO retention, not a
working SuperSpeed link. Native platform defaults request CIO settings through
an optional property absent from the admitted J616s ADT; the older literal
values remain experimental rather than established T6040 requirements.
An omission comparison also reaches RUN with zero LFPS/bandwidth offset words,
but still enumerates only at 480 Mbps. It does not establish those legacy
defaults as the missing link fix.
A separate Gen1 request comparison also reaches RUN and falls back to 480 Mbps.
Restricting the request from Gen2 to Gen1 is not sufficient to establish a
physical SuperSpeed link; the remaining PHY/link behavior needs qualification.
A source-bound configured-PCLK getter reports its predicate set before routing,
after RUN and through the one-second link observation. This specific predicate
is not missing. The changing low DSTS bits are SOF-counter changes; the sampled
link state remains RX.Detect. These observations do not establish every clock
prerequisite or a physical link fix.

Native macOS negotiates 10 Gbit/s in both plug orientations. After returning
to the original orientation and unlocking macOS, USB0 resumes in U0 on lane 1.
Four individually gated common-PHY reads then report CFG `0x1001d110`,
lane control `0x0610242a`, crossbar `0x111` and UC status `0xc81`. CFG,
crossbar and UC status exactly match the failed Gen1 RAM candidate. Lane
control differs only at offset 0x6c bit 20, whose producer and meaning remain
unqualified. The native lane-0 values differ in additional fields, so those
earlier differences do not establish missing lane-1 configuration. This
comparison matches orientation but not negotiated rate or startup history;
it does not justify copying the native lane-control value into the proxy.

A fresh RAM comparison omits the historical full-word snapshot writes to
PIPE offset 0x44 and PHY core offset 0x6c. The same frozen image reaches stage
15 with result 0 and no exceptions, but still falls back to 480 Mbps and stays
in RX.Detect through one second. Lane control becomes `0x0600202a`; the other
three sampled common-PHY words retain their earlier values. Watchdog recovery
returns a responsive installed proxy with both watchdogs disabled. These two
snapshot writes are unnecessary for the observed startup, and their omission
does not resolve the physical link failure.

With the snapshot writes still omitted, a further frozen RAM control changes
only the early-route configured capability from Gen1 to Gen2. DCFG reads back
`0x200805`, and startup again reaches stage 15 with result 0 and no exceptions.
The host still sees 480 Mbps; RX.Detect and the sampled common-PHY words remain
unchanged through one second. Cold watchdog recovery passes. Neither speed
request establishes a physical SuperSpeed link; configured capability alone
does not explain the failure.

A read-only native reference now resolves PHY0's CPU phandle through the
current registry to an actual `RTBuddy(ACIOPHY0)` service and independently
retained `AppleA7IOPNub`. The loaded UUIDs, allocator and code windows match
the saved source. Paired scalar reads confirm the nub relationship, a clear
standalone selector, current state 8 and a clear transition flag. For the
PHY's foreign-owner state-1 request, this selects the managed power action;
it does not select the standalone `startCPU` path. This is a current-state
observation, not proof that any earlier managed transition completed. The
native link was independently observed at 10 Gbit/s with controller power 4.
No power method, register write or pointer-following operation was performed.
A separate retained PHY-driver observation matches its CPU pointer to that
RTBuddy object and reads initialization-guard bit 0 set. The native initializer
sets the flag without checking the hardware initializer's return value, so the
flag does not establish electrical success or earlier transition completion.

A further RAM comparison omits only the initial one-second UC-tunables poll
before RTKit negotiation. It preserves RUN, the settling delay, every register
write, full startup, the final tunables gate and reset-release order. Startup
and the final gate pass, and activation reaches stage 15 without exceptions,
but the link remains in RX.Detect at 480 Mbps. Watchdog recovery passes. The
extra initial wait is not the observed link fix; native provider power-state
handling remains a separate sequence-equivalence gap.

An exploratory comparison starts advertised lane1 endpoint 34 once after
fresh power acknowledgements and the existing endpoint-32 start. It sends no
application payload. A bounded observation receives no traffic, and the final
tunables gate and activation pass, but USB remains at 480 Mbps in RX.Detect.
Cold recovery passes. This does not establish lane endpoint support or prove
that macOS enables the lane services; registration alone is insufficient.

A native macOS reconnect capture records the driver closing ACIO before USB2,
then opening USB2 before ACIO. The successful ACIO request uses lane 1,
`multifunction` mode, orientation 1 and power level 2. Both open requests
return zero, and independent host enumeration returns at 10 Gbit/s. Native
open/configuration logging was enabled temporarily with mask `0x91`; the
original mask `0x1` was restored and read back afterward. No kext replacement
or register writes were needed for this capture.

This establishes the native driver sequence for a warm cable reconnect. It
does not trace firmware-internal activity or prove that a cold aura startup
has the same firmware lifetime. In particular, the short native ACIO open
does not establish whether it restarts the PHY firmware. The proxy still
requires a separately qualified physical SuperSpeed link.

A second reconnect also samples the retained RTBuddy object in software
states Sleep (2) and Running (8). Native logs show a transient XHCI owner
before returning to XDCI and 10 Gbit/s. These samples have gaps and do not
prove physical power state or uninterrupted firmware execution. The native
managed wake path can still issue RUN `0x10`; the state observations do not
justify omitting that command.

An exploratory RAM comparison completes startup after the three validated
power messages and one nonconsuming empty-outbox check, instead of waiting
the full 500 ms collection window. The observed collection takes 28.72 ms;
the final tunables gate and activation pass without CPU exceptions. USB
still returns at 480 Mbps in RX.Detect, and watchdog recovery passes. The
extra collection delay is not the observed physical link fix.

Bounded 32-bit reads of the active PHY firmware's identification block report
`AppleTypeCPhyFirmware-270~1498`, `RTKit_release-3514.0.15.release` and UUID
`a42150fc-8c1f-3bd7-a707-1ba93f9660a8`. The local macOS firmware payload
has the same UUID, version strings and all 48 observed words. It is distinct
from the older 25G76 firmware reference, which reports version 262.100.2.
Use the matching version for further firmware analysis. These observations
identify the declared firmware build; they do not establish a hash of every
executing byte or qualify the SuperSpeed link. Watchdog recovery to the
configured macOS default passed after each read-only probe.

The matching firmware reports current and requested power state `0x21` after
the proxy startup. Its startup code requests that state itself; the absence
of an additional host power request does not establish a missing wake step.

A subsequent comparison reads the firmware's lane-mode caches, selected
lane kinds and main work-queue handler. Native macOS at 10 Gbit/s and the
proxy preparation agree: caches `2/2`, kinds `2/1`, handler `0x115c65` and
queue counts zero. The handler follows a common-initialization call, but
that call has an early bypass and its caller does not check a return value.
Neither the handler nor an empty queue proves electrical initialization.

Two additional firmware words at IOP addresses `0x1000f800/804` are cleared
by that bypass and set by the common body's final path; a separate path can
clear the second word. Both native macOS and proxy preparation read `0/0`.
These sequential observations describe state at sampling time, not a full
execution history or PLL-lock result. They do not justify forcing the
common-initialization path. Physical SuperSpeed remains unqualified in the
proxy despite these matching firmware states.

The separate per-lane scheduler does differ. On the active native lane 1,
the sampled priority is `0x14`, last action `0x4b` and next action zero.
Both proxy lanes instead report priority `0x1f` and last/next action zero.
The matching firmware gates its lane action dispatcher on priority below
14; the sampled native values therefore describe prior action progress,
not work currently eligible to run. Action `0x4b` can repeat or return to
next-action zero and is not itself a PLL-lock indication. This difference
narrows investigation to the lane event/action path. It does not establish
which native event is missing or justify an invented endpoint payload.
The first proxy reading was taken before controller activation. A subsequent
capture after activation and USB re-enumeration confirms the same priority
and action values at 480 Mbps. It rechecks the complete executable, boot
metadata, firmware identity and power state while preserving the original
watchdog. The difference therefore persists after activation, but the event
or hardware condition responsible is still unproven.

A post-activation read also checks all five controller words from the native
reference: `c164=9cc20024`, `d800=8`, `d020=4e928162`, `d024=008b8080` and
`d060=600`, relative to controller base `0x382280000`. All five match while
the proxy remains at 480 Mbps. This excludes a late change to these particular
values; it does not establish equivalence of the complete controller state.

One RAM-only candidate repeats the native post-reset read/OR/write of bit 15
at PIPE offset `0x20`. The previous sequence set that bit before reset. The
candidate retains the existing reset and changes no other hardware operation.
It still returns at 480 Mbps with unchanged lane-action and controller
observations. That ordering change alone is not the link fix.

A separate watchdog-preserving RAM candidate raised only its diagnostic
cpufreq ceiling from state 2 to state 3; the production ceiling stayed at 2.
The candidate image and build configuration matched before testing. Cluster 0
started at state 1 but rejected the state-3 proxy request, so clusters 1 and 2
were not touched. The watchdog restored the installed proxy with NOP passing
and both watchdogs disabled. This is not evidence that the third ADT OPP is
safe to expose. Production continues to permit only the live-qualified
state-1/state-2 transitions.

## Proxy-client DART fixes

Rejected mapping requests previously enabled a stream before validating its
mode and alignment. New stream publication now follows populated tables. A
four-level root-reuse path referenced an undefined variable in an unnecessary
assignment; removing it permits repeated mappings under the same root. Eight
host cases cover rejected requests, publication order, three/four-level
translation, reused roots and additional leaf allocation. Live three-level
firmware mapping/readback and restoration passed. SIO's successful cold
firmware startup also validates four-level hardware operation on its DART.

Direct native calls must use symbols from the matching raw ELF for raw images.
An initial ISP trial used the Mach-O-layout ELF, called the wrong code and
triggered a reboot. Installed stage 1 recovered CDC; the corrected raw-ELF
probe passed. The earlier SEP result was discarded and repeated with verified
raw ELF/image pairing. Proxy-opcode clock tests and scratch-code SMP tests
were unaffected.
A later ISP power probe matched the raw code but hit a synchronous exception
and lost CDC. Its failing step was not preserved before cleanup; the cause
remains unconfirmed. A later probe called matching raw-ELF symbols at their
native addresses and repeated ISP parent/CPU/FE power, revision readback and
state restoration without exceptions. A guarded follow-up fails while reading
the older ASC control offset 0x1400044, after the revision read succeeds. The
watchdog automatically restores a responsive installed-stage-1 proxy. This
offset is unqualified for T6040; do not use the older ISP startup sequence.
Hardware watchdog recovery now protects these experiments.


## Hardware watchdog recovery

`p.wdt_arm(seconds)` arms the primary watchdog on the tested T6040 or T8140
version-2 layout. It accepts 1-178 seconds, verifies both ADT apertures and
requires the secondary watchdog to be inactive. The proxy checks the full
64-bit argument before narrowing it. The helper returns zero after alarm and
control readback, or -1 if validation or readback fails. A readback failure
occurs after programming and does not guarantee that the watchdog is disarmed.
`p.wdt_disable()` uses the existing watchdog disable routine. J616s startup
does not arm it; experiments must arm it explicitly. The existing J700 CDC
startup policy still arms its watchdog.

On J616s, counter measurements confirm a 24 MHz clock. A six-second alarm
reset the target after 6.12 seconds and returned CDC in about 15 seconds.
A second test deliberately stalled the CPU in an infinite loop; the watchdog
again reset the target and returned a working proxy in 14.83 seconds. Both
returned to the installed stage 1 without changing installed images. An AOP
microphone query/attach probe also recovered automatically after its shutdown
ACK timed out. Keep the watchdog armed when cleanup cannot establish IOP
quiescence; reboot before creating another proxy heap or reusing DMA memory.

## Reproducing the cold IOP probes

After rebooting into aura proxy mode, run:

```sh
AURADEVICE=/dev/ttyACM0 python proxyclient/experiments/t6040_iop.py sio
```

Select `sio`, `mtp` or `aop` as the final argument. SIO/MTP use existing
StandardASC system endpoints. MTP also captures initialization packets from
DockChannel without enabling any device. Add `--keyboard` to the MTP command
to enable only the announced keyboard and check its ACK/readiness responses.
The capture is bounded to five seconds per phase and 16 KiB; it verifies each
packet checksum before parsing its contents. It does not enable the actuator
or multitouch interface, program GPIOs or upload firmware. Keyboard input
events still require separate validation. AOP
also initializes advertised AFK endpoints to read service announcements, then
waits for all queue-start ACKs and requests AFK shutdown. Fresh responses
currently time out, so the AOP probe returns a failure and requires reboot.
It sends no audio application commands.
AOP retains the IOP with its mappings and boot arguments installed and
reports `reboot_required`: recover by rebooting before further experiments. It
requires the tested cold IOP/DART state and prints a
JSON result. Reboot before running it again: firmware resume is not qualified.
Send, table invalidation, startup and quiescence waits are bounded. If shutdown
is not acknowledged, mappings remain installed and the result requires reboot.

To test the touchpad firmware in RAM, pass the HIDF file produced by Asahi's
firmware extractor for J616s and macOS build 26A428:

```sh
AURADEVICE=/dev/ttyACM0 python proxyclient/experiments/t6040_iop.py mtp \
    --touchpad-firmware /path/to/tpmtfw-j616s.bin
```

This mode requires the tested firmware hash. Add `--keyboard` to capture both
interfaces; keyboard enable follows touchpad startup. Add `--capture-seconds 45`
for a longer input window (1-60 seconds;
up to 1 MiB). It enables the announced multitouch and actuator interfaces,
sends no actuator output reports and makes no GPIO writes. Version-2 power
requests include
Off/On and WillChange/HasChanged phases; replies must echo the exact request
with the expected transfer counter and zero status. DeviceReady must arrive
after the On request. After capture, both Off phases must be acknowledged
before AP/IOP quiescence and register restoration. Failed or uncertain shutdown
returns `reboot_required` and retains the IOP, DART, DAPF and DMA allocations
until reboot. Control requests require an empty receive FIFO before transmission;
continuous input may cause a safe refusal. FIFO reads use bounded native
batches of at most 4096 bytes; capture finishes the current packet within an
additional one-second deadline. Reboot before each probe run. No firmware blob
is included in this PR.

AFK shutdown now waits for a fresh ACK instead of interpreting an initially
false `alive` flag as completion. Its timeout bounds ACK polling; the shared
IOP probe also bounds ASC sends. Ring pointers use one aligned 32-bit store
at each negotiated header offset. This removes duplicate stores that
overwrote the read pointer when the header block size was 128 bytes. Host
tests cover both 64-byte and 128-byte layouts; live AOP queries and report
reception pass with the corrected publication.

StandardASC now accepts an optional DVA mask while preserving its existing
36-bit default. The IOP probes configure a full 42-bit address range and mask.
IOReporting reads its buffer through the ASC/DART translation path, including
firmware-preallocated buffers. Host cases cover both masks across read, write
and translation, preallocated
IOReporting buffers, and invalidation of the selected stream.

An ASC address-range precedence error treated addresses above the DVA window
as physical even when `allow_phys` was false. This made the AOP crash decoder
read its firmware virtual stack as host physical memory, producing additional
exceptions and obscuring the firmware failure. Physical bypass now requires
explicit opt-in on either side of the DVA window; other addresses use the
configured DART and address mask. Host cases cover reads, writes and translation
for addresses below, within and above the window with both opt-in settings.

## OSLog buffer format

The Python type-1 buffer decoder now uses the byte count in bits 55:36 and
the page-number DVA in bits 35:0, matching the C RTKit implementation. The
captured request `0x0105ff8010009a04` decodes to 24,568 bytes at DVA
`0x10009a04000`. Allocation backing is rounded separately to 16 KiB; replies
preserve the requested byte count. Preallocated requests need no allocation or reply;
adopting the DVA does not establish a CPU mapping. Empty or
duplicate requests are refused without replacing the existing buffer.

A fresh native 10 Gbit/s capture also reads the standard DWC3 controls:
`GCTL=0x00692004`, `GUSB3PIPECTL0=0x01021102`, `DCFG=0x00480935`,
`DCTL=0x80f00a00` and `DSTS=0x0083691d`. Native GCTL and USB3 PIPE control
match the post-activation RAM proxy exactly. Both DCFG values request
SuperSpeed Plus; the other configuration fields and current link state differ.
These samples rule out a simple mismatch of the observed GCTL or PIPE-control
values, not a transient sequencing problem. No register was changed.
