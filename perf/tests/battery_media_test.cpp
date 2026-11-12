// The battery's time left and its notices (BatteryWatch), their texts, the notice's picture, and the HUD's video
// controls (MediaStrip): hit areas, presses, pictures. Writes PNGs to $FRAMEDOCK_PNG_DIR when set. No SteamVR.
#include "app_text.h"
#include "battery_watch.h"
#include "config.h"
#include "draw.h"
#include "grid_renderer.h"
#include "media_strip.h"
#include "metric_registry.h"
#include "metric_store.h"
#include "notice_view.h"
#include "widget_text.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using Notice = BatteryWatch::Notice;

std::string pngPath(const char* name) {
    const char* dir = std::getenv("FRAMEDOCK_PNG_DIR");
    if (dir == nullptr || dir[0] == '\0') return "";
    std::filesystem::create_directories(dir);
    return std::string(dir) + "/" + name;
}

FontSet& fonts() {
    static FontSet f;
    static bool loaded = false;
    if (!loaded) {
        const Config cfg;
        f.load(cfg.fontPath, cfg.boldFontPath);
        loaded = true;
    }
    return f;
}

BatteryReading discharging(double pct, double chargeMah, double currentMa) {
    BatteryReading r;
    r.pct = pct;
    r.chargeMah = chargeMah;
    r.currentMa = -currentMa;
    r.discharging = true;
    return r;
}

