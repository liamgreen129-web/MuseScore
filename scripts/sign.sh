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
if security find-identity -v -p codesigning 2>/dev/null | grep -q "\"$IDENTITY\""; then
    codesign --force --deep --sign "$IDENTITY" "$APP"
    echo "signed with \"$IDENTITY\""
else
    codesign --force --deep --sign - "$APP"
    echo "signed ad-hoc (create the \"$IDENTITY\" certificate to keep permissions across rebuilds)"
fi
