#!/bin/sh
# Builds plugcam_dec.dll on Linux with mingw-w64: FFmpeg cut down to the H.264 decoder and its
# Direct3D 12 hwaccel, linked statically into plugcam_dec.c. See docs/decoder.md.
#
#   sudo apt install gcc gcc-mingw-w64-x86-64 make pkg-config xz-utils curl
#   ./build.sh            -> out/plugcam_dec.dll  (FFMPEG_REBUILD=1 to rebuild FFmpeg too)
set -eu

FFMPEG_VERSION=9.1
FFMPEG_SHA256=1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433
# Microsoft's DirectX headers (MIT) for the shim: mingw-w64's have no Direct3D 12 video processor.
DXHEADERS_VERSION=1.620.2
DXHEADERS_SHA256=2b916ef1be5db35313f56d60499274fed39db37b54a1f0befa8aa49041c14855

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/build}
OUT=${OUT:-$HERE/out}
CROSS=x86_64-w64-mingw32-
JOBS=$(nproc)

mkdir -p "$WORK" "$OUT"
cd "$WORK"

tarball=ffmpeg-$FFMPEG_VERSION.tar.xz
# FFmpeg is built once; FFMPEG_REBUILD=1 builds it again (the shim below is always rebuilt).
if [ ! -f prefix/lib/libavcodec.a ] || [ "${FFMPEG_REBUILD:-0}" = 1 ]; then
    if [ ! -f "$tarball" ]; then
        curl -fsSLO "https://ffmpeg.org/releases/$tarball"
    fi
    echo "$FFMPEG_SHA256  $tarball" | sha256sum -c -
    rm -rf "ffmpeg-$FFMPEG_VERSION" prefix
    tar xf "$tarball"

    cd "ffmpeg-$FFMPEG_VERSION"
    # Nothing but what decoding H.264 on the GPU needs. No assembly: the CPU only parses
    # headers here, the GPU does the decoding.
    ./configure \
        --prefix="$WORK/prefix" \
        --target-os=mingw32 --arch=x86_64 --cross-prefix=$CROSS --enable-cross-compile \
        --enable-static --disable-shared \
        --enable-small --disable-debug --disable-x86asm \
        --disable-autodetect --disable-everything \
        --disable-programs --disable-doc --disable-network \
        --disable-avformat --disable-avfilter --disable-avdevice --disable-swscale --disable-swresample \
        --enable-w32threads \
        --enable-d3d12va \
        --enable-decoder=h264 --enable-hwaccel=h264_d3d12va
    # configure drops d3d12va quietly when the headers lack it (older mingw-w64); such a DLL
    # would decode nothing on the GPU.
    grep -q '^#define CONFIG_H264_D3D12VA_HWACCEL 1' config_components.h || { echo "configure disabled h264_d3d12va (mingw-w64 too old?)" >&2; exit 1; }
    make -j"$JOBS"
    make install
    cp COPYING.LGPLv2.1 "$WORK/prefix/"
    cd "$WORK"
fi

dxh=DirectX-Headers-$DXHEADERS_VERSION
if [ ! -d "$dxh" ]; then
    curl -fsSL -o "$dxh.tar.gz" "https://github.com/microsoft/DirectX-Headers/archive/refs/tags/v$DXHEADERS_VERSION.tar.gz"
    echo "$DXHEADERS_SHA256  $dxh.tar.gz" | sha256sum -c -
    tar xzf "$dxh.tar.gz"
fi

# Only the pc_dec_* functions are exported (dllexport); FFmpeg's symbols stay inside. No
# libgcc or winpthread DLL needed next to it.
PKG_CONFIG_PATH="$WORK/prefix/lib/pkgconfig" pkg-config --static --libs libavcodec >libs.txt
${CROSS}gcc -shared -O2 -s -o "$OUT/plugcam_dec.dll" \
    -I"$WORK/$dxh/include/directx" -I"$WORK/prefix/include" "$HERE/plugcam_dec.c" \
    $(cat libs.txt) \
    -static-libgcc -Wl,--gc-sections -Wl,--no-insert-timestamp

# What the LGPL asks to ship with it: the license, and where the source and the way to rebuild
# it are. stage-resources.ps1 puts these in the installer's licenses folder.
cp prefix/COPYING.LGPLv2.1 "$OUT/FFmpeg-LICENSE.txt"
cp "$dxh/LICENSE" "$OUT/DirectX-Headers-LICENSE.txt"
cat >"$OUT/FFmpeg-SOURCE.txt" <<EOF
plugcam_dec.dll contains FFmpeg $FFMPEG_VERSION (libavcodec and libavutil), licensed under the
GNU Lesser General Public License 2.1 or later (FFmpeg-LICENSE.txt), linked statically with
Plugcam's small wrapper plugcam_dec.c, which is under the same license.

FFmpeg source:  https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
                (sha256 $FFMPEG_SHA256)
Wrapper source and the script that builds the DLL, with the exact FFmpeg configuration:
                the decoder/ folder of https://github.com/freefakesmile/plugcam

Rebuilding plugcam_dec.dll with build.sh (on Linux, with mingw-w64) and putting it in place of
this one is all it takes to run Plugcam with a modified FFmpeg.
EOF

${CROSS}objdump -p "$OUT/plugcam_dec.dll" | sed -n '/DLL Name/p;/\[ *[0-9]*\] pc_dec_/p'
ls -l "$OUT"
