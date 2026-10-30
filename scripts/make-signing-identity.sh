#!/usr/bin/env bash
# AGENT: create the local code-signing identity "MuseScore Agent Local" in the login keychain,
# as on the MacBook Pro (DECISIONS 2026-10-29): a self-signed RSA-2048 certificate with the
# codeSigning extended key usage, valid 10 years, imported with access for /usr/bin/codesign.
# Trust settings are deliberately not changed. scripts/sign.sh picks it up by name; macOS then
# asks for Documents access once and remembers it across rebuilds.
#
# Usage: scripts/make-signing-identity.sh      (does nothing if the identity already exists)
set -euo pipefail
CN="MuseScore Agent Local"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

if security find-identity -p codesigning 2>/dev/null | grep -q "\"$CN\""; then
    echo "\"$CN\" already exists:"
    security find-identity -p codesigning | grep "\"$CN\""
    exit 0
fi

D="$(mktemp -d)"
trap 'rm -rf "$D"' EXIT
cat > "$D/cfg" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $CN
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF
# /usr/bin/openssl is macOS's LibreSSL, whose PKCS#12 output `security import` reads
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$D/cfg" \
    -keyout "$D/key.pem" -out "$D/cert.pem" 2>/dev/null
/usr/bin/openssl pkcs12 -export -inkey "$D/key.pem" -in "$D/cert.pem" -name "$CN" \
    -out "$D/id.p12" -passout pass:agent
security import "$D/id.p12" -k "$KEYCHAIN" -P agent -T /usr/bin/codesign

security find-identity -p codesigning | grep "\"$CN\"" \
    || { echo "import ran but the identity is not listed" >&2; exit 1; }
echo "created \"$CN\"; the next scripts/install.sh signs with it"
