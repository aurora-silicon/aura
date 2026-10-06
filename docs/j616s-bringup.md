# J616s / T6040 proxy bring-up

Tested on J616sAP (Mac16,7), board 6, with the ADT supplied by macOS build
26A428. The branch is stacked on `j700` while the draft targets `aurora-wip`.
Linux and U-Boot handoff are outside this bring-up's scope.

## CPU clocks

`cpufreq_init()` validates the device identity, three translated CPM apertures,
and the ADT OPP tables without changing clock settings. Proxy operations
`cpufreq_get_cluster_hz(cluster)` and `cpufreq_set_cluster_pstate(cluster, state)`
use cluster 0 for E, 1 for P0 and 2 for P1. The getter reports the ADT frequency
for the accepted command state; it is not a frequency measurement.

Only states 1 and 2 are enabled. Fixed-work measurements on running cores
confirmed 1020 -> 1404 MHz for E and 1260 -> 1512 MHz for both P clusters, with
ratios of 1.376, 1.200 and 1.200. Each test restored state 1 and its original
execution time. Higher states were rejected without changing the command.
There is no voltage, PLL or thermal-policy programming here.

The driver preserves other command bits, checks the previous expected state,
and bounds busy polling to 2 ms. An ownership change or failed transition
locks out subsequent writes for that cluster until the image is restarted.
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
Deep sleep, hotplug, hypervisor operation and sustained thermal load remain
unqualified.

## Other devices

* CDC proxy and bulk reads work at USB high speed (480 Mb/s). SuperSpeed is
  pending PHY/role bring-up and cable/port qualification.
* AOP and SIO management-only startup attempts returned no endpoint map.
  Their DARTs had no enabled streams or valid initial translation tables;
  firmware mappings need investigation before further startup testing.
* SEP's existing RNG routine returned zero bytes. Successful SEP communication
  is not established.
* ISP firmware and device registers are present in the ADT. ISP boot and its
  power-domain layout remain untested.
* No speaker output, speaker amplifier programming or audio DMA stream was
  started.

The experimental stage 2 was chainloaded into RAM. Installed boot images,
boot policy and partitions were not changed. Preserve a working CDC recovery
path before experimenting with further devices.
