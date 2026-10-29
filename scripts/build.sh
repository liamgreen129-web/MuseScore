#!/usr/bin/env bash
# AGENT: configure, build and install MuseScore Agent (arm64, RelWithDebInfo)
# into agent.install/, then run check-identity.sh.
#
# Usage: scripts/build.sh [extra ninja_build.sh args]
# Log: run as `scripts/build.sh > build.log 2>&1` for long builds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source scripts/env.sh

for tool in cmake ninja ccache; do
    command -v "$tool" >/dev/null || { echo "error: $tool not found (brew install $tool)" >&2; exit 1; }
done
[[ -x "$QT_ROOT_DIR/bin/qmake" ]] || { echo "error: Qt not found at $QT_ROOT_DIR" >&2; exit 1; }
[[ -d "$MUSESCORE_MACOS_DEPS_PATH" ]] || { echo "error: deps not found at $MUSESCORE_MACOS_DEPS_PATH" >&2; exit 1; }

# ninja_build.sh has no pass-through for extra cmake options and does not
# override cached ones, so seed the cache with what it can't set.
mkdir -p build.release
cat > build.release/agent-cache-seed.cmake <<'EOF'
set(MUE_BUILD_MACOS_INTEGRATION OFF CACHE BOOL "AGENT: no Quick Look extension sharing the official id" FORCE)
set(CMAKE_C_COMPILER_LAUNCHER ccache CACHE STRING "AGENT: ccache" FORCE)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache CACHE STRING "AGENT: ccache" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "AGENT: cmake 4.x vs old thirdparty minimums" FORCE)
EOF
if [[ ! -f build.release/CMakeCache.txt ]] || ! grep -q '^MUE_BUILD_MACOS_INTEGRATION:BOOL=OFF' build.release/CMakeCache.txt; then
    cmake -S . -B build.release -GNinja -C build.release/agent-cache-seed.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DMUE_COMPILE_MACOS_PRECOMPILED_DEPS_PATH="$MUSESCORE_MACOS_DEPS_PATH" -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
fi

echo "== $(date) build start, revision $(git rev-parse --short=7 HEAD), jobs $AGENT_JOBS"
ccache --zero-stats >/dev/null

MUSESCORE_MACOS_DEPS_PATH="$MUSESCORE_MACOS_DEPS_PATH" \
CMAKE_OSX_ARCHITECTURES="arm64" \
MUSESCORE_INSTALL_DIR="../agent.install" \
MUSE_APP_BUILD_MODE=release \
MUSESCORE_BUILD_NUMBER="$(date +%y%m%d%H)" \
MUSESCORE_REVISION="$(git rev-parse --short=7 HEAD)" \
MUSESCORE_BUILD_VST_MODULE=ON \
MUSESCORE_BUILD_WEBSOCKET=ON \
MUSESCORE_MODULE_UPDATE=OFF \
MUSESCORE_BUILD_CRASHPAD_CLIENT=OFF \
bash ./ninja_build.sh -t installrelwithdebinfo -j "$AGENT_JOBS" "$@"

echo "== $(date) build done"
ccache --show-stats | head -12
scripts/check-identity.sh
