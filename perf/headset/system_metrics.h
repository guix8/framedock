// The system sensor sources: CPU, GPU & RAM, Power, Battery, Temp, Network, and General's clock. Each scheduler
// source calls one Sensors reader and stores its metrics, with an N/A reason for anything that can't be read.
// SensorBridge registers these next to its own (OpenVR) sources.
#pragma once

#include "metric_store.h"
#include "scheduler.h"
#include "sensors.h"

class AsyncSampler;

/**
 * Registers a reader for each system source: CpuLoad, CpuClock, GpuClock, Ram, Temps, BoardTemps, Throttle, Fan,
 * Power, Battery, Network, Clock, Uptime, SelfCost.
 * @param scheduler where to register
 * @param sensors discover() already called; must outlive the scheduler
 * @param store where values go; must outlive the scheduler
 * @param sampler non-null: the slow sources (Power, BoardTemps, Battery, SelfCost) are read on its thread and stored
 *                when the main loop calls its applyReady(); null: read in place (the offline modes)
 */
void registerSystemSources(frame_perf_overlay::Scheduler& scheduler, Sensors& sensors,
                           frame_perf_overlay::MetricStore& store, AsyncSampler* sampler = nullptr);
