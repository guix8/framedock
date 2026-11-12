#include "gaze_fit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace framecolor {

namespace {
constexpr float kDegPerUnit = 45.0f;
constexpr float kOpenAtLeast = 0.5f, kMaxSpreadDeg = 2.7f, kUsableShare = 0.6f;
constexpr int kMinUsable = 12, kEnoughUsable = 45;
constexpr float kGridX[3] = {-20, 0, 20}, kGridY[3] = {15, 0, -15};  // degrees; rows top to bottom
constexpr float kMinGapDeg = 5.0f;  // neighbouring dots must read at least this far apart, in the right order
constexpr float kMaxExtrapolate = 1.5f;  // beyond the grid, at most this many quarters' widths
constexpr float kMaxShift = 6.0f / kDegPerUnit;  // a re-centre moving straight ahead further is a dot looked at wrongly

using Point = std::array<float, 2>;

int gridIndex(const GazeTarget& t) {
    int row = -1, column = -1;
    for (int i = 0; i < 3; ++i) {
        if (t.yDeg == kGridY[i]) row = i;
        if (t.xDeg == kGridX[i]) column = i;
    }
    return row < 0 || column < 0 ? -1 : row * 3 + column;
}

float cross(const Point& o, const Point& a, const Point& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

/**
 * (u, v) with p = bilinear(a, b, c, d; u, v): a top left, b top right, c bottom left, d bottom right. Newton steps from
 * the middle. Nothing if it does not converge.
 */
std::optional<Point> inverseBilinear(const Point& a, const Point& b, const Point& c, const Point& d, const Point& p) {
    float u = 0.5f, v = 0.5f;
    for (int i = 0; i < 16; ++i) {
        const float fx = (1 - u) * (1 - v) * a[0] + u * (1 - v) * b[0] + (1 - u) * v * c[0] + u * v * d[0] - p[0];
        const float fy = (1 - u) * (1 - v) * a[1] + u * (1 - v) * b[1] + (1 - u) * v * c[1] + u * v * d[1] - p[1];
        if (std::fabs(fx) < 1e-7f && std::fabs(fy) < 1e-7f) break;
        const float dux = (1 - v) * (b[0] - a[0]) + v * (d[0] - c[0]), duy = (1 - v) * (b[1] - a[1]) + v * (d[1] - c[1]);
        const float dvx = (1 - u) * (c[0] - a[0]) + u * (d[0] - b[0]), dvy = (1 - u) * (c[1] - a[1]) + u * (d[1] - b[1]);
        const float det = dux * dvy - dvx * duy;
        if (!std::isfinite(det) || std::fabs(det) < 1e-9f) return std::nullopt;
        u -= (fx * dvy - fy * dvx) / det;
        v -= (dux * fy - duy * fx) / det;
        if (!std::isfinite(u) || !std::isfinite(v)) return std::nullopt;
    }
    const float fx = (1 - u) * (1 - v) * a[0] + u * (1 - v) * b[0] + (1 - u) * v * c[0] + u * v * d[0] - p[0];
    const float fy = (1 - u) * (1 - v) * a[1] + u * (1 - v) * b[1] + (1 - u) * v * c[1] + u * v * d[1] - p[1];
    if (!(std::fabs(fx) < 1e-4f && std::fabs(fy) < 1e-4f)) return std::nullopt;  // did not converge
    return Point{{u, v}};
}
}  // namespace

GazeTarget gridTarget(int row, int column) { return {kGridX[column], kGridY[row]}; }

std::array<float, 2> correctGaze(const std::array<float, 2>& angles, const GazeFit& fit) {
    if (!fit.fitted) return angles;
    const Point p{{angles[0] - fit.shiftX, angles[1] - fit.shiftY}};
    const auto& m = fit.measured;
    // The quarter: which side of the measured middle column and middle row the gaze is on. Each is a broken line
    // through the centre dot; the half of it on the gaze's side is taken, extended past the grid. Neighbouring quarters'
    // maps agree along the line they share (also extended), so the result does not jump from one to the other
    const Point& centre = m[4];
    const bool above = p[1] >= centre[1];
    const Point& columnEnd = above ? m[1] : m[7];  // the middle column's top or bottom dot
    const bool right = above ? cross(centre, columnEnd, p) < 0 : cross(centre, columnEnd, p) > 0;
    const bool leftHalf = p[0] < centre[0];
    const Point& rowEnd = leftHalf ? m[3] : m[5];  // the middle row's left or right dot
    const bool upper = leftHalf ? cross(centre, rowEnd, p) < 0 : cross(centre, rowEnd, p) > 0;
    const int row = upper ? 0 : 1, column = right ? 1 : 0;
    const int i = row * 3 + column;
    const auto mapped = [&](const Point& q) -> std::optional<Point> {
        const auto uv = inverseBilinear(m[i], m[i + 1], m[i + 3], m[i + 4], q);
        if (!uv) return std::nullopt;
        const float u = std::clamp((*uv)[0], -kMaxExtrapolate, 1 + kMaxExtrapolate);
        const float v = std::clamp((*uv)[1], -kMaxExtrapolate, 1 + kMaxExtrapolate);
        return Point{{(kGridX[column] + u * (kGridX[column + 1] - kGridX[column])) / kDegPerUnit,
                      (kGridY[row] + v * (kGridY[row + 1] - kGridY[row])) / kDegPerUnit}};
    };
    auto out = mapped(p);
    if (!out) {
        // Far past a noisy grid the quarter's extended map folds over and has no answer there. Go back towards the
        // centre dot to the furthest point that has one and scale from straight ahead (the centre maps to 0, 0), so the
        // result goes on smoothly from there instead of jumping
        float good = 0, bad = 1;
        for (int k = 0; k < 24; ++k) {
            const float t = 0.5f * (good + bad);
            if (mapped({{centre[0] + t * (p[0] - centre[0]), centre[1] + t * (p[1] - centre[1])}})) good = t;
            else bad = t;
        }
        const auto edge = good > 0.05f ? mapped({{centre[0] + good * (p[0] - centre[0]),
                                                  centre[1] + good * (p[1] - centre[1])}})
                                       : std::nullopt;
        if (!edge) return angles;
        out = Point{{(*edge)[0] / good, (*edge)[1] / good}};
    }
    return {{std::clamp((*out)[0], -1.0f, 1.0f), std::clamp((*out)[1], -1.0f, 1.0f)}};
}

const std::vector<GazeTarget>& fullFitTargets() {
    static const std::vector<GazeTarget> targets{{0, 0},    {0, 15},   {0, -15}, {-20, 0}, {20, 0},
                                                 {-20, 15}, {20, 15},  {-20, -15}, {20, -15}};
    return targets;
}

const std::vector<GazeTarget>& recenterTargets() {
    static const std::vector<GazeTarget> targets{{0, 0}};
    return targets;
}

void DotMeasure::add(const std::array<float, 2>& angles, const std::array<float, 2>& openness) {
    ++received_;
    if (!(openness[0] >= kOpenAtLeast) || !(openness[1] >= kOpenAtLeast)) return;
    if (!std::isfinite(angles[0]) || !std::isfinite(angles[1])) return;
    samples_.push_back(angles);
}

DotResult DotMeasure::result() const {
    DotResult r;
    r.received = received_;
    r.usable = static_cast<int>(samples_.size());
    if (samples_.empty()) return r;
    for (const auto& s : samples_) {
        r.mean[0] += s[0];
        r.mean[1] += s[1];
    }
    r.mean[0] /= r.usable;
    r.mean[1] /= r.usable;
    double sum = 0;
    for (const auto& s : samples_) {
        const double dx = (s[0] - r.mean[0]) * kDegPerUnit, dy = (s[1] - r.mean[1]) * kDegPerUnit;
        sum += dx * dx + dy * dy;
    }
    r.spreadDeg = static_cast<float>(std::sqrt(sum / r.usable));
    const int needed = std::min(kEnoughUsable, std::max(kMinUsable, static_cast<int>(std::ceil(kUsableShare * received_))));
    r.ok = r.usable >= needed && r.spreadDeg <= kMaxSpreadDeg;
    return r;
}

std::optional<GazeFit> fitFromDots(const std::vector<DotResult>& dots) {
    const auto& targets = fullFitTargets();
    if (dots.size() != targets.size()) return std::nullopt;
    GazeFit fit;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!dots[i].ok) return std::nullopt;
        fit.measured[gridIndex(targets[i])] = dots[i].mean;
    }
    const auto& m = fit.measured;
    const float gap = kMinGapDeg / kDegPerUnit;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (!(m[i * 3 + j + 1][0] - m[i * 3 + j][0] >= gap)) return std::nullopt;  // row i reads left to right
            if (!(m[j * 3 + i][1] - m[(j + 1) * 3 + i][1] >= gap)) return std::nullopt;  // column i top to bottom
        }
    }
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            // Corners around the quarter (top left, top right, bottom right, bottom left): all turn the same way
            const int i = row * 3 + column;
            const Point q[4] = {m[i], m[i + 1], m[i + 4], m[i + 3]};
            for (int k = 0; k < 4; ++k)
                if (!(cross(q[k], q[(k + 1) % 4], q[(k + 2) % 4]) < 0)) return std::nullopt;
        }
    }
    fit.fitted = true;
    return fit;
}

