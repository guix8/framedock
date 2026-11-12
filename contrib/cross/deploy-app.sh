#!/bin/sh
# Build Frame Dock on the PC (cross, see README.md) and install the app on the headset, the way a release archive
# installs: bin/framedock and contrib/ are streamed over ssh and contrib/install.sh --app-only --no-restart runs
# there. The Vulkan layer is left as it is and SteamVR is never restarted. Nothing is compiled on the headset.
#
#   sh contrib/cross/deploy-app.sh steamos@<headset-ip>
#   sh contrib/cross/deploy-app.sh --with-layer steamos@<headset-ip>   (also the Vulkan layer, built by
#       build-layer.sh; it is only loaded when SteamVR starts again, which this script never does)
#   SSH=ssh.exe sh contrib/cross/deploy-app.sh steamos@<headset-ip>   (WSL: use Windows' ssh and its keys)
#
# Environment: FRAME_SYSROOT (default ~/frame-sysroot), BUILD_DIR (default build-cross), SSH (default ssh).
set -eu

with_layer=false
if [ "${1:-}" = --with-layer ]; then
    with_layer=true
    shift
fi
host="${1:?usage: deploy-app.sh [--with-layer] user@headset}"
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
sysroot="${FRAME_SYSROOT:-$HOME/frame-sysroot}"
build_dir="${BUILD_DIR:-$source_dir/build-cross}"
ssh_cmd="${SSH:-ssh}"

if [ ! -f "$build_dir/build.ninja" ]; then
    cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$source_dir/contrib/cross/aarch64-sysroot.cmake" -DFRAME_SYSROOT="$sysroot"
fi
cmake --build "$build_dir" --target framedock

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
install -Dm755 "$build_dir/framedock" "$stage/bin/framedock"
for name in install.sh uninstall.sh framecolor-guard framedockctl framedock.service framedock.desktop \
    90-framecolor.conf icons/framedock-48.png icons/framedock-128.png icons/framedock-256.png; do
    install -Dm644 "$source_dir/contrib/$name" "$stage/contrib/$name"
done
install_args="--app-only --no-restart"
if [ "$with_layer" = true ]; then
    BUILD_DIR="$build_dir" sh "$source_dir/contrib/cross/build-layer.sh"
    install -Dm755 "$build_dir/vkbasalt/src/libvkbasalt.so" "$stage/lib/libvkbasalt.so"
    install -Dm644 "$source_dir/tools/generate_vulkan_lut.py" "$stage/tools/generate_vulkan_lut.py"
    install_args="--no-restart"
fi
commit=$(git -C "$source_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)
printf 'Installing Frame Dock %s on %s (%s)\n' "$commit" "$host" "$install_args"
tar -C "$stage" -cf - . | "$ssh_cmd" -o BatchMode=yes "$host" '
    set -e
    dir=$(mktemp -d /tmp/framedock-deploy.XXXXXX)
    trap "rm -rf \"$dir\"" EXIT
    tar -xf - -C "$dir"
    sh "$dir/contrib/install.sh" '"$install_args"'
    if [ -f "$dir/lib/libvkbasalt.so" ]; then
        if cmp -s "$dir/lib/libvkbasalt.so" "$HOME/.local/share/framecolor/lib/libvkbasalt.so"; then
            echo "layer installed (loaded at the next SteamVR start)"
        else
            echo "the installed layer is not the one sent" >&2
            exit 1
        fi
    fi
    if cmp -s "$dir/bin/framedock" "$HOME/.local/bin/framedock"; then
        echo "installed: $(sha256sum "$HOME/.local/bin/framedock" | cut -c1-16)"
    else
        echo "the installed framedock is not the one sent" >&2
        exit 1
    fi'