void testTimeLeft() {
    // Rounding: 5 minutes from 15 up, whole minutes below
    CHECK(roundMinutes(42.4) == 40 && roundMinutes(43) == 45 && roundMinutes(14.4) == 14 && roundMinutes(0.2) == 1);
    CHECK(std::isnan(roundMinutes(std::nan(""))));
    CHECK(minutesText(40) == "약 40분" && minutesText(70) == "약 1시간 10분" && minutesText(120) == "약 2시간");
    CHECK(minutesText(std::nan("")).empty());
    // In the app's language
    setUiLanguage(Language::En);
    CHECK(minutesText(70) == "~1 h 10 min");
    setUiLanguage(Language::Ja);
    CHECK(minutesText(40) == "約40分");
    setUiLanguage(Language::Ko);
    // The widgets' text in the three languages
    using frame_perf_overlay::WidgetLanguage;
    using frame_perf_overlay::remainText;
    using frame_perf_overlay::widgetText;
    CHECK(remainText(widgetText(WidgetLanguage::Ko), 40) == "약 40분");
    CHECK(remainText(widgetText(WidgetLanguage::Ko), 70) == "약 1시간 10분");
    CHECK(remainText(widgetText(WidgetLanguage::En), 60) == "~1 h");
    CHECK(remainText(widgetText(WidgetLanguage::Ja), 75) == "約1時間15分");
    CHECK(remainText(widgetText(WidgetLanguage::Ko), std::nan("")).empty());
    using frame_perf_overlay::remainTextShort;
    CHECK(remainTextShort(widgetText(WidgetLanguage::Ko), 150) == "약 2.5시간");
    CHECK(remainTextShort(widgetText(WidgetLanguage::Ko), 125) == "약 2시간");
    CHECK(remainTextShort(widgetText(WidgetLanguage::En), 100) == "~1.5 h");
    CHECK(remainTextShort(widgetText(WidgetLanguage::Ja), 60) == "約1時間");
    CHECK(remainTextShort(widgetText(WidgetLanguage::Ko), 40) == "약 40분");
    CHECK(remainTextShort(widgetText(WidgetLanguage::Ko), 990) == "약 17시간");

    // 2000 mAh left at a steady 1000 mA: 2 hours, once kMinDataSec of discharge has been averaged
    BatteryWatch w;
    double t = 0;
    for (; t < BatteryWatch::kMinDataSec; t += 15) {
        w.feed(discharging(80, 2000, 1000), t);
        CHECK(std::isnan(w.minutesLeft()));
    }
    w.feed(discharging(80, 2000, 1000), t);
    CHECK(w.minutesLeft() == 120);
    // A short spike barely moves the average; the time given changes at most once a minute
    t += 15;
    w.feed(discharging(80, 1990, 3000), t);
    CHECK(w.minutesLeft() == 120);  // held: not a minute yet
    for (int i = 0; i < 4; ++i) w.feed(discharging(80, 1980, 1000), t += 15);
    CHECK(w.minutesLeft() >= 105 && w.minutesLeft() <= 120);
    // Charging (or full, or plugged in): no time, and it starts over after
    BatteryReading charging = discharging(80, 1980, 1000);
    charging.discharging = false;
    charging.status = "Charging";
    charging.currentMa = 800;
    w.feed(charging, t += 15);
    CHECK(std::isnan(w.minutesLeft()));
    w.feed(discharging(80, 1980, 1000), t += 15);
    CHECK(std::isnan(w.minutesLeft()));
    // Almost nothing drawn (the gauge settling): no time
    BatteryWatch idle;
    for (double s = 0; s <= 120; s += 15) idle.feed(discharging(80, 2000, 10), s);
    CHECK(std::isnan(idle.minutesLeft()));
    // A gap (the headset off): starts over, no time from before it
    BatteryWatch gap;
    for (double s = 0; s <= 90; s += 15) gap.feed(discharging(80, 2000, 1000), s);
    CHECK(gap.minutesLeft() == 120);
    gap.feed(discharging(70, 1800, 1000), 90 + BatteryWatch::kGapSec + 1);
    CHECK(std::isnan(gap.minutesLeft()));
    // Never more than a day
    BatteryWatch tiny;
    for (double s = 0; s <= 90; s += 15) tiny.feed(discharging(80, 2000, 31), s);
    CHECK(tiny.minutesLeft() <= BatteryWatch::kMaxMinutes);
    // A reading that can't be used ("Not charging" off the charger, a failed current read) is skipped: the time stays
    BatteryWatch blip;
    for (double s = 0; s <= 90; s += 15) blip.feed(discharging(80, 2000, 1000), s);
    CHECK(blip.minutesLeft() == 120);
    BatteryReading notCharging = discharging(80, 2000, 1000);
    notCharging.discharging = false;
    notCharging.status = "Not charging";
    blip.feed(notCharging, 105);
    CHECK(blip.minutesLeft() == 120);
    BatteryReading unread = discharging(80, 2000, 1000);
    unread.currentMa = std::nan("");
    blip.feed(unread, 120);
    CHECK(blip.minutesLeft() == 120);
    blip.feed(discharging(80, 2000, 1000), 135);
    CHECK(blip.minutesLeft() == 120);
    // ... but no usable one for kGapSec starts over, and "Not charging" on the charger is no time
    for (double s = 150; s <= 135 + BatteryWatch::kGapSec + 15; s += 15) blip.feed(unread, s);
    CHECK(std::isnan(blip.minutesLeft()));
    notCharging.chargerOnline = true;
    for (double s = 0; s <= 90; s += 15) blip.feed(discharging(80, 2000, 1000), 1000 + s);
    CHECK(blip.minutesLeft() == 120);
    blip.feed(notCharging, 1105);
    CHECK(std::isnan(blip.minutesLeft()));
}