std::optional<GazeFit> recenterFromDot(const DotResult& ahead, const GazeFit& before) {
    if (!before.fitted || !ahead.ok) return std::nullopt;
    const float shiftX = ahead.mean[0] - before.measured[4][0], shiftY = ahead.mean[1] - before.measured[4][1];
    if (!(std::hypot(shiftX, shiftY) <= kMaxShift)) return std::nullopt;
    GazeFit fit = before;
    fit.shiftX = shiftX;
    fit.shiftY = shiftY;
    return fit;
}

std::string describeDots(const std::vector<GazeTarget>& targets, const std::vector<DotResult>& dots, const GazeFit& fit) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    for (std::size_t i = 0; i < targets.size() && i < dots.size(); ++i) {
        const DotResult& d = dots[i];
        const float rawX = d.mean[0] * kDegPerUnit, rawY = d.mean[1] * kDegPerUnit;
        const auto fitted = correctGaze(d.mean, fit);
        const float errRaw = std::hypot(rawX - targets[i].xDeg, rawY - targets[i].yDeg);
        const float errFit = std::hypot(fitted[0] * kDegPerUnit - targets[i].xDeg, fitted[1] * kDegPerUnit - targets[i].yDeg);
        out << "dot (" << targets[i].xDeg << "," << targets[i].yDeg << "): gaze (" << rawX << "," << rawY
            << ") off " << errRaw << " deg, fitted off " << errFit << " deg, spread " << d.spreadDeg << ", "
            << d.usable << "/" << d.received << (d.ok ? "" : " (not steady)") << "\n";
    }
    return out.str();
}

