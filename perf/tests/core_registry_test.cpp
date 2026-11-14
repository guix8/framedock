// core/ registry and value store tests (CTest runs them; no SteamVR, no platform code).
#include "metric_registry.h"
#include "metric_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace frame_perf_overlay;

bool hasProblem(const std::vector<std::string>& problems, const std::string& needle) {
    for (const std::string& p : problems) {
        if (p.find(needle) != std::string::npos) return true;
    }
    return false;
}

void testCompiledRegistry() {
    const std::vector<std::string> problems = validateRegistry(registry());
    for (const std::string& p : problems) std::printf("registry problem: %s\n", p.c_str());
    CHECK(problems.empty());
    // The simple templates borrow metrics that exist and own none themselves
    const std::vector<std::string> borrowed = validateBorrowed(registry());
    for (const std::string& p : borrowed) std::printf("borrow problem: %s\n", p.c_str());
    CHECK(borrowed.empty());
    CHECK(!borrowedMetrics(WidgetType::SimpleA).empty() && !borrowedMetrics(WidgetType::SimpleB).empty());
    CHECK(borrowedMetrics(WidgetType::Frame).empty());
    // Simple A colours its CPU % by the busiest core (the thresholds are for that), so it borrows that too
    const std::vector<uint16_t>& a = borrowedMetrics(WidgetType::SimpleA);
    CHECK(std::find(a.begin(), a.end(), metric::kCpuMaxCorePct) != a.end());
    CHECK(std::find(a.begin(), a.end(), metric::kCpuUsagePct) != a.end());
    for (const MetricDef& d : registry()) CHECK(d.owner != WidgetType::SimpleA && d.owner != WidgetType::SimpleB);
    // The PC stream's metrics (2026-11-15): 113-126, Frame's, read with the render size once a second; the state is
    // an enum, the rest numbers
    for (uint16_t id = metric::kStreamState; id <= metric::kPcPingMs; ++id) {
        const MetricDef* d = findMetric(id);
        CHECK(d != nullptr);
        if (d == nullptr) continue;
        CHECK(d->owner == WidgetType::Frame && d->source == SourceId::RenderSize && d->intervalMs == 1000);
        CHECK(d->type == (id == metric::kStreamState ? WireType::Enum : WireType::F32));
    }
    CHECK(metric::kStreamState == 113 && metric::kPcPingMs == 126);
    CHECK(findMetric(metric::kFrameRenderWidth) != nullptr && findMetric(metric::kFrameRenderWidth)->intervalMs == 1000);
    // Both simple templates follow the stream's state and show the game's fps on the PC
    const std::vector<uint16_t>& b = borrowedMetrics(WidgetType::SimpleB);
    for (const uint16_t id : {metric::kStreamState, metric::kPcFps}) {
        CHECK(std::find(a.begin(), a.end(), id) != a.end() && std::find(b.begin(), b.end(), id) != b.end());
    }
    // The PC stream card (widget 22) owns nothing: it borrows the stream's metrics and, from the headset's frame
    // timing, the refresh rate and the reprojection
    CHECK(static_cast<int>(WidgetType::PcStream) == 22 && kWidgetTypeCount == 23);
    const std::vector<uint16_t>& pc = borrowedMetrics(WidgetType::PcStream);
    for (uint16_t id = metric::kStreamState; id <= metric::kPcPingMs; ++id) {
        CHECK(std::find(pc.begin(), pc.end(), id) != pc.end());
    }
    // ...and the summary of the stream that ended last (127-133: Frame's like the rest, read with the render size;
    // two whole numbers, two percentages, three names)
    for (uint16_t id = metric::kPcLastEndedUnix; id < metric::kPcLastCause0 + metric::kPcLastCauseSlots; ++id) {
        const MetricDef* d = findMetric(id);
        CHECK(d != nullptr && std::find(pc.begin(), pc.end(), id) != pc.end());
        if (d == nullptr) continue;
        CHECK(d->owner == WidgetType::Frame && d->source == SourceId::RenderSize && d->intervalMs == 1000);
        CHECK(d->type == (id >= metric::kPcLastCause0 ? WireType::ShortString
                          : id >= metric::kPcLastLinkBadPct ? WireType::F32
                                                            : WireType::I32));
    }
    CHECK(metric::kPcLastEndedUnix == 127 && metric::kPcLastCause0 + metric::kPcLastCauseSlots - 1 == 133);
    CHECK(std::find(pc.begin(), pc.end(), metric::kFrameDisplayHz) != pc.end());
    CHECK(std::find(pc.begin(), pc.end(), metric::kFrameReprojectedPct) != pc.end());
    for (const MetricDef& d : registry()) CHECK(d.owner != WidgetType::PcStream);
    // A borrowed id missing from a table is reported
    const std::vector<MetricDef> tiny = {{metric::kFrameAppFps, "frame.app_fps", "fps", WidgetType::Frame,
                                          SourceId::FrameTiming, 500, WireType::F32}};
    CHECK(!validateBorrowed(tiny).empty());
}

