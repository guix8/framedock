#include "system_metrics.h"

#include "async_sampler.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>

using frame_perf_overlay::Reason;
using frame_perf_overlay::SourceId;
namespace metric = frame_perf_overlay::metric;

namespace {

uint32_t batteryStatusCode(const std::string& status) {
    using frame_perf_overlay::BatteryStatus;
    if (status == "Charging") return static_cast<uint32_t>(BatteryStatus::Charging);
    if (status == "Discharging") return static_cast<uint32_t>(BatteryStatus::Discharging);
    if (status == "Full") return static_cast<uint32_t>(BatteryStatus::Full);
    if (status == "Not charging") return static_cast<uint32_t>(BatteryStatus::NotCharging);
    return static_cast<uint32_t>(BatteryStatus::Unknown);
}

void storePower(frame_perf_overlay::MetricStore& st, const SensorSample& s, double now) {
    bool haveVph = false;
    for (const PowerChannel& ch : s.powerChannels) haveVph = haveVph || ch.label == "vph";
    st.setF32(metric::kPowerVphW, s.powerMainW, now, haveVph ? Reason::ReadFailed : Reason::SensorMissing);
    // Never vph + rails: the rails are fed from vph, so that sum double-counts
    const bool haveRails = s.powerChannels.size() > (haveVph ? 1u : 0u);
    st.setF32(metric::kPowerRailsW, s.powerRailsW, now, haveRails ? Reason::ReadFailed : Reason::SensorMissing);
}

void storeBattery(frame_perf_overlay::MetricStore& st, const SensorSample& s, double now) {
    const Reason bat = s.haveBattery ? Reason::ReadFailed : Reason::SensorMissing;
    st.setF32(metric::kBatteryPct, s.batteryPct, now, bat);
    st.setF32(metric::kBatteryVoltageV, s.batteryVoltageV, now, bat);
    st.setF32(metric::kBatteryCurrentMa, s.batteryCurrentMa, now, bat);
    st.setF32(metric::kTempBatteryC, s.batteryTempC, now, bat);
    if (s.haveBattery) {
        st.setEnum(metric::kBatteryStatus, batteryStatusCode(s.batteryStatus), now);
    } else {
        st.setNa(metric::kBatteryStatus, Reason::SensorMissing, now);
    }
    if (s.haveCharger) {
        st.setEnum(metric::kBatteryChargerOnline, s.chargerOnline ? 1 : 0, now);
    } else {
        st.setNa(metric::kBatteryChargerOnline, Reason::SensorMissing, now);
    }
    // Driver sign: positive = into the battery (checked while charging)
    st.setF32(metric::kBatteryPowerW, s.batteryVoltageV * s.batteryCurrentMa / 1000.0, now, bat);
}

void storeWifi(frame_perf_overlay::MetricStore& st, const WifiInfo& w, bool resumed, double now) {
    st.setEnum(metric::kNetInterfaceUp, w.interfaceUp ? 1 : 0, now);
    st.setEnum(metric::kNetConnected, w.connected ? 1 : 0, now);
    st.setEnum(metric::kNetHomeWifi, w.homeWifi ? 1 : 0, now);
    const Reason missing = w.connected ? Reason::ReadFailed : Reason::NotConnected;
    st.setF32(metric::kNetSignalDbm, w.signalDbm, now, missing);
    st.setF32(metric::kNetTxLinkMbps, w.txLinkMbps, now, missing);
    st.setF32(metric::kNetRxLinkMbps, w.rxLinkMbps, now, missing);
    // The rates are a delta: NaN on the first read after a (re)start
    const Reason noRate = !w.connected ? Reason::NotConnected : (resumed ? Reason::NotSampledYet : Reason::ReadFailed);
    st.setF32(metric::kNetRxMbps, w.rxMbps, now, noRate);
    st.setF32(metric::kNetTxMbps, w.txMbps, now, noRate);
}

}  // namespace