GazeFit loadGazeFit(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string version;
    if (!(in >> version) || version != "v2") return GazeFit{};  // missing, or the 5-dot fit before 2026-11-09
    GazeFit fit;
    for (auto& point : fit.measured)
        if (!(in >> point[0] >> point[1]) || !std::isfinite(point[0]) || !std::isfinite(point[1])) return GazeFit{};
    int fitted = 0;
    if (!(in >> fit.shiftX >> fit.shiftY >> fitted) || !std::isfinite(fit.shiftX) || !std::isfinite(fit.shiftY))
        return GazeFit{};
    if (!(std::hypot(fit.shiftX, fit.shiftY) <= kMaxShift)) {  // keep the grid, drop a shift not believed
        fit.shiftX = fit.shiftY = 0;
    }
    if (fitted == 0) return GazeFit{};
    // The same checks as a new fit: a file that would fold the grid is not believed
    std::vector<DotResult> dots;
    for (const GazeTarget& t : fullFitTargets()) {
        DotResult dot;
        dot.ok = true;
        dot.mean = fit.measured[gridIndex(t)];
        dots.push_back(dot);
    }
    const auto checked = fitFromDots(dots);
    if (!checked) return GazeFit{};
    fit.fitted = true;
    return fit;
}

bool saveGazeFit(const std::filesystem::path& file, const GazeFit& fit) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out.precision(9);
        out << "v2\n";
        for (const auto& point : fit.measured) out << point[0] << " " << point[1] << "\n";
        out << fit.shiftX << " " << fit.shiftY << " " << (fit.fitted ? 1 : 0) << "\n";
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}

}  // namespace framecolor
