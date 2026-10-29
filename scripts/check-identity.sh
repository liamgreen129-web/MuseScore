#!/usr/bin/env bash
# AGENT: verifies that a built or installed MuseScore Agent bundle shares no
# identity with the official MuseScore app (docs/agent/PHASE0.md §3).
#
# Usage: scripts/check-identity.sh [path/to/Bundle.app] [build dir]
#   default bundle: the only .app in agent.install/
#   default build dir: build.release/ (for the CMake cache check)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-}"
BUILD_DIR="${2:-$ROOT/build.release}"

if [[ -z "$APP" ]]; then
    shopt -s nullglob
    apps=("$ROOT"/agent.install/*.app)
    if [[ ${#apps[@]} -ne 1 ]]; then
        echo "error: expected exactly one .app in agent.install/, found ${#apps[@]}; pass the bundle path" >&2
        exit 2
    fi
    APP="${apps[0]}"
fi

PLIST="$APP/Contents/Info.plist"
[[ -f "$PLIST" ]] || { echo "error: no Info.plist at $PLIST" >&2; exit 2; }

EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST")"
EXE="$APP/Contents/MacOS/$EXE_NAME"
[[ -f "$EXE" ]] || { echo "error: no executable at $EXE" >&2; exit 2; }

failures=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; failures=$((failures + 1)); }

plist_get() { /usr/libexec/PlistBuddy -c "Print $1" "$PLIST" 2>/dev/null; }

echo "Checking $APP"

# 3.1 bundle identifier
id="$(plist_get :CFBundleIdentifier)"
[[ "$id" == "com.liamgreen.MuseScoreAgent" ]] && pass "bundle id $id" || fail "bundle id is '$id'"

# bundle display name
name="$(plist_get :CFBundleName)"
[[ "$name" == "MuseScore Agent" ]] && pass "bundle name $name" || fail "bundle name is '$name'"

# 3.5 file-handler rank: no Owner claims
owners="$(plutil -convert xml1 -o - "$PLIST" | grep -A1 '<key>LSHandlerRank</key>' | grep -c '<string>Owner</string>')"
[[ "$owners" -eq 0 ]] && pass "no LSHandlerRank Owner" || fail "$owners LSHandlerRank entries are Owner"

# document types are imported, not exported
if plist_get :UTExportedTypeDeclarations >/dev/null; then
    fail "UTExportedTypeDeclarations present (official app owns these types)"
else
    pass "no exported UTIs"
fi

# URL scheme
schemes="$(plutil -convert xml1 -o - "$PLIST" | sed -n '/<key>CFBundleURLSchemes<\/key>/,/<\/array>/p' | grep '<string>' | sed 's/.*<string>\(.*\)<\/string>.*/\1/')"
if grep -qx 'musescore' <<<"$schemes"; then fail "claims the musescore:// URL scheme"; else pass "URL schemes: $(echo $schemes)"; fi

# 3.2 and 3.3: strings compiled into the binary
bin_strings="$(strings -a "$EXE")"
grep -q 'musescore-agent-ipc-' <<<"$bin_strings" && pass "IPC server name musescore-agent-ipc-*" || fail "agent IPC server name not found in binary"
grep -q 'musescore-app-ipc-' <<<"$bin_strings" && fail "official IPC server name musescore-app-ipc-* found in binary" || pass "official IPC name absent"
grep -qx 'MuseScoreAgent' <<<"$bin_strings" && pass "application name MuseScoreAgent" || fail "application name MuseScoreAgent not found in binary"
grep -qx -e 'MuseScore4' -e 'MuseScore4Development' <<<"$bin_strings" && fail "official application name MuseScore4 found in binary" || pass "official application name absent"

# no embedded app extension (Quick Look previewer) sharing the official extension's id
shared_appex=0
for appex in "$APP"/Contents/PlugIns/*.appex; do
    [[ -d "$appex" ]] || continue
    appex_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$appex/Contents/Info.plist" 2>/dev/null)"
    if [[ "$appex_id" == org.musescore.* ]]; then
        fail "embedded extension $(basename "$appex") has official id $appex_id"
        shared_appex=1
    fi
done
[[ $shared_appex -eq 0 ]] && pass "no extensions with official ids"

# 3.4 update module off (needs the build tree's CMake cache)
CACHE="$BUILD_DIR/CMakeCache.txt"
if [[ -f "$CACHE" ]]; then
    upd="$(grep '^MUSE_MODULE_UPDATE:' "$CACHE" | cut -d= -f2)"
    [[ "$upd" == "OFF" ]] && pass "MUSE_MODULE_UPDATE=OFF" || fail "MUSE_MODULE_UPDATE is '$upd'"
else
    fail "no CMake cache at $CACHE; cannot confirm the update module is off"
fi

echo
if [[ $failures -eq 0 ]]; then
    echo "check-identity: PASS"
else
    echo "check-identity: FAIL ($failures)"
    exit 1
fi
