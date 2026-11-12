#!/bin/sh
# Install or update Frame Dock in the invoking user's home directory. No sudo, no network.
#
#   sh contrib/install.sh                 the app, its service, framedockctl, and FrameColor's Vulkan layer
#   sh contrib/install.sh --app-only      the same without the Vulkan layer: the layer is left as it is, so
#                                         SteamVR is never restarted
#   sh contrib/install.sh --no-autostart  don't start it with SteamVR (start it from + in the dashboard)
#   sh contrib/install.sh --no-restart    never restart SteamVR (a changed, enabled layer then says how to load it)
#   sh contrib/install.sh --replace-old-apps  remove Frame Perf Overlay and FrameColor's dashboard without asking
#
# Frame Dock takes the place of Frame Perf Overlay (frame-perf-overlay.service) and FrameColor's dashboard
# (framecolor.service). When either is installed, the script asks before it stops, disables and removes them (with
# FrameColor's framecolorctl: framedockctl replaces it); --replace-old-apps answers yes. Without a terminal to ask
# and without the option they are kept, and the script says how to remove them later. Their settings stay either
# way: ~/.config/frame-perf-overlay and ~/.config/framecolor are what Frame Dock reads. Autostart follows them the
# first time: on if either one started with SteamVR, off if neither did.
#
# Everything that can fail (build, LUT, copies) is done first in a staging folder; the old apps are only removed
# after that, so a failed install leaves them as they were.
# Works from a release archive (bin/framedock, lib/libvkbasalt.so) or from the repository (builds them).
set -eu

fs_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fs_name=framedock
fs_unit="$fs_name.service"
fs_config_root="$HOME/.config"
fs_data_root="$HOME/.local/share"
fs_unit_dir="$fs_config_root/systemd/user"
fs_bin_dir="$HOME/.local/bin"
fs_libexec_dir="$HOME/.local/libexec"
fs_bin="$fs_bin_dir/$fs_name"
fs_desktop="$fs_data_root/applications/$fs_name.desktop"
fs_icons="$fs_data_root/icons/hicolor"
fs_icon_sizes="48 128 256"
fc_config_dir="$fs_config_root/framecolor"
fc_data_dir="$fs_data_root/framecolor"
fc_manifest="$fc_data_dir/layer/vkBasalt.json"
fc_dropin="$fs_unit_dir/steamvr.service.d/90-framecolor.conf"
fs_runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

usage() {
    printf 'Usage: sh contrib/install.sh [--app-only] [--no-autostart] [--no-restart]\n'
    printf '  --app-only      leave the Vulkan layer as it is (SteamVR is not restarted)\n'
    printf '  --no-autostart  do not start Frame Dock with SteamVR (start it from + in the SteamVR dashboard)\n'
    printf '  --no-restart    never restart SteamVR; if the layer changed, say how to load it later\n'
    printf '  --replace-old-apps  remove Frame Perf Overlay and FrameColor'"'"'s dashboard without asking\n'
}

fs_app_only=false
fs_autostart=true
fs_no_restart=false
fs_replace_old=ask
for fs_arg in "$@"; do
    case "$fs_arg" in
        --app-only) fs_app_only=true ;;
        --no-autostart) fs_autostart=false ;;
        --no-restart) fs_no_restart=true ;;
        --replace-old-apps) fs_replace_old=yes ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$fs_arg" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    printf 'Run this as the normal Steam Frame user, without sudo.\n' >&2
    exit 1
fi
if [ "$(uname -m)" != aarch64 ] || [ ! -d /opt/steamvr ]; then
    printf 'Frame Dock is for Steam Frame (SteamOS ARM64) only.\n' >&2
    exit 1
fi
fs_needed="install systemctl sed mktemp"
[ "$fs_app_only" = true ] || fs_needed="$fs_needed python3 cmp"
for fs_command in $fs_needed; do
    command -v "$fs_command" >/dev/null 2>&1 || { printf 'Missing required command: %s\n' "$fs_command" >&2; exit 1; }
