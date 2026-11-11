# J613 RTC alarm wiring for a trial Stage 2

The Linux RTC alarm driver needs three PMIC NVMEM cells and SPMI IRQ2.
The SPMI provider additionally needs its AIC parent IRQ437. Declaring
`interrupt-controller` without `interrupt-parent` leaves this controller in
polling mode: it can supply NVMEM but never creates the RTC IRQ domain.

This is an opt-in offline DTB preparation tool, not an installed boot-path
update. It accepts the two J613 board compatibles only. Feed its output to
`tools/compose_stage2.py` instead of the corresponding input DTB. Keep the
kernel build identity and the original DTB with the composition receipt.
Use a one-shot `aura-try` marker with an independently preserved normal image.
The alarm driver and the SPMI IRQ domain with IRQCHIP_SKIP_SET_WAKE must
already be in the chosen kernel. This is not a deep-sleep enablement patch.

Failure cases to check before generating an image: wrong board, wrong SPMI
controller or AIC, missing PMIC layout/offset cell, partial existing alarm
wiring, phandle collisions, loss of unrelated firmware resources, unresolved
interrupt supplier, and an output accidentally replacing its input. After
composition compare all properties by resolved provider path, then check the
loaded tree and device probes before attempting any alarm or sleep.

The resource layout follows the public alarm/SPMI work:
Eryk Wieliczko (https://github.com/omacom/linux-aurora/pull/274) and
Samuel Carreno (https://github.com/omacom/linux-aurora/pull/285), and the explicit AIC parent in
Linux's working `t6030.dtsi`. These references describe the approach; J613
qualification and limits are recorded below.

Example (requires dtc tools `fdtget` and `fdtput`):

```sh
python3 tools/prepare_j613_rtc.py t8122-j613.dtb t8122-j613-rtc.dtb
# Use the new DTB as that board's --dtb input to compose_stage2.py.
```

The tool emits an input/output SHA256 receipt and refuses existing output or
partial alarm wiring. It never reads an installed image or touches the ESP.

Offline validation on Aura main a2d51b77: the PMP boot-data check passes;
the Python suite has 651 passing tests and 17 skips (including two tests
rerun after restoring Git metadata and removing archive sidecars). Both J613
DTBs produced by this tool have exactly the same resolved properties as
the RTC-only DTBs used in the hardware trials. Existing output and repeated
application are refused, with input hashes unchanged.

J613/T8122 hardware trial, kernel 7.3.0-aurora73-upstream-2-ARCH (qualified
30.9, source112007b70271361ed5c9270b144732f427c6feb8): awake alarms5/5;
alarm-armed pm_test freezer/devices/platform5.164/6.649/6.699 s; real RTC
s2idle wakes60.050/60.432/60.684/300.869/1800.789 s. Every real wake was
IRQ93, with one RTC interrupt and self-disarm. Device/service health passed
after return; idle-blanked internal display re-enabled through DPMS.
Battery energy delta0 uWh while AC online; external-supply energy was not
measured. External-display s2idle and total-system sleep power remain untested.
This does not qualify deep sleep or installing a new default loader.
Aura's GPU26 powered-identity host test also passed.