void testBadTable() {
    using W = WidgetType;
    using S = SourceId;
    using T = WireType;
    const std::vector<MetricDef> bad = {
        {1, "a.ok", "", W::Cpu, S::CpuLoad, 500, T::F32},
        {1, "a.dup_id", "", W::Cpu, S::CpuLoad, 500, T::F32},       // duplicate id
        {2, "a.no_owner", "", W::None, S::CpuLoad, 500, T::F32},     // no owner
        {3, "a.shared", "", W::Cpu, S::CpuLoad, 500, T::F32},
        {4, "a.shared", "", W::Power, S::CpuLoad, 500, T::F32},      // same name, two owners
        {5, "a.no_source", "", W::Cpu, S::None, 500, T::F32},        // no source
        {6, "a.no_interval", "", W::Cpu, S::CpuLoad, 0, T::F32},     // zero interval
    };
    const std::vector<std::string> problems = validateRegistry(bad);
    CHECK(hasProblem(problems, "used more than once"));
    CHECK(hasProblem(problems, "no owner"));
    CHECK(hasProblem(problems, "two owners"));
    CHECK(hasProblem(problems, "no source"));
    CHECK(hasProblem(problems, "no read interval"));
}

void testFindMetric() {
    const MetricDef* cpu = findMetric(metric::kCpuUsagePct);
    CHECK(cpu != nullptr);
    CHECK(cpu != nullptr && cpu->id == metric::kCpuUsagePct);
    CHECK(findMetric(65535) == nullptr);
}

void testStore() {
    MetricStore store;
    const MetricValue* v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::NotSampledYet);
    CHECK(store.get(65535) == nullptr);

    CHECK(store.setF32(metric::kCpuUsagePct, 42.5, 1.0));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && v->ok && v->reason == Reason::None && v->f32 == 42.5f);

    // NaN -> N/A with the given reason
    CHECK(store.setF32(metric::kCpuUsagePct, std::nan(""), 2.0, Reason::SensorMissing));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::SensorMissing);

    // wrong-type setters fail and leave the value alone
    CHECK(store.setF32(metric::kCpuUsagePct, 10.0, 3.0));
    CHECK(!store.setI32(metric::kCpuUsagePct, 7, 4.0));
    CHECK(!store.setEnum(metric::kCpuUsagePct, 1, 4.0));
    CHECK(!store.setText(metric::kCpuUsagePct, "x", 4.0));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && v->ok && v->f32 == 10.0f && v->sampledAt == 3.0);
    CHECK(!store.setF32(65535, 1.0, 1.0));

    // setText cuts to kShortStringMax: needs a ShortString metric, so use a small table
    const std::vector<MetricDef> defs = {
        {1, "t.text", "", WidgetType::General, SourceId::Ram, 500, WireType::ShortString},
    };
    MetricStore textStore(defs);
    const std::string longText(kShortStringMax + 20, 'x');
    CHECK(textStore.setText(1, longText, 1.0));
    const MetricValue* t = textStore.get(1);
    CHECK(t != nullptr && t->ok && t->text.size() == kShortStringMax);
    // ... at a character start: 11 Hangul syllables (33 bytes) keep 10 (30 bytes), not 31 bytes of a broken one
    std::string hangul;
    for (int k = 0; k < 11; ++k) hangul += "가";
    CHECK(textStore.setText(1, hangul, 2.0));
    t = textStore.get(1);
    CHECK(t != nullptr && t->text.size() == 30 && t->text == hangul.substr(0, 30));
}

}  // namespace

int main() {
    testCompiledRegistry();
    testBadTable();
    testFindMetric();
    testStore();
    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
