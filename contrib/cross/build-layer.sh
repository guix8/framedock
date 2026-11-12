#!/bin/sh
# Build the Vulkan layer (third_party/vkbasalt, meson, release) on the PC for the Steam Frame: clang + lld against the
# sysroot from make-sysroot.sh, shaders compiled by the PC's glslangValidator (apt install meson glslang-tools).
#   sh contrib/cross/build-layer.sh            -> build-cross/vkbasalt/src/libvkbasalt.so
# Environment: FRAME_SYSROOT (default ~/frame-sysroot), BUILD_DIR (default build-cross).
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
sysroot="${FRAME_SYSROOT:-$HOME/frame-sysroot}"
build_dir="${BUILD_DIR:-$source_dir/build-cross}/vkbasalt"
[ -e "$sysroot/usr/include/vulkan/vulkan.h" ] || { printf 'No Vulkan headers in %s (make-sysroot.sh)\n' "$sysroot" >&2; exit 1; }
mkdir -p "$build_dir"
sed "s|@SYSROOT@|$sysroot|g" "$source_dir/contrib/cross/aarch64-meson.ini.in" > "$build_dir.cross.ini"
if [ ! -f "$build_dir/build.ninja" ]; then
    meson setup --buildtype=release --cross-file "$build_dir.cross.ini" "$build_dir" "$source_dir/third_party/vkbasalt"
fi
ninja -C "$build_dir"
printf 'Layer: %s\n' "$build_dir/src/libvkbasalt.so"
