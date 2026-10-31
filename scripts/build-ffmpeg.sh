#!/usr/bin/env bash
# Gradi staticki FFmpeg (samo dekoderi, demuxeri, parseri i swscale/swresample)
# za PS5 u $PREFIX. Isti nacin kao VLC-PS5 (ps5/deps/build-deps.sh):
# --target-os=freebsd, --cpu=znver2, bez autodetect-a i bez mreze.
#
#   PS5_PAYLOAD_SDK=... bash scripts/build-ffmpeg.sh
set -euo pipefail

FFMPEG_VERSION=${FFMPEG_VERSION:-7.1.1}
ZLIB_VERSION=${ZLIB_VERSION:-1.3.1}
ROOT=$(pwd)
PREFIX=${PREFIX:-$ROOT/ffmpeg-prefix}
WORK=${WORK:-$ROOT/ffmpeg-work}
LOGS=${LOGS:-$ROOT/ffmpeg-logs}
: "${PS5_PAYLOAD_SDK:?postavi PS5_PAYLOAD_SDK}"
SDK=$PS5_PAYLOAD_SDK
JOBS=$(nproc)

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$WORK/dl" "$WORK/src" "$LOGS"

# ---- alati iz SDK-a ----
if [ ! -x "$SDK/bin/prospero-clang" ]; then
    echo "NEMA $SDK/bin/prospero-clang. Sadrzaj $SDK/bin:" >&2
    ls -la "$SDK/bin" >&2 || true
    exit 1
fi
tool() { # ime-u-sdk rezervni
    if [ -x "$SDK/bin/$1" ]; then echo "$SDK/bin/$1"; else command -v "$2"; fi
}
export CC="$SDK/bin/prospero-clang"
export CXX="$SDK/bin/prospero-clang++"
export AR=$(tool prospero-ar llvm-ar-18)
export RANLIB=$(tool prospero-ranlib llvm-ranlib-18)
export NM=$(tool prospero-nm llvm-nm-18)
export STRIP=$(tool prospero-strip llvm-strip-18)
export CHOST=x86_64-pc-freebsd
export CFLAGS="-O2 -march=znver2 -fPIC"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_PATH || true

echo "CC=$CC"
echo "AR=$AR NM=$NM"
"$CC" --version | head -3 || true

fetch() { # url
    local f="$WORK/dl/$(basename "$1")"
    [ -f "$f" ] && return
    curl -fsSL --retry 3 "$1" -o "$f.part"
    mv -f "$f.part" "$f"
}

# ---- libm: u SDK-u je matematika u libc-u; prazna libm.a da configure-ovi
# "-lm" provjere ne padaju (isto radi VLC-PS5) ----
echo "== libm stub"
echo "" > "$WORK/empty.c"
"$CC" $CFLAGS -c "$WORK/empty.c" -o "$WORK/empty.o"
rm -f "$PREFIX/lib/libm.a"
"$AR" rcs "$PREFIX/lib/libm.a" "$WORK/empty.o"

# ---- zlib (MKV ga koristi za kompresovane zaglavlja) ----
echo "== zlib $ZLIB_VERSION"
fetch "https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz"
rm -rf "$WORK/src/zlib-$ZLIB_VERSION"
tar xf "$WORK/dl/zlib-$ZLIB_VERSION.tar.gz" -C "$WORK/src"
(
    cd "$WORK/src/zlib-$ZLIB_VERSION"
    ./configure --static --prefix="$PREFIX"
    make -j"$JOBS" libz.a
    make install
) 2>&1 | tee "$LOGS/zlib.log"

# ---- FFmpeg ----
echo "== FFmpeg $FFMPEG_VERSION"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
rm -rf "$WORK/src/ffmpeg-$FFMPEG_VERSION"
tar xf "$WORK/dl/ffmpeg-$FFMPEG_VERSION.tar.xz" -C "$WORK/src"
cd "$WORK/src/ffmpeg-$FFMPEG_VERSION"

if ! ./configure --prefix="$PREFIX" \
        --enable-cross-compile --target-os=freebsd --arch=x86_64 --cpu=znver2 \
        --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" \
        --strip="$STRIP" \
        --enable-static --disable-shared --enable-pic --disable-autodetect \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-devices --disable-avfilter --disable-filters \
        --disable-network --disable-iconv --disable-bzlib --disable-lzma \
        --disable-encoders --disable-muxers --disable-hwaccels \
        --enable-zlib \
        --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
        2>&1 | tee "$LOGS/ffmpeg-configure.log"; then
    echo "!!! configure nije uspio. Kraj ffbuild/config.log:" >&2
    tail -n 120 ffbuild/config.log >&2 || true
    cp ffbuild/config.log "$LOGS/ffmpeg-config.log" 2>/dev/null || true
    exit 1
fi
cp ffbuild/config.log "$LOGS/ffmpeg-config.log" 2>/dev/null || true

make -j"$JOBS" 2>&1 | tee "$LOGS/ffmpeg-make.log"
make install 2>&1 | tee "$LOGS/ffmpeg-install.log"

echo "== gotovo"
ls -la "$PREFIX/lib"/*.a
