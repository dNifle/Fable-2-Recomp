#!/usr/bin/env bash
# Build the SDK from the thirdparty/rexglue-sdk fork and stage it: runtime,
# Xenos GPU plugin (Vulkan backend - the only backend on non-Windows), codegen
# tool and CMake package. Optional arg: Debug or Release.
# Debug needs the Release SDK too, because only that one ships the tool.
# Requires clang 18+, Ninja, CMake and Python 3 on PATH.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/thirdparty/rexglue-sdk"
CONFIGURATION="${1:-Release}"
if [ "$CONFIGURATION" != "Debug" ] && [ "$CONFIGURATION" != "Release" ]; then
  echo "Error: runtime configuration must be Debug or Release." >&2
  exit 1
fi
case "$(uname -m)" in
  x86_64)        PLATFORM="linux-amd64"; MARCH="-march=x86-64-v2" ;;
  aarch64|arm64) PLATFORM="linux-arm64"; MARCH="-march=armv8-a" ;;
  *) echo "Error: unsupported architecture $(uname -m)." >&2; exit 1 ;;
esac
if [ ! -f "$SOURCE/CMakeLists.txt" ]; then
  git -C "$ROOT" submodule update --init thirdparty/rexglue-sdk || exit 1
fi
python3 "$ROOT/tools/prepare_runtime_sdk.py" "$SOURCE" || exit 1

# Local Linux portability fixes for the pinned SDK checkout (see the patch
# header). Idempotent: skipped when already applied or when the SDK carries
# the fix natively. The Windows flow builds the pristine submodule.
apply_sdk_patch() {
  local patch="$ROOT/thirdparty/rexglue-sdk-linux.patch"
  [ -f "$patch" ] || return 0
  if git -C "$SOURCE" apply --reverse --check "$patch" >/dev/null 2>&1; then
    return 0  # already applied
  fi
  if git -C "$SOURCE" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$SOURCE" apply "$patch" || exit 1
    echo "Applied thirdparty/rexglue-sdk-linux.patch to the SDK checkout"
    return 0
  fi
  echo "Error: thirdparty/rexglue-sdk-linux.patch does not apply to the SDK" >&2
  echo "       checkout; the submodule may have been updated. Refresh the patch:" >&2
  echo "       git -C thirdparty/rexglue-sdk diff > thirdparty/rexglue-sdk-linux.patch" >&2
  exit 1
}
apply_sdk_patch

build_sdk() {
  local config="$1"
  local build="$ROOT/out/build/runtime-sdk"
  local staged="$ROOT/out/tooling/runtime-sdk/$PLATFORM"
  if [ "$config" = "Debug" ]; then
    build="$ROOT/out/build/runtime-sdk-debug"
    staged="$ROOT/out/tooling/runtime-sdk-debug/$PLATFORM"
  fi
  # Only configure when needed: it stamps the SDK's version header, which would
  # rebuild the SDK on every run. The build reconfigures itself if CMake files
  # change.
  # -g: always emit debug info (Release + -g keeps production codegen but
  # produces full-symbol builds the fable_2 build can use for symbolization).
  if ! grep -q "REXGLUE_REGISTER_PACKAGE:BOOL=OFF" "$build/CMakeCache.txt" 2>/dev/null; then
    cmake -S "$SOURCE" -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE="$config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS="$MARCH -g" -DCMAKE_CXX_FLAGS="$MARCH -g" \
      -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON \
      -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF \
      -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_REGISTER_PACKAGE=OFF \
      -DCMAKE_INSTALL_MESSAGE=NEVER || exit 1
  fi
  cmake --build "$build" || exit 1
  cmake --install "$build" --prefix "$staged" || exit 1
  python3 "$ROOT/tools/stage_renderer_sdk.py" "$SOURCE" "$staged" \
    --configuration "$config" \
    --codegen-stamp "$ROOT/generated/default/codegen.stamp" || exit 1
}

build_sdk Release
if [ "$CONFIGURATION" = "Debug" ]; then
  build_sdk Debug
fi
