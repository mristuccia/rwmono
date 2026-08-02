#!/bin/bash
# Builds a standalone, dependency-free rwmono for macOS (universal arm64+x86_64).
#
# LibRaw is compiled from source as a static library with a minimal
# configuration (no LCMS, no jasper, no libjpeg, no OpenMP), so the final
# binary links only against macOS system libraries (libc++, libz, libSystem).
# The one functional trade-off: raw formats that require libjpeg internally
# (lossy-compressed DNG, some Kodak formats) are not supported. Hasselblad
# 3FR and all conventionally packed Bayer raws are unaffected.
#
# Usage: scripts/build-static-macos.sh
# Output: dist/rwmono (universal binary), dist/rwmono-macos.zip

set -euo pipefail

LIBRAW_VERSION=0.22.2
MACOS_MIN=11.0
ARCHS=(-arch arm64 -arch x86_64)

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-static"
PREFIX="$WORK/libraw-install"
mkdir -p "$WORK" "$ROOT/dist"
cd "$WORK"

if [ ! -f "$PREFIX/lib/libraw_r.a" ]; then
  if [ ! -d "LibRaw-$LIBRAW_VERSION" ]; then
    curl -fLO "https://www.libraw.org/data/LibRaw-$LIBRAW_VERSION.tar.gz"
    tar xzf "LibRaw-$LIBRAW_VERSION.tar.gz"
  fi
  cd "LibRaw-$LIBRAW_VERSION"
  ./configure \
      --prefix="$PREFIX" \
      --enable-static --disable-shared \
      --disable-lcms --disable-jasper --disable-jpeg --disable-openmp \
      --disable-examples \
      CC=clang CXX=clang++ \
      CFLAGS="${ARCHS[*]} -O2 -mmacosx-version-min=$MACOS_MIN" \
      CXXFLAGS="${ARCHS[*]} -O2 -mmacosx-version-min=$MACOS_MIN" \
      LDFLAGS="${ARCHS[*]} -mmacosx-version-min=$MACOS_MIN"
  make -j"$(sysctl -n hw.ncpu)"
  make install
  cd "$WORK"
fi

clang++ -std=c++17 -O2 "${ARCHS[@]}" -mmacosx-version-min=$MACOS_MIN \
    -I"$PREFIX/include" \
    "$ROOT"/src/main.cpp "$ROOT"/src/dng_writer.cpp "$ROOT"/src/ljpeg_encoder.cpp \
    "$PREFIX/lib/libraw_r.a" -lz \
    -o "$ROOT/dist/rwmono"

codesign --force -s - "$ROOT/dist/rwmono"

echo "--- architectures:"
lipo -info "$ROOT/dist/rwmono"
echo "--- dynamic dependencies (should be system-only):"
otool -L "$ROOT/dist/rwmono"

cd "$ROOT/dist"
zip -q -X rwmono-macos.zip rwmono
echo "--- done: dist/rwmono, dist/rwmono-macos.zip"
