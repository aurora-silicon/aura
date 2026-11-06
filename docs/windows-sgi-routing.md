# Windows software SGI routing

## Failure model recorded before the fix

The diagnostic-05 hardware log reports ten active physical CPUs (0 through 9),
with boot CPU 6. All nine secondary CPUs started. `smp_cpu_count()` counts only
the secondary spin-table alive flags, returning nine; this is not a maximum
physical CPU index. The software CPU-interface SGI loop used `cpu < count`,
omitting CPU 9. Windows enabled that CPU's Group 1 interface, but the observed
SGI0 acknowledgements came only from CPUs 0–5, 7 and 8. This is a concrete
routing defect; the capture alone does not prove it is the only boot blocker.

The replay must catch these failure modes before implementation changes:

- Nine live secondary flags plus one BSP must still route to all ten mapped
  CPUs. The BSP may be neither physical CPU 0 nor the highest physical ID.
- A dense redistributor frame index is not a physical CPU ID. A sparse map
  must reach its highest CPU and must not target a hole.
- IRM broadcast excludes the sender; targeted delivery may include the sender
  and must synchronize its local virtual IRQ instead of sending itself an IPI.
- Aff3, Aff2, Aff1, RangeSelector, and TargetList must all match. RangeSelector
  selects the upper nibble of Aff0, as in the architectural SGI path.
- An empty target list must do nothing. Invalid frames, dead secondary CPUs,
  and uninitialized software interfaces must not receive pending state or IPI.
- The boot CPU must remain targetable despite its clear secondary alive flag.

The offline runner compiles the exact production SGI function, atomic pending
bit setter, frame accessors and secondary count helper. Only CPU identity,
spin-table state and physical IPI/local-line side effects are modeled. It
records source snapshots, extracted function hashes, compiler commands, exact
target masks and sanitizer output. Physical interrupt delivery and Windows
rendezvous completion require a separate hardware run before merge.

## Source comparison and correction

The public legacy implementation pinned at
`aurora-silicon/m1n1@69534d4899e20108d2c7a085565a7097509fa17f` uses the same
count-based SGI loop, but its `smp_cpu_count()` counts ADT CPU nodes, including
the BSP. The current Aura compatibility adapter counts secondary alive flags.
That change in meaning explains the missing final CPU on this dense topology;
count-based iteration is also unsafe for sparse physical IDs.

The software CPU-interface route now uses `hv_vgic3_num_cpus()` and
`hv_vgic3_cpu_for_frame()`, matching the existing architectural SGI route. It
also applies RangeSelector to Aff0 before testing the 16-bit target list.
The existing interface-valid and secondary-alive checks remain. Linux's public
Apple AIC fast-IPI route similarly translates each selected logical CPU through
`cpu_logical_map()` before constructing the physical MPIDR-based IPI request.

## Repeatable offline replay

```sh
python3 tools/windows_sgi_e2e.py --output build/windows-sgi-replay
```

Use a new output directory. `result.json` records nine cases and their exact
pending/IPI target masks; `replay.log` records assertion failures or the same
case results. `source/` contains the source snapshot, while
`production_sgi.inc` contains the verbatim production functions compiled into
the sanitizer-enabled replay. The receipt hashes both the full source files
and each extracted function. This is an isolated production-path replay, not
a Windows boot or physical SGI-delivery qualification.
