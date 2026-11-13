// The metric registry: one table that says, for every metric, which widget owns it and which source
// reads it.
//
// Rules (enforced by validateRegistry(), run by the unit tests):
// - every id is unique, and ids are stable forever: new metrics get new ids, retired ids are never reused,
// - every metric has exactly one owner widget (no owner, or the same metric under two owners, fails),
// - every metric names a source and a read interval.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frame_perf_overlay {

/**
 * Widget types. Stable: they go into saved layouts. Numbers without a widget yet (10, 12, 13) are reserved, never
 * reused. 7 and 8 are upstream's (PR #5, ported); 14 and 15 are Frame Dock's own (the simple templates), past
 * upstream's numbers.
 */
enum class WidgetType : uint8_t {
    None = 0,
    Frame = 1,
    Cpu = 2,
    GpuRam = 3,
    Power = 4,
    Battery = 5,
    Temp = 6,
    CpuItems = 7,  ///< processes by CPU
    RamItems = 8,  ///< processes by memory
    Network = 9,
    General = 11,
    SimpleA = 14,  ///< Frame Dock: one card of the essentials (fps, reprojection, throttle, CPU, GPU, RAM, power, battery)
    SimpleB = 15,  ///< Frame Dock: a small tile, fps and battery
    // Frame Dock's controls: buttons on the panel itself (the Dock's laser presses them), no metrics
    ColorPresets = 16,  ///< 색 보정: Standard / Vivid / Night and hold to compare
    Volume = 17,        ///< 소리: mute, −, the level, +
    Passthrough = 18,   ///< 패스스루: off / mono / colour
    Profiles = 19,      ///< 프로필: the first four profiles
    Browser = 20,       ///< 브라우저: open or close the browser panel
    Record = 21,        ///< 녹화: start or stop a framecorder recording (only while framecorder is installed)
};

/** Number of WidgetType values including None and the reserved gaps (for arrays indexed by type). */
constexpr size_t kWidgetTypeCount = 22;

/** @return true for the controls (ColorPresets…Record): buttons, nothing read */
constexpr bool isControlWidget(WidgetType type) {
    return type >= WidgetType::ColorPresets && type <= WidgetType::Record;
}

/**
 * A source is one thing that gets read (a sysfs file group, a /proc file, an nl80211 query, an OpenVR call).
 * Internal: source ids aren't saved anywhere, so they may be renumbered.
 */
enum class SourceId : uint8_t {
    None = 0,
    CpuLoad,      ///< /proc/stat
    CpuClock,     ///< cpufreq scaling_cur_freq
    GpuClock,     ///< devfreq cur_freq
    Ram,          ///< /proc/meminfo
    Temps,        ///< thermal_zone*/temp
    BoardTemps,   ///< vadc iio board temperatures
    Throttle,     ///< cooling_device*/cur_state
    Fan,          ///< hwmon fan1_input
    Power,        ///< max34417 power*_input
    Battery,      ///< power_supply battery + charger
    Network,      ///< nl80211 + interface statistics
    FrameTiming,  ///< OpenVR GetFrameTimings, batched
    Controllers,  ///< OpenVR controller battery properties
    Clock,        ///< local time
    Processes,    ///< /proc/[pid] scan (CPU items, RAM items)
    SceneApp,     ///< OpenVR current scene app (also updated on the scene-app event)
    Uptime,       ///< /proc/uptime
    SelfCost,     ///< own CPU time and memory, CPU rails
    RenderSize,   ///< the scene app's render size: the layer's status file, or the PC helper (render_size.h)
    Count,
};

/** Number of SourceId values including None and excluding Count. */
constexpr size_t kSourceCount = static_cast<size_t>(SourceId::Count);

/** How a value is typed. */
enum class WireType : uint8_t {
    F32 = 0,
    I32 = 1,
    Enum = 2,         ///< a small unsigned code, meaning documented next to the metric
    ShortString = 3,  ///< up to kShortStringMax bytes of UTF-8
};

/**
 * The interval of the metrics read on every panel update, like the fixed panel did (frame timing, CPU, GPU clock,
 * memory, temperatures, throttle, fan). It's update_interval_ms's default: the scheduler moves these sources to the
 * configured interval (Scheduler::setUpdateInterval). Sources with any other interval keep it.
 */
constexpr uint32_t kPerUpdateMs = 500;

/** Longest short string a metric may carry (bytes). */
constexpr size_t kShortStringMax = 31;

/** One registry entry. */
struct MetricDef {
    uint16_t id;          ///< stable id
    const char* name;     ///< dotted name, unique ("cpu.usage_pct")
    const char* unit;     ///< display unit ("%", "W", "" for none)
    WidgetType owner;     ///< the one widget that may show it
    SourceId source;      ///< what reads it
    uint32_t intervalMs;  ///< how often the source reads it while needed
    WireType type;
};

