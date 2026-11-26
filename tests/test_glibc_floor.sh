#!/bin/sh
# The newest glibc and libstdc++ symbol versions each binary may need. The cross build's sysroot comes from a Frame
# that may run a newer SteamOS, and a symbol from a newer library there makes the release fail to start on stable
# ("version `GLIBC_2.43' not found", guix8/framedock#3).
# - glibc: SteamOS stable's 2.39 (2026-11-18).
# - libstdc++: GLIBCXX_3.4.32 (GCC 13.2), the newest the 0.1.2 layer needed and stable's compositor loaded; a newer
#   one is not known to be on stable.
#   sh tests/test_glibc_floor.sh FILE...   exit 0 ok, 1 too new, 77 nothing checked (ctest: skipped)
set -eu
glibc_max=39
glibcxx_max=32
command -v readelf >/dev/null 2>&1 || { echo "readelf not found"; exit 77; }
status=0
checked=0
for file in "$@"; do
    [ -f "$file" ] || { echo "skipped (not built): $file"; continue; }
    symbols=$(readelf --dyn-syms -W "$file") || { echo "readelf failed: $file"; status=1; continue; }
    checked=$((checked + 1))
    newer=$(printf '%s\n' "$symbols" | grep -oE '@(GLIBC_2|GLIBCXX_3\.4)\.[0-9]+' | sort -u |
        awk -F. -v g="$glibc_max" -v x="$glibcxx_max" '
            /GLIBCXX/ { if ($NF + 0 > x) print substr($0, 2); next }
            { if ($NF + 0 > g) print substr($0, 2) }')
    if [ -n "$newer" ]; then
        echo "$file needs a newer glibc or libstdc++ than SteamOS stable has:"
        printf '%s\n' "$symbols" | grep -E "@($(echo $newer | tr ' ' '|'))( |$)" | awk '{ print "  " $8 }' | sort -u
        status=1
    else
        echo "ok: $file"
    fi
done
[ "$checked" -gt 0 ] || [ "$status" -ne 0 ] || exit 77
exit $status
