// The widgets. Drawing helpers (levels, graphs, badges, icons) are the fixed panel's ones (panel.cpp), moved here.
#include "widgets.h"

#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace frame_perf_overlay {

namespace {

using namespace metric;

// ---- Card geometry (relative to the card's top-left; the card sits 8 px inside its 512 px column) ----
constexpr double kCardW = kColumnWidth - 16;  // 496
constexpr double kInX = 6;                    // content left = the fixed panel's 14 px padding
constexpr double kInR = kCardW - 6;           // content right
constexpr double kTop = 6;                    // the fixed panel's "row top" sits 6 px below the card top
constexpr double kGraphX = 202;               // the fixed panel: kPad + kLeftWidth + 8 = 210 from the panel edge
constexpr double kGraphW = 288;
constexpr double kGraphH = 84;
constexpr double kRowH = 20;                  // compact rows (battery)
constexpr double kLineH = 22;                 // text rows (temps, general)

// Heights (card)
constexpr int kGraphCardH = 102;  // the fixed panel's row: 92 + 6 above + 4 below
constexpr int kBatteryCompactH = static_cast<int>(kTop) + 82;  // % + the W line
constexpr int kBatteryFullH = static_cast<int>(kTop + 106 + kRowH) + 10;
constexpr int kTempSpots = 4;
constexpr int kTempFullH = kGraphCardH + (kTempSpots + 2) / 3 * static_cast<int>(kLineH) + 8;
constexpr int kCpuH = static_cast<int>(kTop) + 58;
constexpr int kNetworkShortH = 64;   // no link: the state line only
constexpr int kNetworkFullH = static_cast<int>(kTop) + 82;

// The extra rows each card shows with Compact off, below its usual ones (Compact on = the card as before them).
// Upstream PR #5's geometry
// Frame: a label line, then the CPU and GPU frame-time graphs side by side
constexpr double kFrameTimesGap = 12;                                // between the two graphs
constexpr double kFrameTimesW = (kInR - kInX - kFrameTimesGap) / 2;  // 236
constexpr double kFrameTimesLabelY = kGraphCardH + 14;               // label baseline
constexpr double kFrameTimesBedY = kGraphCardH + 22;
constexpr double kFrameTimesH = 60;
constexpr int kFrameFullH = static_cast<int>(kFrameTimesBedY + kFrameTimesH) + 10;  // 194
// CPU: one bar per core under the usage line, then the clusters two per row
constexpr double kCoreBarTop = 58;   // from the card top
constexpr double kCoreBarH = 56;
constexpr double kClusterTop = kCoreBarTop + kCoreBarH + 38;  // first cluster row's baseline
constexpr int kCpuFullMinH = static_cast<int>(kCoreBarTop + kCoreBarH) + 30;  // bars and their labels
// General: uptime, the app in front, this app's own cost
constexpr int kGeneralMoreRows = 3;
// Process lists: a heading, then one row per process
constexpr double kListTop = 26;

/** Value state. */
enum class Level { Normal, Warn, Crit };

Level levelOf(double value, double warn, double crit, bool lowerIsWorse = false) {
    if (std::isnan(value)) return Level::Normal;
    if (lowerIsWorse) {
        if (value <= crit) return Level::Crit;
        if (value <= warn) return Level::Warn;
        return Level::Normal;
    }
    if (value >= crit) return Level::Crit;
    if (value >= warn) return Level::Warn;
    return Level::Normal;
}

Color colorOf(Level level, Color normal = kText) {
    switch (level) {
        case Level::Warn: return kWarn;
        case Level::Crit: return kDanger;
        default: return normal;
    }
}

Level worse(Level a, Level b) {
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

/** @return the value with the given decimals, "--" for NaN */
std::string num(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

}  // namespace

RateText formatRate(double mbps) {
    if (std::isnan(mbps)) return {"--", " Mbps"};
    const long kbps = std::lround(std::max(0.0, mbps) * 1000.0 + 1e-6);  // + 1e-6: 0.9995 is stored as 0.99949999…
    if (kbps < 1000) return {std::to_string(kbps), " kbps"};  // 0.024 Mbps reads "24 kbps", not "0.0"
    return {num(mbps, mbps >= 100 ? 0 : 1), " Mbps"};
}

namespace {

/** @return the value rounded with thousands separators, "--" for NaN */
std::string grouped(double value) {
    if (std::isnan(value)) return "--";
    std::string digits = std::to_string(static_cast<long long>(std::lround(value)));
    const size_t start = digits[0] == '-' ? 1 : 0;
    for (int i = static_cast<int>(digits.size()) - 3; i > static_cast<int>(start); i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

// ---- Store access ----

const MetricValue* value(const WidgetContext& ctx, uint16_t id) {
    return ctx.store != nullptr ? ctx.store->get(id) : nullptr;
}

bool ok(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v != nullptr && v->ok;
}

/** @return the f32 value, NaN if N/A */
double f(const WidgetContext& ctx, uint16_t id) {
    return ctx.store != nullptr ? ctx.store->f32Or(id, std::nan("")) : std::nan("");
}

/** @return the i32 / enum value, fallback if N/A */
int64_t i(const WidgetContext& ctx, uint16_t id, int64_t fallback = 0) {
    return ctx.store != nullptr ? ctx.store->intOr(id, fallback) : fallback;
}

/** @return the short string, "" if N/A */
std::string text(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v != nullptr && v->ok ? v->text : std::string();
}

Reason reasonOf(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v == nullptr ? Reason::SensorMissing : v->reason;
}

const WidgetText& texts(const WidgetContext& ctx) {
    return widgetText(ctx.language);
}

/** Whether the instance draws its extra rows: Compact off. */
bool moreRows(const WidgetInstance& w) {
    return hasCompactView(w.type) && !w.compact;
}

/** "N/A: <reason>" */
std::string naText(const WidgetContext& ctx, uint16_t id) {
    const WidgetText& t = texts(ctx);
    return std::string(t.naPrefix) + reasonLabel(t, reasonOf(ctx, id));
}

// ---- Graphs (the fixed panel's) ----

struct GraphArea {
    double x, y, w, h;
    double minV, maxV;
    double now;
    double seconds;

    double xOf(double t) const { return x + w - (now - t) / seconds * w; }
    double yOf(double v) const {
        const double ratio = (std::clamp(v, minV, maxV) - minV) / (maxV - minV);
        return y + h - ratio * h;
    }
};

GraphArea graphArea(double x, double y, double minV, double maxV, const WidgetContext& ctx) {
    return GraphArea {x + kGraphX, y + kTop + 4, kGraphW, kGraphH, minV, maxV, ctx.now,
                      static_cast<double>(std::max(1, ctx.graphSeconds))};
}

void drawGraphLabel(const Pen& pen, const GraphArea& area, const std::string& topLabel) {
    pen.text(area.x + 6, area.y + 14, topLabel, 12, kTextMuted);
}

void drawGuide(const Pen& pen, const GraphArea& area, double v, Color c) {
    if (std::isnan(v) || v <= area.minV || v >= area.maxV) return;
    const double y = std::round(area.yOf(v)) + 0.5;
    const double dashes[] = {4.0, 4.0};
    cairo_save(pen.cr);
    cairo_set_dash(pen.cr, dashes, 2, 0);
    cairo_set_line_width(pen.cr, 1.0);
    pen.color(c);
    cairo_move_to(pen.cr, area.x + 4, y);
    cairo_line_to(pen.cr, area.x + area.w - 4, y);
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

void drawSeries(const Pen& pen, const GraphArea& area, const History& history, Color c) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    cairo_set_line_width(pen.cr, 2.0);
    cairo_set_line_join(pen.cr, CAIRO_LINE_JOIN_ROUND);
    pen.color(c);
    bool drawing = false;
    for (const auto& p : history.points()) {
        if (std::isnan(p.v)) {
            drawing = false;
            continue;
        }
        const double px = area.xOf(p.t);
        const double py = area.yOf(p.v);
        if (drawing) {
            cairo_line_to(pen.cr, px, py);
        } else {
            cairo_move_to(pen.cr, px, py);
            drawing = true;
        }
    }
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

void drawSeriesFill(const Pen& pen, const GraphArea& area, const History& history, Color c, double alpha) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    pen.color(c, alpha);
    const double baseY = area.y + area.h;
    bool open = false;
    double lastX = 0;
    for (const auto& p : history.points()) {
        const double px = area.xOf(p.t);
        if (std::isnan(p.v)) {
            if (open) {
                cairo_line_to(pen.cr, lastX, baseY);
                cairo_close_path(pen.cr);
                open = false;
            }
            continue;
        }
        if (!open) {
            cairo_move_to(pen.cr, px, baseY);
            open = true;
        }
        cairo_line_to(pen.cr, px, area.yOf(p.v));
        lastX = px;
    }
    if (open) {
        cairo_line_to(pen.cr, lastX, baseY);
        cairo_close_path(pen.cr);
    }
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
}

void drawBadge(const Pen& pen, double right, double top, const std::string& s, Color c, Color textColor = kOnAccent) {
    const double size = 14;
    const double width = pen.measure(s, size, true) + 16;
    pen.color(c, 0.95);
    pen.roundedRect(right - width, top, width, 22, 6);
    cairo_fill(pen.cr);
    pen.text(right - width + 8, top + 16, s, size, textColor, true);
}

/** Small up / down arrow. @return width */
double drawArrow(const Pen& pen, double x, double baseline, bool up, Color c) {
    const double h = 11;
    const double top = baseline - h;
    const double cx = x + 4.5;
    pen.color(c);
    cairo_save(pen.cr);
    cairo_set_line_width(pen.cr, 1.6);
    cairo_move_to(pen.cr, cx, top + (up ? 3 : 0));
    cairo_line_to(pen.cr, cx, baseline - (up ? 0 : 3));
    cairo_stroke(pen.cr);
    const double tipY = up ? top : baseline;
    const double backY = up ? top + 4.5 : baseline - 4.5;
    cairo_move_to(pen.cr, cx, tipY);
    cairo_line_to(pen.cr, cx - 4, backY);
    cairo_line_to(pen.cr, cx + 4, backY);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
    return 11;
}

/** Charging bolt. @return width */
double drawBolt(const Pen& pen, double x, double baseline, Color c) {
    const double h = 13;
    const double top = baseline - h + 1;
    pen.color(c);
    cairo_move_to(pen.cr, x + 6, top);
    cairo_line_to(pen.cr, x + 1, top + h * 0.58);
    cairo_line_to(pen.cr, x + 4.5, top + h * 0.58);
    cairo_line_to(pen.cr, x + 3, top + h);
    cairo_line_to(pen.cr, x + 8.5, top + h * 0.38);
    cairo_line_to(pen.cr, x + 5, top + h * 0.38);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    return 10;
}

constexpr double kWifiFullBarsDbm = -60.0;

int wifiBars(double dbm, const Thresholds& th) {
    if (std::isnan(dbm)) return 0;
    if (dbm > kWifiFullBarsDbm) return 4;
    if (dbm > th.wifiWarnDbm) return 3;
    if (dbm > th.wifiCritDbm) return 2;
    return 1;
}

double drawWifiIcon(const Pen& pen, double x, double baseline, int bars, Color c, bool crossed) {
    const double width = 26;
    const double cx = x + width / 2;
    const double cy = baseline;
    const double radii[] = {6.0, 10.5, 15.0};
    cairo_save(pen.cr);
    cairo_set_line_cap(pen.cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(pen.cr, 2.4);
    if (bars >= 1) pen.color(c); else pen.color(kTextMuted, 0.35);
    cairo_arc(pen.cr, cx, cy - 1.5, 2.2, 0, 2 * M_PI);
    cairo_fill(pen.cr);
    for (int k = 0; k < 3; ++k) {
        if (bars >= k + 2) pen.color(c); else pen.color(kTextMuted, 0.35);
        cairo_new_sub_path(pen.cr);
        cairo_arc(pen.cr, cx, cy - 1.5, radii[k], -M_PI * 0.75, -M_PI * 0.25);
        cairo_stroke(pen.cr);
    }
    if (crossed) {
        pen.color(kTextMuted);
        cairo_set_line_width(pen.cr, 2.0);
        cairo_move_to(pen.cr, cx - 9, cy - 15);
        cairo_line_to(pen.cr, cx + 9, cy + 1);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
    return width;
}

/** Big number + unit ("8.2" + "ms"). @return width */
double drawBigValue(const Pen& pen, double x, double y, const std::string& v, const std::string& unit, Color c) {
    const double w = pen.text(x, y, v, 30, c, true);
    return w + 3 + pen.text(x + w + 3, y, unit, 16, c);
}

/** A label (muted, left) + value (right-aligned at right) row. */
void labelValue(const Pen& pen, double x, double right, double y, const std::string& label, const std::string& v,
                Color vc = kText, double size = 15) {
    pen.text(x, y, label, size, kTextMuted);
    pen.text(right, y, v, size, vc, false, true);
}

/** "YYYY-MM-DD HH:MM:SS" → "YYYY-MM-DD  HH:MM" or "YYYY-MM-DD  h:MM PM" */
std::string clockText(const std::string& stamp, int format) {
    if (stamp.size() < 16) return stamp.empty() ? "--" : stamp;
    const std::string date = stamp.substr(0, 10);
    int hour = std::atoi(stamp.substr(11, 2).c_str());
    const std::string minute = stamp.substr(14, 2);
    if (format == 12) {
        const char* ampm = hour >= 12 ? "PM" : "AM";
        hour %= 12;
        if (hour == 0) hour = 12;
        return date + "  " + std::to_string(hour) + ":" + minute + " " + ampm;
    }
    return date + "  " + stamp.substr(11, 5);
}

/** "YYYY-MM-DD HH:MM:SS" → "HH:MM" or "h:MM PM" (the simple templates); empty with the clock "Off" or no time yet */
std::string timeOfDayText(const std::string& stamp, int format) {
    if (format == 0 || stamp.size() < 16) return "";
    const std::string text = clockText(stamp, format);
    return text.substr(12);  // after "YYYY-MM-DD  "
}

/** "≈" drawn as two tildes (not every system font has U+2248). @return width */
double drawApprox(const Pen& pen, double x, double baseline, double size, Color c) {
    const double w = size * 0.55;
    const double amp = size * 0.07;
    cairo_save(pen.cr);
    pen.color(c);
    cairo_set_line_width(pen.cr, std::max(1.2, size * 0.08));
    cairo_set_line_cap(pen.cr, CAIRO_LINE_CAP_ROUND);
    for (const double dy : {size * 0.40, size * 0.22}) {
        const double y = baseline - dy;
        cairo_move_to(pen.cr, x + 1, y + amp);
        cairo_curve_to(pen.cr, x + w * 0.35, y - 2 * amp, x + w * 0.65, y + 2 * amp, x + w - 1, y - amp);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
    return w + 2;
}

/** "V × A = W" with N/A parts as "--" */
std::string vawText(double v, double a, double w) {
    return num(v, 2) + " V × " + num(a, 2) + " A = " + num(w, 1) + " W";
}

// ---- Static parts ----

void staticCard(const Pen& pen, double x, double y, int h) {
    drawCard(pen, x, y, kCardW, h, 12, kCard, kCard, 0);
}

void staticGraphBed(const Pen& pen, double x, double y) {
    pen.color(kBg);
    pen.roundedRect(x + kGraphX, y + kTop + 4, kGraphW, kGraphH, 6);
    cairo_fill(pen.cr);
}

/** Frame's extra graphs (Compact off), CPU (k = 0) on the left, GPU (1) on the right. */
GraphArea frameTimesArea(double x, double y, int k, double maxV, const WidgetContext& ctx) {
    return GraphArea {x + kInX + k * (kFrameTimesW + kFrameTimesGap), y + kFrameTimesBedY, kFrameTimesW, kFrameTimesH,
                      0.0, maxV, ctx.now, static_cast<double>(std::max(1, ctx.graphSeconds))};
}

void staticFrameTimesBeds(const Pen& pen, double x, double y) {
    pen.color(kBg);
    for (int k = 0; k < 2; ++k) {
        pen.roundedRect(x + kInX + k * (kFrameTimesW + kFrameTimesGap), y + kFrameTimesBedY, kFrameTimesW,
                        kFrameTimesH, 6);
        cairo_fill(pen.cr);
    }
}

void staticTitle(const Pen& pen, double x, double y, const std::string& title) {
    pen.text(x + kInX, y + kTop + 17, title, 16, kTextMuted);
}

const char* titleOf(const WidgetInstance& w, const WidgetText& t) {
    switch (w.type) {
        case WidgetType::Frame: return t.frame;
        case WidgetType::Cpu: return t.cpu;
        case WidgetType::GpuRam: return t.gpuRam;
        case WidgetType::Power: return t.power;
        case WidgetType::Battery: return t.battery;
        case WidgetType::Temp: return t.temp;
        case WidgetType::CpuItems: return t.cpuItems;
        case WidgetType::RamItems: return t.ramItems;
        case WidgetType::Network: return t.network;
        case WidgetType::General: return t.general;
        case WidgetType::SimpleA: return t.simpleA;
        case WidgetType::SimpleB: return t.simpleB;
        case WidgetType::ColorPresets: return t.ctlColor;
        case WidgetType::Volume: return t.ctlSound;
        case WidgetType::Passthrough: return t.ctlPassthrough;
        case WidgetType::Profiles: return t.ctlProfiles;
        case WidgetType::Browser: return t.ctlBrowser;
        case WidgetType::Record: return t.ctlRecord;
        case WidgetType::None: break;
    }
    return "";
}

bool hasGraph(WidgetType type) {
    return type == WidgetType::Frame || type == WidgetType::GpuRam || type == WidgetType::Power ||
           type == WidgetType::Temp;
}

// ---- Widgets ----

void drawFrame(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
               const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double hz = f(ctx, kFrameDisplayHz);
    const double fps = f(ctx, kFrameAppFps);
    const double targetMs = hz > 0 ? 1000.0 / hz : std::nan("");
    const auto msLevel = [&](double ms) {
        return levelOf(ms, targetMs * th.frameWarnRatio, targetMs * th.frameCritRatio);
    };
    // 2026-11-13: the size the game renders each eye at, small on the title row, right-aligned before the graph;
    // "PC" when the PC helper said it. Nothing while it's N/A
    if (ok(ctx, kFrameRenderWidth) && ok(ctx, kFrameRenderHeight)) {
        std::string size = std::to_string(i(ctx, kFrameRenderWidth)) + "×" + std::to_string(i(ctx, kFrameRenderHeight));
        if (i(ctx, kFrameRenderSource) == static_cast<int64_t>(RenderSource::Pc)) size += std::string(" ") + t.renderPc;
        pen.text(x + kGraphX - 8, top + 17, size, 13, kTextMuted, false, true);
    }
    if (!ok(ctx, kFrameAppFps)) {
        drawBigValue(pen, cx, top + 52, "--", "fps", kTextMuted);
        const Reason why = reasonOf(ctx, kFrameAppFps);
        const std::string note = why == Reason::NoSteamVr ? std::string(t.noSteamVr)
                                 : why == Reason::NoFrames ? std::string(t.waiting)
                                                           : naText(ctx, kFrameAppFps);
        pen.text(cx, top + 72, note, 15, kTextMuted);
    } else {
        const Level fpsLevel = hz > 0 ? levelOf(fps, hz * th.fpsWarnRatio, hz * th.fpsCritRatio, true) : Level::Normal;
        pen.dot(cx + 4, top + 42, kAccent);
        const double bigWidth = drawBigValue(pen, cx + 14, top + 52, num(fps, 0), "fps", colorOf(fpsLevel));
        if (hz > 0) pen.text(cx + 14 + bigWidth + 6, top + 52, "/ " + num(hz, 0) + "Hz", 14, kTextMuted);
        const double gpuMs = f(ctx, kFrameGpuMs);
        const double cpuMs = f(ctx, kFrameCpuMs);
        double lx = cx + 14;
        lx += pen.text(lx, top + 72, "GPU " + num(gpuMs, 1), 15, colorOf(msLevel(gpuMs), kTextMuted));
        lx += pen.text(lx, top + 72, "  CPU " + num(cpuMs, 1), 15, colorOf(msLevel(cpuMs), kTextMuted));
        pen.text(lx, top + 72, " ms", 13, kTextMuted);
        const double reproj = f(ctx, kFrameReprojectedPct);
        const int64_t dropped = i(ctx, kFrameDropped);
        const Level reprojLevel = worse(levelOf(reproj, th.reprojWarnPct, th.reprojCritPct),
                                        dropped > 0 ? Level::Warn : Level::Normal);
        pen.text(cx + 14, top + 90, std::string(t.reproj) + num(reproj, 0) + "%  " + t.dropped + std::to_string(dropped),
                 15, colorOf(reprojLevel, kTextMuted));
    }
    // 0 fps to 25% above the refresh rate
    const double scaleHz = hz > 0 ? hz : 90.0;
    const double maxV = std::ceil(std::fmax(scaleHz, h.a.maxValue()) * 1.25 / 10.0) * 10.0;
    const GraphArea area = graphArea(x, y, 0.0, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "fps");
    drawGuide(pen, area, scaleHz, kTextMuted);
    drawSeries(pen, area, h.a, kAccent);
    const int64_t throttled = i(ctx, kFrameThrottled);
    if (ok(ctx, kFrameAppFps) && throttled > 0) {
        drawBadge(pen, area.x + area.w - 4, area.y + 4, throttleBadgeText(t, scaleHz, static_cast<int>(throttled)),
                  kWarn);
    }
    if (!moreRows(w)) return;

    // CPU and GPU frame times, the worst frame per read, on one scale so the two compare. Dotted line: the budget
    const double budget = std::isnan(targetMs) ? 1000.0 / scaleHz : targetMs;
    const double dataMax = std::fmax(h.c.maxValue(), h.d.maxValue());
    const double msMax = std::ceil(std::fmax(budget * 1.5, std::isnan(dataMax) ? 0.0 : dataMax * 1.15) / 5.0) * 5.0;
    const struct {
        const char* name;
        uint16_t avgId, maxId;
        const History* history;
        Color color;
    } kTimes[] = {{"CPU ", kFrameCpuMs, kFrameCpuMaxMs, &h.c, kAccent}, {"GPU ", kFrameGpuMs, kFrameGpuMaxMs, &h.d, kGpu}};
    for (int k = 0; k < 2; ++k) {
        const GraphArea times = frameTimesArea(x, y, k, msMax, ctx);
        const double avg = f(ctx, kTimes[k].avgId);
        const double worst = f(ctx, kTimes[k].maxId);
        double lx = times.x + 2;
        const double ly = y + kFrameTimesLabelY;
        lx += pen.text(lx, ly, kTimes[k].name, 14, kTextMuted);
        lx += pen.text(lx, ly, num(avg, 1), 14, colorOf(msLevel(avg)), true);
        lx += pen.text(lx, ly, t.msMaxSep, 13, kTextMuted);
        pen.text(lx, ly, num(worst, 1), 14, colorOf(msLevel(worst)), true);
        drawGraphLabel(pen, times, num(msMax, 0) + " ms");
        drawGuide(pen, times, targetMs, kTextMuted);
        drawSeries(pen, times, *kTimes[k].history, kTimes[k].color);
    }
}

/** @return whether cluster k has a clock (a cluster this SoC doesn't have is N/A SensorMissing) */
bool hasCluster(const WidgetContext& ctx, uint16_t k) {
    return ok(ctx, kCpuCluster0Mhz + k) || reasonOf(ctx, kCpuCluster0Mhz + k) != Reason::SensorMissing;
}

/** CPU's height with Compact off: the core bars, then one row per two clusters that exist. */
int cpuFullHeight(const WidgetContext* ctx) {
    int clusters = 4;  // no values yet: the Frame's 4 cpufreq policies (2 rows)
    if (ctx != nullptr) {
        clusters = 0;
        for (uint16_t k = 0; k < kCpuClusterSlots; ++k) {
            if (hasCluster(*ctx, k)) clusters = k + 1;
        }
    }
    if (clusters == 0) return kCpuFullMinH;
    return static_cast<int>(kClusterTop + (clusters - 1) / 2 * kLineH) + 14;
}

/** CPU's extra rows: one bar per core, then each cluster's current / max clock ("capped" under a Playtime cap). */
void drawCpuCores(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double barTop = y + kCoreBarTop;
    const double gap = 10;
    const double barW = (kInR - kInX - gap * (kCpuCoreSlots - 1)) / kCpuCoreSlots;
    for (uint16_t c = 0; c < kCpuCoreSlots; ++c) {
        const uint16_t id = kCpuCore0Pct + c;
        if (!ok(ctx, id) && reasonOf(ctx, id) == Reason::SensorMissing) continue;  // past the last core
        const double bx = cx + c * (barW + gap);
        pen.color(kBg);
        pen.roundedRect(bx, barTop, barW, kCoreBarH, 4);
        cairo_fill(pen.cr);
        const double pct = f(ctx, id);
        if (!std::isnan(pct)) {
            const double fill = std::clamp(pct, 0.0, 100.0) / 100.0 * kCoreBarH;
            pen.color(colorOf(levelOf(pct, th.cpuWarnPct, th.cpuCritPct), kAccent));
            pen.roundedRect(bx, barTop + kCoreBarH - fill, barW, fill, std::min(4.0, fill / 2));
            cairo_fill(pen.cr);
        }
        textCentered(pen, bx + barW / 2, barTop + kCoreBarH + 16, num(pct, 0), 13, kTextMuted, false);
    }
    const double half = (kInR - kInX) / 2;
    for (uint16_t k = 0; k < kCpuClusterSlots; ++k) {
        if (!hasCluster(ctx, k)) continue;
        const double rx = cx + (k % 2) * half;
        const double ry = y + kClusterTop + (k / 2) * kLineH;
        const double cur = f(ctx, kCpuCluster0Mhz + k);
        const double max = f(ctx, kCpuCluster0MaxMhz + k);
        const double hwMax = f(ctx, kCpuCluster0HwMaxMhz + k);
        double lx = rx + pen.text(rx, ry, "C" + std::to_string(k) + " ", 14, kTextMuted);
        lx += pen.text(lx, ry, num(cur / 1000.0, 2), 15, kText, true);
        lx += pen.text(lx, ry, "/" + num(max / 1000.0, 2) + "GHz", 14, kTextMuted);
        if (!std::isnan(max) && !std::isnan(hwMax) && max < hwMax - 1.0) {
            pen.text(lx + 6, ry, std::string(t.capped) + num(hwMax / 1000.0, 2), 12, kWarn, true);
        }
    }
}

void drawCpu(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    if (moreRows(w)) drawCpuCores(pen, x, y, ctx);
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double top = y + kTop;
    // The fixed panel's CPU line: "5%  (max core 43%)  3.05GHz" (usage, the busiest core, the fastest cluster)
    double lx = x + kInX + 8;
    const double ly = top + 46;
    if (ok(ctx, kCpuUsagePct)) {
        lx += pen.text(lx, ly, num(f(ctx, kCpuUsagePct), 0) + "%", 17, kText, true);
    } else {
        lx += pen.text(lx, ly, naText(ctx, kCpuUsagePct), 15, kTextMuted);
    }
    const double maxCore = f(ctx, kCpuMaxCorePct);
    lx += pen.text(lx, ly, t.maxCoreOpen, 15, kTextMuted);
    lx += pen.text(lx, ly, num(maxCore, 0) + "%", 15, colorOf(levelOf(maxCore, th.cpuWarnPct, th.cpuCritPct)));
    lx += pen.text(lx, ly, t.maxCoreClose, 15, kTextMuted);
    pen.text(lx + 4, ly, num(f(ctx, kCpuFastestClockMhz) / 1000.0, 2) + "GHz", 16, kText);
}

void drawGpuRam(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double busy = f(ctx, kGpuBusyPct);
    pen.dot(cx + 4, top + 42, kGpu);
    if (ok(ctx, kGpuBusyPct)) {
        const double w = drawBigValue(pen, cx + 14, top + 52, num(busy, 0), "%",
                                      colorOf(levelOf(busy, th.gpuWarnPct, th.gpuCritPct)));
        pen.text(cx + 14 + w + 6, top + 52, t.gpuLoad, 13, kTextMuted);
    } else {
        drawBigValue(pen, cx + 14, top + 52, "--", "%", kTextMuted);
        pen.text(cx + 14 + 60, top + 52, naText(ctx, kGpuBusyPct), 12, kTextMuted);
    }
    pen.text(cx + 14, top + 72,
             std::string(t.clock) + num(f(ctx, kGpuClockMhz), 0) + " / " + num(f(ctx, kGpuMaxClockMhz), 0) + " MHz", 15,
             kTextMuted);
    const double used = f(ctx, kRamUsedGib);
    const double total = f(ctx, kRamTotalGib);
    double lx = cx + 14 + pen.text(cx + 14, top + 90, t.ram, 15, kTextMuted);
    lx += pen.text(lx, top + 90, num(used, 1), 15, kText, true);
    pen.text(lx, top + 90, " / " + num(total, 1) + " GiB", 15, kTextMuted);
    const GraphArea area = graphArea(x, y, 0.0, 100.0, ctx);
    drawGraphLabel(pen, area, "100%");
    drawSeriesFill(pen, area, h.a, kGpu, kGpuFillAlpha);
    drawGuide(pen, area, th.gpuWarnPct, kWarn);
    drawSeries(pen, area, h.a, kGpu);
}

void drawPower(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double vph = f(ctx, kPowerVphW);
    pen.dot(cx + 4, top + 42, kAccent);
    if (ok(ctx, kPowerVphW)) {
        drawBigValue(pen, cx + 14, top + 52, num(vph, 2), "W", colorOf(levelOf(vph, th.powerWarnW, th.powerCritW)));
    } else {
        drawBigValue(pen, cx + 14, top + 52, "--", "W", kTextMuted);
        pen.text(cx + 14, top + 90, naText(ctx, kPowerVphW), 14, kTextMuted);
    }
    pen.text(cx + 14, top + 72, std::string(t.rails) + num(f(ctx, kPowerRailsW), 2) + " W", 15, kTextMuted);

    const double maxData = h.a.maxValue();
    const double maxV = std::isnan(maxData) ? 5.0 : std::max(2.0, std::ceil(maxData * 1.2));
    const GraphArea area = graphArea(x, y, 0.0, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "W");
    drawGuide(pen, area, th.powerWarnW, kWarn);
    drawGuide(pen, area, th.powerCritW, kDanger);
    drawSeries(pen, area, h.a, kAccent);
}

std::string statusText(const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    switch (static_cast<BatteryStatus>(i(ctx, kBatteryStatus))) {
        case BatteryStatus::Charging: return t.statusCharging;
        case BatteryStatus::Discharging: return t.statusDischarging;
        case BatteryStatus::Full: return t.statusFull;
        case BatteryStatus::NotCharging: return i(ctx, kBatteryChargerOnline) != 0 ? t.statusPlugged : t.statusIdle;
        case BatteryStatus::Unknown: break;
    }
    return "";
}

void drawBattery(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    const double pct = f(ctx, kBatteryPct);
    pen.dot(cx + 4, top + 42, kAccent);
    double bx = cx + 14;
    if (ok(ctx, kBatteryPct)) {
        bx += drawBigValue(pen, bx, top + 52, num(pct, 0), "%",
                           colorOf(levelOf(pct, th.batteryWarnPct, th.batteryCritPct, true)));
        bx += 8;
        if (static_cast<BatteryStatus>(i(ctx, kBatteryStatus)) == BatteryStatus::Charging) {
            bx += drawBolt(pen, bx, top + 50, kWarn) + 4;
        }
        pen.text(bx, top + 50, statusText(ctx), 15, kTextMuted);
    } else {
        drawBigValue(pen, bx, top + 52, "--", "%", kTextMuted);
        pen.text(bx + 70, top + 50, naText(ctx, kBatteryPct), 14, kTextMuted);
    }
    const double watts = f(ctx, kBatteryPowerW);
    std::string flow = watts < 0 ? t.flowOut : t.flowIn;
    if (!flow.empty()) flow[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(flow[0])));
    pen.dot(cx + 4, top + 67, kGpu);
    pen.text(cx + 14, top + 72, std::isnan(watts) ? "--" : num(std::fabs(watts), 2) + " W " + flow, 15, kText);
    if (w.compact) return;  // % and the W into / from the battery
    pen.text(cx + 14, top + 88, t.batteryNote, 13, kTextMuted);
    labelValue(pen, cx + 8, right, top + 106 + kRowH, !std::isnan(watts) && watts < 0 ? t.flowOut : t.flowIn,
               vawText(f(ctx, kBatteryVoltageV), std::fabs(f(ctx, kBatteryCurrentMa)) / 1000.0, std::fabs(watts)),
               kText, 14);
}

void drawTemp(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
              const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double cpu = f(ctx, kTempCpuC);
    const double gpu = f(ctx, kTempGpuC);
    pen.dot(cx + 4, top + 42, kAccent);
    drawBigValue(pen, cx + 14, top + 52, num(cpu, 0), t.cpuTempUnit, colorOf(levelOf(cpu, th.tempWarnC, th.tempCritC)));
    pen.dot(cx + 4, top + 67, kGpu);
    const double gw = pen.text(cx + 14, top + 72, "GPU " + num(gpu, 0) + t.celsius, 15,
                               colorOf(levelOf(gpu, th.tempWarnC, th.tempCritC)));
    if (!w.compact)  // Compact is CPU / GPU / fan and the graph: no DDR, no other temperatures
        pen.text(cx + 14 + gw + 10, top + 72, std::string(t.tDdr) + " " + num(f(ctx, kTempDdrC), 0) + t.celsius, 15,
                 kTextMuted);
    pen.text(cx + 14, top + 90, std::string(t.fan) + grouped(f(ctx, kFanRpm)) + " rpm", 15, kTextMuted);

    const bool cpuThrottle = i(ctx, kThrottleCpu) != 0;
    const bool gpuThrottle = i(ctx, kThrottleGpu) != 0;
    const std::string throttleText =
        std::string(t.thermalLimit) + (cpuThrottle && gpuThrottle ? t.cpuAndGpu : (cpuThrottle ? "CPU" : "GPU"));

    const double dataMin = std::fmin(h.a.minValue(), h.b.minValue());
    const double dataMax = std::fmax(h.a.maxValue(), h.b.maxValue());
    const double minV = std::isnan(dataMin) ? 30.0 : std::min(30.0, std::floor(dataMin / 5.0) * 5.0 - 5.0);
    const double maxV = std::isnan(dataMax) ? th.tempCritC + 5.0 : std::max(th.tempCritC + 5.0, dataMax + 5.0);
    const GraphArea area = graphArea(x, y, minV, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + t.celsius);
    drawGuide(pen, area, th.tempWarnC, kWarn);
    drawGuide(pen, area, th.tempCritC, kDanger);
    drawSeries(pen, area, h.b, kGpu);
    drawSeries(pen, area, h.a, kAccent);

    // The other temperatures, three per row (full card only)
    const struct {
        const char* label;
        uint16_t id;
    } kSpots[kTempSpots] = {
        {t.tBattery, kTempBatteryC}, {t.tDisplay, kTempDisplayC}, {t.tExhaust, kTempExhaustC},
        {t.tHeatsink, kTempHeatsinkC},
    };
    const double colW = (kInR - kInX) / 3;
    for (int k = 0; k < (w.compact ? 0 : kTempSpots); ++k) {
        const double sx = cx + (k % 3) * colW;
        const double sy = top + 114 + (k / 3) * kLineH;
        const double v = f(ctx, kSpots[k].id);
        labelValue(pen, sx + 8, sx + colW - 10, sy, kSpots[k].label, num(v, 0) + t.celsius,
                   colorOf(levelOf(v, th.tempWarnC, th.tempCritC)), 14);
    }

    if (cpuThrottle || gpuThrottle) {
        // The fixed panel framed the whole panel in red; Temp owns the throttle flags, so its card gets the frame
        strokeRounded(pen, x, y, kCardW, widgetHeight(w), 12, kDanger, 3.0);
        drawBadge(pen, area.x + area.w - 4, area.y + 4, throttleText, kDanger);
    }
}

bool networkExpanded(const WidgetContext& ctx) {
    return i(ctx, kNetInterfaceUp) != 0 && i(ctx, kNetConnected) != 0;
}

void drawNetwork(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    const bool up = ok(ctx, kNetInterfaceUp) && i(ctx, kNetInterfaceUp) != 0;
    const bool connected = up && i(ctx, kNetConnected) != 0;
    // Which link, after the title: the dongle if it has a peer, otherwise home Wi-Fi
    const bool dongle = connected && i(ctx, kNetHomeWifi) == 0;
    pen.text(cx + pen.measure(t.network, 16) + 8, top + 17, dongle ? t.linkDongle : t.linkHome, 13, kTextMuted);
    if (!ok(ctx, kNetInterfaceUp)) {  // not read (yet), or the read failed
        drawWifiIcon(pen, right - 26, top + 18, 0, kTextMuted, true);
        pen.text(cx + 8, top + 44, naText(ctx, kNetInterfaceUp), 15, kTextMuted);
        return;
    }
    if (!up) {  // no dongle AP and no home Wi-Fi station: like the fixed panel, "Not connected"
        drawWifiIcon(pen, right - 26, top + 18, 0, kTextMuted, true);
        pen.text(cx + 8, top + 44, t.notConnected, 16, kTextMuted);
        return;
    }
    const double dbm = f(ctx, kNetSignalDbm);
    const Level signalLevel = levelOf(dbm, th.wifiWarnDbm, th.wifiCritDbm, true);
    // Signal: icon + dBm at the right of the title row
    std::string signal = std::isnan(dbm) ? "--" : num(std::fabs(dbm), 0);
    if (!std::isnan(dbm) && dbm < 0) signal = "−" + signal;
    const double sw = connected ? pen.text(right, top + 17, signal + " dBm", 13, colorOf(signalLevel, kTextMuted), false, true)
                                : 0.0;
    drawWifiIcon(pen, right - sw - 32, top + 18, connected ? wifiBars(dbm, th) : 0,
                 connected ? colorOf(signalLevel) : kTextMuted, !connected);
    double lx = cx + 8;
    const double ry = top + 44;
    if (!connected) {
        pen.text(lx, ry, t.notConnected, 16, kTextMuted);
        return;
    }
    // Traffic (peer → Frame first: with Steam Link the video comes this way). Each value carries its unit
    const RateText down = formatRate(f(ctx, kNetRxMbps));
    const RateText upRate = formatRate(f(ctx, kNetTxMbps));
    lx += drawArrow(pen, lx, ry, false, kTextMuted);
    lx += pen.text(lx, ry, down.value, 17, kText, true);
    lx += pen.text(lx, ry, down.unit, 14, kTextMuted);
    lx += 8;
    lx += drawArrow(pen, lx, ry, true, kTextMuted);
    lx += pen.text(lx, ry, upRate.value, 15, kText);
    pen.text(lx, ry, upRate.unit, 14, kTextMuted);
    pen.text(cx + 8, top + 68,
             std::string(t.link) + num(f(ctx, kNetTxLinkMbps), 0) + "/" + num(f(ctx, kNetRxLinkMbps), 0) + " Mbps", 14,
             kTextMuted);
}

/** "L 80%⚡" style. @return width */
double drawController(const Pen& pen, double x, double y, const char* label, uint16_t pctId, uint16_t chargingId,
                      const WidgetContext& ctx) {
    const double start = x;
    x += pen.text(x, y, label, 15, kTextMuted);
    if (!ok(ctx, pctId)) return x - start + pen.text(x, y, "--", 15, kTextMuted);
    const double pct = f(ctx, pctId);
    const Thresholds& th = ctx.thresholds;
    x += pen.text(x, y, num(pct, 0) + "%", 16, colorOf(levelOf(pct, th.controllerWarnPct, th.controllerCritPct, true)), true);
    if (i(ctx, chargingId) != 0) x += 2 + drawBolt(pen, x + 2, y, kWarn);
    return x - start;
}

void drawGeneral(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX + 8;
    const double right = x + kInR;
    const double top = y + kTop;
    const double valueX = cx + 110;
    double ry = top + 20 + kLineH;
    pen.text(cx, ry, t.controllers, 15, kTextMuted);
    double lx = valueX;
    lx += drawController(pen, lx, ry, t.left, kCtrlLeftPct, kCtrlLeftCharging, ctx) + 16;
    drawController(pen, lx, ry, t.right, kCtrlRightPct, kCtrlRightCharging, ctx);
    // Clock "Off" on the dashboard (clock_format 0) hides the Time row
    if (ctx.clockFormat != 0) {
        ry += kLineH;
        pen.text(cx, ry, t.time, 15, kTextMuted);
        pen.text(valueX, ry, clockText(text(ctx, kClock), ctx.clockFormat), 16, kText, true);
    }
    if (!moreRows(w)) return;
    // Compact off: uptime, the app in front, and what this app costs
    ry += kLineH;
    pen.text(cx, ry, t.uptime, 15, kTextMuted);
    pen.text(valueX, ry, ok(ctx, kUptimeS) ? uptimeText(t, static_cast<double>(i(ctx, kUptimeS))) : naText(ctx, kUptimeS),
             15, kText);
    ry += kLineH;
    pen.text(cx, ry, t.app, 15, kTextMuted);
    // The name, else the app key (a running app without a name); "none" only when no scene app runs (pid 0)
    std::string app = text(ctx, kSceneAppName);
    if (app.empty()) app = text(ctx, kSceneAppKey);
    if (app.empty()) app = ok(ctx, kScenePid) && i(ctx, kScenePid) == 0 ? t.noApp : naText(ctx, kSceneAppName);
    pen.text(valueX, ry, ellipsize(pen, app, 15, right - valueX, false), 15, kText);
    ry += kLineH;
    pen.text(cx, ry, t.hudCost, 15, kTextMuted);
    // Own power is an estimate (own CPU share × CPU rails): always shown with "≈"
    double hx = valueX;
    hx += pen.text(hx, ry, num(f(ctx, kSelfCpuPct), 1) + "% CPU  ·  " + num(f(ctx, kSelfRssMib), 0) + " MiB  ·  ", 15,
                   kText);
    hx += drawApprox(pen, hx, ry, 15, kText);
    pen.text(hx + 2, ry, num(f(ctx, kSelfPowerW), 2) + " W", 15, kText);
}

void drawProcessList(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    const bool cpu = w.type == WidgetType::CpuItems;
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    pen.text(right, top + 17, cpu ? t.pctOfCore : t.rss, 13, kTextMuted, false, true);
    const uint16_t rowsId = cpu ? kCpuItemsRows : kRamItemsRows;
    if (ctx.processes == nullptr || !ok(ctx, rowsId)) {
        const bool first = reasonOf(ctx, rowsId) == Reason::NotSampledYet;
        pen.text(cx + 8, top + kListTop + kRowH, first ? std::string(t.waiting) : naText(ctx, rowsId), 14, kTextMuted);
        return;
    }
    const std::vector<ProcessItem>& rows = cpu ? ctx.processes->byCpu : ctx.processes->byRam;
    // A bad hide_rules.json edit takes the last row: the rules in use are the last ones that loaded
    const bool hint = w.hideSystem && ctx.processes->hideRulesError;
    if (hint) {
        const double ry = top + kListTop + kRowH * static_cast<double>(std::max<uint32_t>(w.maxItems, 1));
        pen.text(cx + 8, ry, ellipsize(pen, t.hideRulesError, 13, right - (cx + 8), false), 13, kTextMuted);
    }
    // The HUD isn't scrollable: the top rows that fit
    const size_t n = std::min<size_t>(rows.size(), hint && w.maxItems > 0 ? w.maxItems - 1 : w.maxItems);
    for (size_t k = 0; k < n; ++k) {
        const ProcessItem& r = rows[k];
        const double ry = top + kListTop + kRowH * static_cast<double>(k + 1);
        const std::string v = cpu ? num(r.cpuCorePct, 1) + "%" : num(r.rssMiB, 0) + " MiB";
        const double vw = pen.text(right, ry, v, 14, kText, false, true);
        std::string name = r.name;
        if (r.count > 1) name += " ×" + std::to_string(r.count);
        pen.text(cx + 8, ry, ellipsize(pen, name, 14, right - vw - 12 - (cx + 8), false), 14, k == 0 ? kText : kTextMuted);
    }
}

// ---- The simple templates (Frame Dock): the approved mockup's look, larger type than the cards above ----

constexpr double kSimpleARow1H = 104;  // frame: fps / Hz, reprojection, state pill
constexpr double kSimpleARowH = 56;    // CPU / GPU / RAM, power / battery
constexpr double kSimpleAGap = 8;
constexpr int kSimpleAH = static_cast<int>(kSimpleARow1H + 2 * (kSimpleAGap + kSimpleARowH));
constexpr int kSimpleBH = 100;  // fps, then battery

/** "label value unit" on one line, value bold. @return the x after it */
double simplePair(const Pen& pen, double x, double baseline, const std::string& label, const std::string& value,
                  const std::string& unit, Color valueColor) {
    x += pen.text(x, baseline, label + " ", 22, kTextMuted);
    x += pen.text(x, baseline, value, 26, valueColor, true);
    return x + 3 + pen.text(x + 3, baseline, unit, 20, kTextMuted);
}

/** @return the width simplePair would take */
double simplePairWidth(const Pen& pen, const std::string& label, const std::string& value, const std::string& unit) {
    return pen.measure(label + " ", 22) + pen.measure(value, 26, true) + 3 + pen.measure(unit, 20);
}

/** The app's fps against the display's refresh rate: its level (fps thresholds), Normal when unknown. */
Level fpsLevel(const WidgetContext& ctx) {
    const double hz = f(ctx, kFrameDisplayHz);
    if (!(hz > 0) || !ok(ctx, kFrameAppFps)) return Level::Normal;
    return levelOf(f(ctx, kFrameAppFps), hz * ctx.thresholds.fpsWarnRatio, hz * ctx.thresholds.fpsCritRatio, true);
}

/**
 * The big fps reading shared by both templates: dot, number, "fps", "/ 90Hz" (or the reason it's missing).
 * @return the x after it
 */
double simpleFps(const Pen& pen, double x, double baseline, double dotY, double size, const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    const double hz = f(ctx, kFrameDisplayHz);
    const bool have = ok(ctx, kFrameAppFps);
    const Level level = fpsLevel(ctx);
    pen.color(have ? colorOf(level, kAccent) : kTextMuted);
    cairo_arc(pen.cr, x + 6, dotY, 5, 0, 2 * M_PI);
    cairo_fill(pen.cr);
    double tx = x + 18;
    tx += pen.text(tx, baseline, have ? num(f(ctx, kFrameAppFps), 0) : "--", size, have ? colorOf(level) : kTextMuted, true);
    tx += 6;
    tx += pen.text(tx, baseline, "fps", size * 0.5 + 1, have ? kText : kTextMuted);
    if (have && hz > 0) return tx + 8 + pen.text(tx + 8, baseline, "/ " + num(hz, 0) + "Hz", size * 0.4 + 1.6, kTextMuted);
    if (have) return tx;
    const Reason why = reasonOf(ctx, kFrameAppFps);
    const std::string note = why == Reason::NoSteamVr ? std::string(t.noSteamVr)
                             : why == Reason::NoFrames ? std::string(t.waiting)
                                                       : naText(ctx, kFrameAppFps);
    return tx + 10 + pen.text(tx + 10, baseline, note, 16, kTextMuted);
}

/**
 * "배터리 78 %" (both templates): the % coloured by the battery thresholds (also while charging, so a low battery
 * still shows), then its state while it fits before right. On battery, the usual case, the state is the time left
 * ("약 40분", once known, "약 2.5시간" where that doesn't fit; yellow / red with the %); charging is a green bolt and "충전 중"; full and plugged in stay
 * muted.
 */
void simpleBattery(const Pen& pen, double x, double baseline, double right, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double pct = f(ctx, kBatteryPct);
    const double nx = simplePair(pen, x, baseline, t.battery, num(pct, 0), "%",
                                 colorOf(levelOf(pct, th.batteryWarnPct, th.batteryCritPct, true)));
    std::string state;
    Color stateColor = kTextMuted;
    bool bolt = false;
    switch (static_cast<BatteryStatus>(i(ctx, kBatteryStatus))) {
        case BatteryStatus::Charging:
            state = t.statusCharging;
            stateColor = kSuccess;
            bolt = true;
            break;
        case BatteryStatus::Full: state = t.simpleFull; break;
        case BatteryStatus::NotCharging: state = i(ctx, kBatteryChargerOnline) != 0 ? t.statusPlugged : t.statusIdle; break;
        case BatteryStatus::Discharging:
            // The time left (once known), in the %'s colour while low
            state = remainText(t, f(ctx, kBatteryRemainMin));
            if (ok(ctx, kBatteryPct)) stateColor = colorOf(levelOf(pct, th.batteryWarnPct, th.batteryCritPct, true), kTextMuted);
            break;
        case BatteryStatus::Unknown: break;
    }
    if (!ok(ctx, kBatteryPct) || state.empty()) return;
    // A narrow widget (Simple B) with a long time: the shorter form ("약 2.5시간") rather than the full one shrunk
    // below 18 px (it would jump in size round the hour)
    if (static_cast<BatteryStatus>(i(ctx, kBatteryStatus)) == BatteryStatus::Discharging &&
        right - (nx + 8) < pen.measure(state, 18)) {
        state = remainTextShort(t, f(ctx, kBatteryRemainMin));
    }
    double sx = nx + 8;
    if (bolt && right - sx >= 10 + 4 + pen.measure(state, 14)) sx += drawBolt(pen, sx, baseline, kSuccess) + 4;
    const double room = right - sx;
    if (room >= pen.measure(state, 14)) pen.text(sx, baseline, state, fitSize(pen, state, 20, 14, room, false), stateColor);
}

void staticSimpleA(const Pen& pen, double x, double y, const WidgetText& t) {
    const double w = kCardW;
    drawCard(pen, x, y, w, kSimpleARow1H, 12, kCard, kCard, 0);
    drawCard(pen, x, y + kSimpleARow1H + kSimpleAGap, w, kSimpleARowH, 12, kCard, kCard, 0);
    drawCard(pen, x, y + kSimpleARow1H + 2 * kSimpleAGap + kSimpleARowH, w, kSimpleARowH, 12, kCard, kCard, 0);
    pen.text(x + 14, y + 30, t.frame, 22, kTextMuted);
}

void drawSimpleA(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double right = x + kCardW - 14;
    // Row 1: fps / Hz, the state pill (top right), reprojection (bottom right)
    simpleFps(pen, x + 14, y + 84, y + 72, 46, ctx);
    double clockRight = right;  // the time of day, left of the state pill when there is one
    if (ok(ctx, kFrameAppFps)) {
        const int throttled = static_cast<int>(i(ctx, kFrameThrottled));
        const std::string state = simpleThrottleText(t, throttled);
        const double pw = pen.measure(state, 20, true) + 28;
        clockRight = right - pw - 14;
        pen.color(throttled > 0 ? kWarn : kSuccess);
        pen.roundedRect(right - pw, y + 14, pw, 34, 17);
        cairo_fill(pen.cr);
        pen.text(right - pw + 14, y + 38, state, 20, kOnAccent, true);
        const double reproj = f(ctx, kFrameReprojectedPct);
        const std::string value = num(reproj, 0);
        simplePair(pen, right - simplePairWidth(pen, t.simpleReproj, value, "%"), y + 84, t.simpleReproj, value, "%",
                   colorOf(levelOf(reproj, th.reprojWarnPct, th.reprojCritPct)));
    }
    pen.text(clockRight, y + 38, timeOfDayText(text(ctx, kClock), ctx.clockFormat), 20, kTextMuted, false, true);
    // The render size (2026-11-14), like the Frame card's: after the static title, muted, "PC" when the PC helper
    // said it
    if (ok(ctx, kFrameRenderWidth) && ok(ctx, kFrameRenderHeight)) {
        std::string size = std::to_string(i(ctx, kFrameRenderWidth)) + "×" + std::to_string(i(ctx, kFrameRenderHeight));
        if (i(ctx, kFrameRenderSource) == static_cast<int64_t>(RenderSource::Pc)) size += std::string(" ") + t.renderPc;
        pen.text(x + 14 + pen.measure(t.frame, 22) + 12, y + 30, size, 16, kTextMuted);
    }
    // Row 2: CPU %, GPU % (in the GPU blue), RAM used / total
    double ry = y + kSimpleARow1H + kSimpleAGap;
    // CPU: the number is the total (the approved mockup's "CPU 16%"), the colour the busiest core, which is what
    // cpu_warn_pct / cpu_crit_pct are for (the Cpu widget colours its "max core" the same way). A game held back by
    // one busy thread turns it yellow / red while the total stays low; the total alone would hardly ever reach 85 %
    const double cpu = f(ctx, kCpuUsagePct);
    const double busiestCore = f(ctx, kCpuMaxCorePct);
    double nx = simplePair(pen, x + 16, ry + 38, "CPU", num(cpu, 0), "%",
                           colorOf(levelOf(busiestCore, th.cpuWarnPct, th.cpuCritPct)));
    const double gpu = f(ctx, kGpuBusyPct);
    nx = simplePair(pen, nx + 22, ry + 38, "GPU", num(gpu, 0), "%",
                    colorOf(levelOf(gpu, th.gpuWarnPct, th.gpuCritPct), std::isnan(gpu) ? kText : kGpu));
    simplePair(pen, nx + 22, ry + 38, t.simpleRam, num(f(ctx, kRamUsedGib), 1), "/" + num(f(ctx, kRamTotalGib), 1) + "G",
               kText);
    // Row 3: power (W), battery % and its state
    ry += kSimpleARowH + kSimpleAGap;
    const double watts = f(ctx, kPowerVphW);
    nx = simplePair(pen, x + 16, ry + 38, t.power, num(watts, !std::isnan(watts) && watts >= 10 ? 1 : 2), "W",
                    colorOf(levelOf(watts, th.powerWarnW, th.powerCritW)));
    simpleBattery(pen, nx + 22, ry + 38, right, ctx);
}

void drawSimpleB(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const double right = x + kSimpleBWidth - 16 - 12;
    simpleFps(pen, x + 14, y + 46, y + 32, 44, ctx);
    // The time of day in the top right corner, above "/ 90Hz"
    pen.text(right, y + 22, timeOfDayText(text(ctx, kClock), ctx.clockFormat), 16, kTextMuted, false, true);
    // Under it the battery, as on Simple A's last row (yellow / red at Perf's battery thresholds)
    simpleBattery(pen, x + 16, y + 84, right, ctx);
}

// ---- The controls (Frame Dock): a title line and one row of buttons the Dock's laser presses ----

constexpr int kControlH = 100;
constexpr double kCtlRowY = 32;   // the buttons' top, from the card's
constexpr double kCtlRowH = 58;   // ~1.6 cm at 14 cm a column: a laser hits it
constexpr double kCtlGap = 8;
constexpr double kSegInset = 4;   // a segmented pill's selected part, inside its border

/** Buttons side by side across the card by weight; a negative id is room left for something else (the level) */
std::vector<ControlButton> buttonRow(double x, double y, const std::vector<std::pair<int, double>>& parts, double gap) {
    std::vector<ControlButton> out;
    double weights = 0;
    for (const auto& p : parts) weights += p.second;
    const double room = (kInR - kInX) - gap * static_cast<double>(parts.size() - 1);
    double bx = x + kInX;
    for (const auto& p : parts) {
        const double w = room * p.second / weights;
        out.push_back({p.first, bx, y + kCtlRowY, w, kCtlRowH, true});
        bx += w + gap;
    }
    return out;
}

/** The segments of a pill (gap 0): hit areas are the whole segment */
std::vector<ControlButton> segmentRow(double x, double y, int count) {
    std::vector<std::pair<int, double>> parts;
    for (int i = 0; i < count; ++i) parts.push_back({i, 1});
    std::vector<ControlButton> out = buttonRow(x, y, parts, 0);
    // The pill's border: the segments sit kSegInset inside it
    const double inner = (kInR - kInX - 2 * kSegInset) / std::max(1, count);
    for (int i = 0; i < count; ++i) {
        out[static_cast<size_t>(i)].x = x + kInX + kSegInset + inner * i;
        out[static_cast<size_t>(i)].w = inner;
    }
    return out;
}

void ctlLabel(const Pen& pen, const ControlButton& b, const std::string& label, double size, Color c) {
    const double fit = fitSize(pen, label.c_str(), size, size * 0.7, b.w - 14, true);
    const double w = pen.measure(label, fit, true);
    pen.text(b.x + (b.w - w) / 2, centerBaseline(b.y, b.h, fit), label, fit, c, true);
}

/** A plain button: accent when selected, a little lighter while held, dim when it can't be pressed */
void ctlButton(const Pen& pen, const ControlButton& b, const std::string& label, bool selected, bool held) {
    if (selected) {
        drawCard(pen, b.x, b.y, b.w, b.h, 14, kAccent, kAccent, 0);
    } else {
        drawCard(pen, b.x, b.y, b.w, b.h, 14, held ? kControlHover : kControl, kBorder, b.usable ? 1.5 : 0);
    }
    ctlLabel(pen, b, label, 22, selected ? kOnAccent : b.usable ? kText : kTextDisabled);
}

/** A segmented pill: the border round all, the selected segment filled */
void ctlSegments(const Pen& pen, double x, double y, const std::vector<ControlButton>& segs,
                 const std::vector<std::string>& labels, int selected, int held) {
    const double px = x + kInX, py = y + kCtlRowY, pw = kInR - kInX;
    drawCard(pen, px, py, pw, kCtlRowH, kCtlRowH / 2, kControl, kBorder, 1.5);
    for (size_t i = 0; i < segs.size(); ++i) {
        const ControlButton& b = segs[i];
        const ControlButton inner {b.id, b.x, b.y + kSegInset, b.w, b.h - 2 * kSegInset, b.usable};
        const bool sel = static_cast<int>(i) == selected;
        if (sel || static_cast<int>(i) == held) {
            drawCard(pen, inner.x, inner.y, inner.w, inner.h, inner.h / 2, sel ? kAccent : kControlHover,
                     kAccent, 0);
        }
        ctlLabel(pen, inner, i < labels.size() ? labels[i] : std::string(), 20,
                 sel ? kOnAccent : b.usable ? kText : kTextDisabled);
    }
}

void drawControl(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    const ControlsView empty;
    const ControlsView& v = ctx.controls != nullptr ? *ctx.controls : empty;
    const std::vector<ControlButton> buttons = controlButtons(w, ctx.controls, x, y);
    const int held = v.pressedType == w.type ? v.pressedButton : -1;
    // The line right of the title: what is in use, or why nothing can be pressed
    std::string status;
    Color statusColor = kTextMuted;
    const double titleRight = x + kInX + pen.measure(titleOf(w, t), 16) + 16;
    switch (w.type) {
        case WidgetType::ColorPresets:
            if (v.presetInUse >= 0 && v.presetInUse < 3) status = v.presetNames[v.presetInUse];
            break;
        case WidgetType::Volume:
            status = !v.soundKnown ? t.ctlUnknown
                                   : v.outputName + " · " + (v.muted ? std::string(t.ctlMute) : std::to_string(v.volume) + "%");
            break;
        case WidgetType::Profiles:
            if (v.armedProfile >= 0) {
                status = t.ctlPressAgain;
                statusColor = kAccentText;
            } else if (v.activeProfile >= 0 && v.activeProfile < static_cast<int>(v.profileNames.size())) {
                status = t.ctlInUse + v.profileNames[static_cast<size_t>(v.activeProfile)];
            }
            break;
        case WidgetType::Browser:
            status = v.chromeRunning ? t.ctlChromeOn : t.ctlChromeOff;
            statusColor = v.chromeRunning ? kSuccess : kTextMuted;
            break;
        case WidgetType::Record:
            switch (v.recordState) {
                case 1:
                    status = recordElapsedText(v.recordSeconds);
                    statusColor = kDanger;
                    break;
                case 2: status = t.ctlRecordSaving; break;
                case 3:
                    status = t.ctlRecordFailed;
                    statusColor = kDanger;
                    break;
                case 4:
                    status = t.ctlRecordBusy;
                    statusColor = kWarn;
                    break;
                case 5:
                    status = t.ctlRecordClipSaved;
                    statusColor = kSuccess;
                    break;
                case 6:
                    status = t.ctlRecordRefused;
                    statusColor = kWarn;
                    break;
                default: break;
            }
            break;
        default: break;
    }
    if (!v.interactive) {
        status = t.ctlNeedsDock;
        statusColor = kTextMuted;
    }
    if (!status.empty()) {
        pen.text(x + kInR, y + kTop + 17, ellipsize(pen, status, 15, x + kInR - titleRight, false), 15, statusColor,
                 false, true);
    }
    switch (w.type) {
        case WidgetType::ColorPresets:
            for (const ControlButton& b : buttons) {
                if (b.id < 3) {
                    const std::string& name = v.presetNames[b.id];
                    ctlButton(pen, b, name.empty() ? "-" : name, v.presetInUse == b.id, held == b.id);
                } else {
                    ctlButton(pen, b, v.comparing ? t.ctlComparing : t.ctlCompare, v.comparing, held == b.id);
                }
            }
            break;
        case WidgetType::Volume: {
            for (const ControlButton& b : buttons) {
                if (b.id < 0) {
                    // The level between − and +: a track, the filled part and a knob
                    const double tx = b.x + 12, tw = b.w - 24, cy = b.y + b.h / 2;
                    const double level = v.soundKnown ? std::clamp(v.volume, 0, 100) / 100.0 : 0;
                    drawCard(pen, tx, cy - 5, tw, 10, 5, kTextDisabled, kTextDisabled, 0);
                    if (level > 0) drawCard(pen, tx, cy - 5, tw * level, 10, 5, v.muted ? kTextMuted : kAccent, kAccent, 0);
                    pen.color(v.muted || !v.soundKnown ? kTextMuted : kAccent);
                    cairo_arc(pen.cr, tx + tw * level, cy, 12, 0, 2 * M_PI);
                    cairo_fill(pen.cr);
                    continue;
                }
                const char* label = b.id == 0 ? t.ctlMute : b.id == 1 ? "−" : "+";
                ctlButton(pen, b, label, b.id == 0 && v.muted, held == b.id);
            }
            break;
        }
        case WidgetType::Passthrough:
            if (v.monoColorUsable) {
                ctlSegments(pen, x, y, buttons, {t.ctlOff, t.ctlMono, t.ctlColorView}, v.passthrough, held);
            } else {
                // Without Camera Switch only on / off (as the 기기 page): on is whatever the room view shows
                ctlSegments(pen, x, y, buttons, {t.ctlOff, t.ctlOn}, v.passthrough < 0 ? -1 : std::min(v.passthrough, 1),
                            held);
            }
            break;
        case WidgetType::Profiles:
            if (buttons.empty()) {
                drawCard(pen, x + kInX, y + kCtlRowY, kInR - kInX, kCtlRowH, kCtlRowH / 2, kControl, kBorder, 1.5);
            } else {
                ctlSegments(pen, x, y, buttons, v.profileNames, v.activeProfile, held >= 0 ? held : v.armedProfile);
            }
            break;
        case WidgetType::Browser:
            for (const ControlButton& b : buttons) ctlButton(pen, b, v.browserOpen ? t.ctlClose : t.ctlOpen, false, held == b.id);
            break;
        case WidgetType::Record:
            for (const ControlButton& b : buttons) {
                const bool recording = v.recordState == 1;
                const char* label = b.id == 1   ? t.ctlRecordClip
                                    : recording ? t.ctlRecordStop
                                    : v.recordClips ? t.ctlRecord  // half the card: the short word
                                                    : t.ctlRecordStart;
                ctlButton(pen, b, label, recording, held == b.id);
            }
            break;
        default: break;
    }
}

uint64_t controlSignature(const ControlsView* v) {
    if (v == nullptr) return 0;
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 1099511628211ull;
    };
    const auto str = [&](const std::string& s) {
        mix(s.data(), s.size());
        mix("\0", 1);
    };
    const int ints[] = {v->interactive, v->colorAvailable, v->presetInUse, v->comparing, v->compareUsable,
                        v->soundKnown, v->volume, v->muted, v->passthrough, v->monoColorUsable, v->activeProfile,
                        v->armedProfile, v->chromeRunning, v->browserOpen, v->recordState, v->recordSeconds,
                        v->recordClips, v->recordClipUsable, static_cast<int>(v->pressedType), v->pressedButton};
    mix(ints, sizeof(ints));
    for (const std::string& n : v->presetNames) str(n);
    str(v->outputName);
    for (const std::string& n : v->profileNames) str(n);
    for (const std::string& n : v->profileIds) str(n);
    return h;
}

}  // namespace

std::vector<ControlButton> controlButtons(const WidgetInstance& w, const ControlsView* v, double x, double y) {
    std::vector<ControlButton> out;
    const bool on = v != nullptr && v->interactive;
    switch (w.type) {
        case WidgetType::ColorPresets:
            out = buttonRow(x, y, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, kCtlGap);
            for (ControlButton& b : out) b.usable = on && v->colorAvailable && (b.id < 3 || v->compareUsable);
            break;
        case WidgetType::Volume:
            out = buttonRow(x, y, {{0, 1.3}, {1, 0.8}, {-1, 2.2}, {2, 0.8}}, kCtlGap);
            for (ControlButton& b : out) b.usable = on && v->soundKnown && b.id >= 0;
            break;
        case WidgetType::Passthrough:
            out = segmentRow(x, y, v != nullptr && !v->monoColorUsable ? 2 : 3);
            for (ControlButton& b : out) b.usable = on;
            break;
        case WidgetType::Profiles: {
            const int n = v != nullptr ? std::min(kControlProfiles, static_cast<int>(v->profileNames.size())) : 0;
            if (n > 0) out = segmentRow(x, y, n);
            for (ControlButton& b : out) b.usable = on;
            break;
        }
        case WidgetType::Browser:
            out = buttonRow(x, y, {{0, 1}}, kCtlGap);
            for (ControlButton& b : out) b.usable = on;
            break;
        case WidgetType::Record:
            if (v != nullptr && v->recordClips) {
                out = buttonRow(x, y, {{0, 1}, {1, 1}}, kCtlGap);
            } else {
                out = buttonRow(x, y, {{0, 1}}, kCtlGap);
            }
            for (ControlButton& b : out) {
                b.usable = on && (b.id == 1 ? v->recordClipUsable : v->recordState != 2);  // not while it's saved
            }
            break;
        default: break;
    }
    return out;
}

uint64_t widgetSignature(const WidgetInstance& w, const WidgetContext& ctx) {
    // Metric ids per widget (owned and borrowed), built once from the registry
    static const std::vector<std::vector<uint16_t>> kOwned = [] {
        std::vector<std::vector<uint16_t>> owned(kWidgetTypeCount);
        for (const MetricDef& d : registry()) owned[static_cast<size_t>(d.owner)].push_back(d.id);
        for (size_t t = 1; t < kWidgetTypeCount; ++t) {  // and what it borrows (the simple templates)
            for (const uint16_t id : borrowedMetrics(static_cast<WidgetType>(t))) owned[t].push_back(id);
        }
        return owned;
    }();
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the inputs' bytes
    const auto mix = [&h](const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 1099511628211ull;
    };
    const size_t type = static_cast<size_t>(w.type);
    if (type < kOwned.size()) {
        for (const uint16_t id : kOwned[type]) {
            const MetricValue* v = value(ctx, id);
            if (v != nullptr) mix(&v->sampledAt, sizeof(v->sampledAt));
        }
    }
    mix(&ctx.thresholds, sizeof(ctx.thresholds));
    mix(&ctx.graphSeconds, sizeof(ctx.graphSeconds));
    mix(&ctx.clockFormat, sizeof(ctx.clockFormat));
    mix(&ctx.language, sizeof(ctx.language));
    if (isProcessList(w.type) && ctx.processes != nullptr) {
        mix(&ctx.processes->generation, sizeof(ctx.processes->generation));
    }
    if (isControlWidget(w.type)) {
        const uint64_t c = controlSignature(ctx.controls);
        mix(&c, sizeof(c));
    }
    const bool hasStore = ctx.store != nullptr;
    const bool hasLists = ctx.processes != nullptr;
    mix(&hasStore, sizeof(hasStore));
    mix(&hasLists, sizeof(hasLists));
    return h;
}

int widgetHeight(const WidgetInstance& w, const WidgetContext* ctx) {
    const bool more = moreRows(w);
    switch (w.type) {
        case WidgetType::Frame: return more ? kFrameFullH : kGraphCardH;
        case WidgetType::GpuRam: return kGraphCardH;
        case WidgetType::Power: return kGraphCardH;
        // Compact cards keep only their top row (Battery: % and the W line)
        case WidgetType::Temp: return w.compact ? kGraphCardH : kTempFullH;
        case WidgetType::Cpu: return more ? cpuFullHeight(ctx) : kCpuH;
        case WidgetType::Battery: return w.compact ? kBatteryCompactH : kBatteryFullH;
        case WidgetType::CpuItems:
        case WidgetType::RamItems:
            return static_cast<int>(kTop + kListTop + kRowH * static_cast<double>(w.maxItems)) + 10;
        case WidgetType::Network: return ctx && !networkExpanded(*ctx) ? kNetworkShortH : kNetworkFullH;
        case WidgetType::General: {
            int rows = ctx && ctx->clockFormat == 0 ? 1 : 2;
            if (more) rows += kGeneralMoreRows;
            return static_cast<int>(kTop) + 20 + rows * static_cast<int>(kLineH) + 12;
        }
        case WidgetType::SimpleA: return kSimpleAH;
        case WidgetType::SimpleB: return kSimpleBH;
        case WidgetType::ColorPresets:
        case WidgetType::Volume:
        case WidgetType::Passthrough:
        case WidgetType::Profiles:
        case WidgetType::Browser:
        case WidgetType::Record: return kControlH;
        case WidgetType::None: break;
    }
    return 0;
}

int widgetWidth(const WidgetInstance& w) {
    return w.type == WidgetType::SimpleB ? kSimpleBWidth : kColumnWidth;
}

const char* widgetTitle(const WidgetInstance& w, WidgetLanguage language) {
    return titleOf(w, widgetText(language));
}

void drawWidgetStatic(const Pen& pen, double x, double y, const WidgetInstance& w, WidgetLanguage language,
                      int heightOverride) {
    // The simple templates: their own cards (three rows; one small tile), no shared title
    if (w.type == WidgetType::SimpleA) {
        staticSimpleA(pen, x, y, widgetText(language));
        return;
    }
    if (w.type == WidgetType::SimpleB) {
        drawCard(pen, x, y, widgetWidth(w) - 16, heightOverride > 0 ? heightOverride : kSimpleBH, 13, kCard, kCard, 0);
        return;
    }
    staticCard(pen, x, y, heightOverride > 0 ? heightOverride : widgetHeight(w));
    if (hasGraph(w.type)) staticGraphBed(pen, x, y);
    if (w.type == WidgetType::Frame && moreRows(w)) staticFrameTimesBeds(pen, x, y);
    staticTitle(pen, x, y, titleOf(w, widgetText(language)));
}

void pushWidgetHistory(const WidgetInstance& w, const WidgetContext& ctx, WidgetHistory& history) {
    const double keep = std::max(1, ctx.graphSeconds);
    switch (w.type) {
        case WidgetType::Frame:
            history.a.push(ctx.now, f(ctx, kFrameAppFps), keep);
            // Kept while compact too, so turning Compact off shows full graphs at once
            history.c.push(ctx.now, f(ctx, kFrameCpuMaxMs), keep);
            history.d.push(ctx.now, f(ctx, kFrameGpuMaxMs), keep);
            break;
        case WidgetType::GpuRam: history.a.push(ctx.now, f(ctx, kGpuBusyPct), keep); break;
        case WidgetType::Power: history.a.push(ctx.now, f(ctx, kPowerVphW), keep); break;
        case WidgetType::Temp:
            history.a.push(ctx.now, f(ctx, kTempCpuC), keep);
            history.b.push(ctx.now, f(ctx, kTempGpuC), keep);
            break;
        default: break;
    }
}

void drawWidgetDynamic(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
                       const WidgetHistory& history) {
    switch (w.type) {
        case WidgetType::Frame: drawFrame(pen, x, y, w, ctx, history); break;
        case WidgetType::Cpu: drawCpu(pen, x, y, w, ctx); break;
        case WidgetType::GpuRam: drawGpuRam(pen, x, y, ctx, history); break;
        case WidgetType::Power: drawPower(pen, x, y, ctx, history); break;
        case WidgetType::Battery: drawBattery(pen, x, y, w, ctx); break;
        case WidgetType::Temp: drawTemp(pen, x, y, w, ctx, history); break;
        case WidgetType::CpuItems:
        case WidgetType::RamItems: drawProcessList(pen, x, y, w, ctx); break;
        case WidgetType::Network: drawNetwork(pen, x, y, ctx); break;
        case WidgetType::General: drawGeneral(pen, x, y, w, ctx); break;
        case WidgetType::SimpleA: drawSimpleA(pen, x, y, ctx); break;
        case WidgetType::SimpleB: drawSimpleB(pen, x, y, ctx); break;
        case WidgetType::ColorPresets:
        case WidgetType::Volume:
        case WidgetType::Passthrough:
        case WidgetType::Profiles:
        case WidgetType::Browser:
        case WidgetType::Record: drawControl(pen, x, y, w, ctx); break;
        case WidgetType::None: break;
    }
}

}  // namespace frame_perf_overlay
