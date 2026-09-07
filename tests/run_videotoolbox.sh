#!/bin/bash
# Run against an existing static VGL/ANGLE/MoltenVK build. Downloads nothing.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "Usage: bash tests/run_videotoolbox.sh VGL_BUILD_DIR ANGLE_PREFIX MOLTENVK_ARCHIVE" >&2
    exit 2
fi
build_dir=$(cd "$1" && pwd)
angle_prefix=$(cd "$2" && pwd)
moltenvk_archive="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"
common=(
    -O1 -g -fsanitize=address,undefined -DHAVE_CONFIG_H
    -imacros "$build_dir/config.h"
    -Isrc -Isrc/venus -Isrc/gallium/include -Isrc/gallium/auxiliary
    -Isrc/mesa -Isrc/mesa/compat -Isrc/mesa/pipe
    -I"$build_dir" -I"$build_dir/src" -I"$build_dir/src/gallium"
    -I"$angle_prefix/include"
)
libraries=(
    "$build_dir/src/libvirglrenderer.a"
    "$angle_prefix/lib/libANGLE.a"
    "$moltenvk_archive" -Wl,-dead_strip -lc++
    -framework Foundation -framework Metal -framework QuartzCore
    -framework IOSurface -framework Cocoa -framework CoreFoundation
    -framework CoreGraphics -framework IOKit -framework VideoToolbox
    -framework CoreMedia -framework CoreVideo
)
xcrun clang -O1 -g -fsanitize=address,undefined -Isrc \
    tests/test_video_metal.m src/virgl_video_metal.m \
    -framework Foundation -framework Metal -framework CoreVideo \
    -o "$build_dir/test_video_metal"
"$build_dir/test_video_metal"
for source in tests/test_video_videotoolbox.c tests/test_video_commands.m tests/test_video_vulkan.m; do
    name=${source##*/test_}
    name=${name%.*}
    xcrun clang "${common[@]}" "$source" "${libraries[@]}" \
        -o "$build_dir/test_$name"
    "$build_dir/test_$name"
done
TEST_THREADED_FENCES=1 "$build_dir/test_video_commands"
