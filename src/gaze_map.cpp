#include "gaze_map.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace framecolor {

Uv ndcToRenderUv(float x, float y) { return {(x + 1.0f) * 0.5f, (1.0f - y) * 0.5f}; }

std::optional<Uv> directionToNdc(const std::array<float, 3>& head, const EyeView& view) {
    // Into the eye's frame: the transpose of its rotation
    const auto& r = view.eyeToHead;
    const float x = r[0][0] * head[0] + r[1][0] * head[1] + r[2][0] * head[2];
    const float y = r[0][1] * head[0] + r[1][1] * head[1] + r[2][1] * head[2];
    const float z = r[0][2] * head[0] + r[1][2] * head[1] + r[2][2] * head[2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z > -1e-3f) return std::nullopt;
    const float tanX = x / -z, tanY = y / -z;
    const float width = view.right - view.left, height = view.bottom - view.top;
    if (!(std::fabs(width) > 1e-6f) || !(std::fabs(height) > 1e-6f)) return std::nullopt;
    return Uv{(2.0f * tanX - (view.left + view.right)) / width, (2.0f * tanY - (view.top + view.bottom)) / height};
}

std::array<float, 2> gazeAngles(const std::array<float, 3>& d) {
    constexpr float kScale = 4.0f / 3.14159265f;
    return {{std::clamp(std::atan2(d[0], -d[2]) * kScale, -1.0f, 1.0f),
             std::clamp(std::atan2(d[1], -d[2]) * kScale, -1.0f, 1.0f)}};
}

std::array<float, 3> anglesToDirection(float x, float y) {
    constexpr float kQuarter = 3.14159265f / 4.0f;
    return {{std::tan(std::clamp(x, -1.0f, 1.0f) * kQuarter), std::tan(std::clamp(y, -1.0f, 1.0f) * kQuarter), -1.0f}};
}

float OneEuro::filter(float value, float dt) {
    if (!started_) {
        started_ = true;
        value_ = value;
        velocity_ = 0;
        return value_;
    }
    const auto alpha = [dt](float cutoff) {
        const float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    };
    velocity_ += alpha(derivativeCutoff_) * ((value - value_) / dt - velocity_);
    const float cutoff = minCutoff_ + beta_ * std::fabs(velocity_);
    value_ += alpha(cutoff) * (value - value_);
    return value_;
}

float Deadzone::apply(float value) {
    value_ = started_ ? std::clamp(value_, value - width_, value + width_) : value;
    started_ = true;
    return value_;
}

namespace {
constexpr float kNominalDt = 1.0f / 90.0f;
constexpr double kMaxGap = 0.25, kOneEyeHoldMax = 1.0;
constexpr float kHoldBelow = 0.5f, kDownHoldDeg = 24.0f, kDownFadeDeg = 10.0f;
float median3(float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }
}  // namespace

void GazeSmoother::reset() {
    lastTime_.reset();
    lowTime_.reset();
    downHoldX_.reset();
    recent_ = {};
    alone_ = {};
    up_ = {};
    last_.reset();
    for (auto& filter : filters_) filter.reset();
    for (auto& deadzone : deadzones_) deadzone.reset();
}