// Metric ids. Grouped in blocks of 100 by owner widget, only for readability.
namespace metric {
// Frame (widget 1), read from FrameTiming
constexpr uint16_t kFrameAppFps = 100;
constexpr uint16_t kFrameDisplayHz = 101;
constexpr uint16_t kFrameGpuMs = 102;
constexpr uint16_t kFrameCpuMs = 103;
constexpr uint16_t kFrameGpuMaxMs = 104;
constexpr uint16_t kFrameCpuMaxMs = 105;
constexpr uint16_t kFrameReprojectedPct = 106;
constexpr uint16_t kFrameDropped = 107;
constexpr uint16_t kFrameThrottled = 108;  ///< 0 = not throttled, 1 = half rate, 2 = third
constexpr uint16_t kFrameCount = 109;      ///< frames aggregated since the last read
// Read from RenderSize (2026-11-13): the size the game renders each eye at. N/A NotConnected when no game is
// known (no scene app, a stale file, no PC helper), NoSteamVr without OpenVR
constexpr uint16_t kFrameRenderWidth = 110;   ///< i32, pixels
constexpr uint16_t kFrameRenderHeight = 111;  ///< i32, pixels
constexpr uint16_t kFrameRenderSource = 112;  ///< enum, see RenderSource
// CPU (widget 2)
constexpr uint16_t kCpuUsagePct = 200;
constexpr uint16_t kCpuMaxCorePct = 201;
constexpr uint16_t kCpuFastestClockMhz = 202;
// 203 is retired (the big core's clock; the CPU card shows the fastest cluster). Never reuse it.
constexpr uint16_t kCpuCore0Pct = 210;  ///< 210-217: load of core 0-7 (N/A SensorMissing past the last core)
constexpr uint16_t kCpuCoreSlots = 8;
constexpr uint16_t kCpuCluster0Mhz = 220;       ///< 220-223: current clock of cluster 0-3 (cpufreq policies, ascending)
constexpr uint16_t kCpuCluster0MaxMhz = 224;    ///< 224-227: scaling_max_freq of cluster 0-3 (drops under a Playtime cap)
constexpr uint16_t kCpuCluster0HwMaxMhz = 228;  ///< 228-231: cpuinfo_max_freq of cluster 0-3 ("capped" = max < this)
constexpr uint16_t kCpuClusterSlots = 4;
// GPU & RAM (widget 3)
constexpr uint16_t kGpuBusyPct = 300;  ///< GPU load (frame GPU ms × app fps), see core/gpu_load.h
constexpr uint16_t kGpuClockMhz = 301;
constexpr uint16_t kGpuMaxClockMhz = 302;
constexpr uint16_t kRamUsedGib = 303;
constexpr uint16_t kRamTotalGib = 304;
// Power (widget 4)
constexpr uint16_t kPowerVphW = 400;
// 401 is retired (the "All ch" sum, which double-counts vph). Never reuse it.
constexpr uint16_t kPowerRailsW = 402;  ///< sum of the measured rails under vph (every channel but vph)
// Battery (widget 5)
constexpr uint16_t kBatteryPct = 500;
constexpr uint16_t kBatteryVoltageV = 501;
constexpr uint16_t kBatteryCurrentMa = 502;
constexpr uint16_t kBatteryStatus = 503;         ///< enum, see BatteryStatus
constexpr uint16_t kBatteryChargerOnline = 504;  ///< enum, 0 / 1
constexpr uint16_t kBatteryPowerW = 508;         ///< V × A of the battery, + = into it, − = out of it
/// minutes left on battery (NaN: charging or not known yet). Stored by the main loop's BatteryWatch, which reads on
/// its own while the headset is worn (the Battery source pauses with the HUD). Frame Dock's own id: upstream's PR #5
/// later gave 509 to battery.time_to_full_s, which Frame Dock doesn't carry. Never take upstream's 509 here
constexpr uint16_t kBatteryRemainMin = 509;
// Temp (widget 6)
constexpr uint16_t kTempCpuC = 600;
constexpr uint16_t kTempGpuC = 601;
constexpr uint16_t kTempDdrC = 602;
constexpr uint16_t kTempBatteryC = 603;  ///< read by the Battery source, owned by Temp
constexpr uint16_t kTempDisplayC = 604;
constexpr uint16_t kTempExhaustC = 605;
constexpr uint16_t kTempHeatsinkC = 606;
constexpr uint16_t kThrottleCpu = 607;   ///< enum, 0 / 1
constexpr uint16_t kThrottleGpu = 608;   ///< enum, 0 / 1
constexpr uint16_t kFanRpm = 609;
// CPU items (widget 7). The rows themselves aren't metrics (WidgetContext::processes); these describe the
// scan both lists come from
constexpr uint16_t kProcScanned = 700;   ///< processes read in the last scan
constexpr uint16_t kProcVanished = 701;  ///< processes that exited mid-scan (skipped)
constexpr uint16_t kProcScanMs = 702;    ///< wall time of the last scan
constexpr uint16_t kCpuItemsRows = 703;  ///< rows in the CPU list (N/A NotSampledYet on the first scan)
// RAM items (widget 8)
constexpr uint16_t kRamItemsRows = 800;
// Network (widget 9): the wireless link (the dongle if it has a peer, else home Wi-Fi)
constexpr uint16_t kNetInterfaceUp = 900;  ///< enum, 0 / 1
constexpr uint16_t kNetConnected = 901;    ///< enum, 0 / 1
constexpr uint16_t kNetHomeWifi = 902;     ///< enum, 0 = dongle, 1 = home Wi-Fi
constexpr uint16_t kNetSignalDbm = 903;
constexpr uint16_t kNetTxLinkMbps = 904;
constexpr uint16_t kNetRxLinkMbps = 905;
constexpr uint16_t kNetRxMbps = 906;
constexpr uint16_t kNetTxMbps = 907;
// General (widget 11)
constexpr uint16_t kCtrlLeftPct = 1100;       ///< N/A NotConnected when the controller is absent
constexpr uint16_t kCtrlLeftCharging = 1101;  ///< enum, 0 / 1
constexpr uint16_t kCtrlRightPct = 1102;
constexpr uint16_t kCtrlRightCharging = 1103;
constexpr uint16_t kSceneAppKey = 1104;   ///< short string, e.g. "steam.app.438100" (cut to 31 B)
constexpr uint16_t kSceneAppName = 1105;  ///< short string, e.g. "VRChat" (cut to 31 B)
constexpr uint16_t kScenePid = 1106;      ///< i32, 0 = no scene app (the compositor is showing)
// 1107-1109 are reserved
constexpr uint16_t kUptimeS = 1110;  ///< i32, since cold boot, sleep included (/proc/uptime)
constexpr uint16_t kClock = 1111;  ///< short string, local "YYYY-MM-DD HH:MM:SS"
constexpr uint16_t kSelfCpuPct = 1112;  ///< this app's own CPU, % of one core (all threads)
constexpr uint16_t kSelfRssMib = 1113;
constexpr uint16_t kSelfPowerW = 1114;  ///< estimate, shown with "≈": own share of busy CPU × CPU rails (apc0-2)
}  // namespace metric

