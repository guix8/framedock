cmake_minimum_required(VERSION 3.16)
# Frame Dock: Frame Perf Overlay (perf/) with FrameColor's display controls (src/) as a fourth dashboard page.
# One binary, framedock. The Vulkan layer (third_party/vkbasalt) is built with meson by contrib/install.sh.
project(framedock VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The OpenVR library that ships with SteamVR (its path on the Frame's SteamOS)
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory containing libopenvr_api.so")
set(OPENVR_LIB "${OPENVR_LIB_DIR}/libopenvr_api.so")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-genl-3.0)

set(PERF perf)
set(OPENVR_INCLUDE third_party/openvr)  # the same header as perf/third_party/openvr

# ---- Perf: core/ (platform-free) and core/render/ (cairo + freetype) ----
add_library(frame-perf-core STATIC
  ${PERF}/core/reason.cpp
  ${PERF}/core/metric_registry.cpp
  ${PERF}/core/metric_store.cpp
  ${PERF}/core/scheduler.cpp
  ${PERF}/core/json.cpp
  ${PERF}/core/layout.cpp
  ${PERF}/core/layout_edit.cpp
  ${PERF}/core/atomic_file.cpp
)
target_include_directories(frame-perf-core PUBLIC ${PERF}/core)

add_library(frame-perf-render STATIC
  ${PERF}/core/render/draw.cpp
  ${PERF}/core/render/theme.cpp
  ${PERF}/core/render/widget_text.cpp
  ${PERF}/core/render/widgets.cpp
  ${PERF}/core/render/grid_renderer.cpp
)
target_include_directories(frame-perf-render PUBLIC ${PERF}/core/render)
target_link_libraries(frame-perf-render PUBLIC frame-perf-core PkgConfig::CAIRO PkgConfig::FREETYPE)

# ---- FrameColor: the Color page's model and drawing (no OpenVR here; ColorHost hides it) ----
add_library(framecolor-ui STATIC
  src/color_i18n.cpp
  src/auto_profile.cpp
  src/color_model.cpp
  src/color_page.cpp
  src/gaze_map.cpp
  src/gaze_fit.cpp
)
target_include_directories(framecolor-ui PUBLIC src)
target_link_libraries(framecolor-ui PUBLIC frame-perf-render)

# ---- The browser panel (Phase 3): Chrome over the DevTools protocol, the VR keyboard. No OpenVR here. Frame Dock's
# string table (app_text) is here too: the lowest library whose code shows text ----
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
add_library(framedock-browser STATIC
  ${PERF}/headset/app_text.cpp
  browser/hangul.cpp
  browser/websocket.cpp
  browser/devtools_pipe.cpp
  browser/cdp.cpp
  browser/jpeg_frame.cpp
  browser/chrome_process.cpp
  browser/browser_session.cpp
  browser/browser_worker.cpp
  browser/vr_keyboard.cpp
  browser/browser_view.cpp
  browser/browser_controller.cpp
  browser/favorites.cpp
)
target_include_directories(framedock-browser PUBLIC browser ${PERF}/headset)
target_link_libraries(framedock-browser PUBLIC frame-perf-render PkgConfig::TURBOJPEG Threads::Threads)

# ---- The dashboard tab and what it pulls in (shared by the app and the tests) ----
add_library(framedock-dashboard STATIC
  ${PERF}/headset/placement.cpp
  ${PERF}/headset/dock_math.cpp
  ${PERF}/headset/dock.cpp
  ${PERF}/headset/dock_view.cpp
  ${PERF}/headset/battery_watch.cpp
  ${PERF}/headset/media_strip.cpp
  ${PERF}/headset/notice_view.cpp
  ${PERF}/headset/dock_manifest.cpp
  ${PERF}/headset/dock_loop.cpp
  ${PERF}/headset/browser_loop.cpp
  ${PERF}/headset/dashboard_preview.cpp
  ${PERF}/headset/frame_sync_flow.cpp
  ${PERF}/headset/device_model.cpp
  ${PERF}/headset/device_page.cpp
  ${PERF}/headset/clock.cpp
  ${PERF}/headset/config.cpp
  ${PERF}/headset/settings_panel.cpp
  ${PERF}/headset/layout_editor.cpp
  ${PERF}/headset/i18n.cpp
  ${PERF}/headset/wifi_link.cpp
)
target_include_directories(framedock-dashboard PUBLIC ${PERF}/headset ${OPENVR_INCLUDE})
target_link_libraries(framedock-dashboard PUBLIC framecolor-ui framedock-browser frame-perf-render PkgConfig::LIBNL)

# ---- The 기기 page's backends: sound (libpulse), Camera Switch (sd-bus), SteamVR's room view (vrserver's mailbox
# WebSocket), Frame Sync (libcurl). Its own library so only the app and the device tests link these ----
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
add_library(framedock-device STATIC
  ${PERF}/headset/audio_sinks.cpp
  ${PERF}/headset/pulse_audio.cpp
  ${PERF}/headset/camera_switch.cpp
  ${PERF}/headset/room_view.cpp
  ${PERF}/headset/frame_steam.cpp
  ${PERF}/headset/frame_sync_agent.cpp
  ${PERF}/headset/frame_sync_worker.cpp
  ${PERF}/headset/device_backend.cpp
)
target_include_directories(framedock-device PUBLIC ${PERF}/headset)
target_link_libraries(framedock-device PUBLIC framedock-dashboard framedock-browser frame-perf-core
  PkgConfig::PULSE PkgConfig::SYSTEMD PkgConfig::CURL Threads::Threads)

add_executable(framedock
  ${PERF}/headset/main.cpp
  ${PERF}/headset/sensors.cpp
  ${PERF}/headset/sensor_math.cpp
  ${PERF}/headset/async_sampler.cpp
  ${PERF}/headset/system_metrics.cpp
  ${PERF}/headset/sensor_bridge.cpp
  ${PERF}/headset/render_size.cpp
  ${PERF}/headset/update_check.cpp
  ${PERF}/headset/process_scan.cpp
  ${PERF}/headset/hide_rules.cpp
  ${PERF}/headset/dump_metrics.cpp
  ${PERF}/headset/vr_overlay.cpp
  ${PERF}/headset/vk_texture.cpp
  ${PERF}/headset/layout_authority.cpp
  ${PERF}/headset/profile_store.cpp
  ${PERF}/headset/autostart.cpp
  ${PERF}/headset/framecorder.cpp
  ${PERF}/headset/paths.cpp
  src/color_openvr.cpp
  src/gaze_feed.cpp
  src/eye_server.cpp
  src/bench.cpp
)
target_link_libraries(framedock PRIVATE
  framedock-device
  framedock-dashboard
  PkgConfig::CAIRO
  PkgConfig::FREETYPE
  PkgConfig::VULKAN
  PkgConfig::LIBNL
  Threads::Threads
  "${OPENVR_LIB}"
)
target_compile_definitions(framedock PRIVATE FRAMEDOCK_VERSION="${PROJECT_VERSION}")

set(DOCK_TARGETS frame-perf-core frame-perf-render framecolor-ui framedock-browser framedock-dashboard framedock-device
  framedock)
# Release: Perf's size-optimised build (-Os, sections + gc, LTO) on the app and its static libraries
option(FRAMEDOCK_OPTIMIZE_SIZE "Optimize the release build for installed size" ON)
include(CheckIPOSupported)
check_ipo_supported(RESULT DOCK_IPO_SUPPORTED OUTPUT DOCK_IPO_ERROR)
foreach(target ${DOCK_TARGETS})
  target_compile_options(${target} PRIVATE -Wall -Wextra)
  if(FRAMEDOCK_OPTIMIZE_SIZE)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-Os>)
  endif()
  target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>)
  if(DOCK_IPO_SUPPORTED)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endforeach()
