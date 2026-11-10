#!/usr/bin/env bash
# Assemble the Linux AppImage release from a finished Release build.
#
# Usage: tools/package_appimage.sh [--version X.Y.Z]
# Inputs (built by ./build.sh -release fable_2 and ./build.sh launcher):
#   out/build/linux-amd64-release/    game binary, SDK runtime/plugin, data
#   out/tests/launcher-build-linux/   self-contained Fable2Launcher
# Output:
#   out/dist/Fable_2_recomp_v<version>_linux.AppImage + SHA256SUMS
#
# Needs appimagetool (and mksquashfs from squashfs-tools) on PATH. Nothing is
# installed here; see appimage/AppRun for how the AppImage lays out its
# writable install folder at first run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        *) echo "usage: $0 [--version X.Y.Z]" >&2; exit 2 ;;
    esac
done
if [ -z "$VERSION" ]; then
    VERSION="$(git -C "$ROOT" describe --tags 2>/dev/null || true)"
fi
if [ -z "$VERSION" ]; then
    echo "package_appimage: cannot determine the version (no git tags? pass --version)" >&2
    exit 1
fi

for tool in appimagetool mksquashfs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "package_appimage: $tool not found on PATH." >&2
        echo "  Install squashfs-tools (mksquashfs) and appimagetool, then retry." >&2
        exit 1
    fi
done

case "$(uname -m)" in
    x86_64)        APPIMAGE_ARCH=x86_64 ;;
    aarch64|arm64) APPIMAGE_ARCH=aarch64 ;;
    *) echo "package_appimage: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

BUILD="$ROOT/out/build/linux-$([ "$APPIMAGE_ARCH" = aarch64 ] && echo arm64 || echo amd64)-release"
LAUNCHER="$ROOT/out/tests/launcher-build-linux/Fable2Launcher"

require() {
    [ -f "$1" ] || { echo "package_appimage: missing $1" >&2; exit 1; }
}
require "$BUILD/fable_2"
require "$BUILD/librexruntime.so"
require "$BUILD/librexgpu-xenos.so"
require "$BUILD/fable2_build.json"
require "$BUILD/app-icon.png"
require "$BUILD/README.md"
require "$LAUNCHER"
require "$ROOT/config/fable2_config.toml"
require "$ROOT/launcher/Fable2.Launcher/XenosTiling/LICENSE.txt"
if ! ls "$BUILD"/data/scripts/recomp/*.lua >/dev/null 2>&1; then
    echo "package_appimage: no Lua scripts in $BUILD/data/scripts/recomp" >&2
    exit 1
fi

APPDIR="$ROOT/out/appimage/Fable2Recomp.AppDir"
PAYLOAD="$APPDIR/usr/share/fable2-recomp"
rm -rf "$APPDIR"
mkdir -p "$PAYLOAD/data/scripts/recomp" "$PAYLOAD/licenses" \
         "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# --- Payload (the Windows-zip equivalent; AppRun copies it on first run) ----
cp "$BUILD/fable_2" "$BUILD/librexruntime.so" "$BUILD/librexgpu-xenos.so" \
   "$BUILD/app-icon.png" "$BUILD/README.md" "$PAYLOAD/"
cp "$LAUNCHER" "$PAYLOAD/Fable2Launcher"
# From the repo: the build folder's copy may hold a developer's own settings.
cp "$ROOT/config/fable2_config.toml" "$PAYLOAD/"
cp "$BUILD"/data/scripts/recomp/*.lua "$PAYLOAD/data/scripts/recomp/"
cp "$ROOT/launcher/Fable2.Launcher/XenosTiling/LICENSE.txt" "$PAYLOAD/licenses/XenosTiling-LICENSE.txt"
# Stamp the version into fable2_build.json (the launcher's update check reads
# it) and into the marker AppRun compares against.
python3 - "$BUILD/fable2_build.json" "$PAYLOAD/fable2_build.json" "$VERSION" <<'EOF'
import json, sys
src, dst, version = sys.argv[1:4]
info = json.loads(open(src, encoding="utf8").read())
info["version"] = version
open(dst, "w", encoding="utf8").write(json.dumps(info, indent=2) + "\n")
EOF
printf '%s\n' "$VERSION" > "$PAYLOAD/appimage-version.txt"

# --- AppImage metadata ------------------------------------------------------
cp "$ROOT/appimage/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp "$ROOT/appimage/fable2recomp.desktop" "$APPDIR/fable2recomp.desktop"
cp "$ROOT/appimage/fable2recomp.desktop" "$APPDIR/usr/share/applications/"
cp "$ROOT/assets/app-icon.png" "$APPDIR/fable2recomp.png"
cp "$ROOT/assets/app-icon.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/fable2recomp.png"

# --- Build ------------------------------------------------------------------
OUT="$ROOT/out/dist"
mkdir -p "$OUT"
IMAGE="$OUT/Fable_2_recomp_v${VERSION}_linux.AppImage"
rm -f "$IMAGE"
ARCH="$APPIMAGE_ARCH" appimagetool "$APPDIR" "$IMAGE"

# SHA256SUMS covers everything currently in out/dist (zips from the Windows
# flow included, when they were packaged on the same machine).
cd "$OUT"
find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%f\n' | sort | \
    xargs -r sha256sum > SHA256SUMS
echo "Packaged $IMAGE"