std::optional<std::array<float, 2>> GazeSmoother::step(const GazeReading& reading) {
    const auto finite = [](const auto& values) {
        for (float value : values)
            if (!std::isfinite(value)) return false;
        return true;
    };
    if (!std::isfinite(reading.time) || !finite(reading.fixation) || !finite(reading.gaze[0]) ||
        !finite(reading.gaze[1]) || !finite(reading.openness))
        return std::nullopt;  // skipped, as frameeyeosc skips such a sample
    // advance(): the time since the previous sample; a gap starts everything over (frameeyeosc Smoother::reset)
    float dt = kNominalDt;
    if (lastTime_ && reading.time > *lastTime_ && reading.time - *lastTime_ < kMaxGap)
        dt = static_cast<float>(reading.time - *lastTime_);
    else if (!lastTime_ || reading.time > *lastTime_)
        reset();
    lastTime_ = reading.time;
    // doubt_one_low_eye: an eye read alone below the line for over a second is a wrong reading, not a shut eye
    const std::array<bool, 2> low{{reading.openness[0] < kHoldBelow, reading.openness[1] < kHoldBelow}};
    const double lowDt = lowTime_ ? std::clamp(reading.time - *lowTime_, 0.0, kMaxGap) : 0.0;
    lowTime_ = reading.time;
    std::array<bool, 2> doubted{};
    for (int eye = 0; eye < 2; ++eye) {
        if (low[eye]) {
            up_[eye] = 0;
            if (!low[1 - eye]) alone_[eye] += lowDt;
        } else if (++up_[eye] >= 2) {
            alone_[eye] = 0;
        }
        doubted[eye] = !low[1 - eye] && alone_[eye] > kOneEyeHoldMax;
    }
    const auto shut = [&](float left, float right) {
        return (left < kHoldBelow && !doubted[0]) || (right < kHoldBelow && !doubted[1]);
    };
    // left x, y, right x, y, fused x, y (raw_gaze)
    const auto l = gazeAngles(reading.gaze[0]), r = gazeAngles(reading.gaze[1]), f = gazeAngles(reading.fixation);
    std::array<float, 6> angles{{l[0], l[1], r[0], r[1], f[0], f[1]}};
    // hold_down_x: far down (judged on the fused vertical) the Frame's sideways gaze jumps, so each x fades to its
    // value from just before; only eyes open and not doubted are remembered (the fused one if either is)
    const float downDeg = -angles[5] * 45.0f;
    const bool open = !shut(reading.openness[0], reading.openness[1]);
    const std::array<bool, 2> trusted{{!doubted[0] && open, !doubted[1] && open}};
    if (downDeg <= kDownHoldDeg) {
        auto remembered = downHoldX_.value_or(std::array<float, 3>{});
        const bool keep[3] = {trusted[0], trusted[1], trusted[0] || trusted[1]};
        for (int pair = 0; pair < 3; ++pair)
            if (keep[pair]) remembered[pair] = angles[pair * 2];
        downHoldX_ = remembered;
    } else {
        const auto held = downHoldX_.value_or(std::array<float, 3>{});  // straight ahead if it starts that low
        const float live = std::clamp(1.0f - (downDeg - kDownHoldDeg) / kDownFadeDeg, 0.0f, 1.0f);
        for (int pair = 0; pair < 3; ++pair) angles[pair * 2] = live * angles[pair * 2] + (1.0f - live) * held[pair];
    }
    // correct_gaze: the eye fit (straight ahead and the up gain), on each eye's gaze and the fused one
    for (int pair = 0; pair < 3; ++pair) {
        const auto corrected = correctGaze({{angles[pair * 2], angles[pair * 2 + 1]}}, fit_);
        angles[pair * 2] = corrected[0];
        angles[pair * 2 + 1] = corrected[1];
    }
    // despike: the median of this sample and the previous two
    std::array<float, 8> values{};
    for (int i = 0; i < 6; ++i) values[i] = angles[i];
    values[6] = reading.openness[0];
    values[7] = reading.openness[1];
    std::array<float, 8> median = values;
    if (recent_[0] && recent_[1])
        for (int i = 0; i < 8; ++i) median[i] = median3((*recent_[0])[i], (*recent_[1])[i], values[i]);
    recent_ = {recent_[1], values};
    // choose_gaze: one doubted eye leaves the fused gaze, and the other eye's own stands in
    const std::array<bool, 2> usable{{!doubted[0], !doubted[1]}};
    std::array<float, 2> chosen{{median[4], median[5]}};
    if (usable[0] && !usable[1]) chosen = {{median[0], median[1]}};
    if (!usable[0] && usable[1]) chosen = {{median[2], median[3]}};
    // Held while the eyes are (mostly) shut; only both eyes unusable holds it otherwise
    if (last_ && (shut(median[6], median[7]) || (!usable[0] && !usable[1]))) return last_;
    std::array<float, 2> out{};
    for (int i = 0; i < 2; ++i) out[i] = deadzones_[i].apply(filters_[i].filter(chosen[i], dt));
    last_ = out;
    return last_;
}

Uv applyDrawFit(const DrawFit& fit, int eye, Uv uv) {
    if (!fit.valid || eye < 0 || eye > 1) return uv;
    const auto& c = fit.c[eye];
    const float u = uv.u, v = uv.v;
    return {c[0] + c[1] * u + c[2] * v + c[3] * u * u + c[4] * u * v + c[5] * v * v,
            c[6] + c[7] * u + c[8] * v + c[9] * u * u + c[10] * u * v + c[11] * v * v};
}

