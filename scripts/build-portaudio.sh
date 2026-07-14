#!/bin/bash
# Build a static universal (arm64 + x86_64) libportaudio.a for macOS.
#
# The result lands in deps/portaudio-universal/ (lib + headers) and is
# consumed by scripts/build-macos-universal.sh via the Makefile's
# PORTAUDIO_STATIC variable.  deps/ is gitignored; rerun this script to
# reproduce the library from the pinned release tag.

set -euo pipefail
cd "$(dirname "$0")/.."

# The last tagged release (v19.7.0, 2021) no longer compiles with
# current clang; pin a known-good master commit instead.
PA_COMMIT=f88b5841575b43bfa024a6861635b69d7eb98d6d
DEPS=deps
SRC=$DEPS/portaudio
OUT=$DEPS/portaudio-universal
MACOS_MIN=14.0

mkdir -p "$DEPS"

if [ ! -d "$SRC" ]; then
	git clone https://github.com/PortAudio/portaudio.git "$SRC"
fi
git -C "$SRC" checkout --quiet "$PA_COMMIT"

build_arch() {
	local arch=$1
	local builddir=$DEPS/portaudio-build-$arch

	rm -rf "$builddir"
	mkdir -p "$builddir"
	(
		cd "$builddir"
		CFLAGS="-arch $arch -mmacosx-version-min=$MACOS_MIN" \
		LDFLAGS="-arch $arch" \
		../portaudio/configure \
			--host="$arch-apple-darwin" \
			--enable-static --disable-shared \
			--disable-mac-universal >/dev/null
		# build only the library; the out-of-tree test programs fail to
		# build and are not needed
		make -j "$(sysctl -n hw.ncpu)" lib/libportaudio.la >/dev/null
	)
	echo "$builddir/lib/.libs/libportaudio.a"
}

echo "Building portaudio $PA_COMMIT for arm64 ..."
LIB_ARM=$(build_arch arm64)
echo "Building portaudio $PA_COMMIT for x86_64 ..."
LIB_X86=$(build_arch x86_64)

mkdir -p "$OUT/lib" "$OUT/include"
lipo -create "$LIB_ARM" "$LIB_X86" -output "$OUT/lib/libportaudio.a"
cp "$SRC/include/portaudio.h" "$OUT/include/"

lipo -info "$OUT/lib/libportaudio.a"
echo "Done: $OUT/lib/libportaudio.a"
