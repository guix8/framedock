#!/bin/sh
# Build a Frame Dock release archive on a Steam Frame (SteamOS, ARM64). Nothing is uploaded.
#
#   sh contrib/package.sh [VERSION]     VERSION: letters, digits, . _ - (default "dev")
#
# Builds the app (CMake, Release) and FrameColor's Vulkan layer (meson, release), then writes
#   dist/framedock-VERSION-steamframe-aarch64.tar.gz and its .sha256
# The archive extracts to framedock/: bin/framedock, lib/libvkbasalt.so (debug symbols stripped), contrib/ (install.sh,
# uninstall.sh, framedockctl, the service, desktop entry, icons, framecolor-guard and the SteamVR drop-in),
# tools/generate_vulkan_lut.py, the README and every license and notice file. On another Frame,
#   tar -xzf framedock-VERSION-steamframe-aarch64.tar.gz && sh framedock/contrib/install.sh
# installs it without building (it needs python3 for the layer's colour table; no compiler).
# Needs here: cmake, ninja, pkg-config, meson, glslangValidator, strip, and the libraries in THIRD_PARTY_LICENSES.md.
set -eu

fc_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fc_version="${1:-dev}"
case "$fc_version" in *[!A-Za-z0-9._-]*|'') printf 'Invalid version.\n' >&2; exit 2 ;; esac
[ "$(uname -m)" = aarch64 ] || { printf 'Build this archive on Steam Frame ARM64.\n' >&2; exit 1; }
command -v strip >/dev/null 2>&1 || { printf 'Missing required command: strip\n' >&2; exit 1; }

cmake -G Ninja -S "$fc_source_dir" -B "$fc_source_dir/build"
cmake --build "$fc_source_dir/build" --target framedock
fc_layer_build="$fc_source_dir/build/vkbasalt"
# Release build: meson defaults to an unoptimized debug build (see install.sh)
if [ ! -f "$fc_layer_build/build.ninja" ]; then
    meson setup --buildtype=release "$fc_layer_build" "$fc_source_dir/third_party/vkbasalt"
else
    meson setup --wipe --buildtype=release "$fc_layer_build" "$fc_source_dir/third_party/vkbasalt"
fi
ninja -C "$fc_layer_build"

fc_temp_dir=$(mktemp -d)
trap 'rm -rf "$fc_temp_dir"' EXIT HUP INT TERM
fc_stage_dir="$fc_temp_dir/framedock"
install -Dm755 "$fc_source_dir/build/framedock" "$fc_stage_dir/bin/framedock"
install -Dm755 "$fc_layer_build/src/libvkbasalt.so" "$fc_stage_dir/lib/libvkbasalt.so"
strip --strip-debug "$fc_stage_dir/bin/framedock" "$fc_stage_dir/lib/libvkbasalt.so"
for fc_notice in README.md README.ko.md README.ja.md LICENSE THIRD_PARTY_LICENSES.md THIRD_PARTY_LICENSES.ja.md \
    perf/LICENSE perf/THIRD_PARTY_LICENSES.md perf/licenses/LGPL-2.1.txt \
    third_party/frameeyeosc-LICENSE third_party/frametop-LICENSE third_party/openvr/LICENSE \
    third_party/vkbasalt/LICENSE; do
    install -Dm644 "$fc_source_dir/$fc_notice" "$fc_stage_dir/$fc_notice"
done
install -Dm644 "$fc_source_dir/tools/generate_vulkan_lut.py" "$fc_stage_dir/tools/generate_vulkan_lut.py"
for fc_name in install.sh uninstall.sh framecolor-guard framedockctl framedock.service framedock.desktop \
    90-framecolor.conf icons/framedock-48.png icons/framedock-128.png icons/framedock-256.png; do
    install -Dm644 "$fc_source_dir/contrib/$fc_name" "$fc_stage_dir/contrib/$fc_name"
done
install -d "$fc_source_dir/dist"
fc_archive_name="framedock-$fc_version-steamframe-aarch64.tar.gz"
fc_archive="$fc_source_dir/dist/$fc_archive_name"
tar -czf "$fc_archive" -C "$fc_temp_dir" framedock
(cd "$fc_source_dir/dist" && sha256sum "$fc_archive_name" > "$fc_archive_name.sha256")
printf 'Created %s and %s.sha256\n' "$fc_archive" "$fc_archive"
