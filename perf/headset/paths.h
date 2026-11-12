// Where Frame Dock keeps things on the Frame (the config stays in ~/.config/frame-perf-overlay, see config.cpp).
#pragma once

#include <string>

namespace paths {

/** App name used for the unit, the lock file, the debug dump and the binary. */
constexpr const char* kAppName = "framedock";

/** @return $XDG_RUNTIME_DIR, else /run/user/<uid> if writable, else /tmp (no trailing slash) */
std::string runtimeDir();

/** @return $XDG_DATA_HOME, else ~/.local/share */
std::string dataHome();

/** @return where a running instance writes its debug dump (tmpfs, never the disk) */
std::string debugDumpPath();

}  // namespace paths
