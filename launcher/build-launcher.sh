#!/bin/sh
# POSIX equivalent of build-launcher.cmd for Linux and cross-publishing.
# Builds the Avalonia launcher self-contained (the publish bundles the .NET 8
# runtime, so users need no .NET install).
#
# Usage: build-launcher.sh [linux|win|all] [dev]
#   linux  linux-x64 self-contained single file -> out/tests/launcher-build-linux/Fable2Launcher
#   win    win-x64 self-contained single file   -> out/tests/launcher-build/Fable2Launcher.exe
#   all    both (default)
#   dev    framework-dependent build (small, fast; needs the .NET 8 runtime on
#          the target machine) instead of self-contained
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROJECT="$ROOT/launcher/Fable2.Launcher/Fable2.Launcher.csproj"
WIN_OUTPUT="$ROOT/out/tests/launcher-build"
LINUX_OUTPUT="$ROOT/out/tests/launcher-build-linux"

TARGET=${1:-all}
SELF_CONTAINED=true
if [ "${2:-}" = "dev" ] || [ "${2:-}" = "framework-dependent" ]; then
  SELF_CONTAINED=false
fi

build()
{
  rid=$1
  output=$2
  if [ "$SELF_CONTAINED" = "true" ]; then
    dotnet publish "$PROJECT" -c Release -r "$rid" --self-contained true \
      -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
      -o "$output"
  else
    dotnet publish "$PROJECT" -c Release -r "$rid" --self-contained false \
      -p:PublishSingleFile=true -o "$output"
  fi
}

case "$TARGET" in
  linux) build linux-x64 "$LINUX_OUTPUT"
         echo "Launcher built: $LINUX_OUTPUT/Fable2Launcher" ;;
  win)   build win-x64 "$WIN_OUTPUT"
         echo "Launcher built: $WIN_OUTPUT/Fable2Launcher.exe" ;;
  all)   build linux-x64 "$LINUX_OUTPUT"
         build win-x64 "$WIN_OUTPUT"
         echo "Launcher built: $LINUX_OUTPUT/Fable2Launcher"
         echo "Launcher built: $WIN_OUTPUT/Fable2Launcher.exe" ;;
  *)     echo "usage: $0 [linux|win|all] [dev]" >&2
         exit 2 ;;
esac
