#!/usr/bin/env bash
# MuseScore Agent: one-file setup for the Mac mini. AirDrop it over, then double-click it
# (or run: bash ~/Downloads/setup-mac-mini.command). Safe to run again: every step is skipped
# when it is already done. At the end it starts Claude Code in the repo with the prompt below.
set -euo pipefail

REPO_URL="https://github.com/liamgreen129-web/MuseScore.git"
REPO_DIR="$HOME/dev/musescore-agent"
BRANCH="agent/4.7-nfhpdz"
QT_DIR="$HOME/Qt/6.10.2/macos"
DEPS_DIR="$HOME/musescore_deps_macos"
QT_URL="https://github.com/cbjeukendrup/musescore_build_qt/releases/download/v21960860930/Qt-6.10.2-macOS-10.15.7z"
DEPS_URL="https://raw.githubusercontent.com/cbjeukendrup/musescore_deps/main/musescore_deps_macos.tar.gz"

PROMPT='We have moved MuseScore Agent to the Mac mini; setup-mac-mini.command has installed the tools, Qt, the deps, the repo (branch agent/4.7-nfhpdz) and the signing identity. Read CLAUDE.md, docs/agent/STATE.md and docs/agent/MACMINI.md. Then, without stopping to ask: record the PHASE0.md section 1 preflight for this machine in DECISIONS.md and set AGENT_JOBS for its cores and RAM; build with scripts/build.sh and fix every compile error (the cloud work on this branch was only syntax-checked without Qt); scripts/check-identity.sh must pass, then scripts/install.sh; if tests/fixtures/corpus/ is empty, regenerate it with ~/.venvs/MuseClaude/bin/python -I tools/fixtures/make_corpus_fixtures.py; run scripts/test.sh, every tests/agent/*.py, and tests/agent/scenarios/run.py (first with --update-goldens for N03, N04, TX* and PT*, review the goldens, then without); fix failures at their root and rerun everything; when all pass, merge agent/4.7-nfhpdz into agent/4.7 per PLAN.md and push. G2 has no soak (Liam, DECISIONS 2026-10-30). Then give me a short report and STOP for G2 sign-off.'

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Checks"
[[ "$(uname -m)" == "arm64" ]] || { echo "This script expects an Apple silicon Mac."; exit 1; }
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Installing the Command Line Tools: finish the dialog, then run this file again."
    xcode-select --install || true
    exit 0
fi

say "Homebrew and build tools"
if ! command -v brew >/dev/null 2>&1; then
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    eval "$(/opt/homebrew/bin/brew shellenv)"
fi
brew install cmake ninja wget p7zip ccache

say "Qt 6.10.2"
if [[ ! -x "$QT_DIR/bin/qmake" ]]; then
    mkdir -p "$HOME/Qt/6.10.2"
    tmp="$(mktemp -d)"
    wget -q --show-progress -O "$tmp/qt.7z" "$QT_URL"
    7z x "$tmp/qt.7z" -o"$QT_DIR" >/dev/null
    rm -rf "$tmp"
fi
"$QT_DIR/bin/qmake" -query QT_VERSION

say "MuseScore prebuilt dependencies"
if [[ ! -d "$DEPS_DIR/lib" ]]; then
    tmp="$(mktemp -d)"
    wget -q --show-progress -O "$tmp/deps.tar.gz" "$DEPS_URL"
    mkdir -p "$DEPS_DIR"
    tar xf "$tmp/deps.tar.gz" -C "$DEPS_DIR"
    rm -rf "$tmp"
fi

say "Repository ($BRANCH)"
mkdir -p "$HOME/dev"
if [[ ! -d "$REPO_DIR/.git" ]]; then
    git clone "$REPO_URL" "$REPO_DIR"
fi
cd "$REPO_DIR"
git remote get-url upstream >/dev/null 2>&1 || git remote add upstream https://github.com/musescore/MuseScore.git
git fetch origin agent/4.7 "$BRANCH"
git switch "$BRANCH" 2>/dev/null || git switch -c "$BRANCH" --track "origin/$BRANCH"
git pull --ff-only origin "$BRANCH"

say "Signing identity (asks for your login keychain password)"
scripts/make-signing-identity.sh

say "Python for the corpus fixtures"
if [[ ! -x "$HOME/.venvs/MuseClaude/bin/python" ]]; then
    python3 -m venv "$HOME/.venvs/MuseClaude"
fi
"$HOME/.venvs/MuseClaude/bin/python" -m pip install -q --upgrade pip music21

say "Claude Code"
if ! command -v claude >/dev/null 2>&1; then
    curl -fsSL https://claude.ai/install.sh | bash
    export PATH="$HOME/.local/bin:$PATH"
fi

say "Starting Claude Code in $REPO_DIR"
cd "$REPO_DIR"
exec claude "$PROMPT"