/** Values of metric::kFrameRenderSource. */
enum class RenderSource : uint32_t { None = 0, Headset = 1 /**< a standalone game, via the layer */, Pc = 2 /**< a streamed game, via the PC helper */ };

/** Values of metric::kBatteryStatus. */
enum class BatteryStatus : uint32_t { Unknown = 0, Charging = 1, Discharging = 2, Full = 3, NotCharging = 4 };

/**
 * The registry compiled into this build.
 * @return all entries (static storage)
 */
const std::vector<MetricDef>& registry();

/**
 * Looks a metric up by id.
 * @param id metric id
 * @return the entry, or nullptr if this build doesn't know the id
 */
const MetricDef* findMetric(uint16_t id);

/**
 * The metrics a widget shows without owning them. The simple templates (SimpleA, SimpleB) show readings the other
 * widgets own, so they own none: the scheduler reads a borrowed metric's source for them (and only that source),
 * and their redraws follow those metrics. Every other widget borrows nothing.
 * @param type a widget type
 * @return metric ids (static storage; empty for most types)
 */
const std::vector<uint16_t>& borrowedMetrics(WidgetType type);

/**
 * Checks the borrowed metrics against a registry table: every id exists there and isn't the borrower's own.
 * @param defs the table
 * @return one English line per problem; empty if fine
 */
std::vector<std::string> validateBorrowed(const std::vector<MetricDef>& defs);

/**
 * Checks a registry table against the rules at the top of this file.
 * @param defs the table
 * @return one English line per problem; empty if the table is valid
 */
std::vector<std::string> validateRegistry(const std::vector<MetricDef>& defs);

/**
 * @param type a widget type
 * @return its name for logs and the debug dump ("Frame", "GPU & RAM", ...)
 */
const char* widgetName(WidgetType type);

/**
 * @param source a source id
 * @return its name for logs and the debug dump ("cpu_load", ...)
 */
const char* sourceName(SourceId source);

}  // namespace frame_perf_overlay
