#!/usr/bin/env bash
# AGENT: sign a MuseScore Agent bundle. Uses the local code-signing certificate
# "MuseScore Agent Local" when it exists in the login keychain, so macOS privacy
# permissions (Documents, etc.) survive rebuilds; falls back to ad-hoc signing,
# where macOS asks again after every rebuild.
#
# Usage: scripts/sign.sh path/to/Bundle.app
set -euo pipefail
APP="$1"
IDENTITY="MuseScore Agent Local"
# self-signed and not trusted, so look it up without -v and sign by hash; codesign
# accepts it, and the designated requirement pins the leaf hash, which TCC remembers
HASH="$(security find-identity -p codesigning 2>/dev/null | awk -v n="\"$IDENTITY\"" '$0 ~ n {print $2; exit}')"
if [[ -n "$HASH" ]]; then
    codesign --force --deep --sign "$HASH" "$APP"
    echo "signed with \"$IDENTITY\""
else
    codesign --force --deep --sign - "$APP"
    echo "signed ad-hoc (create the \"$IDENTITY\" certificate to keep permissions across rebuilds)"
fi
