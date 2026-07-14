#!/bin/bash
# Build a distributable universal (arm64 + x86_64) ardopcf for macOS,
# statically linked against the libportaudio.a produced by
# scripts/build-portaudio.sh.
#
# Output: dist/ardopcf (universal, unsigned).  Sign afterwards with e.g.
#   codesign --force --options runtime --timestamp \
#     --sign "Developer ID Application: ..." dist/ardopcf

set -euo pipefail
cd "$(dirname "$0")/.."

PA=deps/portaudio-universal
MACOS_MIN=14.0

[ -f "$PA/lib/libportaudio.a" ] || {
	echo "static portaudio missing - run scripts/build-portaudio.sh first" >&2
	exit 1
}

build_arch() {
	local arch=$1

	make clean >/dev/null 2>&1 || true
	make ardopcf \
		CFLAGS="-g -MMD -O2 -arch $arch -mmacosx-version-min=$MACOS_MIN -I$PA/include" \
		LDFLAGS="-arch $arch -mmacosx-version-min=$MACOS_MIN" \
		PORTAUDIO_STATIC="$PA/lib/libportaudio.a" >/dev/null
	mv ardopcf "dist/ardopcf-$arch"
}

mkdir -p dist
rm -f dist/ardopcf dist/ardopcf-arm64 dist/ardopcf-x86_64

echo "Building ardopcf for arm64 ..."
build_arch arm64
echo "Building ardopcf for x86_64 ..."
build_arch x86_64

lipo -create dist/ardopcf-arm64 dist/ardopcf-x86_64 -output dist/ardopcf
rm dist/ardopcf-arm64 dist/ardopcf-x86_64

# leave the tree in a normal state for development builds
make clean >/dev/null 2>&1 || true

lipo -info dist/ardopcf
otool -L dist/ardopcf | grep -i portaudio && {
	echo "ERROR: dist/ardopcf still links the portaudio dylib" >&2
	exit 1
}
echo "Done: dist/ardopcf"
