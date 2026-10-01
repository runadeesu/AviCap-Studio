#!/usr/bin/env bash
# Builds the Windows release on a Linux host (MinGW-w64 cross build) and
# produces in release/:
#   AviCapStudio.exe (+ its DLLs, runnable in place)
#   AviCapStudio-Setup.exe            Inno Setup installer
#   AviCap-Studio-Windows-Portable.zip portable build (settings next to the exe)
#   SHA256SUMS.txt
#
# Requirements: MinGW-w64 (posix threads), CMake, Ninja, zip, Wine with Inno
# Setup 6 (ISCC) for the installer, dependencies from tools/deps/build_deps_mingw.sh.
#
#   tools/package_windows.sh [--skip-installer] [--smoke-test]
#
# Environment: ISCC_WINEPREFIX (default /root/.wine-tools), ISCC_PATH
# (default C:\InnoSetup\ISCC.exe), APP_WINEPREFIX for the smoke test.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/mingw-release"
RELEASE="$ROOT/release"
STAGE="$RELEASE/stage"
PREFIX="${AVICAP_MINGW_PREFIX:-$ROOT/third_party/prefix-mingw64}"
DEPS_SRC="${AVICAP_DEPS_WORK:-$ROOT/build/deps-mingw-src}"
HOST=x86_64-w64-mingw32
SKIP_INSTALLER=0
SMOKE=0
for a in "$@"; do
  case "$a" in
    --skip-installer) SKIP_INSTALLER=1 ;;
    --smoke-test) SMOKE=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
echo "== AviCap Studio $VERSION: Windows release"

# ---------------------------------------------------------------- build
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/mingw-w64-x86_64.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAVICAP_BUILD_APP=ON -DAVICAP_BUILD_TESTS=OFF \
  -DAVICAP_MINGW_PREFIX="$PREFIX" > /dev/null
cmake --build "$BUILD" --target AviCapStudio avicap_cli -j"$(nproc)"

# ---------------------------------------------------------------- stage
rm -rf "$STAGE"
mkdir -p "$STAGE/licenses"
cp "$BUILD/bin/AviCapStudio.exe" "$BUILD/bin/avicap-cli.exe" "$STAGE/"

