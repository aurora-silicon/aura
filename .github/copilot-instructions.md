# Aurora Silicon assistant policy

**Rulebook:** follow the Aurora Silicon rulebook, https://github.com/aurora-silicon/agent-guidelines/blob/main/AGENTS.md. It applies to every agent and contributor, whoever runs them. This file adds the repo specifics.

This is Aurora Silicon's own policy for AI coding assistants working in
this repository. It is the only assistant instruction file this tree
carries. Upstream projects' instruction files are never carried here.

AGENTS.md, CLAUDE.md, GEMINI.md and .github/copilot-instructions.md are
copies of .github/aurora/AGENTS.md. The aurora-agent-policy workflow
puts them back if anything else replaces them, including an upstream
merge. To change the policy, edit .github/aurora/AGENTS.md.

## Rules

- Only public code goes in. Never copy code from proprietary sources.
- Changes are tested on the hardware they affect before they are merged.

## Pre-PR checklist

Synced from [aurora-silicon/agent-guidelines](https://github.com/aurora-silicon/agent-guidelines/blob/f7edafb3ce3b/pre-pr-checklist.md)
at f7edafb3ce3b. Change it there, then sync the copy here; do not edit this section
on its own. These are the checks Aurora's review agents apply.

Each item is a hard rule unless it says "default". If your change fails one, fix the change, or say in the PR why it can't be done yet and keep the PR a draft.

### 1. No model gating: works on every Mac that has the hardware (hard)

- **No chip, SoC, board or firmware-version gates** unless truly unavoidable. Decide from what the machine reports: ADT/DT properties of the block, its compatible, capability or feature bits, probed registers, firmware capability fields, ALSA control ranges. Never `if chip == …`, `if board == …`, `if fw == 27.2`, or a per-chip table where a property can carry the value.
- If a gate really is unavoidable, the commit message says why, and the gate uses the narrowest hardware property.
- Prove the change on every Mac you have that contains the block, not only the one you wrote it for. Ask for test runs on the ones you don't have.
- **Chicken and egg:** if the property must first come from the boot loader or DT, land that side first. The consumer reads it with today's behaviour as the fallback when it is absent, and the fallback is removed after a release has carried the property.

### 2. Production-ready, no hacks (hard)

- Works on the **default kernel command line**: no new cmdline or module parameters, environment variables or manual steps needed for a feature to work.
- No sleeps or retries over a race, no magic delay or depth ("the value that worked"), no disabled checks, no lab-only code paths.
- Development boots (one-shot test boots) may use extra parameters. Remove them and re-prove on the default command line before the PR.
- One kernel tree and one command line for every Mac. Only the devicetrees differ.

### 3. Safety (hard)

- **Safety values come from the machine, never from hardcoding.** Limits, caps, thresholds, timings and filters that protect hardware (speaker excursion and thermal limits, gain caps, high-pass corners, power and thermal limits, voltage and current ceilings) are read from what the Mac hands over: its ADT/DT, its own firmware or tuning bundle on the ESP, or control ranges the driver reports. No per-model safety tables or magic numbers in code. If the machine doesn't provide a value, the feature stays off or muted; it never falls back to a guessed constant.
- Speakers: **speakerguardd v2 only**. Never weaken or bypass protection. If a capability or limit can't be established from the machine, stay muted.
- Every test boot has a dead-man. Read registers before writing them.
- No Stage 1, boot-object, LocalPolicy or 1TR change without the machine's owner.
- Before any USB-C VDM or HPM reset, confirm which Mac is on the far end of the cable.

### 4. Clean-room and attribution (hard)

- No Apple code, strings, firmware, disassembly output or Apple tuning values in public code, PRs, issues or channels. Our own traces and measurements are fine.
- Commits carry human identity only, with a human `Signed-off-by`. No agent co-author or sign-off and no `Assisted-by` in commits. When an agent helped, disclose it at the end of the PR body with a plain `Assisted-by:` line naming the model or tool (exact wording, vendor prefix, and punctuation are flexible; do not nitpick format).
- Carry other people's commits unchanged. No force pushes to shared branches.

### 5. Evidence (default, high bar)

- A run on the affected hardware with the numbers: what ran, on which Mac, kernel and build, and the result. Include a regression run: 0 SELinux denials, 0 failed units, 0 new kernel errors.
- Performance claims: a same-machine A/B with conformance unchanged on every GPU generation the change applies to.

### 6. Naming (hard)

- The Vulkan driver is **Borealis** (Aurora Silicon's Vulkan driver, in Mesa). Never call our work Honeykrisp or "hk" in prose: PR and issue text, commit messages, docs, reports and user-facing names.
- Code keeps the identifiers it has today. Until the rename in [aurora-silicon/mesa#591](https://github.com/aurora-silicon/mesa/pull/591) is merged, Borealis lives in `src/asahi/vulkan/` with `hk_`/`HK_` symbols; after it lands, new code uses its `brl_` prefix.
- Names shared with upstream or fixed by an interface stay as they are: the Gallium `asahi` driver, `src/asahi/`, the DRM UAPI (`asahi_drm.h`, `DRM_ASAHI_*`) and Asahi or m1n1 names in upstream code. New paths and names of our own use Aurora, not Asahi or m1n1.

## Testing and collaboration

- Use the Context Mode plugin whenever it can reduce low-signal output.
- Fan independent work out to isolated agents and preserve unrelated worktrees.
- Never write unit tests after you write code.
- Highly prefer E2E tests as the sole testing mechanism. Use them to verify
  complex features work. At the end of E2E tests, produce a verifiable and
  repeatable artifact.
- If you must test a system in isolation, first write down all the ways it
  could fail, then write the code.
- Delete low-signal unit tests that would not catch a real bug missed by E2E
  tests. Preserve tests that provide distinct failure coverage.
