#include "sensor_math.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sensor_math {

bool parseProcStat(const char* text, CpuTicks& ticks) {
    ticks = CpuTicks();
    if (text == nullptr) return false;
    bool haveTotal = false;
    const char* line = text;
    while (line != nullptr && std::strncmp(line, "cpu", 3) == 0) {
        // "cpu  user nice system idle iowait irq softirq steal guest guest_nice" or "cpu<n> ..."
        const char* p = line + 3;
        size_t index = 0;
        if (*p >= '0' && *p <= '9') {
            char* end = nullptr;
            index = static_cast<size_t>(std::strtoul(p, &end, 10)) + 1;
            p = end;
        }
        uint64_t fields[8] = {};
        int count = 0;
        while (count < 8) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(p, &end, 10);
            if (end == p) break;
            fields[count++] = v;
            p = end;
        }
        if (count >= 5 && index < 1024) {
            uint64_t all = 0;
            for (int i = 0; i < count; ++i) all += fields[i];  // guest is already inside user, so stop at steal
            const uint64_t idle = fields[3] + fields[4];       // idle + iowait
            if (ticks.total.size() <= index) {
                ticks.total.resize(index + 1, 0);
                ticks.busy.resize(index + 1, 0);
                ticks.present.resize(index + 1, false);
            }
            ticks.busy[index] = all - idle;
            ticks.total[index] = all;
            ticks.present[index] = true;
            if (index == 0) haveTotal = true;
        }
        const char* next = std::strchr(line, '\n');
        line = next != nullptr ? next + 1 : nullptr;
    }
    return haveTotal;
}

std::vector<double> cpuPercents(const CpuTicks& prev, const CpuTicks& cur) {
    std::vector<double> pct(cur.total.size(), kNaN);
    for (size_t i = 0; i < cur.total.size(); ++i) {
        if (!cur.present[i] || i >= prev.total.size() || !prev.present[i]) continue;
        if (cur.total[i] <= prev.total[i] || cur.busy[i] < prev.busy[i]) continue;
        const double dt = static_cast<double>(cur.total[i] - prev.total[i]);
        const double db = static_cast<double>(cur.busy[i] - prev.busy[i]);
        pct[i] = 100.0 * db / dt;
    }
    return pct;
}

bool parseUptime(const char* text, double& seconds) {
    if (text == nullptr) return false;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || v < 0) return false;
    seconds = v;
    return true;
}

bool parseStatmResident(const char* text, long long& residentPages) {
    if (text == nullptr) return false;
    char* end = nullptr;
    std::strtoll(text, &end, 10);  // size
    if (end == text) return false;
    const char* p = end;
    const long long resident = std::strtoll(p, &end, 10);
    if (end == p || resident < 0) return false;
    residentPages = resident;
    return true;
}

double selfWatts(double ownCpuSec, double systemBusySec, double cpuRailsW) {
    if (std::isnan(ownCpuSec) || std::isnan(systemBusySec) || std::isnan(cpuRailsW)) return kNaN;
    if (systemBusySec <= 0 || ownCpuSec < 0) return kNaN;
    // The system's busy time is counted in 10 ms ticks and our own in ns, so the share can poke over 1
    const double share = std::fmin(1.0, ownCpuSec / systemBusySec);
    return share * cpuRailsW;
}

}  // namespace sensor_math