DrawFit loadDrawFit(const std::string& file) {
    std::ifstream in(file);
    std::string version;
    DrawFit fit;
    if (!(in >> version) || version != "v1") return {};
    for (auto& eye : fit.c)
        for (float& value : eye)
            if (!(in >> value) || !std::isfinite(value)) return {};
    // Sane: the panel centre stays within a quarter of the panel of where it was
    for (int eye = 0; eye < 2; ++eye) {
        const Uv centre = applyDrawFit({true, fit.c}, eye, {0.5f, 0.5f});
        if (std::fabs(centre.u - 0.5f) > 0.25f || std::fabs(centre.v - 0.5f) > 0.25f) return {};
    }
    fit.valid = true;
    return fit;
}

DrawFit builtInDrawFit() {
    DrawFit fit;
    fit.valid = true;
    fit.c[0] = {-0.0874039f, 1.310686f, 0.06537525f, -0.5627599f, 0.3585255f, -0.04049034f,
                0.09054324f, -0.1495819f, 0.5489778f, 0.2870449f, -0.5474936f, 0.6225863f};
    fit.c[1] = {0.3316223f, 0.2022695f, -0.4174542f, 0.5619164f, 0.3388486f, 0.04942217f,
                0.2195019f, -0.4030752f, 0.05008167f, 0.2757032f, 0.5330467f, 0.5914897f};
    return fit;
}

bool DistortionGrid::build(const std::function<bool(float, float, Uv&)>& forward) {
    ready_ = false;
    samples_.assign(kSize * kSize, Uv{});
    for (int j = 0; j < kSize; ++j) {
        for (int i = 0; i < kSize; ++i) {
            Uv out;
            const float u = static_cast<float>(i) / (kSize - 1), v = static_cast<float>(j) / (kSize - 1);
            if (!forward(u, v, out) || !std::isfinite(out.u) || !std::isfinite(out.v)) return false;
            samples_[j * kSize + i] = out;
        }
    }
    ready_ = true;
    return true;
}

Uv DistortionGrid::forward(Uv panel) const {
    const float x = std::clamp(panel.u, 0.0f, 1.0f) * (kSize - 1), y = std::clamp(panel.v, 0.0f, 1.0f) * (kSize - 1);
    const int i = std::min(static_cast<int>(x), kSize - 2), j = std::min(static_cast<int>(y), kSize - 2);
    const float fx = x - i, fy = y - j;
    const Uv& a = samples_[j * kSize + i];
    const Uv& b = samples_[j * kSize + i + 1];
    const Uv& c = samples_[(j + 1) * kSize + i];
    const Uv& d = samples_[(j + 1) * kSize + i + 1];
    return {(a.u * (1 - fx) + b.u * fx) * (1 - fy) + (c.u * (1 - fx) + d.u * fx) * fy,
            (a.v * (1 - fx) + b.v * fx) * (1 - fy) + (c.v * (1 - fx) + d.v * fx) * fy};
}

std::optional<Uv> DistortionGrid::inverse(Uv render) const {
    if (!ready_ || !std::isfinite(render.u) || !std::isfinite(render.v)) return std::nullopt;
    // The panel centre maps to the render centre and the map is close to the identity there, so start at the target
    Uv p{std::clamp(render.u, 0.0f, 1.0f), std::clamp(render.v, 0.0f, 1.0f)};
    constexpr float kStep = 1.0f / (kSize - 1) / 4.0f;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const Uv f = forward(p);
        const float ex = f.u - render.u, ey = f.v - render.v;
        if (std::fabs(ex) < 2e-4f && std::fabs(ey) < 2e-4f) return p;
        // Central differences on the interpolated grid (stay inside the panel)
        const float hu = p.u < kStep ? kStep : (p.u > 1 - kStep ? -kStep : kStep);
        const float hv = p.v < kStep ? kStep : (p.v > 1 - kStep ? -kStep : kStep);
        const Uv fu = forward({p.u + hu, p.v}), fv = forward({p.u, p.v + hv});
        const float a = (fu.u - f.u) / hu, b = (fv.u - f.u) / hv, c = (fu.v - f.v) / hu, d = (fv.v - f.v) / hv;
        const float det = a * d - b * c;
        if (!std::isfinite(det) || std::fabs(det) < 1e-6f) return std::nullopt;
        const float du = (d * ex - b * ey) / det, dv = (-c * ex + a * ey) / det;
        p.u = std::clamp(p.u - du, 0.0f, 1.0f);
        p.v = std::clamp(p.v - dv, 0.0f, 1.0f);
    }
    const Uv f = forward(p);
    if (std::fabs(f.u - render.u) < 1e-3f && std::fabs(f.v - render.v) < 1e-3f) return p;
    return std::nullopt;
}

}  // namespace framecolor
