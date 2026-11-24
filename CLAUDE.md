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

- Use the Context Mode plugin when it can reduce low-signal output.
- Fan independent reviews and implementation work out to isolated agents.
- Never write unit tests after you write code.
- Highly prefer end-to-end tests as the sole testing mechanism. Use them to
  verify complex features, and produce a verifiable, repeatable artifact at the
  end of every end-to-end campaign.
- If a system must be tested in isolation, first write down all the ways it
  could fail, then write the code.
- Remove low-signal unit tests that would not catch a real bug missed by the
  end-to-end tests. Preserve tests that provide distinct failure coverage.