done

# ---- The apps Frame Dock takes the place of: removed only when the user says so ----
fs_old_apps=
for fs_old in framecolor frame-perf-overlay; do
    if [ -f "$fs_unit_dir/$fs_old.service" ] || [ -x "$fs_bin_dir/$fs_old" ]; then
        fs_old_apps="$fs_old_apps $fs_old"
    fi
done
if [ -n "$fs_old_apps" ] && [ "$fs_replace_old" = ask ]; then
    printf 'Installed now:%s. Frame Dock does what they do (their settings are kept and used).\n' "$fs_old_apps"
    # Asked on the terminal even when stdin is a pipe (curl ... | sh); no terminal at all: kept
    fs_tty=
    if [ -t 0 ]; then
        fs_tty=/dev/stdin
    elif (exec </dev/tty) 2>/dev/null; then  # in a subshell: a failed redirection on ":" would end sh
        fs_tty=/dev/tty
    fi
    if [ -n "$fs_tty" ]; then
        printf 'Stop and remove them? [y/N] '
        read -r fs_answer <"$fs_tty" || fs_answer=
        case "$fs_answer" in
            y|Y|yes|YES) fs_replace_old=yes ;;
            *) fs_replace_old=no ;;
        esac
    else
        fs_replace_old=no
    fi
fi

# ---- Binaries: from the release archive, or built here ----
if [ -x "$fs_source_dir/bin/$fs_name" ] && { [ "$fs_app_only" = true ] || [ -f "$fs_source_dir/lib/libvkbasalt.so" ]; }; then
    fs_app_binary="$fs_source_dir/bin/$fs_name"
    fc_layer_binary="$fs_source_dir/lib/libvkbasalt.so"
else
    fs_build_tools="cmake ninja pkg-config"
    [ "$fs_app_only" = true ] || fs_build_tools="$fs_build_tools meson glslangValidator"
    for fs_command in $fs_build_tools; do
        command -v "$fs_command" >/dev/null 2>&1 || {
            printf 'No prebuilt binaries found; building from source needs %s.\n' "$fs_command" >&2
            exit 1
        }
    done
    cmake -G Ninja -S "$fs_source_dir" -B "$fs_source_dir/build"
    cmake --build "$fs_source_dir/build" --target "$fs_name"
    fs_app_binary="$fs_source_dir/build/$fs_name"
    fc_layer_binary=
    if [ "$fs_app_only" = false ]; then
        fc_layer_build="$fs_source_dir/build/vkbasalt"
        # The layer runs inside vrcompositor on every frame. Meson defaults to an unoptimized debug build, so ask
        # for a release build.
        if [ ! -f "$fc_layer_build/build.ninja" ]; then
            meson setup --buildtype=release "$fc_layer_build" "$fs_source_dir/third_party/vkbasalt"
        else
            meson setup --wipe --buildtype=release "$fc_layer_build" "$fs_source_dir/third_party/vkbasalt"
        fi
        ninja -C "$fc_layer_build"
        fc_layer_binary="$fc_layer_build/src/libvkbasalt.so"
    fi
fi
for fs_file in "contrib/$fs_unit" "contrib/$fs_name.desktop" contrib/framedockctl; do
    [ -f "$fs_source_dir/$fs_file" ] || { printf 'Missing %s\n' "$fs_source_dir/$fs_file" >&2; exit 1; }
done

