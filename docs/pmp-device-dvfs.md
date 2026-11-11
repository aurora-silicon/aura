# PMP device performance requests

Stage 2 derives media-engine PMP performance requests from the ADT that was
used to boot the machine. It does not carry a SoC/model table of request entry
numbers or levels.

For each Linux PMP report entry that carries the boolean `apple,adt-dvfs` and
whose `label` identifies VENC, AVD, JPEG, the scaler (MSR0) or the ProRes codec
(SCODEC), Stage 2 uses the report `reg` as the PMP soc-device ID minus one. It then:

1. reads the `SOC-DEV-DVFS` base and count from
   `/arm-io/pmp/iop-pmp-nub/ptd-range`;
2. counts the DVFS-present selectors in
   `/arm-io/pmp/iop-pmp-nub/soc-device` up to that device;
3. selects the named PMGR virtual performance sources in
   `/arm-io/pmgr/devices` (`VMAX` for VENC, AVD, MSR0 and SCODEC, `VNOM` for
   JPEG); and
4. writes the derived entry, main level and all derived request-class levels
   as `apple,dvfs` and `apple,dvfs-request-levels` on the report entry.

VENC selects the named `VENC-SOC-VMAX` or `VENC-AVE-VMAX` source; unrelated
fabric sources are left to their owner. The VENC default lane is populated
only when the ADT names the legacy `VENC-SOC-VMAX` source. All other lanes and
levels come directly from the selected records. Missing, malformed, duplicate
or out-of-range ADT records leave the existing device-tree properties
unchanged. Entries without `apple,adt-dvfs` are never touched, so a device
tree written for a kernel that does not parse these properties keeps working.

Run

```sh
pytest -q tests/python/test_pmp_device_dvfs.py
```

for synthetic ADT and real-libfdt coverage of the slot allocation, both PMGR
class encodings, multi-class VENC words, named AVD/JPEG policy and fail-soft
handoff.
