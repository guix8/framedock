#include "metric_registry.h"

#include <algorithm>
#include <map>
#include <set>

namespace frame_perf_overlay {

namespace {

using W = WidgetType;
using S = SourceId;
using T = WireType;

// Read every panel update (update_interval_ms, see kPerUpdateMs): frame timing, CPU, GPU clock, memory, temperatures,
// throttle, fan. The rest keep the fixed panel's slower intervals
constexpr uint32_t kTick = kPerUpdateMs;
constexpr uint32_t kCpuMs = kPerUpdateMs;      // /proc/stat, cpufreq
constexpr uint32_t kGpuMs = kPerUpdateMs;      // devfreq clock
constexpr uint32_t kRamMs = kPerUpdateMs;
constexpr uint32_t kThermalMs = kPerUpdateMs;  // thermal zones, throttle, fan
constexpr uint32_t kBoardMs = 5000;    // vadc board temps (an ADC conversion takes a few ms each)
constexpr uint32_t kPowerMs = 2000;    // max34417 updates every 1.5-2 s
constexpr uint32_t kChargeMs = 5000;   // battery, charger
constexpr uint32_t kClockMs = 1000;
constexpr uint32_t kUptimeMs = 1000;
constexpr uint32_t kSelfMs = 2000;
constexpr uint32_t kSceneAppMs = 5000;  // a safety net: the scene-app event updates it at once
constexpr uint32_t kProcessMs = 2000;   // one /proc scan costs 5-8 ms on the Frame
constexpr uint32_t kRenderSizeMs = 2000;  // a file, or one LAN request (on the sampler's thread)

const std::vector<MetricDef> kRegistry = {
    // Frame
    {metric::kFrameAppFps, "frame.app_fps", "fps", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameDisplayHz, "frame.display_hz", "Hz", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameGpuMs, "frame.gpu_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameCpuMs, "frame.cpu_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameGpuMaxMs, "frame.gpu_max_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameCpuMaxMs, "frame.cpu_max_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameReprojectedPct, "frame.reprojected_pct", "%", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameDropped, "frame.dropped", "", W::Frame, S::FrameTiming, kTick, T::I32},
    {metric::kFrameThrottled, "frame.throttled", "", W::Frame, S::FrameTiming, kTick, T::Enum},
    {metric::kFrameCount, "frame.frames", "", W::Frame, S::FrameTiming, kTick, T::I32},
    {metric::kFrameRenderWidth, "frame.render_width", "px", W::Frame, S::RenderSize, kRenderSizeMs, T::I32},
    {metric::kFrameRenderHeight, "frame.render_height", "px", W::Frame, S::RenderSize, kRenderSizeMs, T::I32},
    {metric::kFrameRenderSource, "frame.render_source", "", W::Frame, S::RenderSize, kRenderSizeMs, T::Enum},
    // CPU
    {metric::kCpuUsagePct, "cpu.usage_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuMaxCorePct, "cpu.max_core_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuFastestClockMhz, "cpu.fastest_clock_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // Compact off: per core and per cluster
    {metric::kCpuCore0Pct + 0, "cpu.core0_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 1, "cpu.core1_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 2, "cpu.core2_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 3, "cpu.core3_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 4, "cpu.core4_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 5, "cpu.core5_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 6, "cpu.core6_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 7, "cpu.core7_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 0, "cpu.cluster0_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 1, "cpu.cluster1_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 2, "cpu.cluster2_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 3, "cpu.cluster3_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 0, "cpu.cluster0_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 1, "cpu.cluster1_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 2, "cpu.cluster2_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 3, "cpu.cluster3_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // The hardware max per cluster (read once at start; "capped" when scaling_max_freq is below it)
    {metric::kCpuCluster0HwMaxMhz + 0, "cpu.cluster0_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 1, "cpu.cluster1_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 2, "cpu.cluster2_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 3, "cpu.cluster3_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // GPU & RAM
    // GPU load = frame GPU ms × app fps, written by the FrameTiming reader (core/gpu_load.h). Its source is
    // FrameTiming, so a GPU & RAM widget alone keeps frame timing read
    {metric::kGpuBusyPct, "gpu.busy_pct", "%", W::GpuRam, S::FrameTiming, kTick, T::F32},
    {metric::kGpuClockMhz, "gpu.clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kGpuMaxClockMhz, "gpu.max_clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kRamUsedGib, "ram.used_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    {metric::kRamTotalGib, "ram.total_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    // Power. 401 (power.all_channels_w) is retired
    {metric::kPowerVphW, "power.vph_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerRailsW, "power.rails_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    // Battery
    {metric::kBatteryPct, "battery.pct", "%", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryVoltageV, "battery.voltage_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryCurrentMa, "battery.current_ma", "mA", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryStatus, "battery.status", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryChargerOnline, "battery.charger_online", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryPowerW, "battery.power_w", "W", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryRemainMin, "battery.remaining_min", "min", W::Battery, S::Battery, kChargeMs, T::F32},
    // Temp
    {metric::kTempCpuC, "temp.cpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempGpuC, "temp.gpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempDdrC, "temp.ddr_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempBatteryC, "temp.battery_c", "°C", W::Temp, S::Battery, kChargeMs, T::F32},
    {metric::kTempDisplayC, "temp.display_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kTempExhaustC, "temp.exhaust_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kTempHeatsinkC, "temp.heatsink_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kThrottleCpu, "temp.throttle_cpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kThrottleGpu, "temp.throttle_gpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kFanRpm, "temp.fan_rpm", "rpm", W::Temp, S::Fan, kThermalMs, T::F32},
    // CPU / RAM items: one /proc scan feeds both lists
    {metric::kProcScanned, "cpu_items.scanned", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kProcVanished, "cpu_items.vanished", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kProcScanMs, "cpu_items.scan_ms", "ms", W::CpuItems, S::Processes, kProcessMs, T::F32},
    {metric::kCpuItemsRows, "cpu_items.rows", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kRamItemsRows, "ram_items.rows", "", W::RamItems, S::Processes, kProcessMs, T::I32},
    // Network: nl80211 and the byte counters every 2 s (like the fixed panel)
    {metric::kNetInterfaceUp, "net.interface_up", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetConnected, "net.connected", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeWifi, "net.home_wifi", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetSignalDbm, "net.signal_dbm", "dBm", W::Network, S::Network, 2000, T::F32},
    {metric::kNetTxLinkMbps, "net.tx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetRxLinkMbps, "net.rx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetRxMbps, "net.rx_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetTxMbps, "net.tx_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    // General
    {metric::kCtrlLeftPct, "general.controller_left_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlLeftCharging, "general.controller_left_charging", "", W::General, S::Controllers, 10000, T::Enum},
    {metric::kCtrlRightPct, "general.controller_right_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlRightCharging, "general.controller_right_charging", "", W::General, S::Controllers, 10000,
     T::Enum},
    {metric::kClock, "general.clock", "", W::General, S::Clock, kClockMs, T::ShortString},
    // Compact off: the app in front, uptime, this app's own cost
    {metric::kSceneAppKey, "general.app_key", "", W::General, S::SceneApp, kSceneAppMs, T::ShortString},
    {metric::kSceneAppName, "general.app_name", "", W::General, S::SceneApp, kSceneAppMs, T::ShortString},
    {metric::kScenePid, "general.scene_pid", "", W::General, S::SceneApp, kSceneAppMs, T::I32},
    {metric::kUptimeS, "general.uptime_s", "s", W::General, S::Uptime, kUptimeMs, T::I32},
    {metric::kSelfCpuPct, "general.self_cpu_pct", "%", W::General, S::SelfCost, kSelfMs, T::F32},
    {metric::kSelfRssMib, "general.self_rss_mib", "MiB", W::General, S::SelfCost, kSelfMs, T::F32},
    {metric::kSelfPowerW, "general.self_power_est_w", "W", W::General, S::SelfCost, kSelfMs, T::F32},
};

}  // namespace

const std::vector<MetricDef>& registry() {
    return kRegistry;
}

const std::vector<uint16_t>& borrowedMetrics(WidgetType type) {
    using namespace metric;
    // Simple A: app fps / display Hz, reprojection, throttle; CPU % (total shown, coloured by the busiest core); GPU
    // load, RAM; power; battery %, its state and the time left; the time of day. Their sources: FrameTiming, CpuLoad, Ram, Power,
    // Battery, Clock. No CPU / GPU clocks, temperatures, network or controllers
    static const std::vector<uint16_t> kSimpleA = {
        kFrameAppFps, kFrameDisplayHz, kFrameReprojectedPct, kFrameThrottled, kCpuUsagePct,   kCpuMaxCorePct,
        kGpuBusyPct,  kRamUsedGib,     kRamTotalGib,         kPowerVphW,      kBatteryPct,    kBatteryStatus,
        kBatteryChargerOnline, kBatteryRemainMin, kClock,
    };
    // Simple B: app fps / display Hz, battery % and its state, the time of day (FrameTiming, Battery and Clock)
    static const std::vector<uint16_t> kSimpleB = {kFrameAppFps, kFrameDisplayHz, kBatteryPct, kBatteryStatus,
                                                   kBatteryChargerOnline, kBatteryRemainMin, kClock};
    static const std::vector<uint16_t> kNone;
    switch (type) {
        case WidgetType::SimpleA: return kSimpleA;
        case WidgetType::SimpleB: return kSimpleB;
        default: return kNone;
    }
}

std::vector<std::string> validateBorrowed(const std::vector<MetricDef>& defs) {
    std::vector<std::string> problems;
    for (size_t t = 1; t < kWidgetTypeCount; ++t) {
        const WidgetType type = static_cast<WidgetType>(t);
        for (const uint16_t id : borrowedMetrics(type)) {
            const auto def = std::find_if(defs.begin(), defs.end(), [id](const MetricDef& d) { return d.id == id; });
            const std::string label = std::string(widgetName(type)) + " borrows metric " + std::to_string(id);
            if (def == defs.end()) {
                problems.push_back(label + ", which isn't in the registry");
            } else if (def->owner == type) {
                problems.push_back(label + ", which it owns");
            }
        }
    }
    return problems;
}

const MetricDef* findMetric(uint16_t id) {
    // Built once: id -> index. The registry is small, but this is called per value set.
    static const std::map<uint16_t, size_t> index = [] {
        std::map<uint16_t, size_t> m;
        for (size_t i = 0; i < kRegistry.size(); ++i) m.emplace(kRegistry[i].id, i);
        return m;
    }();
    const auto it = index.find(id);
    return it == index.end() ? nullptr : &kRegistry[it->second];
}

std::vector<std::string> validateRegistry(const std::vector<MetricDef>& defs) {
    std::vector<std::string> problems;
    std::set<uint16_t> ids;
    std::map<std::string, WidgetType> owners;  // metric name -> first owner seen
    for (const MetricDef& d : defs) {
        const std::string name = d.name != nullptr ? d.name : "";
        const std::string label = "metric " + std::to_string(d.id) + " (" + name + ")";
        if (!ids.insert(d.id).second) problems.push_back(label + ": id is used more than once");
        if (name.empty()) problems.push_back(label + ": has no name");
        if (d.owner == WidgetType::None || static_cast<size_t>(d.owner) >= kWidgetTypeCount) {
            problems.push_back(label + ": has no owner widget");
        }
        if (!name.empty()) {
            const auto it = owners.find(name);
            if (it == owners.end()) {
                owners.emplace(name, d.owner);
            } else if (it->second != d.owner) {
                problems.push_back(label + ": has two owners (" + widgetName(it->second) + " and " +
                                   widgetName(d.owner) + ")");
            } else {
                problems.push_back(label + ": name is registered more than once");
            }
        }
        if (d.source == SourceId::None || static_cast<size_t>(d.source) >= kSourceCount) {
            problems.push_back(label + ": has no source");
        }
        if (d.intervalMs == 0) problems.push_back(label + ": has no read interval");
        if (d.unit == nullptr) problems.push_back(label + ": unit is null (use \"\" for none)");
    }
    return problems;
}

const char* widgetName(WidgetType type) {
    switch (type) {
        case WidgetType::None: return "none";
        case WidgetType::Frame: return "Frame";
        case WidgetType::Cpu: return "CPU";
        case WidgetType::GpuRam: return "GPU & RAM";
        case WidgetType::Power: return "Power";
        case WidgetType::Battery: return "Battery";
        case WidgetType::Temp: return "Temp";
        case WidgetType::CpuItems: return "CPU items";
        case WidgetType::RamItems: return "RAM items";
        case WidgetType::Network: return "Network";
        case WidgetType::General: return "General";
        case WidgetType::SimpleA: return "Simple A";
        case WidgetType::SimpleB: return "Simple B";
        case WidgetType::ColorPresets: return "Color presets";
        case WidgetType::Volume: return "Volume";
        case WidgetType::Passthrough: return "Passthrough";
        case WidgetType::Profiles: return "Profiles";
        case WidgetType::Browser: return "Browser";
        case WidgetType::Record: return "Record";
    }
    return "unknown";
}

const char* sourceName(SourceId source) {
    switch (source) {
        case SourceId::None: return "none";
        case SourceId::CpuLoad: return "cpu_load";
        case SourceId::CpuClock: return "cpu_clock";
        case SourceId::GpuClock: return "gpu_clock";
        case SourceId::Ram: return "ram";
        case SourceId::Temps: return "temps";
        case SourceId::BoardTemps: return "board_temps";
        case SourceId::Throttle: return "throttle";
        case SourceId::Fan: return "fan";
        case SourceId::Power: return "power";
        case SourceId::Battery: return "battery";
        case SourceId::Network: return "network";
        case SourceId::FrameTiming: return "frame_timing";
        case SourceId::Controllers: return "controllers";
        case SourceId::Clock: return "clock";
        case SourceId::Processes: return "processes";
        case SourceId::SceneApp: return "scene_app";
        case SourceId::Uptime: return "uptime";
        case SourceId::SelfCost: return "self_cost";
        case SourceId::RenderSize: return "render_size";
        case SourceId::Count: break;
    }
    return "unknown";
}

}  // namespace frame_perf_overlay
