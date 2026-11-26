#!/bin/sh
# The newest glibc symbol version each binary may need: SteamOS stable's glibc (2.39 on 2026-11-18). The cross
# build's sysroot comes from a Frame that may run a newer SteamOS, and a symbol from a newer glibc there makes the
# release fail to start on stable ("version `GLIBC_2.43' not found", guix8/framedock#3).
#   sh tests/test_glibc_floor.sh FILE...   (a missing file is skipped: the layer is built apart from the app)
set -eu
max_minor=39
command -v readelf >/dev/null 2>&1 || { echo "readelf not found: skipped"; exit 0; }
status=0
for file in "$@"; do
    [ -f "$file" ] || { echo "skipped (not built): $file"; continue; }
    newer=$(readelf --dyn-syms -W "$file" | grep -oE '@GLIBC_2\.[0-9]+' | sort -u |
        awk -F. -v max="$max_minor" '$2 + 0 > max { print substr($0, 2) }')
    if [ -n "$newer" ]; then
        echo "$file needs a glibc newer than 2.$max_minor:"
        readelf --dyn-syms -W "$file" | grep -E "@($(echo $newer | tr ' ' '|'))( |$)" | awk '{ print "  " $8 }' | sort -u
        status=1
    else
        echo "ok: $file"
    fi
done
exit $status
