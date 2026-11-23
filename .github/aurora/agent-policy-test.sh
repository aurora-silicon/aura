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
# merged <branch>: create refs/heads/mr-<branch> = <branch> merged into main (what refs/pull/N/merge holds).
merged() { git checkout -q -B "mr-$1" main && git merge -q --no-edit "$1" >/dev/null 2>&1; local rc=$?
	[ $rc = 0 ] || git merge --abort 2>/dev/null || true; git reset -q --hard; git checkout -q main; return $rc; }
# on_main <branch> <cmd...>: new branch from main with one commit made by <cmd>.
on_main() { local b=$1; shift; git checkout -q -b "$b" main; eval "$*"; git add -A; git commit -qm "$b"; git checkout -q main; }

git init -q -b main "$WORK/r"
cd "$WORK/r"
git config user.name test; git config user.email test@example.invalid
mkdir -p .github/aurora
echo v1 > .github/aurora/AGENTS.md
for f in "${COPIES[@]}"; do cp .github/aurora/AGENTS.md "$f"; done
echo code > a; git add -A; git commit -qm base
git branch stale; git branch foreign; git branch second

git checkout -q foreign
echo upstream > CLAUDE.md; mkdir -p sub .claude; echo x > sub/CLAUDE.md; echo y > .claude/s
git add -A; git commit -qm foreign
git checkout -q stale; echo s > b; git add b; git commit -qm stale
git checkout -q main; echo v2 > .github/aurora/AGENTS.md; git commit -qam "policy v2"
# upstream layout: CLAUDE.md and GEMINI.md as symlinks to AGENTS.md
on_main symlinks 'rm CLAUDE.md GEMINI.md; ln -s AGENTS.md CLAUDE.md; ln -s AGENTS.md GEMINI.md; cp .github/aurora/AGENTS.md AGENTS.md'

stale_tip=$(git rev-parse stale); foreign_tip=$(git rev-parse foreign); symlinks_tip=$(git rev-parse symlinks)
LOCAL=1 DEFAULT_BRANCH=main POLICY_BRANCHES="main second" bash "$SCRIPT" sweep 2>/dev/null
git reset -q --hard main

echo "# heal"
check "main copies synced to the new policy" '[ "$(git show main:AGENTS.md)" = v2 ] && [ "$(git show main:.github/copilot-instructions.md)" = v2 ]'
check "second policy branch with stale copies synced" '[ "$(git show second:CLAUDE.md)" = v2 ] && [ "$(git show second:GEMINI.md)" = v2 ]'
check "branch with an older consistent snapshot untouched" '[ "$(git rev-parse stale)" = "$stale_tip" ]'
check "foreign branch untouched (report only)" '[ "$(git rev-parse foreign)" = "$foreign_tip" ]'
check "symlink branch untouched (report only)" '[ "$(git rev-parse symlinks)" = "$symlinks_tip" ]'
check "stale branch merges into main cleanly" 'merged stale'
on_main pushed 'echo junk > CLAUDE.md'; pushed_tip=$(git rev-parse pushed)
policy push pushed
check "push to a feature branch changes nothing" '[ "$(git rev-parse pushed)" = "$pushed_tip" ]'
git checkout -q main; echo junk > GEMINI.md; git commit -qam "main junk"; main_tip=$(git rev-parse main)
policy push main; git reset -q --hard main
check "push to the default branch heals it" '[ "$(git rev-parse main)" != "$main_tip" ] && [ "$(git show main:GEMINI.md)" = v2 ]'
check "the heal commit is the only change" '[ "$(git rev-parse main^)" = "$main_tip" ]'

echo "# pull request check (on the merge result)"
check "stale snapshot passes" 'policy pr mr-stale'
on_main junk 'echo junk > GEMINI.md'; merged junk
check "foreign copy fails" '! policy pr mr-junk'
on_main gone 'git rm -q AGENTS.md'; merged gone
check "deleted copy fails" '! policy pr mr-gone'
on_main nopolicy 'git rm -q .github/aurora/AGENTS.md "${COPIES[@]}"'; merged nopolicy
check "deleted policy file and copies fails" '! policy pr mr-nopolicy'
on_main rewrite 'for f in .github/aurora/AGENTS.md "${COPIES[@]}"; do echo evil > "$f"; done'; merged rewrite
check "self-consistent policy rewrite fails without the label" '! policy pr mr-rewrite'
check "policy change passes with the label" 'POLICY_CHANGE_OK=1 policy pr mr-rewrite'
on_main policyonly 'echo v3 > .github/aurora/AGENTS.md'; merged policyonly
check "labelled policy change with copies left to the sync passes" 'POLICY_CHANGE_OK=1 policy pr mr-policyonly'
on_main sneak 'echo v3 > .github/aurora/AGENTS.md; echo other > CLAUDE.md'; merged sneak
check "labelled policy change still rejects a third text" '! POLICY_CHANGE_OK=1 policy pr mr-sneak'
check "unmergeable stale head fails (update the branch)" '! policy pr stale'
check "unhealed foreign branch fails its pull request check" '! policy pr foreign'
check "unhealed symlink branch fails its pull request check" '! policy pr symlinks'
merged pushed; check "feature branch with a foreign copy fails its pull request check" '! policy pr mr-pushed'

git branch -q -D junk gone nopolicy rewrite policyonly sneak mr-junk mr-gone mr-nopolicy mr-rewrite mr-policyonly mr-sneak mr-stale mr-pushed
before=$(git for-each-ref --format='%(objectname)' refs/heads/ | sort | sha1sum)
LOCAL=1 DEFAULT_BRANCH=main POLICY_BRANCHES="main second" bash "$SCRIPT" sweep 2>/dev/null
check "second sweep changes nothing" '[ "$(git for-each-ref --format="%(objectname)" refs/heads/ | sort | sha1sum)" = "$before" ]'
exit $fail
