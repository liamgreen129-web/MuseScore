#!/usr/bin/env bash
# AGENT: move agent/4.7 onto the newest upstream 4.7.x tag, rebuild and
# re-check identity.
#
# Usage: scripts/sync-upstream.sh [--dry-run]
#   --dry-run  fetch tags and report what would happen; change nothing
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DRY_RUN=0
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=1

BRANCH=agent/4.7
[[ "$(git branch --show-current)" == "$BRANCH" ]] || { echo "error: not on $BRANCH" >&2; exit 1; }
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || { echo "error: uncommitted changes" >&2; exit 1; }

git fetch upstream --tags --filter=blob:none --quiet

# The upstream tag agent/4.7 currently sits on
current="$(git describe --tags --abbrev=0 --match 'v4.7.*' "$BRANCH")"
latest="$(git tag --list 'v4.7.*' --sort=-v:refname | head -1)"

echo "agent/4.7 is based on: $current"
echo "newest upstream 4.7.x: $latest"

if [[ "$current" == "$latest" ]]; then
    echo "up to date"
    exit 0
fi

echo "would rebase $(git rev-list --count "$current..$BRANCH") agent commits onto $latest"
echo "upstream files touched by agent commits (expect conflicts here):"
git diff --name-only "$current" "$BRANCH" -- . ':!docs/agent' ':!scripts' ':!src/agent' ':!CLAUDE.md' ':!.claude' | sed 's/^/  /'

if [[ $DRY_RUN -eq 1 ]]; then
    echo "dry run: nothing changed"
    exit 0
fi

git tag -f "pre-sync-$(date +%Y%m%d-%H%M)" "$BRANCH"
git rebase --onto "$latest" "$current" "$BRANCH"
scripts/build.sh
echo "synced to $latest; run the scenario tests before installing"
