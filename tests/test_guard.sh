#!/bin/sh
# Run on Steam Frame: sh tests/test_guard.sh
set -eu

fc_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fc_temp=$(mktemp -d)
fc_data="$fc_temp/.local/share/framecolor"
fc_manifest="$fc_data/layer/vkBasalt.json"
fc_disabled="$fc_manifest.disabled"
fc_marker="$fc_temp/runtime/framecolor/steamvr-started"
fc_failures="$fc_temp/runtime/framecolor/failed-starts"
fc_limit="$fc_temp/.config/framecolor/guard-failure-limit"

cleanup() {
    rm -f "$fc_manifest" "$fc_disabled" "$fc_marker" "$fc_failures" "$fc_limit" "$fc_temp/not-a-dir" "$fc_temp/guard"
    rmdir "$fc_data/layer" "$fc_data" "$fc_temp/.local/share" "$fc_temp/.local" \
        "$fc_temp/runtime/framecolor" "$fc_temp/runtime" "$fc_temp/.config/framecolor" \
        "$fc_temp/.config" "$fc_temp" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$fc_data/layer" "$fc_temp/runtime" "$fc_temp/.config/framecolor"
: > "$fc_manifest"

HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ ! -e "$fc_manifest" ] && [ -e "$fc_disabled" ] && [ ! -e "$fc_marker" ]

mv "$fc_disabled" "$fc_manifest"
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
SERVICE_RESULT=success HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ -e "$fc_manifest" ] && [ ! -e "$fc_disabled" ] && [ ! -e "$fc_failures" ] && [ ! -e "$fc_marker" ]

HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
printf '%s\n' "$(($(date +%s) - 20))" > "$fc_marker"
SERVICE_RESULT=success HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ -e "$fc_manifest" ] && [ ! -e "$fc_disabled" ] && [ ! -e "$fc_marker" ]

HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
[ -e "$fc_manifest" ] && [ -e "$fc_marker" ]
printf '3\n' > "$fc_limit"
SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ -e "$fc_manifest" ] && [ "$(cat "$fc_failures")" = 1 ]
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
SERVICE_RESULT=success HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ -e "$fc_manifest" ] && [ ! -e "$fc_failures" ]
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
for fc_attempt in 1 2; do
    SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
        sh "$fc_root/contrib/framecolor-guard" stop
    [ -e "$fc_manifest" ] && [ "$(cat "$fc_failures")" = "$fc_attempt" ]
    HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
done
SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ ! -e "$fc_manifest" ] && [ -e "$fc_disabled" ] && [ ! -e "$fc_failures" ]

mv "$fc_disabled" "$fc_manifest"
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start
printf '%s\n' "$(($(date +%s) - 20))" > "$fc_marker"
SERVICE_RESULT=exit-code HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" \
    sh "$fc_root/contrib/framecolor-guard" stop
[ -e "$fc_manifest" ] && [ ! -e "$fc_failures" ]
# The start branch never blocks SteamVR (it is ExecCondition): exit 0 when its notes can't be written
[ -e "$fc_manifest" ] || : > "$fc_manifest"
: > "$fc_temp/not-a-dir"
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/not-a-dir" sh "$fc_root/contrib/framecolor-guard" start 2>/dev/null
(unset HOME; XDG_RUNTIME_DIR="$fc_temp/runtime" sh "$fc_root/contrib/framecolor-guard" start)  # no HOME: still 0
rm -f "$fc_temp/not-a-dir"
# The drop-in runs it through sh so a missing guard doesn't block either; both parts exit 0
fc_cond=$(sed -n 's/^ExecCondition=//p' "$fc_root/contrib/90-framecolor.conf" | sed 's/\$\$/$/g')
fc_cmd=$(printf '%s' "$fc_cond" | sed "s|^/bin/sh -c '\(.*\)' %h/.*|\1|")
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh -c "$fc_cmd" "$fc_root/contrib/framecolor-guard-missing"
cp "$fc_root/contrib/framecolor-guard" "$fc_temp/guard"
chmod 755 "$fc_temp/guard"
HOME="$fc_temp" XDG_RUNTIME_DIR="$fc_temp/runtime" sh -c "$fc_cmd" "$fc_temp/guard"
[ -e "$fc_marker" ]
rm -f "$fc_temp/guard"
printf 'FrameColor guard tests: OK\n'
