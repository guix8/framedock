// Parsing and derivation for the system sensors, kept free of file I/O so the unit tests can feed it
// fixture text. Sensors (sensors.cpp) does the reading; SensorBridge stores the results as metrics.
// Ported from upstream Frame Perf Overlay (PR #5, 066a5c8): the CPU and General parts only (Frame Dock doesn't
// carry the power tree or the battery details).
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace sensor_math {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------------------------
// CPU

/** Busy / total jiffies per CPU line of /proc/stat. Index 0 = the "cpu" total line, index n+1 = "cpu<n>". */
struct CpuTicks {
    std::vector<uint64_t> busy;
    std::vector<uint64_t> total;
    std::vector<bool> present;  ///< false for a core whose line is missing (offline)
};

/**
 * Parses the cpu lines at the top of /proc/stat.
 * busy = user + nice + system + irq + softirq + steal (fields 1-8 minus idle + iowait; guest is inside user).
 * @param text the file's text (only the leading cpu lines are looked at)
 * @param ticks result; sized to the highest core number seen + 2
 * @return true if at least the total line parsed
 */
bool parseProcStat(const char* text, CpuTicks& ticks);

/**
 * Load in % per line between two /proc/stat reads.
 * @return same indexing as CpuTicks; NaN where either read lacks that line or no time passed
 */
std::vector<double> cpuPercents(const CpuTicks& prev, const CpuTicks& cur);

// ---------------------------------------------------------------------------------------------
// General

/**
 * @param text /proc/uptime ("8914.95 62459.80")
 * @param seconds result (first field: since boot, including suspend)
 */
bool parseUptime(const char* text, double& seconds);

/**
 * @param text /proc/self/statm ("617 292 271 7 0 146 0")
 * @param residentPages result (second field)
 */
bool parseStatmResident(const char* text, long long& residentPages);

/**
 * This app's estimated power: its share of all busy CPU time × the CPU rails' watts. An estimate
 * (shown with "≈"): it assumes every busy CPU second costs the same, whichever core and clock.
 * @param ownCpuSec own CPU time over the interval (all threads)
 * @param systemBusySec busy CPU time of the whole system over the same interval (all cores)
 * @param cpuRailsW apc0 + apc1 + apc2 watts
 * @return watts; NaN if an input is NaN or the system was idle
 */
double selfWatts(double ownCpuSec, double systemBusySec, double cpuRailsW);

}  // namespace sensor_math
