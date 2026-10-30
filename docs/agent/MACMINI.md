# Moving to the Mac mini

**Shortcut:** AirDrop `setup-mac-mini.command` (repo root) to the mini and double-click it. It does §2-§4 and starts Claude Code with the prompt for §2-§6.

Runbook for moving MuseScore Agent from the MacBook Pro to the Mac mini (DECISIONS
2026-10-29: "Start on the MacBook Pro, move to the Mac mini later"). Everything in git
carries over through the fork. What does not: Qt and the prebuilt deps, ccache, the
signing identity, the corpus fixtures (git-ignored), and the build trees. The mini redoes
preflight, installs and the first build, and signs off G0 itself.

All work so far is on the branch **`agent/4.7-nfhpdz`**: the cloud session's Wave A,
**not yet compiled**. `agent/4.7` holds the last Mac-verified state (8e7a2425d2).

## 1. On the MacBook Pro, before leaving

1. In `~/dev/musescore-agent`: `git status`. Commit and push anything local (`git push`).
2. Copy the corpus fixtures to the mini (they are git-ignored; regenerating them needs a
   working build and network):
   `tests/fixtures/corpus/` (20 `.mscz` files) → AirDrop or `scp -r` to the same path on the mini.
3. Optional, saves about 80 MB of downloads: copy `~/Qt/6.10.2/macos` and `~/musescore_deps_macos`.
4. Nothing else moves. The "MuseScore Agent Local" certificate is made fresh on the mini (§4).

## 2. Preflight on the mini (P0.1)

Run PHASE0.md §1 and record each line in DECISIONS.md: chip, macOS, Command Line Tools,
free disk (≥ 35 GB; ≥ 65 GB for two implementers at once), RAM and cores, Homebrew, the
official app's version (should be 4.7.6), Muse Sounds.

Then set `AGENT_JOBS` for the mini's cores and RAM (env.sh defaults to 6, sized for 16 GB),
e.g. add `export AGENT_JOBS=10` to `~/.zshrc` on a 12-core mini with 32 GB.

## 3. Installs (P0.2)

```bash
xcode-select --install                 # if Command Line Tools are missing; no Xcode.app needed
brew install cmake ninja wget p7zip ccache
mkdir -p ~/Qt/6.10.2 && cd /tmp        # skip if copied in §1
wget -O Qt-6.10.2-macOS-10.15.7z \
  'https://github.com/cbjeukendrup/musescore_build_qt/releases/download/v21960860930/Qt-6.10.2-macOS-10.15.7z'
7z x Qt-6.10.2-macOS-10.15.7z -o"$HOME/Qt/6.10.2/macos"
# prebuilt deps → ~/musescore_deps_macos (as in PHASE0.md §4, download_dependencies.sh)
```

## 4. Repository and signing

```bash
mkdir -p ~/dev && cd ~/dev
git clone https://github.com/liamgreen129-web/MuseScore.git musescore-agent
cd musescore-agent
git remote add upstream https://github.com/musescore/MuseScore.git
git fetch origin agent/4.7 agent/4.7-nfhpdz
git switch agent/4.7-nfhpdz            # the newest work; agent/4.7 is the last verified state
scripts/make-signing-identity.sh       # new "MuseScore Agent Local" in the login keychain
```

The certificate's hash differs from the MacBook Pro's, so macOS asks for Documents access
once more on the mini, then remembers it.

## 5. First build and G0 on the mini

```bash
scripts/build.sh            # first build ~10 min at -j6 on the M1 Pro; ccache is cold
scripts/check-identity.sh   # must PASS before install (CLAUDE.md rule 1)
scripts/install.sh          # /Applications/MuseScore Agent.app
```

Expect compile errors first: the cloud work was only syntax-checked without Qt. Fix them
before anything else. Then re-run the G0 checklist (`docs/agent/G0-checklist.md`) on the mini,
including the two items deferred on the MacBook Pro: the reverse-quit check, and Muse Sounds
playback (if Muse Sounds is installed for 4.7 there).

## 6. Verify the cloud work, then merge

```bash
scripts/test.sh                                   # unit tests, incl. text_tests and pitch_tests
python3 -I tests/agent/test_server.py
python3 -I tests/agent/test_hello.py
python3 -I tests/agent/test_read.py
python3 -I tests/agent/test_core.py
python3 -I tests/agent/test_export.py
python3 -I tests/agent/scenarios/run.py --update-goldens   # once for N03, N04, TX*, PT*; review them
python3 -I tests/agent/scenarios/run.py                    # then everything, no FAIL
```

Also check that `test_modal`'s dialog does not bring the hidden instance forward (S18).
When all of it passes, merge `agent/4.7-nfhpdz` into `agent/4.7` per PLAN.md's protocol.

## 7. No soak for G2

Liam dropped the soak from G2 (DECISIONS 2026-10-30).

## 8. First message to Claude Code on the mini

> Read CLAUDE.md, docs/agent/STATE.md and docs/agent/MACMINI.md. We have moved to the Mac
> mini. Do MACMINI.md §2 and §3, then §4 and §5. Ask me before any `sudo` or keychain step.
> Fix compile errors, then do §6 and report the results.
