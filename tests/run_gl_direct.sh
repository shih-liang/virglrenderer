#!/bin/bash
# Requires a prebuilt static, Metal-only ANGLE SDK and GPU access.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: bash tests/run_gl_direct.sh VGL_BUILD_DIR ANGLE_PREFIX" >&2
    exit 2
fi
build_dir=$(cd "$1" && pwd)
angle_prefix=$(cd "$2" && pwd)
cd "$(dirname "$0")/.."
xcrun clang -O1 -g -fsanitize=address,undefined -DENABLE_ANGLE \
    -Isrc -I"$angle_prefix/include" tests/test_gl_direct.c src/vrend/vrend_gl.c \
    "$angle_prefix/lib/libANGLE.a" -lc++ -framework Foundation -framework Metal \
    -framework QuartzCore -framework CoreGraphics -framework IOSurface -framework IOKit \
    -o "$build_dir/test_gl_direct"
"$build_dir/test_gl_direct"
