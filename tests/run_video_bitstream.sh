#!/bin/bash
# Decoder oracles are test-only dependencies. No downloads, guest changes or
# application replacement. Supply an existing build and an encoded test corpus.
set -euo pipefail
shopt -s nullglob
if [[ $# != 2 ]]; then
    echo "Usage: bash tests/run_video_bitstream.sh VGL_BUILD_DIR MEDIA_DIR" >&2
    exit 2
fi
build_dir=$(cd "$1" && pwd)
media_dir=$(cd "$2" && pwd)
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"
av1=("$media_dir"/av1-*.ivf)
hevc=("$media_dir"/hevc-*.mp4)
if [[ ${#av1[@]} == 0 || ${#hevc[@]} == 0 ]]; then
    echo "Expected av1-*.ivf and hevc-*.mp4 test streams in $media_dir" >&2
    exit 2
fi
common=(-std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g
    -fsanitize=address,undefined -DHAVE_CONFIG_H -imacros "$build_dir/config.h"
    -Isrc -Isrc/gallium/include -Isrc/gallium/auxiliary -Isrc/mesa
    -Isrc/mesa/compat -Isrc/mesa/pipe -I"$build_dir" -I"$build_dir/src" -I"$build_dir/src/gallium")
frameworks=(-framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox)
sources=(src/vrend/virgl_video_hevc.c src/vrend/virgl_video_av1.c)
xcrun clang "${common[@]}" tests/test_video_hevc.c "${sources[@]}" \
    "$build_dir/src/libvirglrenderer.a" $(pkg-config --cflags --libs libavformat libavcodec libavutil) \
    "${frameworks[@]}" -o "$build_dir/test_video_hevc"
xcrun clang "${common[@]}" tests/test_video_av1.c src/vrend/virgl_video_av1.c \
    $(pkg-config --cflags --libs libavcodec libavutil dav1d) -o "$build_dir/test_video_av1"
xcrun clang "${common[@]}" -DTEST_VIDEOTOOLBOX tests/test_video_av1.c "${sources[@]}" \
    "$build_dir/src/libvirglrenderer.a" $(pkg-config --cflags --libs libavcodec libavutil dav1d) \
    "${frameworks[@]}" -o "$build_dir/test_video_av1_vt"
for file in "${hevc[@]}"; do "$build_dir/test_video_hevc" "$file"; done
for file in "${av1[@]}"; do "$build_dir/test_video_av1" "$file" "$build_dir/failed-av1.obu"; done
for file in "${av1[@]}"; do
    if "$build_dir/test_video_av1_vt" "$file"; then continue; else result=$?; fi
    if [[ $result == 77 ]]; then break; fi
    exit "$result"
done
