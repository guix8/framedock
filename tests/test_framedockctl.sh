#!/bin/sh
# Exercise the no-hardware disable/status path with a private fake home.
set -eu

fc_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fc_temp=$(mktemp -d)
fc_layer="$fc_temp/.local/share/framecolor/layer"
fc_log="$fc_temp/systemctl.log"
cleanup() {
    rm -f "$fc_layer/vkBasalt.json" "$fc_layer/vkBasalt.json.disabled" \
        "$fc_temp/bin/systemctl" "$fc_temp/bin/steamvr" "$fc_log" \
        "$fc_temp/.config/framecolor/guard-failure-limit" \
        "$fc_temp/.local/bin/framedock"
    rmdir "$fc_temp/.local/bin" 2>/dev/null || true
    rmdir "$fc_layer" "$fc_temp/.local/share/framecolor" "$fc_temp/.local/share" \
        "$fc_temp/.local" "$fc_temp/.config/framecolor" "$fc_temp/.config" \
        "$fc_temp/bin" "$fc_temp" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$fc_layer" "$fc_temp/bin" "$fc_temp/.config/framecolor"
printf 'test manifest\n' > "$fc_layer/vkBasalt.json"

printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "$FRAMECOLOR_TEST_LOG"\ncase "$*" in\n  "--user is-active framedock.service") printf "inactive\\n" ;;\n  "--user is-enabled framedock.service") printf "disabled\\n" ;;\n  "--user is-active steamvr.service") printf "active\\n" ;;\nesac\n' > "$fc_temp/bin/systemctl"
printf '#!/bin/sh\n[ "$1" = path ] && printf "/opt/steamvr\\n"\n' > "$fc_temp/bin/steamvr"
chmod +x "$fc_temp/bin/systemctl" "$fc_temp/bin/steamvr"

FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" disable
[ ! -f "$fc_layer/vkBasalt.json" ]
[ -f "$fc_layer/vkBasalt.json.disabled" ]
# Only the layer: Frame Dock keeps running (SteamVR's restart restarts it)
[ "$(sed -n '1p' "$fc_log")" = '--user restart steamvr.service' ]
[ "$(wc -l < "$fc_log")" -eq 1 ]
# Already off: no SteamVR restart for nothing
: > "$fc_log"
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" disable | grep -F 'already disabled' >/dev/null
[ ! -s "$fc_log" ]

fc_status=$(FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" status)
printf '%s\n' "$fc_status" | grep -F 'Vulkan correction: disabled' >/dev/null
printf '%s\n' "$fc_status" | grep -F 'Frame Dock service: inactive' >/dev/null
printf '%s\n' "$fc_status" | grep -F 'Frame Dock autostart: disabled' >/dev/null
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" guard 3
[ "$(cat "$fc_temp/.config/framecolor/guard-failure-limit")" = 3 ]
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" guard 1
[ "$(cat "$fc_temp/.config/framecolor/guard-failure-limit")" = 1 ]
# guard status runs the status itself (not "$0", which is "sh"'s script path only by luck): from another directory too
fc_guard_status=$(cd "$fc_temp" && FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh "$fc_root/contrib/framedockctl" guard status)
printf '%s\n' "$fc_guard_status" | grep -F 'Startup guard: disable after 1 failed' >/dev/null
fc_guard_status=$(FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" \
    sh < "$fc_root/contrib/framedockctl" -s guard status)
printf '%s\n' "$fc_guard_status" | grep -F 'Vulkan correction: disabled' >/dev/null

# The language is set in the Frame Dock tab: no language command
if HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" language ko 2>/dev/null; then exit 1; fi
[ ! -e "$fc_temp/.config/framecolor/language.txt" ]

# enable: the layer on, SteamVR restarted, Frame Dock started if it isn't running. Its autostart is left alone
mkdir -p "$fc_temp/.local/share/framecolor/lib" "$fc_temp/.config/systemd/user"
: > "$fc_temp/.local/share/framecolor/lib/libvkbasalt.so"
: > "$fc_temp/.config/systemd/user/framedock.service"
: > "$fc_log"
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" XDG_RUNTIME_DIR="$fc_temp/run" \
    sh "$fc_root/contrib/framedockctl" enable >/dev/null
[ -f "$fc_layer/vkBasalt.json" ] && [ ! -f "$fc_layer/vkBasalt.json.disabled" ]
[ "$(sed -n '1p' "$fc_log")" = '--user restart steamvr.service' ]
[ "$(sed -n '2p' "$fc_log")" = '--user start framedock.service' ]
[ "$(wc -l < "$fc_log")" -eq 2 ]
if grep -F 'enable' "$fc_log" >/dev/null; then exit 1; fi
# Already on: Frame Dock is started if needed, SteamVR isn't restarted
: > "$fc_log"
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" XDG_RUNTIME_DIR="$fc_temp/run" \
    sh "$fc_root/contrib/framedockctl" enable > "$fc_temp/enable.out"
grep -F 'already enabled' "$fc_temp/enable.out" >/dev/null
grep -F 'systemctl --user restart steamvr.service' "$fc_temp/enable.out" >/dev/null  # how to load a newer layer
[ "$(cat "$fc_log")" = '--user start framedock.service' ]
# ...and with the app removed (uninstall.sh --app-only keeps framedockctl): only the layer and SteamVR
rm -f "$fc_temp/.config/systemd/user/framedock.service"
rmdir "$fc_temp/.config/systemd/user" "$fc_temp/.config/systemd"
mv "$fc_layer/vkBasalt.json" "$fc_layer/vkBasalt.json.disabled"
: > "$fc_log"
FRAMECOLOR_TEST_LOG="$fc_log" HOME="$fc_temp" PATH="$fc_temp/bin:$PATH" XDG_RUNTIME_DIR="$fc_temp/run" \
    sh "$fc_root/contrib/framedockctl" enable >/dev/null
[ -f "$fc_layer/vkBasalt.json" ]
[ "$(cat "$fc_log")" = '--user restart steamvr.service' ]
rm -f "$fc_temp/.local/share/framecolor/lib/libvkbasalt.so"
rmdir "$fc_temp/.local/share/framecolor/lib"

mkdir -p "$fc_temp/.local/bin"
printf '#!/bin/sh\nprintf "bench args: %%s\\n" "$*"\n' > "$fc_temp/.local/bin/framedock"
chmod +x "$fc_temp/.local/bin/framedock"
[ "$(HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench)" = 'bench args: --bench 8' ]
[ "$(HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench 20)" = 'bench args: --bench 20' ]
if HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench 0 2>/dev/null; then exit 1; fi
if HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench 8s 2>/dev/null; then exit 1; fi
[ "$(HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench --compare 0 60)" = 'bench args: --bench-compare 0 60 12' ]
[ "$(HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench --compare 35 100 20)" = 'bench args: --bench-compare 35 100 20' ]
for fc_bad in '0' '0 0.6' '0 101' '0 60 4' '0 60 61' '010 60' '0 0100' '99999999999999999999 60' '-5 60' '0 60 12 1'; do
    # shellcheck disable=SC2086
    if HOME="$fc_temp" sh "$fc_root/contrib/framedockctl" bench --compare $fc_bad 2>/dev/null; then
        printf 'accepted bad bench --compare arguments: %s\n' "$fc_bad" >&2; exit 1
    fi
done

printf 'framedockctl disable/status tests passed\n'
