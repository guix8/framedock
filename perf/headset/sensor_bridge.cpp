#include "sensor_bridge.h"

#include "async_sampler.h"
#include "render_size.h"
#include "system_metrics.h"
#include "vr_overlay.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

using frame_perf_overlay::Reason;
using frame_perf_overlay::SourceId;
using frame_perf_overlay::WidgetType;
namespace metric = frame_perf_overlay::metric;

namespace {

std::string batteryStatusString(int64_t code) {
    using frame_perf_overlay::BatteryStatus;
    switch (static_cast<BatteryStatus>(code)) {
        case BatteryStatus::Charging: return "Charging";
        case BatteryStatus::Discharging: return "Discharging";
        case BatteryStatus::Full: return "Full";
        case BatteryStatus::NotCharging: return "Not charging";
        case BatteryStatus::Unknown: break;
    }
    return "";
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : fallback;
}

}  // namespace

SensorBridge::SensorBridge(Sensors& sensors, VrOverlay* vr, frame_perf_overlay::MetricStore& store)
    : sensors_(sensors), vr_(vr), store_(store) {}

SensorBridge::~SensorBridge() = default;

std::vector<WidgetType> SensorBridge::allWidgets() {
    return {WidgetType::Frame,   WidgetType::Cpu,  WidgetType::GpuRam,   WidgetType::Power,
            WidgetType::Battery, WidgetType::Temp, WidgetType::CpuItems, WidgetType::RamItems,
            WidgetType::Network, WidgetType::General};
}

void SensorBridge::registerSources(frame_perf_overlay::Scheduler& scheduler, AsyncSampler* sampler) {
    registerSystemSources(scheduler, sensors_, store_, sampler);
    scheduler.addSource(SourceId::FrameTiming, [this](double now, bool) { readFrameTiming(now); });
    scheduler.addSource(SourceId::Controllers, [this](double now, bool) { readControllers(now); });
    scheduler.addSource(SourceId::Processes, [this](double now, bool resumed) { readProcesses(now, resumed); });
    scheduler.addSource(SourceId::SceneApp, [this](double now, bool) { refreshSceneApp(now); });
    registerRenderSize(scheduler, sampler);
}

void SensorBridge::registerRenderSize(frame_perf_overlay::Scheduler& scheduler, AsyncSampler* sampler) {
    // 2026-11-13: OpenVR is asked on the main thread (the scene app's pid, for the layer's staleness rule); the file
    // and the LAN request to the PC helper (up to 400 ms per address) on the sampler's thread
    renderSize_ = std::make_unique<render_size::RenderSizeReader>(
        envOr("HOME", ""), envOr("XDG_RUNTIME_DIR", "/run/user/" + std::to_string(getuid())));
    render_size::RenderSizeReader* reader = renderSize_.get();
    std::atomic<int>* scenePid = &renderScenePid_;
    AsyncSampler::Read read = [reader, scenePid](SensorSample& s) {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const render_size::Reading r = reader->read(now, scenePid->load());
        s.renderWidth = r.width;
        s.renderHeight = r.height;
        s.renderSource = r.source;
    };
    AsyncSampler::Apply apply = [this](const SensorSample& s, double now) { storeRenderSize(s, now); };
    const auto updateScene = [this] {
        SceneApp app;
        const bool haveVr = vr_ != nullptr && vr_->readSceneApp(app);
        renderNoVr_ = !haveVr;
        renderScenePid_.store(haveVr && !app.key.empty() ? static_cast<int>(app.pid) : 0);
    };
    if (sampler == nullptr) {
        scheduler.addSource(SourceId::RenderSize, [updateScene, read, apply](double now, bool) {
            updateScene();
            SensorSample s;
            read(s);
            apply(s, now);
        });
        return;
    }
    const int key = sampler->add(std::move(read), std::move(apply));
    scheduler.addSource(SourceId::RenderSize, [updateScene, sampler, key](double, bool) {
        updateScene();
        sampler->request(key);
    });
}

void SensorBridge::storeRenderSize(const SensorSample& s, double now) {
    if (s.renderSource == 0) {
        // Nothing known: no game, a previous game's size, no PC helper. Without OpenVR (and no stream): NoSteamVr
        const Reason why = renderNoVr_ ? Reason::NoSteamVr : Reason::NotConnected;
        for (uint16_t id : {metric::kFrameRenderWidth, metric::kFrameRenderHeight, metric::kFrameRenderSource}) {
            store_.setNa(id, why, now);
        }
        return;
    }
    store_.setI32(metric::kFrameRenderWidth, s.renderWidth, now);
    store_.setI32(metric::kFrameRenderHeight, s.renderHeight, now);
    store_.setEnum(metric::kFrameRenderSource, s.renderSource, now);
}

