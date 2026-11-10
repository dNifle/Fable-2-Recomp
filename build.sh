#!/usr/bin/env bash
# Build the Fable 2 ReXGlue project on Linux.
#
# Toolchain: clang + Ninja (the SDK headers use clang builtins, so plain GCC
# cannot compile the generated code; the SDK requires clang 18+). Preset:
# linux-amd64-debug (out/build/linux-amd64-debug) by default,
# linux-amd64-release with -release.
#
# Usage:
#   build.sh                  build fable_2_codegen (runs codegen from fable_2_manifest.toml)
#   build.sh fable_2          build the full recompiled executable
#   build.sh launcher         build the launcher, self-contained by default
#                             (bundles the .NET 8 runtime, so users don't need to install .NET)
#   build.sh launcher-dev     build the small framework-dependent launcher
#                             (needs the .NET 8 runtime installed)
#   build.sh package          build the Release game and launcher, then the
#                             AppImage/zips in out/dist
#   build.sh <other target>   build any other CMake target
#   build.sh -release [t]     build as Release (-O3) instead of Debug
#   build.sh -r [t]           (same, short form)
#                             (out/build/linux-amd64-release; stages the
#                             release rexruntime/rexgpu-xenos plugins)
#   build.sh -clean [t]       wipe the build dir for the config instead of
#   build.sh -c [t]             building
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT="$PWD"

# The launcher only needs the .NET SDK, not the much larger C++ toolchain.
# Self-contained is the default so the shipped binary needs no .NET install.
case "${1:-}" in
    launcher)
        exec "$ROOT/launcher/build-launcher.sh"
        ;;
    launcher-dev)
        exec "$ROOT/launcher/build-launcher.sh" dev
        ;;
    package)
        "$0" -release fable_2
        "$ROOT/launcher/build-launcher.sh"
        exec python3 "$ROOT/tools/package_release.py"
        ;;
esac

# Toolchain discovery (clang is required; see header).
for tool in clang++ ninja cmake python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool not found on PATH." >&2
        echo "       Install LLVM 18+ (clang), Ninja, CMake and Python 3." >&2
        exit 1
    fi
done

# Argument parsing: -release / -r select the Release preset, -clean / -c
# wipes the build dir instead of building, the first non-flag argument is
# the CMake target (default: fable_2_codegen).
case "$(uname -m)" in
    x86_64)        ARCH=amd64 ;;
    aarch64|arm64) ARCH=arm64 ;;
    *) echo "Error: unsupported architecture $(uname -m)." >&2; exit 1 ;;
esac
CONFIG="linux-$ARCH-debug"
TARGET=""
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        -release|-r) CONFIG="linux-$ARCH-release"; shift ;;
        -clean|-c)   CLEAN=1; shift ;;
        *)           TARGET="$1"; shift ;;
    esac
done
if [ -z "$TARGET" ]; then TARGET=fable_2_codegen; fi

# Special target: fable_2_profiler = Release (-O3) + symbols, but WITHOUT the
# post-build stable-hash PE cleaning (a no-op on Linux), kept for parity with
# the Windows flow. Builds into out/build/linux-amd64-release-profiling.
if [ "$TARGET" = "fable_2_profiler" ]; then
    CONFIG="linux-$ARCH-release-profiling"
    TARGET=fable_2
fi

# Clean: wipe the whole build dir for the selected config (a full clean is
# just deleting the tree for a Ninja + CMake build) and stop.
if [ "$CLEAN" = "1" ]; then
    if [ -d "out/build/$CONFIG" ]; then
        rm -rf "out/build/$CONFIG"
        echo "Cleaned out/build/$CONFIG"
    else
        echo "Nothing to clean: out/build/$CONFIG does not exist"
    fi
    exit 0
fi

# Pinned source submodule, needed by every build.
SDKSRC="$ROOT/thirdparty/rexglue-sdk"
if [ ! -f "$SDKSRC/CMakeLists.txt" ]; then
    git submodule update --init thirdparty/rexglue-sdk
fi

# Debug must carry the same fixes as Release, with its own debug libs.
RUNTIMECONFIG=Release
RUNTIMEDIR=runtime-sdk
if [ "$CONFIG" = "linux-$ARCH-debug" ]; then
    RUNTIMECONFIG=Debug
    RUNTIMEDIR=runtime-sdk-debug
fi
"$ROOT/tools/build_runtime_sdk.sh" "$RUNTIMECONFIG"
REXSDK="$ROOT/out/tooling/$RUNTIMEDIR/linux-$ARCH"
# The codegen tool only ships in the Release SDK, whichever config is built.
CODEGENTOOL="$ROOT/out/tooling/runtime-sdk/linux-$ARCH/bin/rexglue"
if [ ! -x "$CODEGENTOOL" ]; then
    echo "Error: codegen tool not found at $CODEGENTOOL" >&2
    exit 1
fi
# GNUInstallDirs may install to lib/ or lib64/ depending on the distro.
REXGLUE_DIR="$(ls -d "$REXSDK"/lib*/cmake/rexglue 2>/dev/null | head -n1 || true)"
if [ -z "$REXGLUE_DIR" ]; then
    echo "Error: rexglue CMake package not found under $REXSDK/lib*/cmake/rexglue" >&2
    exit 1
fi

# Bootstrap CMake integration from the existing manifest on a fresh clone.
# Do not run rexglue init: that would replace the project's authored files.
if [ ! -f generated/rexglue.cmake ]; then
    "$CODEGENTOOL" codegen fable_2_manifest.toml
fi

cmake --preset "$CONFIG" \
    -DCMAKE_PREFIX_PATH="$REXSDK" \
    -Drexglue_DIR="$REXGLUE_DIR" \
    -DREXGLUE_SDK_ROOT="$REXSDK" \
    -DREXGLUE_SDK_SOURCE="$SDKSRC" \
    -DFABLE2_CODEGEN_TOOL="$CODEGENTOOL"
# CMake can only list the generated sources once codegen has written them.
if [ ! -f generated/default/sources.cmake ]; then
    cmake --build "out/build/$CONFIG" --target fable_2_codegen
    cmake "out/build/$CONFIG"
fi
cmake --build "out/build/$CONFIG" --target "$TARGET"
