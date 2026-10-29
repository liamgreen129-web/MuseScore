# Decisions

- 2026-10-28 · Fork point v4.7.6 (96480d5) · matches the daily app.
- 2026-10-28 · Separate app, not side-by-side dev mode · Liam: "completely separate app".
- 2026-10-28 · Build on home Mac mini via Claude Code · planning only until G0 prerequisites are confirmed.
- 2026-10-28 · Muse Sounds path is ~/Library/Application Support/MuseSampler/lib (GenericDataLocation), correcting the assessment, which put it under MuseScore/.

## P0.1 preflight results (2026-10-29)

- 2026-10-29 · **Build machine is a MacBook Pro (MacBookPro18,3, M1 Pro, 8 cores = 6P+2E, 16 GB RAM), not the Mac mini** · preflight ran here; needs Liam's call on whether this is the build machine.
- 2026-10-29 · arm64 confirmed; macOS 26.6.1 (25G76) · well above anything Qt 6.10.2 needs.
- 2026-10-29 · Xcode: Command Line Tools only (clang 21.0.0, /Library/Developer/CommandLineTools); no Xcode.app · [verify] still open: whether the MuseScore macOS build/packaging (macdeployqt, actool for icons) works with CLT alone. If it needs full Xcode, that is a >1 GB download → STOP.
- 2026-10-29 · Free disk 41 GiB of 460 GiB (91% used) · passes the 35 GB minimum by only 6 GB. A single canonical tree fits; parallel implementer worktrees (30 GB free + 15–20 GB each) do **not** fit without freeing space.
- 2026-10-29 · 16 GB RAM / 8 cores · default ninja -j8 likely OK with unity builds; watch for memory pressure, fall back to -j6 if swapping. Plan's "max 2 implementers" should be **1** on this machine.
- 2026-10-29 · Homebrew 7.0.11 present; cmake, ninja, wget, p7zip, ccache all **not installed** (P0.2).
- 2026-10-29 · GitHub CLI logged in as `liamgreen129-web` · fork owner for P0.3.
- 2026-10-29 · **Official app is `/Applications/MuseScore 4.app`, version 4.7.4** (bundle id org.musescore.MuseScore, name "MuseScore Studio", executable mscore), not "MuseScore Studio 4.app" at 4.7.6 · the fork point v4.7.6 no longer "matches the daily app"; Liam to choose: update daily app to 4.7.6, or fork at v4.7.4. `.claude/settings.json` deny rules point at the wrong app path and need fixing.
- 2026-10-29 · MuseScore 3.app also installed · untouched, irrelevant to separation.
- 2026-10-29 · **Muse Sounds library is at `/usr/local/lib/libMuseSamplerCoreLib.dylib`** (universal x86_64+arm64, dated 16 May 2024, installed by Muse Hub), **not** `~/Library/Application Support/MuseSampler/lib/` (that folder exists but holds only `.config`) · corrects the 2026-10-28 line above. /usr/local/lib is app-name independent, so renaming the app still doesn't hide it; G0 still has to confirm the sampler loads in an ad-hoc-signed build.
- 2026-10-29 · Official settings folder confirmed at ~/Library/Application Support/MuseScore/MuseScore4 · snapshot it before the first agent-build launch (G0 check).
- 2026-10-29 · Nothing pre-existing at ~/Qt, ~/musescore_deps_macos, ~/dev, ~/.ccache · clean slate for P0.2.

## P0.2 (partial) and P0.3 (2026-10-29)

- 2026-10-29 · Start on the MacBook Pro, move to the Mac mini later · Liam. Everything in git carries over via the fork; the mini redoes P0.1 preflight, P0.2 installs and the first build, and signs off G0 itself.
- 2026-10-29 · Fork at v4.7.6 (96480d5), still the latest release · Liam: "go with the latest". Liam updates the daily app from 4.7.4 to 4.7.6 himself.
- 2026-10-29 · Repo at ~/dev/musescore-agent on every machine · same path keeps CCACHE_BASEDIR and scripts identical.
- 2026-10-29 · Public fork github.com/liamgreen129-web/MuseScore · PHASE0 §7 default.
- 2026-10-29 · Blobless partial clone (`--filter=blob:none`) · full history is ~1.3 GB, over the 1 GB download STOP; blobless clone downloaded under 250 MB and still has every tag for sync-upstream.sh.
- 2026-10-29 · settings.json deny rules now cover `/Applications/MuseScore 4.app`, the old `MuseScore Studio 4.app` name, and `/usr/local/lib/libMuseSampler*` · preflight found the real paths.
- 2026-10-29 · Brew installed cmake 4.4.5, ninja 1.13.2, wget 1.25.0, p7zip 17.05, ccache 4.14.2 · [verify] at first configure: cmake 4.x removed compatibility with `cmake_minimum_required` < 3.5; if any thirdparty CMakeLists trips on it, add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` in scripts/build.sh.
- 2026-10-29 · Implementers in parallel: max 1 on the MacBook Pro (16 GB RAM, 41 GB free); max 2 stays the rule on the mini.

## P0.2 finished, P0.4 drafted (2026-10-29)

- 2026-10-29 · Qt 6.10.2 (CI archive) in ~/Qt/6.10.2/macos, prebuilt deps in ~/musescore_deps_macos · downloads were 78 MB and 2 MB, not >1 GB; Qt unpacks to 561 MB. qmake reports 6.10.2.
- 2026-10-29 · [verify resolved] The .app bundle is named from OUTPUT_NAME = EXECUTABLE_NAME (mscore → `mscore.app`), not the title · install.sh must copy it to `/Applications/MuseScore Agent.app`.
- 2026-10-29 · [verify resolved] Release mode turns MUSE_APP_UNSTABLE off (SetupConfigure.cmake:66), so the `#else` branches of main.cpp and ipc.h are the ones built; the patch changes both branches anyway.
- 2026-10-29 · Window title also changed (MUSE_APP_TITLE "MuseScore Agent" in version.cmake) · it also sets the bundle display name; the only other uses are Windows/Linux packaging.
- 2026-10-29 · Added beyond PHASE0 §3: document types moved from UTExportedTypeDeclarations to UTImportedTypeDeclarations · two apps exporting the same UTIs lets macOS pick either as the type's owner.
- 2026-10-29 · Added beyond PHASE0 §3: URL scheme `musescore` → `musescore-agent` · otherwise musescore:// links from the web could open in the agent build; in-app `musescore://` page URIs are internal routing and unaffected.
- 2026-10-29 · Update module off and crashpad off stay build options (scripts/build.sh), not source edits · ninja_build.sh already defaults crashpad OFF.
- 2026-10-29 · check-identity.sh has 10 checks; run against the official 4.7.4 app it fails all 10 as expected.
- 2026-10-29 · Found a sixth shared item: the Quick Look extension (`src/macos_integration`, id `org.musescore.MuseScore.QuickLookPreviewExtension`, built by default via MUE_BUILD_MACOS_INTEGRATION=ON) · a second copy with the same id could take over Finder previews of .mscz. Turn it off with `-DMUE_BUILD_MACOS_INTEGRATION=OFF`, seeded into build.release's CMake cache by scripts/build.sh before ninja_build.sh runs (ninja_build.sh has no pass-through for extra cmake options and doesn't override cached ones). The same seeding carries the ccache launcher. check-identity.sh now fails on any embedded .appex with an org.musescore.* id.
