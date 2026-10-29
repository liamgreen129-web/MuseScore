#!/usr/bin/env bash
# AGENT: build and run the agent unit tests in their own tree (build.tests).
# A separate tree because MUSE_ENABLE_UNIT_TESTS changes the framework config header,
# which would rebuild the whole app tree. Only the agent_tests target is built.
#
# Usage: scripts/test.sh [gtest args, e.g. --gtest_filter=Agent_Notation*]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source scripts/env.sh

if [[ ! -f build.tests/CMakeCache.txt ]]; then
    cmake -S . -B build.tests -GNinja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DMUE_COMPILE_MACOS_PRECOMPILED_DEPS_PATH="$MUSESCORE_MACOS_DEPS_PATH" \
        -DMUSE_ENABLE_UNIT_TESTS=ON \
        -DMUE_BUILD_MACOS_INTEGRATION=OFF \
        -DMUSE_MODULE_UPDATE=OFF \
        -DMUE_DOWNLOAD_SOUNDFONT=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
fi

ninja -C build.tests -j "$AGENT_JOBS" agent_tests
exe="$(find build.tests -name agent_tests -type f -perm -u+x | head -1)"
"$exe" "$@"
