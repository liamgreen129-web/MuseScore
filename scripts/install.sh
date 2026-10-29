#!/usr/bin/env bash
# AGENT: install the built bundle as /Applications/MuseScore Agent.app,
# ad-hoc signed. Refuses to run unless check-identity.sh passes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/Applications/MuseScore Agent.app"

shopt -s nullglob
apps=("$ROOT"/agent.install/*.app)
[[ ${#apps[@]} -eq 1 ]] || { echo "error: expected exactly one .app in agent.install/" >&2; exit 1; }
SRC="${apps[0]}"

"$ROOT/scripts/check-identity.sh" "$SRC"

# Never touch the official app
case "$DEST" in
    "/Applications/MuseScore 4.app"|"/Applications/MuseScore Studio 4.app") echo "refusing: $DEST" >&2; exit 1 ;;
esac

if pgrep -f "$DEST/Contents/MacOS/" >/dev/null; then
    echo "error: MuseScore Agent is running; quit it first" >&2
    exit 1
fi

rm -rf "$DEST"
ditto "$SRC" "$DEST"
codesign --force --deep --sign - "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$LSREGISTER" -f "$DEST"
# keep the build-tree copy out of Finder's "Open With"
"$LSREGISTER" -u "$SRC" 2>/dev/null || true

"$ROOT/scripts/check-identity.sh" "$DEST"
echo "installed: $DEST"
