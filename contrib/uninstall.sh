#!/bin/sh
# Remove Frame Dock: the app, its service, framedockctl, and FrameColor's Vulkan layer.
#
#   sh contrib/uninstall.sh             everything; SteamVR restarts only if the layer was enabled and SteamVR runs
#   sh contrib/uninstall.sh --app-only  the app, its service, desktop entry and icons only; the layer and
#                                       framedockctl (to turn the layer off later) stay. SteamVR is not restarted
#
# Saved settings stay: ~/.config/frame-perf-overlay (panel, layout, profiles) and ~/.config/framecolor, and the
# browser panel's Chrome profile (~/.local/share/framedock/browser)
# (colour, presets, auto preset rules). The script says how to delete them. SteamVR settings the Color page changed
# (display brightness) may remain. Does not touch the SteamVR runtime or any OS-owned files.
set -eu

fs_name=framedock
fs_config_root="$HOME/.config"
fs_data_root="$HOME/.local/share"
fs_unit_dir="$fs_config_root/systemd/user"
fs_bin_dir="$HOME/.local/bin"
fc_config_dir="$fs_config_root/framecolor"
fc_data_dir="$fs_data_root/framecolor"
fc_manifest="$fc_data_dir/layer/vkBasalt.json"

fs_app_only=false
for fs_arg in "$@"; do
    case "$fs_arg" in
        --app-only) fs_app_only=true ;;
        -h|--help) printf 'Usage: sh contrib/uninstall.sh [--app-only]\n'; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$fs_arg" >&2; exit 2 ;;
    esac
done

# --now stops it with SIGTERM: a held compare is released and the overlays are removed cleanly
systemctl --user disable --now "$fs_name.service" >/dev/null 2>&1 || true
# One started from + in the dashboard (outside the service), found by its lock file and checked by name
fs_lock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$fs_name.lock"
fs_pid=$(head -n 1 "$fs_lock" 2>/dev/null | tr -dc '0-9' || true)
if [ -n "$fs_pid" ] && [ -r "/proc/$fs_pid/cmdline" ]; then
    fs_argv0=$(tr '\0' '\n' < "/proc/$fs_pid/cmdline" 2>/dev/null | head -n 1 || true)
    if [ "$(basename -- "${fs_argv0:-x}")" = "$fs_name" ]; then
        kill -TERM "$fs_pid" 2>/dev/null || true
    fi
fi

rm -f "$fs_unit_dir/$fs_name.service" "$fs_bin_dir/$fs_name"
rm -f "$fs_data_root/applications/$fs_name.desktop"
for fs_size in 48 128 256; do
    rm -f "$fs_data_root/icons/hicolor/${fs_size}x${fs_size}/apps/$fs_name.png"
done
# SteamVR Input's action manifest for the Dock's grip, which the app writes at start: under $XDG_DATA_HOME when that
# is an absolute path, otherwise ~/.local/share (the same rule as dock_manifest.cpp's defaultManifestDir)
case "${XDG_DATA_HOME:-}" in
    /*) fs_input_root="$XDG_DATA_HOME" ;;
    *) fs_input_root="$fs_data_root" ;;
esac
rm -f "$fs_input_root/$fs_name/input/actions.json" "$fs_input_root/$fs_name/input/"bindings_*.json
rmdir "$fs_input_root/$fs_name/input" "$fs_input_root/$fs_name" 2>/dev/null || true
if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$fs_data_root/applications" ]; then
    update-desktop-database "$fs_data_root/applications" >/dev/null 2>&1 || true
fi

fs_layer_was_enabled=false
if [ "$fs_app_only" = false ]; then
    # framedockctl goes with the layer it switches
    rm -f "$fs_bin_dir/framedockctl"
    if [ -f "$fc_manifest" ]; then
        mv "$fc_manifest" "$fc_manifest.disabled"
        fs_layer_was_enabled=true
    fi
    rm -f "$fs_unit_dir/steamvr.service.d/90-framecolor.conf" "$HOME/.local/libexec/framecolor-guard"
    rm -f "$fc_data_dir/layer/vkBasalt.json.disabled" "$fc_data_dir/lib/libvkbasalt.so"
    rm -f "$fc_data_dir/framecolor.cube" "$fc_data_dir/vkBasalt.conf" "$fc_data_dir/SUPPORTED_STEAMVR_VERSION"
    rm -f "$fc_config_dir/vulkan.env"
    rmdir "$fc_data_dir/layer" "$fc_data_dir/lib" "$fc_data_dir" 2>/dev/null || true
    rmdir "$fs_unit_dir/steamvr.service.d" 2>/dev/null || true
fi
systemctl --user daemon-reload
# vrcompositor keeps a loaded layer until SteamVR restarts; one that isn't running loads nothing
if [ "$fs_layer_was_enabled" = true ] && systemctl --user is-active --quiet steamvr.service; then
    printf 'Restarting SteamVR without the Vulkan layer...\n'
    systemctl --user restart steamvr.service
fi

if [ "$fs_app_only" = true ]; then
    printf 'Frame Dock was removed. The Vulkan layer and framedockctl were left as they were (--app-only).\n'
    if [ -f "$fc_manifest" ]; then
        printf 'The Vulkan correction is still enabled. To turn it off (restarts SteamVR):\n'
        printf '  %s/framedockctl disable\n' "$fs_bin_dir"
    fi
    printf 'To remove the layer and framedockctl too: sh %s/uninstall.sh\n' "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
else
    printf 'Frame Dock was removed.\n'
fi
printf 'Saved settings are kept. To delete them too:\n'
printf '  rm -r "%s" "%s"\n' "$fs_config_root/frame-perf-overlay" "$fc_config_dir"
# The browser panel's own Chrome profile (logins, cookies, history) is user data too: kept, named here
if [ -d "$fs_input_root/$fs_name/browser" ]; then
    printf "The browser panel's Chrome profile (logins, cookies) is kept too. To delete it:\n"
    printf '  rm -r "%s"\n' "$fs_input_root/$fs_name/browser"
fi