void registerSystemSources(frame_perf_overlay::Scheduler& scheduler, Sensors& sensors,
                           frame_perf_overlay::MetricStore& store, AsyncSampler* sampler) {
    frame_perf_overlay::MetricStore& st = store;
    Sensors& se = sensors;
    /** A source read by read() and stored by apply(): in place, or (sampler) read on its thread, stored later */
    const auto slow = [&](SourceId id, AsyncSampler::Read read, AsyncSampler::Apply apply) {
        if (sampler == nullptr) {
            scheduler.addSource(id, [read, apply](double now, bool) {
                SensorSample s;
                read(s);
                apply(s, now);
            });
            return;
        }
        const int key = sampler->add(std::move(read), std::move(apply));
        scheduler.addSource(id, [sampler, key](double, bool) { sampler->request(key); });
    };

    scheduler.addSource(SourceId::CpuLoad, [&st, &se](double now, bool resumed) {
        // A delta: after a pause, rebuild the baseline instead of showing the average over the pause
        if (resumed) se.resetCpuBaseline();
        SensorSample s;
        se.readCpuUsage(s);
        const Reason nan = resumed ? Reason::NotSampledYet : Reason::ReadFailed;
        st.setF32(metric::kCpuUsagePct, s.cpuUsagePct, now, nan);
        st.setF32(metric::kCpuMaxCorePct, s.cpuMaxCorePct, now, nan);
        for (uint16_t i = 0; i < metric::kCpuCoreSlots; ++i) {
            const uint16_t id = metric::kCpuCore0Pct + i;
            if (i < s.corePct.size()) {
                st.setF32(id, s.corePct[i], now, nan);
            } else {
                // After the first delta, a slot past the last core is a core this SoC doesn't have
                st.setNa(id, s.corePct.empty() ? nan : Reason::SensorMissing, now);
            }
        }
    });
    scheduler.addSource(SourceId::CpuClock, [&st, &se](double now, bool) {
        SensorSample s;
        se.readCpuClocks(s);
        st.setF32(metric::kCpuFastestClockMhz, s.cpuFreqMHz, now);
        for (uint16_t i = 0; i < metric::kCpuClusterSlots; ++i) {
            if (i < s.clusters.size()) {
                st.setF32(metric::kCpuCluster0Mhz + i, s.clusters[i].curMHz, now);
                st.setF32(metric::kCpuCluster0MaxMhz + i, s.clusters[i].maxMHz, now);
                st.setF32(metric::kCpuCluster0HwMaxMhz + i, s.clusters[i].hwMaxMHz, now, Reason::SensorMissing);
            } else {
                st.setNa(metric::kCpuCluster0Mhz + i, Reason::SensorMissing, now);
                st.setNa(metric::kCpuCluster0MaxMhz + i, Reason::SensorMissing, now);
                st.setNa(metric::kCpuCluster0HwMaxMhz + i, Reason::SensorMissing, now);
            }
        }
    });
    scheduler.addSource(SourceId::GpuClock, [&st, &se](double now, bool) {
        SensorSample s;
        se.readGpuClock(s);
        st.setF32(metric::kGpuClockMhz, s.gpuFreqMHz, now);
        st.setF32(metric::kGpuMaxClockMhz, s.gpuMaxFreqMHz, now);
    });
    scheduler.addSource(SourceId::Ram, [&st, &se](double now, bool) {
        SensorSample s;
        se.readMemory(s);
        st.setF32(metric::kRamUsedGib, s.memUsedGiB, now);
        st.setF32(metric::kRamTotalGib, s.memTotalGiB, now);
    });
    scheduler.addSource(SourceId::Temps, [&st, &se](double now, bool) {
        SensorSample s;
        se.readThermal(s);
        st.setF32(metric::kTempCpuC, s.cpuTempC, now);
        st.setF32(metric::kTempGpuC, s.gpuTempC, now);
        st.setF32(metric::kTempDdrC, s.ddrTempC, now);
    });
    slow(SourceId::BoardTemps, [&se](SensorSample& s) { se.readIioTemps(s); },
         [&st](const SensorSample& s, double now) {
             st.setF32(metric::kTempDisplayC, s.displayTempC, now);
             st.setF32(metric::kTempExhaustC, s.exhaustTempC, now);
             st.setF32(metric::kTempHeatsinkC, s.heatsinkTempC, now);
         });
    scheduler.addSource(SourceId::Throttle, [&st, &se](double now, bool) {
        SensorSample s;
        se.readThrottle(s);
        st.setEnum(metric::kThrottleCpu, s.throttleCpu ? 1 : 0, now);
        st.setEnum(metric::kThrottleGpu, s.throttleGpu ? 1 : 0, now);
    });
    scheduler.addSource(SourceId::Fan, [&st, &se](double now, bool) {
        SensorSample s;
        se.readFan(s);
        st.setF32(metric::kFanRpm, s.fanRpm, now);
    });
    slow(SourceId::Power, [&se](SensorSample& s) { se.readPower(s); },
         [&st](const SensorSample& s, double now) { storePower(st, s, now); });
    slow(SourceId::Battery, [&se](SensorSample& s) { se.readBattery(s); },
         [&st](const SensorSample& s, double now) { storeBattery(st, s, now); });
    scheduler.addSource(SourceId::Network, [&st, &se](double now, bool resumed) {
        // Byte-counter rates: after a pause they'd be the average over the pause
        if (resumed) se.resetWifiCounters();
        SensorSample s;
        se.readWifi(s, now);
        storeWifi(st, s.wifi, resumed, now);
    });
    scheduler.addSource(SourceId::Uptime, [&st, &se](double now, bool) {
        SensorSample s;
        se.readUptime(s);
        if (std::isnan(s.uptimeS)) {
            st.setNa(metric::kUptimeS, Reason::ReadFailed, now);
        } else {
            st.setI32(metric::kUptimeS, static_cast<int32_t>(s.uptimeS), now);
        }
    });
    {
        // This app's own cost. It reads the CPU rails (the power monitor, which can block for ms), so it goes on the
        // sampler's thread like Power; its baseline lives there too, and a resumed read asks that thread to start
        // over. A NaN after a fresh baseline is "not sampled yet"
        const auto restart = std::make_shared<std::atomic<bool>>(false);
        AsyncSampler::Read read = [&se, restart](SensorSample& s) {
            if (restart->exchange(false)) se.resetSelfBaseline();
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            se.readSelfCost(s, t);
        };
        AsyncSampler::Apply apply = [&st](const SensorSample& s, double now) {
            const Reason first = std::isnan(s.selfCpuPct) ? Reason::NotSampledYet : Reason::ReadFailed;
            st.setF32(metric::kSelfCpuPct, s.selfCpuPct, now, Reason::NotSampledYet);
            st.setF32(metric::kSelfRssMib, s.selfRssMiB, now);
            st.setF32(metric::kSelfPowerW, s.selfPowerW, now, first);
        };
        if (sampler == nullptr) {
            scheduler.addSource(SourceId::SelfCost, [read, apply, restart](double now, bool resumed) {
                if (resumed) restart->store(true);
                SensorSample s;
                read(s);
                apply(s, now);
            });
        } else {
            const int key = sampler->add(std::move(read), std::move(apply));
            scheduler.addSource(SourceId::SelfCost, [sampler, key, restart](double, bool resumed) {
                if (resumed) restart->store(true);
                sampler->request(key);
            });
        }
    }
    scheduler.addSource(SourceId::Clock, [&st](double now, bool) {
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        char text[32];
        if (localtime_r(&t, &tm) != nullptr && std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm) > 0) {
            st.setText(metric::kClock, text, now);
        } else {
            st.setNa(metric::kClock, Reason::ReadFailed, now);
        }
    });
}
