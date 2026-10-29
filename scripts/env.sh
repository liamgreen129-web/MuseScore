# AGENT: build environment for MuseScore Agent. Source it, don't run it:
#   source scripts/env.sh
# Same on every machine: the repo lives at ~/dev/musescore-agent.

export QT_ROOT_DIR="$HOME/Qt/6.10.2/macos"
export PATH="$QT_ROOT_DIR/bin:$PATH"
export QT_PLUGIN_PATH="$QT_ROOT_DIR/plugins"
export QML2_IMPORT_PATH="$QT_ROOT_DIR/qml"
export MUSESCORE_MACOS_DEPS_PATH="$HOME/musescore_deps_macos"

# ccache: required so parallel worktree builds (Phases 2-3) reuse objects
export CCACHE_DIR="$HOME/.ccache"
export CCACHE_BASEDIR="$HOME/dev"
export CCACHE_MAXSIZE=30G
export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache

# ninja jobs: 16 GB RAM with unity builds, so stay below the 8 cores
export AGENT_JOBS="${AGENT_JOBS:-6}"