void testHeadsetNotices() {
    BatteryWatch w;
    double t = 0;
    w.feed(discharging(25, 600, 1000), t);
    CHECK(w.takeNotices().empty());
    w.feed(discharging(20, 500, 1000), t += 15);
    std::vector<Notice> n = w.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::HeadsetLow && n[0].pct == 20);
    w.feed(discharging(19, 480, 1000), t += 15);
    w.feed(discharging(21, 480, 1000), t += 15);  // a wobble up and down again doesn't tell twice
    w.feed(discharging(20, 480, 1000), t += 15);
    CHECK(w.takeNotices().empty());
    w.feed(discharging(10, 250, 1000), t += 15);
    n = w.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::HeadsetCritical);
    w.feed(discharging(9, 240, 1000), t += 15);
    CHECK(w.takeNotices().empty());
    // Charged: both come back
    BatteryReading plugged = discharging(9, 240, 1000);
    plugged.discharging = false;
    // A status that couldn't be read (or Unknown) re-arms nothing: no second notice from a flicker
    w.feed(plugged, t += 15);
    w.feed(discharging(9, 240, 1000), t += 15);
    CHECK(w.takeNotices().empty());
    // "Not charging" off the charger (the gauge flapping) re-arms nothing either
    plugged.status = "Not charging";
    for (int i = 0; i < 10; ++i) {
        w.feed(plugged, t += 15);
        w.feed(discharging(9, 240, 1000), t += 15);
    }
    CHECK(w.takeNotices().empty());
    // On the charger, "Not charging" (held at a level) does
    plugged.chargerOnline = true;
    w.feed(plugged, t += 15);
    w.feed(discharging(9, 240, 1000), t += 15);
    n = w.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::HeadsetCritical);
    plugged.chargerOnline = false;
    plugged.status = "Charging";
    w.feed(plugged, t += 15);
    w.feed(discharging(9, 240, 1000), t += 15);
    n = w.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::HeadsetCritical);
    // Already low at start: only the more serious one
    BatteryWatch low;
    low.feed(discharging(8, 200, 1000), 0);
    n = low.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::HeadsetCritical);
    // Plugged in at 15 %: nothing to tell
    BatteryWatch atDesk;
    BatteryReading desk = discharging(15, 400, 0);
    desk.discharging = false;
    desk.status = "Not charging";
    atDesk.feed(desk, 0);
    CHECK(atDesk.takeNotices().empty());
    // kRearmPct above the threshold re-arms it
    BatteryWatch rearm;
    rearm.feed(discharging(20, 500, 1000), 0);
    rearm.takeNotices();
    rearm.feed(discharging(BatteryWatch::kLowPct + BatteryWatch::kRearmPct, 560, 1000), 15);
    rearm.feed(discharging(20, 500, 1000), 30);
    CHECK(rearm.takeNotices().size() == 1);
}

void testControllerNotices() {
    BatteryWatch w;
    HandBattery left {true, 30, false}, right {false, std::nan(""), false};
    w.feedControllers(left, right);
    CHECK(w.takeNotices().empty());
    left.pct = 20;
    w.feedControllers(left, right);
    std::vector<Notice> n = w.takeNotices();
    CHECK(n.size() == 1 && n[0].kind == Notice::Kind::ControllerLow && n[0].left && n[0].pct == 20);
    left.pct = 10;
    w.feedControllers(left, right);
    CHECK(w.takeNotices().empty());  // once until charged or back above
    left.charging = true;
    w.feedControllers(left, right);
    left.charging = false;
    w.feedControllers(left, right);
    CHECK(w.takeNotices().size() == 1);
    // 0 % (a controller just connected) is no reading
    BatteryWatch zero;
    zero.feedControllers({true, 0, false}, {false, std::nan(""), false});
    CHECK(zero.takeNotices().empty());
    // The right one turned on low; turned off it says nothing
    right = {true, 15, false};
    w.feedControllers(left, right);
    n = w.takeNotices();
    CHECK(n.size() == 1 && !n[0].left);
    CHECK(w.right().present && w.right().pct == 15);
}

void testNoticePicture() {
    NoticeView view(fonts());
    view.render("헤드셋 배터리 20% · 약 25분 남았어요", NoticeView::Level::Warn);
    // The pill is drawn (opaque in the middle), the corners stay clear
    const uint8_t* px = view.pixels();
    const auto alphaAt = [&](int x, int y) { return px[(static_cast<size_t>(y) * NoticeView::kWidth + x) * 4 + 3]; };
    CHECK(alphaAt(NoticeView::kWidth / 2, NoticeView::kHeight / 2) > 200);
    CHECK(alphaAt(0, 0) == 0);
    std::string path = pngPath("notice-battery-low.png");
    if (!path.empty()) CHECK(view.writePng(path));
    view.render("헤드셋 배터리 9% · 약 10분 남았어요. 충전해 주세요", NoticeView::Level::Danger);
    path = pngPath("notice-battery-critical.png");
    if (!path.empty()) CHECK(view.writePng(path));
}

