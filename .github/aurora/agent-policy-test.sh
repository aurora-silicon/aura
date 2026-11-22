#!/usr/bin/env bash
# Self-test for agent-policy.sh on a scratch repository (LOCAL=1 mode).
# Usage: bash .github/aurora/agent-policy-test.sh
set -eu -o pipefail

SCRIPT=$(cd "$(dirname "$0")" && pwd)/agent-policy.sh
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
COPIES=(AGENTS.md CLAUDE.md GEMINI.md .github/copilot-instructions.md)
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
policy() { LOCAL=1 DEFAULT_BRANCH=main bash "$SCRIPT" "$@" 2>/dev/null; }
merges() { git checkout -q -B m main && git merge -q --no-edit "$1" >/dev/null 2>&1; local rc=$?; git merge --abort 2>/dev/null || true; git reset -q --hard; git checkout -q main; return $rc; }

git init -q -b main "$WORK/r"
cd "$WORK/r"
git config user.name test; git config user.email test@example.invalid
mkdir -p .github/aurora
echo v1 > .github/aurora/AGENTS.md
for f in "${COPIES[@]}"; do cp .github/aurora/AGENTS.md "$f"; done
echo code > a; git add -A; git commit -qm base
git branch stale; git branch foreign

git checkout -q foreign
echo upstream > CLAUDE.md; mkdir -p sub .claude; echo x > sub/CLAUDE.md; echo y > .claude/s
git add -A; git commit -qm foreign
git checkout -q stale; echo s > b; git add b; git commit -qm stale
git checkout -q main; echo v2 > .github/aurora/AGENTS.md; git commit -qam "policy v2"

stale_tip=$(git rev-parse stale)
policy sweep
git reset -q --hard main

check "main copies synced to the new policy" '[ "$(git show main:AGENTS.md)" = v2 ] && [ "$(git show main:.github/copilot-instructions.md)" = v2 ]'
check "branch with an older consistent snapshot untouched" '[ "$(git rev-parse stale)" = "$stale_tip" ]'
check "foreign copy rewritten, not deleted" '[ "$(git show foreign:CLAUDE.md)" = v2 ]'
check "other copies on the foreign branch kept" '[ "$(git show foreign:AGENTS.md)" = v1 ]'
check "stray assistant files removed" '! git cat-file -e foreign:sub/CLAUDE.md 2>/dev/null && ! git cat-file -e foreign:.claude 2>/dev/null'
check "stale branch merges into main cleanly" 'merges stale'
check "healed foreign branch merges into main cleanly" 'merges foreign'
check "pr check passes on a stale snapshot" 'policy pr stale'
git checkout -q -b bad stale; echo junk > GEMINI.md; git commit -qam junk; git checkout -q main
check "pr check rejects a foreign copy" '! policy pr bad'
git branch -q -D bad
before=$(git for-each-ref --format='%(objectname)' refs/heads/ | sort | sha1sum)
policy sweep
check "second sweep changes nothing" '[ "$(git for-each-ref --format="%(objectname)" refs/heads/ | sort | sha1sum)" = "$before" ]'
exit $fail