void SensorBridge::setProcessOptions(const frame_perf_overlay::Layout& layout) {
    ProcessListOptions options;
    options.cpuMax = 0;
    options.ramMax = 0;
    bool ramSum = false;
    for (const frame_perf_overlay::LayoutColumn& column : layout.columns) {
        for (const frame_perf_overlay::WidgetInstance& w : column.widgets) {
            if (w.type == WidgetType::CpuItems) {
                options.cpuMax = std::max<size_t>(options.cpuMax, w.maxItems);
                options.sumSameName = options.sumSameName || w.sumSameName;
                options.cpuHideSystem = options.cpuHideSystem || w.hideSystem;
            } else if (w.type == WidgetType::RamItems) {
                options.ramMax = std::max<size_t>(options.ramMax, w.maxItems);
                ramSum = ramSum || w.sumSameName;
                options.ramHideSystem = options.ramHideSystem || w.hideSystem;
            }
        }
    }
    options.ramSumSameName = ramSum ? 1 : 0;
    processOptions_ = options;
}

void SensorBridge::readProcesses(double now, bool resumed) {
    // CPU % is a delta: after a pause, start a fresh one rather than average over the pause
    if (resumed) processes_.reset();
    const ProcessLists& l = processes_.scan(now, processOptions_);
    const auto convert = [](const std::vector<ProcessRow>& rows) {
        std::vector<frame_perf_overlay::ProcessItem> out;
        out.reserve(rows.size());
        for (const ProcessRow& r : rows) {
            frame_perf_overlay::ProcessItem item;
            item.name = r.name;
            item.pid = r.pid;
            item.count = r.count;
            item.cpuCorePct = r.cpuCorePct;
            item.rssMiB = r.rssMiB;
            out.push_back(std::move(item));
        }
        return out;
    };
    processView_.byCpu = convert(l.byCpu);
    processView_.byRam = convert(l.byRam);
    processView_.cpuValid = l.cpuValid;
    processView_.hideRulesError = l.hideRulesError;
    ++processView_.generation;
    if (l.scanned == 0) {
        for (uint16_t id : {metric::kProcScanned, metric::kProcVanished, metric::kProcScanMs, metric::kCpuItemsRows,
                            metric::kRamItemsRows}) {
            store_.setNa(id, Reason::ReadFailed, now);
        }
        return;
    }
    store_.setI32(metric::kProcScanned, l.scanned, now);
    store_.setI32(metric::kProcVanished, l.vanished, now);
    store_.setF32(metric::kProcScanMs, l.scanMs, now);
    if (l.cpuValid) {
        store_.setI32(metric::kCpuItemsRows, static_cast<int32_t>(l.byCpu.size()), now);
    } else {
        store_.setNa(metric::kCpuItemsRows, Reason::NotSampledYet, now);
    }
    store_.setI32(metric::kRamItemsRows, static_cast<int32_t>(l.byRam.size()), now);
}

void SensorBridge::refreshSceneApp(double now) {
    SceneApp app;
    if (vr_ == nullptr || !vr_->readSceneApp(app)) {
        for (uint16_t id : {metric::kSceneAppKey, metric::kSceneAppName, metric::kScenePid}) {
            store_.setNa(id, Reason::NoSteamVr, now);
        }
        return;
    }
    store_.setI32(metric::kScenePid, static_cast<int32_t>(app.pid), now);
    if (app.key.empty()) {  // the compositor (SteamVR Home) is showing
        store_.setNa(metric::kSceneAppKey, Reason::NotConnected, now);
        store_.setNa(metric::kSceneAppName, Reason::NotConnected, now);
        return;
    }
    store_.setText(metric::kSceneAppKey, app.key, now);
    if (app.name.empty()) {
        store_.setNa(metric::kSceneAppName, Reason::NotConnected, now);
    } else {
        store_.setText(metric::kSceneAppName, app.name, now);
    }
}

