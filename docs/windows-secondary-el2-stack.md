# Cached EL2 stacks for T8142 secondaries (Windows personality)

## Why

Boot 144's EL2 accounting showed every EL2 stay on a non-boot core costing 20-27 us
(absent-register emulation 21.6 us on a P secondary, 26.5 us on an E core) against 0.3-0.5 us
on the boot core for the same code, the same in time on P and E cores.

Production's `smp_t8142_block_init()` puts the T8142 secondaries' stacks and the spin table in
one heap block and maps it with `mmu_map_uncached()`, which is Device-nGnRnE (memory.c). A
secondary runs `hv_enter_secondary()` on that stack and `hv_enter_guest()` erets with SP_EL2
there, so every guest exception on a secondary pushes its frame, runs its C handlers and restores
the guest through strongly-ordered device memory. The boot core's stack is ordinary cached memory.
The tree the Windows hypervisor was qualified on (9956d841) had no such block.

## Change

Windows personality only. Before a T8142 secondary's first guest entry the boot CPU allocates a
64 KiB stack for it from the heap, zeroes it, cleans and invalidates it to the point of coherency
and publishes the pointer. `hv_enter_secondary()` masks DAIF, and `hv_enter_guest_on_stack()`
(hv_asm.S) keeps the old SP in x19, moves SP to the new stack, calls `hv_enter_guest()`, and on
return (`hv_exit_guest()` reloads x19 from the frame `hv_enter_guest()` saved) moves SP back. The
CPU's DAIF from before the switch is restored, so the guest's first PSTATE is unchanged.
Production's smp.c, the spin table and the default image are untouched.

Constraints:
1. The stack is heap memory, never .bss/.data: on T8142 iBoot can lock CTRR over the boot object
   so that a secondary's stores into the image fault. The allocation is refused if it overlaps
   the image or the SMP block.
2. Exit (`hv_exit_guest()` from `hv_maybe_exit()`, SYSTEM_OFF, abort) returns through the
   trampoline to `hv_enter_secondary()` on the old stack, and from there to the SMP wait loop.
3. The switch happens with DAIF masked, before the first guest entry; no exception can be taken
   with SP half-moved.
4. The secondary's MMU and caches are on when it first touches the stack (`mmu_init_secondary()`
   runs before `hv_enter_secondary()`); the boot CPU's clean+invalidate leaves no stale line for
   a core with caches off.

## Failure model (written before the code)

1. A secondary still enters the guest on the uncached stack (switch skipped or done after SP was
   saved).
2. SP is restored to the wrong place after exit, so `hv_enter_secondary()`'s epilogue or the
   wait loop pops garbage: the drain on shutdown fails.
3. A callee-saved register or the link register is clobbered across the trampoline.
4. The new stack is unaligned, too small, overlaps the image (CTRR store fault), the SMP block,
   guest RAM or another CPU's stack.
5. Allocation failure crashes instead of keeping today's stack.
6. A restarted CPU allocates a second stack or two CPUs share one.
7. The guest's first PSTATE changes (DAIF).
8. The boot CPU or another chip takes the new path.
9. The default image changes.

## Verification

`tests/windows_secondary_stack/run.py` assembles the production `hv_enter_guest_on_stack`,
`hv_enter_guest` and `hv_exit_guest` from hv_asm.S for the host (EL2-only instructions replaced
by user-mode equivalents, `eret` by a branch to a modeled guest), runs them natively on arm64, and
checks failures 1-3: SP at "guest entry" lies inside the new stack, exit returns to the caller
with its SP and x19-x30 intact, and frames from repeated entries do not drift. It rejects
mutations of the trampoline. Failures 4-9 are checked by source reading and the build; whether
the per-stay cost actually drops to the boot core's needs a hardware boot.