# The browser's per-frame work (base64, JPEG, the frame copy) runs at -O2: speed over size there
target_compile_options(framedock-browser PRIVATE $<$<CONFIG:Release>:-O2>)
target_link_options(framedock PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
# SteamVR's own libopenvr_api.so at run time too. Only the install RPATH, also in the build tree: the build RPATH
# would end in an empty element, which means "the current directory"
set_target_properties(framedock PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "${OPENVR_LIB_DIR}"
)
install(TARGETS framedock RUNTIME DESTINATION bin)

# ---- Tests: ctest --test-dir build ----
include(CTest)
if(BUILD_TESTING)
  # One test executable, -Wall -Wextra. dock_test(NAME SOURCES ... [INCLUDES ...] [LIBS ...] [ARGS ...])
  function(dock_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;INCLUDES;ARGS" ${ARGN})
    add_executable(${name}-test ${T_SOURCES})
    target_compile_options(${name}-test PRIVATE -Wall -Wextra)
    target_include_directories(${name}-test PRIVATE ${T_INCLUDES})
    target_link_libraries(${name}-test PRIVATE ${T_LIBS})
    add_test(NAME ${name} COMMAND ${name}-test ${T_ARGS})
  endfunction()

  # Perf's tests
  dock_test(wrist-clock SOURCES ${PERF}/tests/wrist_clock_test.cpp LIBS framedock-dashboard)
  dock_test(layout-editor SOURCES ${PERF}/tests/layout_editor_test.cpp LIBS framedock-dashboard)
  dock_test(hold-repeat SOURCES ${PERF}/tests/hold_repeat_test.cpp LIBS framedock-dashboard)
  dock_test(quick-switch SOURCES ${PERF}/tests/quick_switch_test.cpp LIBS framedock-dashboard)
  # The Dock (in-game interaction): its geometry and state machine
  dock_test(dock SOURCES ${PERF}/tests/dock_test.cpp LIBS framedock-dashboard)
  dock_test(dock-view SOURCES ${PERF}/tests/dock_view_test.cpp LIBS framedock-dashboard)
  # The battery's time left and notices, the HUD's video controls
  dock_test(battery-media SOURCES ${PERF}/tests/battery_media_test.cpp LIBS framedock-dashboard)
  # The 기기 page: its model, Frame Sync's flow (ported from flow.py), the page's hit areas and drag
  dock_test(device SOURCES ${PERF}/tests/device_test.cpp LIBS framedock-dashboard)
  # The 기기 page's backends: sinks, the throttle, vdf, Frame Sync's files, DeviceBackend on fakes; then the services
  # against local fakes (an HTTP agent, a vrserver WebSocket, a private dbus-daemon)
  dock_test(device-backend SOURCES ${PERF}/tests/device_backend_test.cpp LIBS framedock-device)
  dock_test(device-services SOURCES ${PERF}/tests/device_services_test.cpp LIBS framedock-device)
  foreach(t device-backend device-services)
    target_compile_definitions(${t}-test PRIVATE FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/device")
  endforeach()
  dock_test(runtime SOURCES ${PERF}/tests/runtime_test.cpp ${PERF}/headset/placement.cpp ${PERF}/headset/config.cpp
             ${PERF}/headset/i18n.cpp
             INCLUDES ${PERF}/headset ${PERF}/core/render ${OPENVR_INCLUDE} LIBS frame-perf-core PkgConfig::VULKAN)
  dock_test(sensors SOURCES ${PERF}/tests/sensors_test.cpp ${PERF}/headset/sensors.cpp ${PERF}/headset/sensor_math.cpp
             ${PERF}/headset/wifi_link.cpp INCLUDES ${PERF}/headset LIBS PkgConfig::LIBNL)
  dock_test(ui-helpers SOURCES ${PERF}/tests/ui_helpers_test.cpp ${PERF}/headset/config.cpp ${PERF}/headset/i18n.cpp
             INCLUDES ${PERF}/headset LIBS frame-perf-render)
  foreach(name core-registry core-scheduler core-json core-layout core-layout-edit)
    string(REPLACE "-" "_" file ${name})
    dock_test(${name} SOURCES ${PERF}/tests/${file}_test.cpp LIBS frame-perf-core)
  endforeach()
  foreach(name render-grid render-rate-text)
    string(REPLACE "-" "_" file ${name})
    dock_test(${name} SOURCES ${PERF}/tests/${file}_test.cpp LIBS frame-perf-render)
  endforeach()
  dock_test(config-layout SOURCES ${PERF}/tests/config_layout_test.cpp ${PERF}/headset/config.cpp
             ${PERF}/headset/i18n.cpp INCLUDES ${PERF}/headset ${PERF}/core/render LIBS frame-perf-core)
  dock_test(profile-store SOURCES ${PERF}/tests/profile_store_test.cpp ${PERF}/headset/profile_store.cpp
             ${PERF}/headset/layout_authority.cpp ${PERF}/headset/config.cpp ${PERF}/headset/i18n.cpp
             ${PERF}/headset/app_text.cpp
             INCLUDES ${PERF}/headset ${PERF}/core/render LIBS frame-perf-render)
  dock_test(system-sources SOURCES ${PERF}/tests/system_sources_test.cpp ${PERF}/headset/sensors.cpp
             ${PERF}/headset/sensor_math.cpp ${PERF}/headset/system_metrics.cpp ${PERF}/headset/async_sampler.cpp
             ${PERF}/headset/wifi_link.cpp
             INCLUDES ${PERF}/headset LIBS frame-perf-core PkgConfig::LIBNL Threads::Threads)
  # The Frame card's render size: the PC helper's JSON, vrlink.txt's peers, the layer's file and its staleness rule
  dock_test(render-size SOURCES ${PERF}/tests/render_size_test.cpp ${PERF}/headset/render_size.cpp
             INCLUDES ${PERF}/headset LIBS framecolor-ui PkgConfig::CURL)
  # The new version notice: the version comparison, GitHub's tag_name, the once-a-day rule, update_check.json
  dock_test(update-check SOURCES ${PERF}/tests/update_check_test.cpp ${PERF}/headset/update_check.cpp
             INCLUDES ${PERF}/headset LIBS frame-perf-core PkgConfig::CURL Threads::Threads)
  # Upstream PR #5's readouts: /proc/stat per core, uptime, own cost (pure, fed from perf/tests/fixtures/sensors);
  # the /proc process scan on fixture trees; Hide system (glob, rules file, the default rules vs a Frame's process list)
  dock_test(sensor-math SOURCES ${PERF}/tests/sensor_math_test.cpp ${PERF}/headset/sensor_math.cpp
             INCLUDES ${PERF}/headset ARGS ${CMAKE_SOURCE_DIR}/${PERF}/tests/fixtures/sensors)
  dock_test(process-scan SOURCES ${PERF}/tests/process_scan_test.cpp ${PERF}/headset/process_scan.cpp
             ${PERF}/headset/hide_rules.cpp ${PERF}/headset/app_text.cpp
             INCLUDES ${PERF}/headset ${PERF}/core/render LIBS frame-perf-core ARGS ${CMAKE_SOURCE_DIR}/${PERF}/tests/fixtures)
  # 녹화: framecorder's settings, its process name, the recorder child against fake recorders (shell scripts), its
  # tab's status.json / command.json against a fake tab
  dock_test(framecorder SOURCES ${PERF}/tests/framecorder_test.cpp ${PERF}/headset/framecorder.cpp
             INCLUDES ${PERF}/headset LIBS frame-perf-core)
  dock_test(hide-rules SOURCES ${PERF}/tests/hide_rules_test.cpp ${PERF}/headset/process_scan.cpp
             ${PERF}/headset/hide_rules.cpp ${PERF}/headset/app_text.cpp
             INCLUDES ${PERF}/headset ${PERF}/core/render LIBS frame-perf-core ARGS ${CMAKE_SOURCE_DIR}/${PERF}/tests/fixtures)

  # The browser panel and the VR keyboard
  dock_test(hangul SOURCES tests/browser/hangul_test.cpp LIBS framedock-browser)
  dock_test(cdp SOURCES tests/browser/cdp_test.cpp LIBS framedock-browser)
  dock_test(keyboard SOURCES tests/browser/keyboard_test.cpp LIBS framedock-browser)
  dock_test(browser-view SOURCES tests/browser/browser_view_test.cpp LIBS framedock-browser)
  # The Chrome session against a fake Chrome (a real process speaking DevTools over a WebSocket)
  add_executable(fake-chrome tests/browser/fake_chrome.cpp)
  target_compile_options(fake-chrome PRIVATE -Wall -Wextra)
  target_link_libraries(fake-chrome PRIVATE framedock-browser)
  dock_test(browser-session SOURCES tests/browser/browser_session_test.cpp LIBS framedock-browser)
  target_compile_definitions(browser-session-test PRIVATE FAKE_CHROME="$<TARGET_FILE:fake-chrome>")
  add_dependencies(browser-session-test fake-chrome)

  # The Vulkan layer's .cube reader (it runs inside vrcompositor: a bad file must fall back, never throw)
  dock_test(lut-cube SOURCES tests/layer/lut_cube_test.cpp third_party/vkbasalt/src/lut_cube.cpp
             third_party/vkbasalt/src/logger.cpp INCLUDES third_party/vkbasalt/src)
  # The layer's effect setup on fake device functions: a failed Vulkan call leaves that swapchain without the effect,
  # never throws out of the layer and frees what was made. Built as "vrcompositor", the only process the layer acts in
  # The layer's own sources as meson builds them (C++20, its own warnings): only the test is -Wextra
  set(VKBASALT_SRC third_party/vkbasalt/src)
  add_library(vkbasalt-under-test OBJECT
    ${VKBASALT_SRC}/basalt.cpp ${VKBASALT_SRC}/buffer.cpp ${VKBASALT_SRC}/command_buffer.cpp ${VKBASALT_SRC}/config.cpp
    ${VKBASALT_SRC}/descriptor_set.cpp ${VKBASALT_SRC}/effect_lut.cpp ${VKBASALT_SRC}/effect_simple.cpp
    ${VKBASALT_SRC}/effect_transfer.cpp ${VKBASALT_SRC}/fake_swapchain.cpp ${VKBASALT_SRC}/format.cpp
    ${VKBASALT_SRC}/framebuffer.cpp ${VKBASALT_SRC}/graphics_pipeline.cpp ${VKBASALT_SRC}/image.cpp
    ${VKBASALT_SRC}/image_view.cpp ${VKBASALT_SRC}/logger.cpp ${VKBASALT_SRC}/logical_swapchain.cpp
    ${VKBASALT_SRC}/lut_cube.cpp ${VKBASALT_SRC}/memory.cpp ${VKBASALT_SRC}/renderpass.cpp ${VKBASALT_SRC}/sampler.cpp
    ${VKBASALT_SRC}/shader.cpp ${VKBASALT_SRC}/util.cpp ${VKBASALT_SRC}/vkdispatch.cpp)
  target_compile_features(vkbasalt-under-test PRIVATE cxx_std_20)
  target_compile_options(vkbasalt-under-test PRIVATE -Wall)
  # The shaders' SPIR-V comes from glslangValidator in the meson build; the fake device never reads it
  target_include_directories(vkbasalt-under-test PRIVATE tests/layer/shader_stub)
  target_link_libraries(vkbasalt-under-test PUBLIC PkgConfig::VULKAN)
  target_include_directories(vkbasalt-under-test PUBLIC ${VKBASALT_SRC})
  dock_test(layer-failure SOURCES tests/layer/layer_failure_test.cpp LIBS vkbasalt-under-test)
  set_target_properties(layer-failure-test PROPERTIES OUTPUT_NAME vrcompositor
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/layer-failure)

  # FrameColor's offline harness: the model, its files, auto presets, bench statistics (bench.cpp links OpenVR)
  dock_test(framecolor-harness SOURCES tests/offline/harness.cpp src/bench.cpp
             INCLUDES ${OPENVR_INCLUDE} LIBS framecolor-ui Threads::Threads "${OPENVR_LIB}")
  set_target_properties(framecolor-harness-test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_LIB_DIR}")
  # The Color page inside the dashboard tab: the page switch, its hit areas and actions
  dock_test(color-page SOURCES tests/color_page_test.cpp LIBS framedock-dashboard)
  dock_test(gaze SOURCES tests/gaze_test.cpp LIBS framecolor-ui)
  # FrameColor's shell and Python tests, and the install scripts (run against a fake home)
  add_test(NAME framedockctl COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_framedockctl.sh)
  add_test(NAME framecolor-guard COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_guard.sh)
  add_test(NAME framedock-python COMMAND python3 -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests)
endif()