void SensorBridge::readFrameTiming(double now) {
    static constexpr uint16_t kIds[] = {
        metric::kFrameAppFps,   metric::kFrameDisplayHz,      metric::kFrameGpuMs,   metric::kFrameCpuMs,
        metric::kFrameGpuMaxMs, metric::kFrameCpuMaxMs,       metric::kFrameReprojectedPct,
        metric::kFrameDropped,  metric::kFrameThrottled,      metric::kFrameCount,   metric::kGpuBusyPct,
    };
    // Batched: VrOverlay::readFrameStats aggregates every frame since the previous call
    const FrameStats f = vr_ != nullptr ? vr_->readFrameStats() : FrameStats();
    if (!f.valid) {
        for (uint16_t id : kIds) store_.setNa(id, vr_ != nullptr ? Reason::NoFrames : Reason::NoSteamVr, now);
        return;
    }
    store_.setF32(metric::kFrameAppFps, f.appFps, now, Reason::NoFrames);
    store_.setF32(metric::kFrameDisplayHz, f.displayHz > 0 ? f.displayHz : kNoValue, now);
    store_.setF32(metric::kFrameReprojectedPct, f.reprojectedPct, now);
    store_.setI32(metric::kFrameDropped, static_cast<int32_t>(f.dropped), now);
    store_.setEnum(metric::kFrameThrottled, static_cast<uint32_t>(f.throttledFrames), now);
    store_.setI32(metric::kFrameCount, static_cast<int32_t>(f.frames), now);
    // No new frame in this batch = no times (FrameStats::haveTimes)
    const Reason noTimes = Reason::NoFrames;
    store_.setF32(metric::kFrameGpuMs, f.haveTimes ? f.gpuMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameCpuMs, f.haveTimes ? f.cpuMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameGpuMaxMs, f.haveTimes ? f.gpuMaxMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameCpuMaxMs, f.haveTimes ? f.cpuMaxMs : kNoValue, now, noTimes);
    store_.setF32(metric::kGpuBusyPct, f.haveTimes ? f.gpuBusyPct : kNoValue, now, noTimes);
}

void SensorBridge::readControllers(double now) {
    const ControllerStatus c = vr_ != nullptr ? vr_->readControllers() : ControllerStatus();
    const Reason absent = vr_ != nullptr ? Reason::NotConnected : Reason::NoSteamVr;
    const auto put = [&](const ControllerBattery& b, uint16_t pctId, uint16_t chargingId) {
        if (!b.present) {
            store_.setNa(pctId, absent, now);
            store_.setNa(chargingId, absent, now);
            return;
        }
        store_.setF32(pctId, b.pct, now);
        store_.setEnum(chargingId, b.charging ? 1 : 0, now);
    };
    put(c.left, metric::kCtrlLeftPct, metric::kCtrlLeftCharging);
    put(c.right, metric::kCtrlRightPct, metric::kCtrlRightCharging);
}

SensorSample SensorBridge::sample() const {
    const frame_perf_overlay::MetricStore& st = store_;
    const auto f = [&st](uint16_t id) { return st.f32Or(id, kNoValue); };
    const auto flag = [&st](uint16_t id) { return st.intOr(id, 0) != 0; };
    SensorSample s;
    s.cpuUsagePct = f(metric::kCpuUsagePct);
    s.cpuMaxCorePct = f(metric::kCpuMaxCorePct);
    s.cpuFreqMHz = f(metric::kCpuFastestClockMhz);
    s.gpuFreqMHz = f(metric::kGpuClockMhz);
    s.gpuMaxFreqMHz = f(metric::kGpuMaxClockMhz);
    s.cpuTempC = f(metric::kTempCpuC);
    s.gpuTempC = f(metric::kTempGpuC);
    s.ddrTempC = f(metric::kTempDdrC);
    s.batteryTempC = f(metric::kTempBatteryC);
    s.displayTempC = f(metric::kTempDisplayC);
    s.exhaustTempC = f(metric::kTempExhaustC);
    s.heatsinkTempC = f(metric::kTempHeatsinkC);
    s.throttleCpu = flag(metric::kThrottleCpu);
    s.throttleGpu = flag(metric::kThrottleGpu);
    s.wifi.interfaceUp = flag(metric::kNetInterfaceUp);
    s.wifi.connected = flag(metric::kNetConnected);
    s.wifi.homeWifi = flag(metric::kNetHomeWifi);
    s.wifi.signalDbm = f(metric::kNetSignalDbm);
    s.wifi.txLinkMbps = f(metric::kNetTxLinkMbps);
    s.wifi.rxLinkMbps = f(metric::kNetRxLinkMbps);
    s.wifi.rxMbps = f(metric::kNetRxMbps);
    s.wifi.txMbps = f(metric::kNetTxMbps);
    s.powerMainW = f(metric::kPowerVphW);
    s.powerRailsW = f(metric::kPowerRailsW);
    s.batteryPct = f(metric::kBatteryPct);
    s.batteryVoltageV = f(metric::kBatteryVoltageV);
    s.batteryCurrentMa = f(metric::kBatteryCurrentMa);
    s.batteryStatus = batteryStatusString(st.intOr(metric::kBatteryStatus, 0));
    s.chargerOnline = flag(metric::kBatteryChargerOnline);
    s.fanRpm = f(metric::kFanRpm);
    s.memUsedGiB = f(metric::kRamUsedGib);
    s.memTotalGiB = f(metric::kRamTotalGib);
    return s;
}
