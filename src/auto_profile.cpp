#include "auto_profile.h"

#include "atomic_file.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace framecolor {

namespace {

bool validIndex(int index) { return index >= -1 && index <= 5; }

}  // namespace

AutoRules loadAutoRules(const std::filesystem::path& path) {
    AutoRules rules;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string kind;
        if (!(row >> kind)) continue;
        if (kind == "app") {
            // "app <index> <key>": the key is the rest of the line, so it may contain spaces.
            int index = -1;
            std::string key;
            if (!(row >> index) || !validIndex(index) || index < 0) continue;
            std::getline(row >> std::ws, key);
            while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
            if (!key.empty()) rules.apps[key] = index;
        } else {
            int index = -1;
            if (!(row >> index) || !validIndex(index)) continue;
            if (kind == "standalone") rules.standalone = index;
            else if (kind == "streaming") rules.streaming = index;
        }
    }
    return rules;
}

bool saveAutoRules(const std::filesystem::path& path, const AutoRules& rules) {
    std::ostringstream out;
    out << "standalone " << rules.standalone << '\n' << "streaming " << rules.streaming << '\n';
    for (const auto& [key, index] : rules.apps) out << "app " << index << ' ' << key << '\n';
    // fsync'd before the rename: a power loss right after a rule change leaves the old file or the new one
    std::string error;
    if (!frame_perf_overlay::atomicWriteFile(path.string(), out.str(), error)) {
        std::fprintf(stderr, "[color] Couldn't save the auto preset rules: %s\n", error.c_str());
        return false;
    }
    return true;
}

int resolveAutoPreset(const AutoRules& rules, const DisplayContext& context) {
    if (!context.appKey.empty()) {
        const auto it = rules.apps.find(context.appKey);
        if (it != rules.apps.end()) return it->second;
    }
    return context.streaming ? rules.streaming : rules.standalone;
}

namespace {

/** @return true if /proc/<pid>/cmdline's argv[0] is vrlink (by its base name) */
bool isVrlinkCmdline(const std::filesystem::path& cmdlinePath) {
    std::ifstream cmdline(cmdlinePath, std::ios::binary);
    std::string arg0;
    if (!std::getline(cmdline, arg0, '\0')) return false;
    const auto slash = arg0.rfind('/');
    return (slash == std::string::npos ? arg0 : arg0.substr(slash + 1)) == "vrlink";
}

/** @return the PID of a vrlink process, or 0 */
long findVrlink(const std::filesystem::path& procRoot) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(procRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] < '0' || name[0] > '9') continue;
        if (isVrlinkCmdline(it->path() / "cmdline")) return std::strtol(name.c_str(), nullptr, 10);
    }
    return 0;
}

}  // namespace

bool isStreaming(const std::filesystem::path& procRoot) { return findVrlink(procRoot) > 0; }

bool StreamingProbe::streaming(double now) {
    // The vrlink found last: one small read says whether it still runs (its PID could be reused by something else,
    // hence the cmdline, not only that /proc/<pid> exists)
    if (pid_ > 0) {
        if (isVrlinkCmdline(procRoot_ / std::to_string(pid_) / "cmdline")) return true;
        pid_ = 0;
        nextScan_ = now;  // it ended: look again at once (another may have started)
    }
    if (now < nextScan_) return false;
    nextScan_ = now + kRescanSec;
    ++scans_;
    pid_ = findVrlink(procRoot_);
    return pid_ > 0;
}

int nextPresetChoice(int current, const bool customValid[3]) {
    for (int next = current + 1; next <= 5; ++next) {
        if (next < 3 || customValid[next - 3]) return next;
    }
    return -1;
}

}  // namespace framecolor
