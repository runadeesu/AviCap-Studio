#!/usr/bin/env bash
# Cross-builds the Windows x64 third-party dependencies of AviCap Studio with
# MinGW-w64 on a Linux host. Everything is built from pinned upstream sources
# (no prebuilt binaries are downloaded).
#
#   tools/deps/build_deps_mingw.sh [PREFIX]
#
# Default PREFIX: <repo>/third_party/prefix-mingw64 (git-ignored).
#
# Resulting FFmpeg is LGPL-2.1-or-later (no --enable-gpl, no --enable-nonfree).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${1:-$REPO_ROOT/third_party/prefix-mingw64}"
WORK="${AVICAP_DEPS_WORK:-$REPO_ROOT/build/deps-mingw-src}"
HOST=x86_64-w64-mingw32
JOBS="${JOBS:-$(nproc)}"

ZLIB_TAG=v1.3.1
DAV1D_TAG=1.5.1
NVCODEC_TAG=n13.0.19.0
AMF_TAG=v1.5.3
LIBVPL_TAG=v2.17.0
FFMPEG_TAG=n8.1.3

mkdir -p "$PREFIX" "$WORK"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

clone() { # url tag dir
  if [ ! -d "$WORK/$3" ]; then
    git clone --depth 1 -b "$2" "$1" "$WORK/$3"
  fi
}

stamp() { [ -f "$PREFIX/.stamp-$1" ]; }
mark()  { touch "$PREFIX/.stamp-$1"; }

# ---------------------------------------------------------------- zlib
if ! stamp zlib-$ZLIB_TAG; then
  clone https://github.com/madler/zlib.git $ZLIB_TAG zlib
  pushd "$WORK/zlib" >/dev/null
  make -f win32/Makefile.gcc clean >/dev/null 2>&1 || true
  make -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a
  mkdir -p "$PREFIX/include" "$PREFIX/lib/pkgconfig"
  cp zlib.h zconf.h "$PREFIX/include/"
  cp libz.a "$PREFIX/lib/"
  cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<PC
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library
Version: 1.3.1
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
PC
  popd >/dev/null
  mark zlib-$ZLIB_TAG
fi

# ---------------------------------------------------------------- dav1d (AV1 software decoder, BSD-2)
if ! stamp dav1d-$DAV1D_TAG; then
  clone https://code.videolan.org/videolan/dav1d.git $DAV1D_TAG dav1d || \
    clone https://github.com/videolan/dav1d.git $DAV1D_TAG dav1d
  cat > "$WORK/meson-cross-mingw64.txt" <<CROSS
[binaries]
c = '$HOST-gcc-posix'
cpp = '$HOST-g++-posix'
ar = '$HOST-ar'
strip = '$HOST-strip'
windres = '$HOST-windres'
pkg-config = 'pkg-config'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS
  rm -rf "$WORK/dav1d/build"
  meson setup "$WORK/dav1d/build" "$WORK/dav1d" --cross-file "$WORK/meson-cross-mingw64.txt" \
    --prefix "$PREFIX" --libdir lib --default-library static --buildtype release \
    -Denable_tools=false -Denable_tests=false
  ninja -C "$WORK/dav1d/build" -j"$JOBS" install
  mark dav1d-$DAV1D_TAG
fi

# ---------------------------------------------------------------- NVIDIA codec headers (MIT)
if ! stamp nvcodec-$NVCODEC_TAG; then
  clone https://github.com/FFmpeg/nv-codec-headers.git $NVCODEC_TAG nv-codec-headers
  make -C "$WORK/nv-codec-headers" PREFIX="$PREFIX" install
  mark nvcodec-$NVCODEC_TAG
fi

# ---------------------------------------------------------------- AMD AMF headers (MIT)
if ! stamp amf-$AMF_TAG; then
  if [ ! -d "$WORK/AMF" ]; then
    git clone --depth 1 -b $AMF_TAG --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "$WORK/AMF"
    git -C "$WORK/AMF" sparse-checkout set amf/public/include
  fi
  mkdir -p "$PREFIX/include/AMF"
  cp -r "$WORK/AMF/amf/public/include/"* "$PREFIX/include/AMF/"
  mark amf-$AMF_TAG
fi

# ---------------------------------------------------------------- Intel oneVPL dispatcher (MIT) for Quick Sync
if ! stamp libvpl-$LIBVPL_TAG; then
  clone https://github.com/intel/libvpl.git $LIBVPL_TAG libvpl
  rm -rf "$WORK/libvpl/build-mingw"
  cmake -S "$WORK/libvpl" -B "$WORK/libvpl/build-mingw" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
    -DCMAKE_C_COMPILER=$HOST-gcc-posix -DCMAKE_CXX_COMPILER=$HOST-g++-posix -DCMAKE_RC_COMPILER=$HOST-windres \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DINSTALL_EXAMPLES=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_EXPERIMENTAL=OFF
  cmake --build "$WORK/libvpl/build-mingw" -j"$JOBS"
  cmake --install "$WORK/libvpl/build-mingw"
  # The static dispatcher needs the C++ runtime and ole32 when linked into
  # FFmpeg; link the runtimes statically so no MinGW runtime DLL is required.
  sed -i -e 's/ -lstdc++//g' -e 's/^Libs: \(.*\)$/Libs: \1 -l:libstdc++.a -l:libwinpthread.a -lole32 -luuid/' "$PREFIX/lib/pkgconfig/vpl.pc"
  mark libvpl-$LIBVPL_TAG
fi

# ---------------------------------------------------------------- FFmpeg (LGPL)
if ! stamp ffmpeg-$FFMPEG_TAG; then
  clone https://github.com/FFmpeg/FFmpeg.git $FFMPEG_TAG ffmpeg
  mkdir -p "$WORK/ffmpeg-build"
  pushd "$WORK/ffmpeg-build" >/dev/null
  "$WORK/ffmpeg/configure" \
    --prefix="$PREFIX" \
    --arch=x86_64 --target-os=mingw32 --cross-prefix=$HOST- \
    --cc=$HOST-gcc-posix --cxx=$HOST-g++-posix \
    --pkg-config=pkg-config --pkg-config-flags=--static \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect \
    --enable-w32threads \
    --enable-zlib \
    --enable-libdav1d \
    --enable-d3d11va --enable-dxva2 \
    --enable-mediafoundation \
    --enable-ffnvcodec --enable-nvenc --enable-nvdec --enable-cuvid \
    --enable-amf \
    --enable-libvpl \
    --enable-schannel \
    --extra-cflags="-I$PREFIX/include -O2" \
    --extra-ldflags="-L$PREFIX/lib -static-libgcc" \
    --extra-libs="-l:libwinpthread.a"
  make -j"$JOBS"
  make install
  popd >/dev/null
  mark ffmpeg-$FFMPEG_TAG
fi

echo "Dependencies installed into $PREFIX"