void testMediaStrip() {
    MediaStrip strip(fonts());
    MediaStripState state;
    state.title = "Lofi Girl - lofi hip hop radio 📚 beats to relax/study to";
    // Symbols the font lacks (❤ ✅ ⭐ 1️⃣, U+FFFD) and broken UTF-8 are left out of the title (see the PNGs)
    MediaStripState odd;
    odd.title = "1\xef\xb8\x8f\xe2\x83\xa3 \xe2\x9d\xa4\xef\xb8\x8f \xe2\x9c\x85 \xe2\xad\x90 Live \xef\xbf\xbd end \xf0";
    strip.render(512, odd, false);
    const std::string oddPath = pngPath("strip-odd-title.png");
    if (!oddPath.empty()) CHECK(strip.writePng(oddPath));
    state.playing = true;
    for (const int width : {512, 250}) {
        strip.render(width, state, false);
        CHECK(strip.width() == width && strip.pixels() != nullptr);
        // Five buttons left to right across the strip, nothing on the title line
        double lastX = -1;
        for (int i = 0; i < MediaStrip::kButtons; ++i) {
            const MediaAction want = static_cast<MediaAction>(i + 1);
            bool found = false;
            for (double x = lastX + 1; x < width && !found; x += 1) {
                if (strip.hit(x, 90) == want) {
                    found = true;
                    lastX = x;
                }
            }
            CHECK(found);
        }
        CHECK(strip.hit(width / 2.0, 30) == MediaAction::None);
        CHECK(strip.hit(width / 2.0, MediaStrip::kHeight + 10) == MediaAction::None);
    }
    // A press on a button acts on its release there; slid off, or the laser gone, nothing
    strip.render(512, state, false);
    double playX = -1;
    for (double x = 0; x < 512; x += 1) {
        if (strip.hit(x, 90) == MediaAction::PlayPause) {
            playX = x + 20;
            break;
        }
    }
    CHECK(strip.pointerDown(playX, 90) && strip.pressing());
    CHECK(strip.pointerUp(playX, 90) == MediaAction::PlayPause && !strip.pressing());
    CHECK(strip.pointerDown(playX, 90));
    CHECK(strip.pointerUp(5, 90) == MediaAction::None);  // on another button
    // Let go just off the pressed button (above it, or below the strip): still that button; far off, nothing
    CHECK(strip.pointerDown(playX, 90));
    CHECK(strip.pointerUp(playX, 50) == MediaAction::PlayPause);
    CHECK(strip.pointerDown(playX, 90));
    CHECK(strip.pointerUp(playX, 30) == MediaAction::None);
    CHECK(strip.pointerDown(playX, 90));
    CHECK(strip.pointerLeave() && !strip.pressing());
    CHECK(strip.pointerUp(playX, 90) == MediaAction::None);
    CHECK(!strip.pointerDown(256, 30));  // the title line: nothing (the main loop doesn't pass it to the Dock either)
    // Hover changes ask for a redraw only when they change
    CHECK(strip.pointerMove(playX, 90) && !strip.pointerMove(playX + 1, 90));

    // The position after a 10 s jump
    CHECK(mediaPositionText(83.4, 296) == "1:23 / 4:56");
    CHECK(mediaPositionText(3723, 7200) == "1:02:03 / 2:00:00");
    CHECK(mediaPositionText(83, std::numeric_limits<double>::infinity()) == "1:23");  // live
    CHECK(mediaPositionText(83, std::nan("")) == "1:23");
    CHECK(mediaPositionText(-1, 300).empty());
    // Control characters in a title draw as spaces, not boxes; a muted strip and a held button draw
    MediaStripState tabs;
    tabs.title = "Song\tTitle\nLine\x7f end";
    tabs.muted = true;
    tabs.position = "1:23 / 4:56";
    strip.render(512, tabs, true);
    CHECK(strip.pointerDown(playX, 90));
    strip.render(512, tabs, true);
    const std::string tabsPath = pngPath("strip-muted-seek.png");
    if (!tabsPath.empty()) CHECK(strip.writePng(tabsPath));
    strip.pointerLeave();

    // Pictures: under Simple A (playing, highlighted), paused after taking the headset off, under Simple B (muted)
    using namespace frame_perf_overlay;
    MetricStore st;
    st.setF32(metric::kFrameAppFps, 90, 1);
    st.setF32(metric::kFrameDisplayHz, 90, 1);
    st.setF32(metric::kFrameReprojectedPct, 0, 1);
    st.setEnum(metric::kFrameThrottled, 0, 1);
    st.setF32(metric::kCpuUsagePct, 16, 1);
    st.setF32(metric::kCpuMaxCorePct, 38, 1);
    st.setF32(metric::kGpuBusyPct, 62, 1);
    st.setF32(metric::kRamUsedGib, 7.8, 1);
    st.setF32(metric::kRamTotalGib, 15.3, 1);
    st.setF32(metric::kPowerVphW, 6.81, 1);
    st.setF32(metric::kBatteryPct, 46, 1);
    st.setEnum(metric::kBatteryStatus, static_cast<uint32_t>(BatteryStatus::Discharging), 1);
    st.setEnum(metric::kBatteryChargerOnline, 0, 1);
    st.setF32(metric::kBatteryRemainMin, 40, 1);
    const auto hudWithStrip = [&](WidgetType type, const MediaStripState& s, bool highlighted, const char* name) {
        GridRenderer renderer(fonts());
        Layout layout;
        WidgetInstance w;
        w.type = type;
        layout.columns = {{{w}}};
        renderer.setLayout(layout);
        renderer.setHighlight(highlighted);
        WidgetContext ctx;
        ctx.store = &st;
        ctx.now = 10;
        ctx.language = WidgetLanguage::Ko;
        renderer.render(ctx);
        const GridGeometry& g = renderer.geometry();
        // The same composition as the main loop: a narrow HUD centred over a kMinButtonsWidth strip
        const int cw = std::max(g.textureWidth, MediaStrip::kMinButtonsWidth);
        MediaStrip m(fonts());
        m.render(cw, s, highlighted);
        if (g.textureWidth < MediaStrip::kMinButtonsWidth) {
            // Simple B: its buttons as wide as at least 57 px
            double first = -1, last = -1;
            for (double x = 0; x < cw; x += 1) {
                if (m.hit(x, 90) == MediaAction::Back) last = x;
                if (first < 0 && m.hit(x, 90) == MediaAction::Back) first = x;
            }
            CHECK(last - first >= 57);
        }
        const std::string path = pngPath(name);
        if (path.empty()) return;
        cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, g.textureHeight + MediaStrip::kHeight);
        CHECK(cairo_image_surface_get_stride(out) == cw * 4);
        uint8_t* data = cairo_image_surface_get_data(out);
        std::memset(data, 0, static_cast<size_t>(cw) * (g.textureHeight + MediaStrip::kHeight) * 4);
        const int left = (cw - g.textureWidth) / 2;
        for (int y = 0; y < g.textureHeight; ++y) {
            std::memcpy(data + (static_cast<size_t>(y) * cw + left) * 4,
                        renderer.pixels() + static_cast<size_t>(y) * g.textureWidth * 4,
                        static_cast<size_t>(g.textureWidth) * 4);
        }
        std::memcpy(data + static_cast<size_t>(cw) * g.textureHeight * 4, m.pixels(),
                    static_cast<size_t>(cw) * MediaStrip::kHeight * 4);
        cairo_surface_mark_dirty(out);
        CHECK(cairo_surface_write_to_png(out, path.c_str()) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy(out);
    };
    MediaStripState removed = state;
    removed.playing = false;
    removed.pausedByRemoval = true;
    MediaStripState muted = state;
    muted.muted = true;
    hudWithStrip(WidgetType::SimpleA, state, true, "hud-media-simple-a.png");
    hudWithStrip(WidgetType::SimpleA, removed, false, "hud-media-removed.png");
    hudWithStrip(WidgetType::SimpleB, muted, false, "hud-media-simple-b.png");
    // A full battery's long time under Simple B: the short form fits; a YouTube title loses "(3) " and " - YouTube"
    st.setF32(metric::kBatteryPct, 100, 1);
    st.setF32(metric::kBatteryRemainMin, 150, 1);
    MediaStripState youtube = state;
    youtube.title = "(3) 아이유 - 밤편지 [MV] - YouTube";
    hudWithStrip(WidgetType::SimpleB, youtube, false, "hud-media-simple-b-full.png");
    // Just over an hour: the short form at its full size rather than "약 1시간 5분" squeezed
    st.setF32(metric::kBatteryPct, 45, 1);
    st.setF32(metric::kBatteryRemainMin, 65, 1);
    hudWithStrip(WidgetType::SimpleB, youtube, false, "hud-media-simple-b-65.png");
}

}  // namespace

int main() {
    testTimeLeft();
    testHeadsetNotices();
    testControllerNotices();
    testNoticePicture();
    testMediaStrip();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("battery-media: all passed\n");
    return 0;
}