# ---- Stage everything new first: a copy or LUT that fails stops here, with the old apps untouched ----
mkdir -p "$fs_data_root"
fs_stage=$(mktemp -d "$fs_data_root/.framedock-install.XXXXXX")
cleanup_stage() {
    # Only what this script put there; then the (empty) folder
    for fs_staged in "$fs_stage"/*; do
        [ -f "$fs_staged" ] && rm -f -- "$fs_staged"
    done
    rmdir -- "$fs_stage" 2>/dev/null || true
}
trap cleanup_stage EXIT
trap 'exit 1' HUP INT TERM
install -m755 "$fs_app_binary" "$fs_stage/$fs_name"
for fs_size in $fs_icon_sizes; do
    install -m644 "$fs_source_dir/contrib/icons/$fs_name-$fs_size.png" "$fs_stage/icon-$fs_size.png"
done
# The dashboard's + list. Exec is the absolute path: Steam may start it without ~/.local/bin in PATH. The path is
# escaped for sed's replacement (a home folder with |, & or \ in its name)
fs_bin_sed=$(printf '%s\n' "$fs_bin" | sed 's/[|&\\]/\\&/g')
sed "s|@BINARY@|$fs_bin_sed|" "$fs_source_dir/contrib/$fs_name.desktop" > "$fs_stage/$fs_name.desktop"
chmod 644 "$fs_stage/$fs_name.desktop"
install -m644 "$fs_source_dir/contrib/$fs_unit" "$fs_stage/$fs_unit"
install -m755 "$fs_source_dir/contrib/framedockctl" "$fs_stage/framedockctl"

if [ "$fs_app_only" = false ]; then
    # FrameColor's Vulkan layer, as FrameColor's install.sh wrote it
    install -m755 "$fc_layer_binary" "$fs_stage/libvkbasalt.so"
    install -m755 "$fs_source_dir/contrib/framecolor-guard" "$fs_stage/framecolor-guard"
    install -m644 "$fs_source_dir/contrib/90-framecolor.conf" "$fs_stage/90-framecolor.conf"
    fc_values="$fc_config_dir/current.txt"
    if [ ! -f "$fc_values" ]; then
        printf '0 0 0 1 1 1 1 1 1 1 1 1 0 0 0\n' > "$fs_stage/current.txt"
        fc_values="$fs_stage/current.txt"
    fi
    python3 "$fs_source_dir/tools/generate_vulkan_lut.py" "$fc_values" "$fs_stage/framecolor.cube"

    python3 - "$fc_data_dir" "$fs_stage" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
stage = Path(sys.argv[2])
manifest = {
    "file_format_version": "1.0.0",
    "layer": {
        "name": "VK_LAYER_VKBASALT_post_processing",
        "type": "GLOBAL",
        "library_path": str(root / "lib/libvkbasalt.so"),
        "api_version": "1.3.223",
        "implementation_version": "1",
        "description": "FrameColor Vulkan correction for Steam Frame",
        "functions": {
            "vkGetInstanceProcAddr": "vkBasalt_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "vkBasalt_GetDeviceProcAddr",
        },
        "enable_environment": {"ENABLE_VKBASALT": "1"},
        "disable_environment": {"DISABLE_VKBASALT": "1"},
    },
}
(stage / "vkBasalt.json").write_text(json.dumps(manifest, indent=2) + "\n")
(stage / "vkBasalt.conf").write_text(
    f"lutFile = {root / 'framecolor.cube'}\n"
)
PY

    {
        printf 'VK_ADD_IMPLICIT_LAYER_PATH=%s/layer\n' "$fc_data_dir"
        printf 'ENABLE_VKBASALT=1\n'
        printf 'VKBASALT_CONFIG_FILE=%s/vkBasalt.conf\n' "$fc_data_dir"
        printf 'VKBASALT_LOG_LEVEL=warn\n'
    } > "$fs_stage/vulkan.env"
fi

# ---- Autostart ----
# --no-autostart: off. A Frame Dock already installed keeps its own setting. Over the two old apps it follows them:
# on when either one started with SteamVR, off when each one installed had its autostart off. Otherwise on.
fs_enable=$fs_autostart
if [ "$fs_autostart" = true ]; then
    if [ -f "$fs_unit_dir/$fs_unit" ]; then
        if ! systemctl --user is-enabled --quiet "$fs_unit"; then
            fs_enable=false
            printf 'Keeping autostart off, as it was. Turn it on in the dashboard (Frame Dock > Autostart).\n'
        fi
    else
        fs_old_installed=false
        fs_old_enabled=false
        for fs_old in framecolor frame-perf-overlay; do
            [ -f "$fs_unit_dir/$fs_old.service" ] || continue
            fs_old_installed=true
            if systemctl --user is-enabled --quiet "$fs_old.service"; then fs_old_enabled=true; fi
        done
        if [ "$fs_old_installed" = true ] && [ "$fs_old_enabled" = false ]; then
            fs_enable=false
            printf 'Autostart stays off, as it was for the apps it replaces. Turn it on in the dashboard (Frame Dock > Autostart).\n'
        fi
    fi
fi

# ---- Whether the layer really changes: only then does SteamVR need a restart ----
# Counted: what vrcompositor reads when SteamVR starts (the library, the manifest, vkBasalt.conf, the environment
# and the drop-in). Not counted: the guard (run afresh at each SteamVR start and stop) and the .cube (only the
# table for the first frames: the layer follows current.txt)
fs_layer_changed=false
layer_file() {
    cmp -s "$1" "$2" 2>/dev/null || fs_layer_changed=true
}
if [ "$fs_app_only" = false ]; then
    # The manifest keeps its present name: enabled stays enabled, disabled (or new) stays disabled
    if [ -f "$fc_manifest" ]; then fc_manifest_target="$fc_manifest"; else fc_manifest_target="$fc_manifest.disabled"; fi
    layer_file "$fs_stage/libvkbasalt.so" "$fc_data_dir/lib/libvkbasalt.so"
    layer_file "$fs_stage/vkBasalt.json" "$fc_manifest_target"
    layer_file "$fs_stage/vkBasalt.conf" "$fc_data_dir/vkBasalt.conf"
    layer_file "$fs_stage/vulkan.env" "$fc_config_dir/vulkan.env"
    layer_file "$fs_stage/90-framecolor.conf" "$fc_dropin"
fi

# Stops an instance of $1 started outside its service (from + in the dashboard), found by its lock file. SIGTERM
# runs the app's normal shutdown (overlays removed, then VR_Shutdown).
stop_manual_instance() {
    fs_lock="$fs_runtime_dir/$1.lock"
    [ -r "$fs_lock" ] || return 0
    fs_pid=$(head -n 1 "$fs_lock" 2>/dev/null | tr -dc '0-9')
    [ -n "$fs_pid" ] || return 0
    # The lock file outlives the process, and the PID may belong to something else by now: check its name
    [ -r "/proc/$fs_pid/cmdline" ] || return 0
    fs_argv0=$(tr '\0' '\n' < "/proc/$fs_pid/cmdline" 2>/dev/null | head -n 1 || true)
    [ "$(basename -- "${fs_argv0:-x}")" = "$1" ] || return 0
    # The service's own process is the service's business (systemctl restart / disable --now)
    fs_service_pid=$(systemctl --user show -p MainPID --value "$1.service" 2>/dev/null || echo 0)
    [ "$fs_pid" != "$fs_service_pid" ] || return 0
    printf 'Stopping %s (PID %s), started outside its service...\n' "$1" "$fs_pid"
    kill -TERM "$fs_pid" 2>/dev/null || return 0
    fs_wait=0
    while [ -d "/proc/$fs_pid" ] && [ "$fs_wait" -lt 50 ]; do
        sleep 0.1
        fs_wait=$((fs_wait + 1))
    done
    if [ -d "/proc/$fs_pid" ]; then
        printf 'Warning: PID %s is still running. Close it from the SteamVR dashboard.\n' "$fs_pid" >&2
    fi
}

# Moves a staged file into place (a rename: a running instance is never broken mid-write)
put() {
    mkdir -p "$(dirname -- "$2")"
    mv -f -- "$1" "$2"
}

# ---- Switch-over. First the apps Frame Dock takes the place of, when the user said so ----
# From here until the new files are all in place, a closed terminal (HUP) or Ctrl-C (INT) doesn't stop the script
# half-way (old apps gone, the new ones not there yet)
trap '' HUP INT
# Kept: their files stay, but they no longer start with SteamVR. They share Frame Dock's settings file
# (~/.config/frame-perf-overlay/config.json) and FrameColor's current.txt, and would overwrite what Frame Dock saves
if [ "$fs_replace_old" = no ]; then
    for fs_old in $fs_old_apps; do
        systemctl --user disable --now "$fs_old.service" >/dev/null 2>&1 || true
    done
fi
if [ "$fs_replace_old" = yes ]; then
    for fs_old in framecolor frame-perf-overlay; do
        if [ -f "$fs_unit_dir/$fs_old.service" ] || [ -x "$fs_bin_dir/$fs_old" ]; then
            printf 'Replacing %s...\n' "$fs_old"
            # --now stops it with SIGTERM: a held compare is released, overlays are removed cleanly
            systemctl --user disable --now "$fs_old.service" >/dev/null 2>&1 || true
            stop_manual_instance "$fs_old"
        fi
    done
    # FrameColor's dashboard kept no lock file: one started by hand is found by its exact process name
    if command -v pkill >/dev/null 2>&1; then
        pkill -TERM -x -u "$(id -u)" framecolor 2>/dev/null || true
    fi
    rm -f "$fs_unit_dir/framecolor.service" "$fs_bin_dir/framecolor"
    rm -f "$fs_unit_dir/frame-perf-overlay.service" "$fs_bin_dir/frame-perf-overlay"
    rm -f "$fs_data_root/applications/frame-perf-overlay.desktop"
    for fs_size in $fs_icon_sizes; do
        rm -f "$fs_icons/${fs_size}x${fs_size}/apps/frame-perf-overlay.png"
    done
    # Frame Perf Overlay's self-updater (not part of Frame Dock) and the options it kept
    rm -f "$fs_data_root/frame-perf-overlay/frame-update.sh" "$fs_config_root/frame-perf-overlay/install-args"
    rmdir "$fs_data_root/frame-perf-overlay" 2>/dev/null || true
    # FrameColor's control script, now framedockctl
    rm -f "$fs_bin_dir/framecolorctl"
fi

# ---- The app ----
put "$fs_stage/$fs_name" "$fs_bin"
for fs_size in $fs_icon_sizes; do
    put "$fs_stage/icon-$fs_size.png" "$fs_icons/${fs_size}x${fs_size}/apps/$fs_name.png"
done
put "$fs_stage/$fs_name.desktop" "$fs_desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$fs_data_root/applications" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -f "$fs_icons/index.theme" ]; then
    gtk-update-icon-cache -q "$fs_icons" || true
fi
put "$fs_stage/$fs_unit" "$fs_unit_dir/$fs_unit"
put "$fs_stage/framedockctl" "$fs_bin_dir/framedockctl"
install -d -m 700 "$fc_config_dir"

# ---- FrameColor's Vulkan layer, unless --app-only ----
if [ "$fs_app_only" = false ]; then
    put "$fs_stage/libvkbasalt.so" "$fc_data_dir/lib/libvkbasalt.so"
    put "$fs_stage/framecolor-guard" "$fs_libexec_dir/framecolor-guard"
    put "$fs_stage/90-framecolor.conf" "$fc_dropin"
    install -d -m 755 "$fc_data_dir/layer"
    [ -f "$fc_config_dir/current.txt" ] || put "$fs_stage/current.txt" "$fc_config_dir/current.txt"
    put "$fs_stage/framecolor.cube" "$fc_data_dir/framecolor.cube"
    put "$fs_stage/vkBasalt.conf" "$fc_data_dir/vkBasalt.conf"
    put "$fs_stage/vulkan.env" "$fc_config_dir/vulkan.env"
    put "$fs_stage/vkBasalt.json" "$fc_manifest_target"
    if [ "$fc_manifest_target" = "$fc_manifest" ]; then rm -f "$fc_manifest.disabled"; fi
fi
trap 'exit 1' HUP INT  # the files are in place: interrupting is safe again

# ---- The service ----
systemctl --user daemon-reload
# A bad build that crashed at every start may have hit the unit's start limit: this one starts again
systemctl --user reset-failed "$fs_unit" >/dev/null 2>&1 || true
if [ "$fs_enable" = true ]; then
    systemctl --user enable "$fs_unit"
else
    systemctl --user disable "$fs_unit" >/dev/null 2>&1 || true
fi
# SteamVR restarts only for an enabled layer whose files really changed: vrcompositor loads it only when it starts
fs_restart_steamvr=false
if [ "$fs_layer_changed" = true ] && [ -f "$fc_manifest" ]; then
    fs_restart_steamvr=true
fi
fs_steamvr_pending=false
if systemctl --user is-active --quiet steamvr.service; then
    if [ "$fs_restart_steamvr" = true ] && [ "$fs_no_restart" = true ]; then
        # Not now (--no-restart): the new layer loads at SteamVR's next start. The app runs on meanwhile
        fs_steamvr_pending=true
        if [ "$fs_enable" = true ] || systemctl --user is-active --quiet "$fs_unit"; then
            stop_manual_instance "$fs_name"
            systemctl --user restart "$fs_unit"
        fi
    elif [ "$fs_restart_steamvr" = true ]; then
        printf '\nWARNING: restarting SteamVR now to load the updated Vulkan layer: a running game closes.\n'
        printf '         (Next time, sh contrib/install.sh --no-restart leaves SteamVR running.)\n'
        printf 'Restarting SteamVR for the updated Vulkan layer (Frame Dock starts with it)...\n'
        systemctl --user restart steamvr.service
    elif [ "$fs_enable" = true ] || systemctl --user is-active --quiet "$fs_unit"; then
        stop_manual_instance "$fs_name"
        # restart stops the old one with SIGTERM first, so it shuts down cleanly before the new binary starts
        systemctl --user restart "$fs_unit"
    fi
fi

printf '\nFrame Dock %s is installed.\n' "$("$fs_bin" --version 2>/dev/null | awk '{print $2}')"
if [ "$fs_enable" = true ]; then
    printf '  It starts together with SteamVR. Dashboard: the Frame Dock tab.\n'
else
    printf '  Autostart is off. Start it from + in the SteamVR dashboard (Frame Dock).\n'
fi
if [ "$fs_app_only" = true ]; then
    printf '  The Vulkan layer was left as it was (--app-only).\n'
elif [ -f "$fc_manifest" ]; then
    printf '  Vulkan correction: enabled.\n'
else
    printf '  Vulkan correction is off until you run: %s/framedockctl enable\n' "$fs_bin_dir"
fi
if [ "$fs_steamvr_pending" = true ]; then
    printf '  The Vulkan layer changed and SteamVR was not restarted (--no-restart). It loads the new layer at its next\n'
    printf '  start; to load it now (a running game closes): systemctl --user restart steamvr.service\n'
fi
if [ "$fs_replace_old" = no ]; then
    printf '  Kept:%s (stopped and no longer started with SteamVR: they share Frame Dock'"'"'s settings file).\n' "$fs_old_apps"
    printf '  To remove them: sh %s/contrib/install.sh --replace-old-apps\n' "$fs_source_dir"
fi
printf '  Logs: journalctl --user -u %s -f\n' "$fs_unit"
printf '  Remove: sh %s/contrib/uninstall.sh\n' "$fs_source_dir"
