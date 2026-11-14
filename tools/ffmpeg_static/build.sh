#!/usr/bin/env bash
# The game's own FFmpeg for Windows (TODO 371): FFmpeg's source, cut down to what the game uses, built as static
# libraries with MSVC. The game decodes its movies with the MPEG-2 video decoder and its parser, and turns the pictures
# into RGBA with swscale (PS2Recomp ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp: avcodec, avutil, swscale - nothing from
# avformat, swresample, avdevice or avfilter). Linked into the game, no FFmpeg DLL sits beside it: fewer files Windows
# (Smart App Control) can refuse. Licence: FFmpeg's LGPL 2.1 or later (no --enable-gpl, no --enable-nonfree); the source
# is the release tarball below, pinned by its SHA-256, and this script is the whole build.
#
# Run by .github/workflows/ffmpeg-static.yml on a Windows runner, in an MSYS2 shell that has MSVC's tools on its PATH
# (cl, link, lib) and nasm. Usage: build.sh <work folder> <output zip>
set -euo pipefail
work=${1:?work folder}
zip=${2:?output zip}
version=7.1
sha256=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
url=https://ffmpeg.org/releases/ffmpeg-$version.tar.xz

mkdir -p "$work"
cd "$work"
curl -fsSL "$url" -o ffmpeg.tar.xz
echo "$sha256  ffmpeg.tar.xz" | sha256sum -c -
tar -xf ffmpeg.tar.xz
cd "ffmpeg-$version"

prefix=$PWD/../install
# (-MD: the C runtime as a DLL, as the game links it; -Z7: the debug information inside the libraries)
./configure \
    --toolchain=msvc --arch=x86_64 --target-os=win64 \
    --prefix="$prefix" \
    --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-debug \
    --disable-everything \
    --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample --disable-postproc \
    --disable-network --disable-autodetect \
    --enable-avcodec --enable-avutil --enable-swscale \
    --enable-decoder=mpeg2video --enable-parser=mpegvideo \
    --extra-cflags="-MD"
make -j"$(nproc)"
make install

cd ..
# (the zip: include\, lib\ (libavcodec.a, libavutil.a, libswscale.a: MSVC static libraries with FFmpeg's names), the
# licence, and what it was made from)
cp "ffmpeg-$version/COPYING.LGPLv2.1" install/
cat > install/BUILD.txt <<EOF
FFmpeg $version, LGPL 2.1 or later, built as static libraries for the Transformers Armada port (MSVC, x86-64).
Source: $url
SHA-256: $sha256
Configure: $(grep -m1 '^#define FFMPEG_CONFIGURATION' ffmpeg-$version/config.h | cut -d' ' -f3- 2>/dev/null || echo see tools/ffmpeg_static/build.sh)
EOF
(cd install && rm -rf share lib/pkgconfig && zip -qr "$zip" include lib COPYING.LGPLv2.1 BUILD.txt)
sha256sum "$zip"
