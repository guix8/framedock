# Frame Dock builds perf/ from the repository root (../CMakeLists.txt): the dashboard tab now includes the Color
# page from ../src, so perf/ no longer builds on its own. Upstream's file (Frame Perf Overlay v0.4.0) is in git
# history; when pulling upstream changes with git subtree, port new sources and tests to ../CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
project(frame-perf-overlay-moved NONE)
message(FATAL_ERROR "Build Frame Dock from the repository root: cmake -G Ninja -S .. -B ../build")
