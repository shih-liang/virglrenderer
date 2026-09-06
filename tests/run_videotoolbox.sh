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
    -Isrc -Isrc/gallium/include -Isrc/gallium/auxiliary
    -Isrc/mesa -Isrc/mesa/compat -Isrc/mesa/pipe
    -I"$build_dir" -I"$build_dir/src" -I"$build_dir/src/gallium"
    -I"$angle_prefix/include"
)
libraries=(
    "$build_dir/src/libvirglrenderer.a"
    "$angle_prefix/lib/libepoxy.a" "$angle_prefix/lib/libANGLE.a"
    "$moltenvk_archive" -Wl,-dead_strip -lc++
    -framework Foundation -framework Metal -framework QuartzCore
    -framework IOSurface -framework Cocoa -framework CoreFoundation
    -framework CoreGraphics -framework IOKit -framework VideoToolbox
    -framework CoreMedia -framework CoreVideo
)
for name in video_videotoolbox video_planes video_commands; do
    xcrun clang "${common[@]}" "tests/test_$name.c" "${libraries[@]}" \
        -o "$build_dir/test_$name"
    "$build_dir/test_$name"
done
