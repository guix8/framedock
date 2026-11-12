#!/bin/sh
# Copy what a cross build needs from the headset into a sysroot on the PC: the headers, the C/C++ runtime and GCC's
# files, every shared library the given headset programs load (ldd), pkg-config files, libopenvr_api.so, and the
# fonts and fontconfig setup (so text-measuring tests give the headset's results under qemu). The headset only reads
# (tar at the lowest CPU and I/O priority, no compression). Run it again after a SteamOS update: the old sysroot is
# replaced only once the new one is complete.
#
#   sh contrib/cross/make-sysroot.sh steamos@<headset-ip> [PROGRAM...]
#   SSH=ssh.exe sh contrib/cross/make-sysroot.sh steamos@<headset-ip>   (WSL: Windows' ssh and its keys)
#
# PROGRAM: headset paths whose libraries to take (default: the installed Frame Dock). Another project lists its own
# programs, or adds libraries with EXTRA="usr/lib/libfoo.so*" (paths without the leading /).
# Environment: FRAME_SYSROOT (default ~/frame-sysroot), SSH (default ssh).
set -eu

host="${1:?usage: make-sysroot.sh user@headset [program...]}"
shift
programs="${*:-\$HOME/.local/bin/framedock}"
sysroot="${FRAME_SYSROOT:-$HOME/frame-sysroot}"
ssh_cmd="${SSH:-ssh}"
new="$sysroot.new"
rm -rf "$new"
mkdir -p "$new"
trap 'rm -rf "$new" "$new.tar"' EXIT HUP INT TERM

# The list is made by bash on the headset (brace patterns); the tar goes to a file first so a failed ssh or tar on
# either side stops here instead of leaving a half sysroot
"$ssh_cmd" -o BatchMode=yes "$host" bash -s > "$new.tar" <<EOF
set -eo pipefail
cd /
list=\$(mktemp)
trap 'rm -f "\$list"' EXIT
{
    ldd $programs | grep -oE '/usr/lib/[^ ]+' | sort -u |
        sed -E 's#^/usr/lib/(pulseaudio/)?##; s#\\.so.*##' | sort -u | while read -r stem; do
            ls -d usr/lib/\$stem.so* usr/lib/pulseaudio/\$stem.so* 2>/dev/null || true
        done
    ls -d usr/lib/lib{c,m,dl,pthread,rt,util,resolv,mvec}.{so,a}* usr/lib/libc_nonshared.a usr/lib/*crt*.o \
        usr/lib/ld-linux-aarch64.so.1 usr/lib/libstdc++.so* usr/lib/libgcc_s.so* 2>/dev/null || true
    for extra in ${EXTRA:-}; do ls -d \$extra; done
    echo usr/lib/gcc; echo usr/include; echo usr/lib/pkgconfig; echo usr/share/pkgconfig
    echo etc/fonts; echo usr/share/fontconfig
    fc-list -f '%{file}\n' | sed 's#^/##' | sort -u
    ls -d opt/steamvr/bin/linuxarm64/libopenvr_api.so 2>/dev/null || true
} | sort -u > "\$list"
nice -n 19 ionice -c3 tar -cf - -T "\$list"
EOF
tar -xf "$new.tar" -C "$new"
rm -f "$new.tar"
# The loader and the libraries as the headset finds them (/lib is /usr/lib there); qemu -L and lld look here
ln -sfn usr/lib "$new/lib"
ln -sfn usr/lib "$new/lib64"
for needed in usr/lib/libc.so.6 usr/lib/libstdc++.so.6 usr/lib/ld-linux-aarch64.so.1 usr/include/stdio.h; do
    [ -e "$new/$needed" ] || { printf 'The sysroot misses %s: kept the old one\n' "$needed" >&2; exit 1; }
done
ls -d "$new"/usr/lib/gcc/*/* >/dev/null 2>&1 || { printf 'The sysroot misses GCC: kept the old one\n' >&2; exit 1; }
rm -rf "$sysroot.old"
[ -e "$sysroot" ] && mv "$sysroot" "$sysroot.old"
mv "$new" "$sysroot"
rm -rf "$sysroot.old"
printf 'Sysroot in %s (%s)\n' "$sysroot" "$(du -sh "$sysroot" | cut -f1)"
