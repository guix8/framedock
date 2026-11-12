# Third-party notices

[日本語版](THIRD_PARTY_LICENSES.ja.md)

## [Frame Perf Overlay](https://github.com/sasaken1102r/frame-perf-overlay) (MIT)

`perf/` is based on Frame Perf Overlay v0.4.0 by sasaken1102r and contributors,
imported with `git subtree`, plus features ported by hand from upstream commit
`066a5c8` (the readouts of its PR #5: CPU and RAM items, the full cards, Hide
system), and changed for Frame Dock (the Color page, Korean, no self-updater, the
framedock name). Frame Dock's dashboard tab, HUD, drawing,
Vulkan texture upload and OpenVR overlay code come from it. Its MIT license is
in [`perf/LICENSE`](perf/LICENSE), and its own third-party notices (the OpenVR
header, libnl under LGPL-2.1, fonts) are in
[`perf/THIRD_PARTY_LICENSES.md`](perf/THIRD_PARTY_LICENSES.md).

## [frameeyeosc](https://github.com/sasaken1102r/frameeyeosc) (MIT)

FrameColor's dashboard drawing, Vulkan texture upload, and OpenVR overlay code
(`src/draw.*`, `src/vk_texture.*`, `src/vr_overlay.*` before Frame Dock) was
adapted from frameeyeosc, and its dashboard design was a reference. Frame Dock
uses Frame Perf Overlay's versions of that code instead. Sharpening that follows
the gaze is adapted from frameeyeosc's eye-tracking path: reading SteamVR's
eye-server shared memory (`src/eye_server.*`), its gaze angles, smoothing and
blink handling (`GazeSmoother` and the 1-euro filter and deadzone in
`src/gaze_map.*`), and its rules for measuring a fit dot and re-centring when
the headset is put on (`src/gaze_fit.*`; the 9-dot grid fit itself is Frame
Dock's own). The original work is by konsti219, with changes in the linked fork
by sasaken1102r.
The copyright and permission notice is preserved in
[`third_party/frameeyeosc-LICENSE`](third_party/frameeyeosc-LICENSE).

## [Frametop](https://github.com/Frametop/frametop) (MIT)

The Dock (operating Frame Dock in a game with the other hand's laser) follows
what Frametop 0.2.2 by DeeJanuz found about SteamVR on the Steam Frame and how
its `screens/vr.cpp` lets controllers use its panels while a game runs:
turning `VROverlayFlags_MakeOverlaysInteractiveIfVisible` on for a panel only
while a hand's laser points at it, with a wider margin to leave than to arrive
and a 0.3 s linger; finding where the laser points from the render model's
`tip` component read with `GetComponentState` (not
`GetComponentStateForDevicePath`, which fails during games); setting each
overlay's mouse scale to its texture size so SteamVR's hit area is the panel;
and carrying a panel rigidly with the controller that grabbed it. The laser tip
lookup (`VrOverlay::tipOffset`) and the ray against a flat panel
(`dockmath::hitPanel`) are adapted from its `TipOffset` / `LaserPose` and
`RayOnSurface`. The browser panel's VR keyboard (`browser/vr_keyboard.*`) follows
the design of Frametop's `screens/keyboard.cpp`: a keyboard drawn on the CPU as
rows of key units under a grab bar that carries it, typed with the laser; its
code is written anew (cairo, Korean layouts). Its copyright and permission notice
is preserved in [`third_party/frametop-LICENSE`](third_party/frametop-LICENSE).

## libjpeg-turbo (system library)

The browser panel decodes Chrome's screencast JPEGs with the system's
libjpeg-turbo (`libturbojpeg`, IJG / BSD-3-Clause / zlib licenses), linked
dynamically from SteamOS. Nothing of it is vendored in this repository.

## Google Chrome (not included)

The browser panel drives a Google Chrome that is already installed (by default the
system flatpak `com.google.Chrome`) over the Chrome DevTools Protocol. Chrome is
not part of Frame Dock and is not redistributed; its own terms apply.

## framecorder (not included)

The optional Record control starts and stops framecorder, a separate recorder
for the Steam Frame, when it is installed on the headset (`~/.local/bin/framecorder`).
framecorder is not part of Frame Dock and is not redistributed; Frame Dock only
runs it.

## [OpenVR](https://github.com/ValveSoftware/openvr) (BSD-3-Clause)

`third_party/openvr/openvr.h` is an OpenVR SDK header. Its license is preserved
in [`third_party/openvr/LICENSE`](third_party/openvr/LICENSE).

## [vkBasalt](https://github.com/DadSchoorse/vkBasalt) (zlib)

The Vulkan layer in `third_party/vkbasalt/` is a modified, LUT-only derivative
of vkBasalt, based on upstream
[commit `4f97f09`](https://github.com/DadSchoorse/vkBasalt/commit/4f97f09ffe91900e6ca136cc26cf7966f8f6970d). The zlib license is preserved in
[`third_party/vkbasalt/LICENSE`](third_party/vkbasalt/LICENSE). FrameColor
limits the layer to `vrcompositor`, updates the LUT from user settings, and
removes unused effects, input hooks, depth capture, image loaders, and
ReShade compiler support.

The source and binary release contain no NVIDIA FXAA, SMAA, DLS, ReShade
compiler, stb image loader, or SPIRV-Headers files. The two retained GLSL
shaders are from vkBasalt. `lut.frag.glsl` also includes the sharpening code
from vkBasalt's `cas.frag.glsl`, an implementation of AMD FidelityFX
Contrast Adaptive Sharpening (MIT, Copyright (c) 2017-2019 Advanced Micro
Devices, Inc.). Its license notice is preserved in that shader.

## System-provided dependencies

[Cairo](https://www.cairographics.org/), [FreeType](https://freetype.org/),
[Vulkan](https://www.vulkan.org/), [libnl](https://github.com/thom311/libnl)
(LGPL-2.1, Wi-Fi link readings), [libpulse](https://www.freedesktop.org/wiki/Software/PulseAudio/)
(LGPL-2.1, the sound controls), [libsystemd](https://systemd.io/) (LGPL-2.1, sd-bus
for Camera Switch), [libcurl](https://curl.se/) (curl license, an optional part of the Device page),
libjpeg-turbo (`libturbojpeg`, above) and the C/C++ runtime are used from the
system and linked dynamically. None of them is included in this repository or
the release archive.

## Icons and thumbnail

`contrib/icons/framedock-*.png` and the dashboard thumbnail are drawn by
Frame Dock itself (`framedock --dump-settings-png ... --thumbnail-png`, the
same drawing code as the dashboard tab). They contain no third-party artwork;
their text is rendered with the system's Noto Sans CJK font (SIL Open Font
License 1.1), which is not included.

## README images

The screenshots and clips in `docs/images/` show Frame Dock itself and are not
part of the release archive. The browser clips play
[Big Buck Bunny](https://peach.blender.org/) (© Blender Foundation, CC BY 3.0).