# Copy the DLLs the executables need (transitively), from the dependency prefix only.
declare -A seen=()
collect() {
  local file="$1"
  for dll in $($HOST-objdump -p "$file" | sed -n 's/^\s*DLL Name: //p'); do
    [ -n "${seen[$dll]:-}" ] && continue
    seen[$dll]=1
    if [ -f "$PREFIX/bin/$dll" ]; then
      cp "$PREFIX/bin/$dll" "$STAGE/"
      collect "$PREFIX/bin/$dll"
    fi
  done
}
collect "$STAGE/AviCapStudio.exe"
collect "$STAGE/avicap-cli.exe"
$HOST-strip --strip-unneeded "$STAGE"/*.exe "$STAGE"/*.dll

# Refuse to ship anything that still needs a non-system DLL we did not stage.
missing=0
for f in "$STAGE"/*.exe "$STAGE"/*.dll; do
  for dll in $($HOST-objdump -p "$f" | sed -n 's/^\s*DLL Name: //p'); do
    lower="$(echo "$dll" | tr 'A-Z' 'a-z')"
    case "$lower" in
      kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|shlwapi.dll|comdlg32.dll|\
      msvcrt.dll|ws2_32.dll|bcrypt.dll|ncrypt.dll|crypt32.dll|secur32.dll|d3d11.dll|d3d12.dll|dxgi.dll|d3dcompiler_47.dll|\
      dwmapi.dll|imm32.dll|mfplat.dll|mf.dll|mfreadwrite.dll|mfuuid.dll|ole32.dll|avicap32.dll|winmm.dll|version.dll|\
      dwrite.dll|d2d1.dll|windowscodecs.dll|uuid.dll|propsys.dll|avrt.dll|mmdevapi.dll|setupapi.dll|cfgmgr32.dll|\
      api-ms-win-*|ntdll.dll|psapi.dll|dbghelp.dll|comctl32.dll|uxtheme.dll|strmiids.dll|evr.dll|winhttp.dll) ;;
      *) [ -f "$STAGE/$dll" ] || { echo "ERROR: $(basename "$f") needs $dll which is not staged" >&2; missing=1; } ;;
    esac
  done
done
[ $missing -eq 0 ] || exit 1

# Licenses of everything we ship.
lic() { [ -f "$1" ] && cp "$1" "$STAGE/licenses/$2" || echo "warning: license file $1 not found" >&2; }
lic "$DEPS_SRC/ffmpeg/COPYING.LGPLv2.1" "FFmpeg-LGPL-2.1.txt"
lic "$DEPS_SRC/ffmpeg/LICENSE.md" "FFmpeg-LICENSE.md"
lic "$DEPS_SRC/dav1d/COPYING" "dav1d-BSD-2-Clause.txt"
lic "$DEPS_SRC/zlib/LICENSE" "zlib.txt"
lic "$DEPS_SRC/libvpl/LICENSE" "libvpl-MIT.txt"
lic "$DEPS_SRC/AMF/LICENSE.txt" "AMD-AMF-MIT.txt"
lic "$ROOT/third_party/imgui/LICENSE.txt" "DearImGui-MIT.txt"
cp "$ROOT/THIRD_PARTY_LICENSES.md" "$STAGE/licenses/" 2>/dev/null || true
cp "$ROOT/installer/FFMPEG-SOURCE.txt" "$STAGE/licenses/"
cp "$ROOT/installer/LICENSE-SUMMARY.txt" "$STAGE/licenses/"

# Quick-start readme (Japanese / English).
sed "s/@VERSION@/$VERSION/" "$ROOT/installer/README.txt" > "$STAGE/README.txt"

# ---------------------------------------------------------------- release folder
mkdir -p "$RELEASE"
rm -f "$RELEASE"/*.exe "$RELEASE"/*.dll "$RELEASE"/*.zip "$RELEASE"/SHA256SUMS.txt
cp "$STAGE"/*.exe "$STAGE"/*.dll "$RELEASE/"
rm -rf "$RELEASE/licenses" && cp -r "$STAGE/licenses" "$RELEASE/licenses"
cp "$STAGE/README.txt" "$RELEASE/"

# Portable ZIP: the marker file switches settings/cache/autosave next to the exe.
PORT="$RELEASE/portable-tmp/AviCap Studio"
rm -rf "$RELEASE/portable-tmp" && mkdir -p "$PORT"
cp -r "$STAGE"/. "$PORT/"
echo "AviCap Studio portable mode: user data is stored in the UserData folder next to AviCapStudio.exe." > "$PORT/portable.txt"
(cd "$RELEASE/portable-tmp" && zip -q -r -9 "$RELEASE/AviCap-Studio-Windows-Portable.zip" "AviCap Studio")
rm -rf "$RELEASE/portable-tmp"

# ---------------------------------------------------------------- installer
if [ $SKIP_INSTALLER -eq 0 ]; then
  ISCC_PREFIX="${ISCC_WINEPREFIX:-/root/.wine-tools}"
  ISCC="${ISCC_PATH:-C:\\InnoSetup\\ISCC.exe}"
  stage_w="$(WINEPREFIX="$ISCC_PREFIX" winepath -w "$STAGE")"
  out_w="$(WINEPREFIX="$ISCC_PREFIX" winepath -w "$RELEASE")"
  iss_w="$(WINEPREFIX="$ISCC_PREFIX" winepath -w "$ROOT/installer/AviCapStudio.iss")"
  WINEPREFIX="$ISCC_PREFIX" WINEDEBUG=-all wine "$ISCC" /Q "/DAppVersion=$VERSION" "/DStageDir=$stage_w" "/O$out_w" "$iss_w"
  [ -f "$RELEASE/AviCapStudio-Setup.exe" ] || { echo "ERROR: installer was not produced" >&2; exit 1; }
fi

(cd "$RELEASE" && sha256sum AviCapStudio.exe AviCap-Studio-Windows-Portable.zip $( [ -f AviCapStudio-Setup.exe ] && echo AviCapStudio-Setup.exe ) > SHA256SUMS.txt)

# ---------------------------------------------------------------- smoke test
if [ $SMOKE -eq 1 ]; then
  APP_PREFIX="${APP_WINEPREFIX:-/root/.wine-avicap}"
  # The app is 64-bit: use the 64-bit loader explicitly (with wine32 installed
  # for Inno Setup, plain "wine" starts the 32-bit loader).
  APP_WINE="${APP_WINE:-wine}"
  [ -x /usr/lib/wine/wine64 ] && APP_WINE=/usr/lib/wine/wine64
  [ -n "${DISPLAY:-}" ] || echo "WARNING: DISPLAY is not set; the smoke test needs an X server (e.g. Xvfb)" >&2
  wpath() { WINEPREFIX="$APP_PREFIX" WINELOADER="$APP_WINE" WINEDEBUG=-all "$APP_WINE" winepath.exe -w "$1" 2>/dev/null | tr -d '\r'; }
  out="$ROOT/build/release-smoke"
  rm -rf "$out" && mkdir -p "$out"
  media=()
  [ -f "$ROOT/build/test_media/av_1080p30.mp4" ] && media=(--media "$(wpath "$ROOT/build/test_media/av_1080p30.mp4")")
  WINEPREFIX="$APP_PREFIX" WINELOADER="$APP_WINE" WINEDEBUG=-all LANG=C.UTF-8 timeout 600 "$APP_WINE" "$RELEASE/AviCapStudio.exe" \
    --self-test "$(wpath "$out")" "${media[@]}" 2>/dev/null
  python3 -c "import json,sys; j=json.load(open(sys.argv[1])); print('smoke test:', 'PASSED' if j['ok'] else 'FAILED'); sys.exit(0 if j['ok'] else 1)" "$out/selftest.json"
fi

echo "== Release files:"
ls -la "$RELEASE" | grep -E "\.exe|\.zip|SHA256"
