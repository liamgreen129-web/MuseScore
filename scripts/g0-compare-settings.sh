#!/usr/bin/env bash
# AGENT: Gate G0 check. Compares the official app's settings folder with the
# snapshot taken before the agent build was first installed. Read-only.
#
# Usage: scripts/g0-compare-settings.sh [g0-snapshot/<stamp>]
# Files the official app rewrites on its own (logs, session) are reported
# separately; anything else that changed needs a look.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SNAP="${1:-$(ls -d "$ROOT"/g0-snapshot/*/ | tail -1)}"
SNAP="${SNAP%/}"
LIVE="$HOME/Library/Application Support/MuseScore/MuseScore4"

[[ -f "$SNAP/MuseScore4.sha1" ]] || { echo "error: no snapshot at $SNAP" >&2; exit 2; }

now="$(mktemp)"
trap 'rm -f "$now"' EXIT
(cd "$LIVE" && find . -type f -exec shasum {} + | sort -k2) > "$now"

echo "Snapshot: $SNAP"
changes="$(diff <(awk '{print $2, $1}' "$SNAP/MuseScore4.sha1") <(awk '{print $2, $1}' "$now") | grep '^[<>]' | awk '{print $2}' | sort -u || true)"

noise="$(grep -E '^\./(logs|session)/' <<<"$changes" || true)"
real="$(grep -vE '^\./(logs|session)/' <<<"$changes" | sed '/^$/d' || true)"

[[ -n "$noise" ]] && echo "Changed by normal use (logs, session): $(wc -l <<<"$noise" | tr -d ' ') files"

if [[ -z "$real" ]]; then
    echo "Settings, palettes, plugins, recent files: unchanged"
    echo "g0-compare-settings: PASS"
else
    echo "Changed since the snapshot:"
    sed 's/^/  /' <<<"$real"
    echo "g0-compare-settings: CHECK (changes above may come from using the official app itself)"
    exit 1
fi
