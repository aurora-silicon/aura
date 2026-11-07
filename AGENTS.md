# Aurora Silicon assistant policy

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
