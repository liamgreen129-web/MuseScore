# Phase 0 runbook — toolchain, separate-app identity, vanilla build

Target: MuseScore Studio **v4.7.6** (tag commit `96480d5`, 22 Oct 2026), built on Liam's
home Mac mini as a separate app called **MuseScore Agent**, with no agent code yet.
Estimated effort: 1 engineering week, most of it waiting on the first build and then
living with the result.

Everything below was checked against the v4.7.6 source on 28 Oct 2026. Items marked
**[verify]** are things the source cannot tell us; Claude Code checks them on the Mac mini.

---

## 1. Preflight on the Mac mini

Claude Code runs these and records the results in `docs/agent/DECISIONS.md`.

| Check | Command | Need |
|---|---|---|
| Chip | `uname -m` | `arm64` (build estimates assume Apple silicon) |
| macOS | `sw_vers` | **[verify]** high enough for the Xcode that Qt 6.10.2 needs |
| Xcode CLT | `xcode-select -p`, `clang --version` | installed |
| Free disk | `df -h ~` | ≥ 35 GB (Qt ~4 GB, deps, build tree ~20 GB, headroom) |
| RAM / cores | `sysctl hw.memsize hw.ncpu` | sets `-j` and whether unity builds stay on |
| Homebrew | `brew --version` | for cmake, ninja, wget, p7zip |
| Official app | `defaults read "/Applications/MuseScore 4.app/Contents/Info" CFBundleShortVersionString` (on Liam's machines the bundle is `MuseScore 4.app`) | 4.7.6, untouched |
| Muse Sounds | `ls ~/Library/Application\ Support/MuseSampler/lib/ /usr/local/lib/libMuseSampler*` | `libMuseSamplerCoreLib.dylib` present (MacBook Pro: in `/usr/local/lib`, from Muse Hub) |

Muse Sounds path note: the library is found through Qt's `GenericDataLocation`
(`~/Library/Application Support`) + `/MuseSampler/lib/…`, independent of the app's own
name, so renaming the app does not hide it. `MUSESAMPLER_PATH` overrides it if needed.

## 2. Repository and branch

1. Create the fork on GitHub. A fork of a public repo cannot be private; if Liam wants it
   private, create an empty private repo and push a mirror of the tag instead. GPL-3.0
   imposes no obligation either way for personal use; distributing the app would.
2. On the Mac mini, in `~/dev/musescore-agent` (or wherever Liam prefers):
   ```bash
   git clone https://github.com/<liam>/MuseScore.git musescore-agent
   cd musescore-agent
   git remote add upstream https://github.com/musescore/MuseScore.git
   git fetch upstream --tags
   git switch -c agent/4.7 v4.7.6
   ```
3. Copy this planning kit in: `CLAUDE.md` and `.claude/` at the repo root,
   `docs/agent/` beside them; append `.gitignore.agent-additions` to `.gitignore`.
   First commit on `agent/4.7`.

## 3. The separate-app identity patch

The official app and this build must share **nothing** at run time except Muse Sounds
and the user's score files. Five things in 4.7.6 would otherwise be shared:

| # | Shared by default | Where | Change |
|---|---|---|---|
| 3.1 | Bundle identifier `org.musescore.MuseScore` (prefs plist, Launch Services, TCC permissions) | `version.cmake`: `MUSE_APP_GUI_IDENTIFIER` | `com.liamgreen.MuseScoreAgent` |
| 3.2 | Settings, autosave, plugins, recent files under `…/Application Support/MuseScore/MuseScore4` | `src/app/main.cpp` ~line 118: `setApplicationName("MuseScore4")` (or `MuseScore4Development` when unstable) | `setApplicationName("MuseScoreAgent")` in both branches; keep organisation `MuseScore` |
| 3.3 | **Multi-window IPC group** `musescore-app-ipc-4`: carries window activation, settings transactions and **quit requests** between instances | `src/framework/multiwindows/internal/multiprocess/ipc/ipc.h` line 38–40 | `"musescore-agent-ipc-" MUSE_APP_VERSION_MAJOR` in both branches |
| 3.4 | Update checker would offer the official 4.7.x / 5.0 download | `ninja_build.sh` → `-DMUSE_MODULE_UPDATE` | `MUSESCORE_MODULE_UPDATE=OFF` |
| 3.5 | Claims Owner rank for `.mscz`/`.mscx` etc., so double-click could start opening the fork | `buildscripts/packaging/macOS/Info.plist.in`, every `LSHandlerRank` (`Owner`) | `Alternate`; official app stays the default opener |

Also:
- Bundle display name: `MACOSX_BUNDLE_BUNDLE_NAME` → `MuseScore Agent`; installed as
  `/Applications/MuseScore Agent.app`. **[verify]** whether the install step names the
  bundle from `EXECUTABLE_NAME` or the title; rename after install if needed.
- Default scores folder follows the app name (`~/Documents/MuseScoreAgent`); set it to
  Liam's real scores folder in Preferences on first launch.
- Optional: a tinted icon so the two Dock icons are distinguishable.
- Crash reporting stays off (`MUSESCORE_BUILD_CRASHPAD_CLIENT=OFF`).

Each changed line gets an `// AGENT:` (or `# AGENT:`) comment. Commit as one
"identity" commit so it rebases as a unit.

`scripts/check-identity.sh` reads the built bundle's `Info.plist` and greps the binary
for the IPC name and application name, failing if any official value is still present.

## 4. Toolchain

Mirror the official CI (`buildscripts/ci/macos/`), with two local deviations: arm64
only (CI builds `arm64;x86_64`, which doubles compile time) and the identity options.

```bash
brew install cmake ninja wget p7zip ccache
# Qt 6.10.2, the exact prebuilt archive CI uses (install_qt.sh):
mkdir -p ~/Qt/6.10.2 && cd /tmp
wget -O Qt-6.10.2-macOS-10.15.7z \
  'https://github.com/cbjeukendrup/musescore_build_qt/releases/download/v21960860930/Qt-6.10.2-macOS-10.15.7z'
7z x Qt-6.10.2-macOS-10.15.7z -o"$HOME/Qt/6.10.2/macos"
# Prebuilt deps (download_dependencies.sh) → ~/musescore_deps_macos
```

Environment for every build (put in `scripts/env.sh`):
```bash
export QT_ROOT_DIR=$HOME/Qt/6.10.2/macos
export PATH=$QT_ROOT_DIR/bin:$PATH
export QT_PLUGIN_PATH=$QT_ROOT_DIR/plugins QML2_IMPORT_PATH=$QT_ROOT_DIR/qml
# ccache: required so parallel worktree builds (Phases 2–3) reuse objects
export CCACHE_DIR=$HOME/.ccache CCACHE_BASEDIR=$HOME/dev CCACHE_MAXSIZE=30G
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
```
**[verify]** that ninja_build.sh passes the launcher through (it does not set
`CMAKE_*_COMPILER_LAUNCHER` itself; if the env vars are ignored, add
`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` in `scripts/build.sh`).

## 5. Build

`scripts/build.sh` wraps `ninja_build.sh` with the official option set:

```bash
source scripts/env.sh
MUSESCORE_MACOS_DEPS_PATH="$HOME/musescore_deps_macos" \
CMAKE_OSX_ARCHITECTURES="arm64" \
MUSESCORE_INSTALL_DIR="../agent.install" \
MUSE_APP_BUILD_MODE=release \
MUSESCORE_BUILD_NUMBER="$(date +%y%m%d%H)" \
MUSESCORE_REVISION="$(git rev-parse --short=7 HEAD)" \
MUSESCORE_BUILD_VST_MODULE=ON \
MUSESCORE_BUILD_WEBSOCKET=ON \
MUSESCORE_MODULE_UPDATE=OFF \
MUSESCORE_BUILD_CRASHPAD_CLIENT=OFF \
bash ./ninja_build.sh -t installrelwithdebinfo
```

Why `release` mode: the daily build should behave like the stable app (no
`MUSE_APP_UNSTABLE` paths, no "Development" title). The identity patch replaces the
separation that dev mode would otherwise provide. **[verify]** grep for other
`MUSE_APP_UNSTABLE` / build-mode switches that matter (`grep -rn MUSE_APP_UNSTABLE src`).

`MUSESCORE_BUILD_WEBSOCKET=ON` keeps parity with the official binary; the agent
module will not use it, and it can be turned off later.

First build: roughly an hour on Apple silicon; run it in the background and log to
`build.log`. Expect incremental rebuilds in minutes.

Then `scripts/install.sh`: copy the bundle to `/Applications/MuseScore Agent.app`,
`codesign --force --deep --sign - …` (ad-hoc), `xattr -dr com.apple.quarantine …`.

## 6. Gate G0 — signed off by Liam

All must hold before Phase 1 starts:

- [ ] `scripts/check-identity.sh` passes.
- [ ] Both apps open at the same time, each with its own score, without interfering:
      quitting one leaves the other running; opening a second score in each spawns its
      window in the right app.
- [ ] Official app's settings, palettes, plugins and recent files are untouched after a
      session in the agent build (compare `~/Library/Application Support/MuseScore/MuseScore4`
      before and after).
- [ ] Double-clicking an `.mscz` in Finder still opens the official app.
- [ ] **Muse Sounds play** in the agent build (the one unknown the source can't answer:
      whether the proprietary sampler accepts a self-built, ad-hoc-signed host).
      If not: record what happens, try `MUSESAMPLER_PATH`, and decide whether the agent
      build can live with MuseScore Basic sounds.
- [ ] Liam uses it for real work for a few days with no crash the official app
      doesn't also have.
- [ ] `scripts/sync-upstream.sh` exists and has been dry-run (fetch tags, report the
      next 4.7.x tag if any, rebase, rebuild, run `check-identity.sh`).

## 7. Decisions still open

| Decision | Default if nobody says otherwise |
|---|---|
| Mac mini model, macOS, free disk | Preflight §1 records them |
| Bundle id / app name | `com.liamgreen.MuseScoreAgent` / "MuseScore Agent" |
| Fork public or private mirror | Public fork |
| Agent MCP port base (Phase 1) | **2412**, not upstream's 2212, so a future official 5.x with its own MCP server can run beside it |
| Fixture scores (Phase 1) | Liam picks 10–20 before Phase 1 |
| Keep mcp-score | Yes: it keeps working against the official app, untouched |

## 8. While the first build runs

The first build takes about an hour. The conductor uses that time for task P0.S in
`PLAN.md`: up to four `api-scout` subagents writing spec sheets, read-only, in the
order listed in `COMMANDS.md`. Nothing they do touches the build.

## 9. First message to Claude Code on the Mac mini

> Read CLAUDE.md, then docs/agent/STATE.md and PLAN.md. Do task P0.1: run the
> PHASE0.md §1 preflight and record the results in DECISIONS.md. Then stop and show
> me the results and any [verify] items you resolved. Don't install or build anything.
