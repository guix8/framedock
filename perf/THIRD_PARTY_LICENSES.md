# Third-party licenses

frame-perf-overlay itself is released under the MIT License (see [LICENSE](LICENSE)).
This file lists the third-party code it contains and the system software it uses at run time.

## Bundled in this repository

### Update helper (removed in Frame Dock)

Upstream bundles the author's update helper in `vendor/frame-updater/`. Frame Dock removed it (a private build is
not updated from upstream releases).

### OpenVR SDK header (`third_party/openvr/openvr.h`)

- Project: OpenVR SDK by Valve Corporation, https://github.com/ValveSoftware/openvr
- Version: v2.15.6 (the file is identical to `headers/openvr.h` at that tag)
- License: BSD-3-Clause
- How it is used: compiled into the program as a header. Only this one header is bundled.

The full license text, as published in the OpenVR repository
(https://github.com/ValveSoftware/openvr/blob/master/LICENSE):

```
Copyright (c) 2015, Valve Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Used at run time from the Steam Frame, not bundled

The release tarball contains only the frame-perf-overlay program, its install script, the files in
`contrib/` and the documentation. The libraries and the font below are **not** included. The program
links to or reads the copies that are already on the Steam Frame (SteamOS and SteamVR).

The direct library dependencies below were checked with `readelf -d` and `ldd` on the release binary
on a Steam Frame. Versions are the SteamOS packages on the headset at the time of checking
(2026-09-27) and will change with SteamOS updates.

| Component | How it is used | License | Source |
|---|---|---|---|
| `libopenvr_api.so` (OpenVR API) | Dynamically linked; the copy that comes with SteamVR (`/opt/steamvr/bin/linuxarm64`) | BSD-3-Clause | https://github.com/ValveSoftware/openvr |
| cairo (`libcairo.so.2`, 1.18.0) | Dynamically linked; draws the panels | LGPL-2.1 or MPL-1.1 | https://gitlab.freedesktop.org/cairo/cairo/-/blob/master/COPYING |
| FreeType (`libfreetype.so.6`, 2.13.2) | Dynamically linked; loads the font | FreeType License (FTL) or GPL-2.0 | https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/LICENSE.TXT |
| libnl (`libnl-3.so.200`, `libnl-genl-3.so.200`, 3.9.0) | Dynamically linked; reads the Wi-Fi link status over nl80211 | LGPL-2.1 | https://github.com/thom311/libnl/blob/main/COPYING |
| Vulkan loader (`libvulkan.so.1`, 1.4.309) | Dynamically linked; hands the panel image to SteamVR | Apache-2.0 | https://github.com/KhronosGroup/Vulkan-Loader/blob/main/LICENSE.txt |
| Noto Sans CJK (`/usr/share/fonts/noto-cjk/`) | Font file read at run time; not embedded in the program | SIL Open Font License 1.1 | https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE |
| GNU C Library (`libc.so.6`, `libm.so.6`, 2.39) | Dynamically linked | LGPL-2.1-or-later | https://www.gnu.org/software/libc/ |
| GCC runtime libraries (`libstdc++.so.6`, `libgcc_s.so.1`) | Dynamically linked | GPL-3.0 with the GCC Runtime Library Exception 3.1 | https://www.gnu.org/licenses/gcc-exception-3.1.html |

cairo and FreeType in turn load other SteamOS libraries (for example pixman, libpng, zlib,
fontconfig, HarfBuzz and the X11/XCB client libraries). Those also come from SteamOS and are not
bundled either.

These libraries are used only as shared libraries loaded at run time from the Steam Frame, so they
can be replaced with any compatible version. The text of the GNU Lesser General Public License 2.1,
which covers cairo, libnl and the GNU C Library, is included in [`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt).

Portions of this software are based in part on the work of the FreeType Team (https://freetype.org).

## Icons and images

The launcher icons in `contrib/icons/` and the screenshots in `docs/images/` were drawn by this
program itself with cairo. They are original work of this project and fall under its MIT License.
