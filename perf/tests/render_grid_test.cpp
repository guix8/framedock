// Grid geometry, texture cap, draw rules and change detection of the widget grid renderer (links frame-perf-render).
// Renders into memory; a few PNGs go to $FRAME_PERF_PNG_DIR (default /tmp/frame-perf-render-tests/png) for a look by eye.
#include "atomic_file.h"
#include "grid_renderer.h"

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
using W = WidgetType;
using Pixels = std::vector<uint8_t>;

constexpr double kNow = 100.0;

// The same default fonts the headset asks for; where they're missing FontSet falls back to fontconfig
FontSet& fonts() {
    static FontSet f;
    static bool loaded = false;
    if (!loaded) {
        f.load("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc");
        loaded = true;
    }
    return f;
}

std::string pngDir() {
    const char* env = std::getenv("FRAME_PERF_PNG_DIR");
    const std::string dir = env != nullptr && env[0] != '\0' ? env : "/tmp/frame-perf-render-tests/png";
    makeDirectories(dir);
    return dir;
}

WidgetInstance inst(W type, bool compact = true) {
    WidgetInstance w;
    w.type = type;
    w.compact = compact;
    return w;
}

Layout oneWidget(const WidgetInstance& w) {
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {w};
    return layout;
}

/** Renders a layout with a fresh renderer (no graph history carried over). */
Pixels renderPx(const Layout& layout, const MetricStore& store, const char* pngName = nullptr) {
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    renderer.render(ctx);
    if (pngName != nullptr) renderer.writePng(pngDir() + "/" + pngName);
    return renderer.toRgba();
}

// ---- Texture sizing ----

void testGeometry() {
    const GridGeometry def = layoutGrid(defaultLayout());
    CHECK(def.width == 1024 && def.columns == 2);  // two columns: wide and low like the fixed panel
    CHECK(def.scale == 1.0);
    CHECK(def.textureWidth == 1024 && def.height < 512);
    CHECK(def.textureHeight == def.height && def.height <= 2048);
    CHECK(def.slots.size() == 8);

    // 5 columns = 2560 px: scaled to fit 2048
    Layout wide;
    wide.columns.resize(5);
    for (LayoutColumn& c : wide.columns) c.widgets = {inst(W::Frame)};
    const GridGeometry gw = layoutGrid(wide);
    CHECK(gw.width == 2560);
    CHECK(gw.scale < 1.0);
    CHECK(gw.textureWidth <= 2048 && gw.textureHeight <= 2048);
    CHECK(std::abs(gw.textureWidth - std::ceil(gw.width * gw.scale)) <= 1);

    // one very tall column (more cards than a real layout allows, just to pass the cap)
    Layout tall;
    tall.columns.resize(1);
    for (int i = 0; i < 20; ++i) tall.columns[0].widgets.push_back(inst(W::Power));
    const GridGeometry gt = layoutGrid(tall);
    CHECK(gt.height > 2048);
    CHECK(gt.scale < 1.0);
    CHECK(gt.textureWidth <= 2048 && gt.textureHeight <= 2048);
    CHECK(std::abs(gt.textureHeight - std::ceil(gt.height * gt.scale)) <= 1);
    CHECK(std::abs(gt.textureWidth - std::ceil(gt.width * gt.scale)) <= 1);

    // the capped panel really renders at the capped size
    MetricStore store;
    GridRenderer renderer(fonts());
    renderer.setLayout(tall);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    renderer.render(ctx);
    CHECK(renderer.toRgba().size() == size_t(gt.textureWidth) * size_t(gt.textureHeight) * 4);
    renderer.writePng(pngDir() + "/tall_scaled.png");

    // empty layout: minimal one-column panel, renders without a crash
    const GridGeometry ge = layoutGrid(Layout());
    CHECK(ge.columns == 1 && ge.slots.empty());
    CHECK(ge.textureWidth == 512 && ge.textureHeight > 0);
    CHECK(!renderPx(Layout(), store).empty());
}

// ---- Heights that follow the data ----

void testGeneralClockOffShrinks() {
    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    const WidgetInstance general = inst(W::General);

    ctx.clockFormat = 24;
    const int withClock = widgetHeight(general, &ctx);
    ctx.clockFormat = 0;
    const int withoutClock = widgetHeight(general, &ctx);
    CHECK(withClock - withoutClock == 22);  // the Time row, exactly one text line

    ctx.clockFormat = 12;
    CHECK(widgetHeight(general, &ctx) == withClock);  // 12 h / 24 h both show the row; only "Off" hides it

    // no context at all (the layout editor doesn't know the dashboard's clock setting): the fixed fallback
    // reserves the room the clock needs
    CHECK(widgetHeight(general) == withClock);
}

void testNetworkShrinksWhenNotExpanded() {
    const WidgetInstance network = inst(W::Network);

    MetricStore absent;  // interface state N/A: not expanded
    WidgetContext absentCtx;
    absentCtx.store = &absent;
    absentCtx.now = kNow;
    CHECK(widgetHeight(network, &absentCtx) == 64);

    MetricStore disconnected;
    disconnected.setEnum(metric::kNetInterfaceUp, 1, kNow);
    disconnected.setEnum(metric::kNetConnected, 0, kNow);
    WidgetContext disconnectedCtx;
    disconnectedCtx.store = &disconnected;
    disconnectedCtx.now = kNow;
    CHECK(widgetHeight(network, &disconnectedCtx) == 64);

    MetricStore connected;
    connected.setEnum(metric::kNetInterfaceUp, 1, kNow);
    connected.setEnum(metric::kNetConnected, 1, kNow);
    WidgetContext connectedCtx;
    connectedCtx.store = &connected;
    connectedCtx.now = kNow;
    CHECK(widgetHeight(network, &connectedCtx) == 88);

    CHECK(widgetHeight(network) == 88);  // no ctx: fixed fallback keeps the full reservation

    CHECK(renderPx(oneWidget(network), disconnected, "network_collapsed.png") !=
          renderPx(oneWidget(network), connected, "network_expanded.png"));
}

void testCompactHeights() {
    CHECK(widgetHeight(inst(W::Temp, false)) == 154);  // Frame Dock didn't take upstream's extra Temp spots
    CHECK(widgetHeight(inst(W::Temp, true)) == 102);
    CHECK(widgetHeight(inst(W::Battery, false)) == 142);  // nor the Battery details
    CHECK(widgetHeight(inst(W::Battery, true)) == 88);
    // the types without the switch
    for (const W type : {W::Power, W::GpuRam, W::Network}) {
        CHECK(widgetHeight(inst(type, true)) == widgetHeight(inst(type, false)));
    }

    // no jitter: values moving doesn't move a height or the grid
    MetricStore low;
    low.setF32(metric::kTempCpuC, 50, kNow);
    low.setF32(metric::kBatteryPct, 40, kNow);
    MetricStore high;
    high.setF32(metric::kTempCpuC, 80, kNow);
    high.setF32(metric::kBatteryPct, 90, kNow);
    WidgetContext lowCtx, highCtx;
    lowCtx.store = &low;
    lowCtx.now = kNow;
    highCtx.store = &high;
    highCtx.now = kNow;
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(W::Frame), inst(W::Temp, false)};
    layout.columns[1].widgets = {inst(W::Power), inst(W::Battery, true)};
    CHECK(sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(layout, &highCtx)));

    // toggling compact changes the geometry
    Layout toggled = layout;
    toggled.columns[0].widgets[1].compact = true;
    CHECK(!sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(toggled, &lowCtx)));
    CHECK(layoutGrid(toggled, &lowCtx).slots[1].h == 102);
    CHECK(layoutGrid(layout, &lowCtx).slots[1].h == 154);

    // Compact Temp keeps the graph: its graph bed (the panel's background colour, sunk into the card) is where the
    // full card's is. The card sits at (8, 8); the bed starts 202 px in and 10 px down, 84 px tall
    MetricStore none;
    const Pixels compact = renderPx(oneWidget(inst(W::Temp, true)), none, "temp-compact.png");
    const Pixels full = renderPx(oneWidget(inst(W::Temp, false)), none);
    const auto rgbAt = [](const Pixels& px, int width, int x, int y) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return (unsigned(px[i]) << 16) | (unsigned(px[i + 1]) << 8) | px[i + 2];
    };
    const int bedX = 8 + 202 + 20, bedY = 8 + 10 + 76;  // low in the bed, clear of the guides and the label
    CHECK(rgbAt(compact, 512, bedX, bedY) == rgbAt(full, 512, bedX, bedY));
    CHECK(rgbAt(compact, 512, bedX, bedY) != rgbAt(compact, 512, 8 + 190, bedY));  // the card, left of the bed
}

void testTitles() {
    for (const W type : {W::Frame, W::Cpu, W::GpuRam, W::Power, W::Battery, W::Temp, W::CpuItems, W::RamItems,
                         W::Network, W::General}) {
        for (const WidgetLanguage language : {WidgetLanguage::En, WidgetLanguage::Ja}) {
            const char* title = widgetTitle(inst(type), language);
            CHECK(title != nullptr && title[0] != '\0');
        }
    }
}

// ---- Draw rules ----

void testEmptyStoreAllTypes() {
    MetricStore store;  // everything N/A NotSampledYet
    for (const W type : {W::Frame, W::Cpu, W::GpuRam, W::Power, W::Battery, W::Temp, W::CpuItems, W::RamItems,
                         W::Network, W::General}) {
        CHECK(!renderPx(oneWidget(inst(type)), store).empty());
        if (hasCompactView(type)) CHECK(!renderPx(oneWidget(inst(type, false)), store).empty());
    }
    // no store at all
    GridRenderer renderer(fonts());
    renderer.setLayout(defaultLayout());
    WidgetContext ctx;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    renderer.writePng(pngDir() + "/default_empty_store.png");
}

void testDefaultFilled() {
    MetricStore store;
    store.setF32(metric::kFrameAppFps, 90, 1.0);
    store.setF32(metric::kFrameDisplayHz, 90, 1.0);
    store.setF32(metric::kCpuUsagePct, 35, 1.0);
    store.setF32(metric::kGpuBusyPct, 60, 1.0);
    store.setF32(metric::kPowerVphW, 7.5, 1.0);
    store.setF32(metric::kBatteryPct, 80, 1.0);
    store.setF32(metric::kTempCpuC, 55, 1.0);
    CHECK(!renderPx(defaultLayout(), store, "default_filled.png").empty());
}

/** Renders one General widget with a given clock format. */
Pixels renderGeneral(const MetricStore& store, int clockFormat) {
    GridRenderer renderer(fonts());
    renderer.setLayout(oneWidget(inst(W::General)));
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.clockFormat = clockFormat;
    renderer.render(ctx);
    return renderer.toRgba();
}

void testClockOffHidesTime() {
    MetricStore a;
    a.setText(metric::kClock, "2026-10-11 23:05:00", 1.0);
    MetricStore b;
    b.setText(metric::kClock, "2026-10-12 07:41:00", 1.0);
    // Shown (12 / 24 h): the time is drawn, so a different time changes pixels
    CHECK(renderGeneral(a, 24) != renderGeneral(b, 24));
    CHECK(renderGeneral(a, 12) != renderGeneral(a, 24));
    // Off (0): the Time row is gone, so the time no longer matters
    CHECK(renderGeneral(a, 0) == renderGeneral(b, 0));
    CHECK(renderGeneral(a, 0) != renderGeneral(a, 24));
}

void testLanguageChangesTitles() {
    MetricStore store;
    const Layout layout = defaultLayout();
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.language = WidgetLanguage::En;
    CHECK(renderer.render(ctx));
    const uint64_t builds = renderer.staticBuilds();
    CHECK(!renderer.render(ctx));
    // a new language rebuilds the static layer (the titles) and repaints
    ctx.language = WidgetLanguage::Ja;
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == builds + 1);
}

// ---- Change detection ----

void testChangeDetection() {
    MetricStore store;
    GridRenderer renderer(fonts());
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;

    renderer.setLayout(defaultLayout());
    CHECK(renderer.render(ctx));          // first render: always new
    CHECK(!renderer.render(ctx));         // same store, same now
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 1);

    store.setF32(metric::kCpuUsagePct, 42, 1.0);
    CHECK(renderer.render(ctx));          // a value changed
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 1);

    // Same widgets, new version: no rebuild
    Layout same = defaultLayout();
    same.version = 9;
    CHECK(!renderer.setLayout(same));
    renderer.render(ctx);
    CHECK(renderer.staticBuilds() == 1);

    // Different widgets: one more static build
    Layout changed = defaultLayout();
    changed.columns[0].widgets.pop_back();
    CHECK(renderer.setLayout(changed));
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);

    // A setting tweak (Compact) is a different widget too
    Layout tweaked = changed;
    tweaked.columns[0].widgets[2].compact = !tweaked.columns[0].widgets[2].compact;  // Temp
    CHECK(renderer.setLayout(tweaked));
    renderer.render(ctx);
    CHECK(renderer.staticBuilds() == 3);

    // Every widget removed: the empty panel must still go up once (else the old widgets stay frozen on
    // screen), and an unchanged empty frame doesn't
    CHECK(renderer.setLayout(Layout()));
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);
    CHECK(!renderer.render(ctx));
}

// ---- Per-widget redraw: partial == full ----

Pixels pixelsOf(const GridRenderer& r) {
    const GridGeometry& g = r.geometry();
    const uint8_t* data = r.pixels();
    return Pixels(data, data + size_t(g.textureWidth) * size_t(g.textureHeight) * 4);
}

void testPartialRedraw() {
    // No graph widgets, so history can't make a partial and a fresh render differ
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(W::Battery, false), inst(W::General), inst(W::Network)};
    layout.columns[1].widgets = {inst(W::Cpu)};

    MetricStore store;
    store.setF32(metric::kBatteryPct, 80, 1.0);

    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 4);  // first render draws everything

    // Nothing changed: no redraw at all
    CHECK(!renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);

    // One battery metric changes: only the Battery widget redraws, and the picture equals a from-scratch one
    store.setF32(metric::kBatteryPct, 79, 2.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 1);
    GridRenderer fresh(fonts());
    fresh.setLayout(layout);
    CHECK(fresh.render(ctx));
    CHECK(pixelsOf(renderer) == pixelsOf(fresh));

    // Same value sampled again later: the signature moves (sampledAt), still the same picture
    store.setF32(metric::kBatteryPct, 79, 3.0);
    renderer.render(ctx);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh));

    // A CPU value changes: only the CPU widget redraws, and the picture follows
    store.setF32(metric::kCpuUsagePct, 63, 4.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 1);
    GridRenderer fresh2(fonts());
    fresh2.setLayout(layout);
    fresh2.render(ctx);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh2));
    CHECK(pixelsOf(renderer) != pixelsOf(fresh));
    CHECK(!renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);

    // markAllDirty redraws everything, same picture
    renderer.markAllDirty();
    renderer.render(ctx);
    CHECK(renderer.lastRedrawn() == 4);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh2));
}

/** One card of the last render, cut out of the panel (RGBA). */
Pixels slotPixels(GridRenderer& renderer, size_t index) {
    const WidgetSlot& slot = renderer.geometry().slots[index];
    const int stride = renderer.geometry().textureWidth * 4;
    const std::vector<uint8_t>& rgba = renderer.toRgba();
    Pixels out;
    for (int row = slot.y; row < slot.y + slot.h; ++row) {
        out.insert(out.end(), rgba.begin() + row * stride + slot.x * 4,
                   rgba.begin() + row * stride + (slot.x + slot.w) * 4);
    }
    return out;
}

// A graph keeps its history when another card's setting changes (Compact on the card below it)
void testCompactToggleKeepsGraphHistory() {
    Layout withFull;
    withFull.columns.resize(1);
    withFull.columns[0].widgets = {inst(W::Power), inst(W::Battery, false)};
    Layout withCompact = withFull;
    withCompact.columns[0].widgets[1].compact = true;

    GridRenderer toggled(fonts());
    GridRenderer steady(fonts());
    toggled.setLayout(withFull);
    steady.setLayout(withFull);
    MetricStore last;
    for (int i = 0; i < 12; ++i) {
        last.setF32(metric::kPowerVphW, 5.0 + 0.5 * (i % 4), kNow + i);
        WidgetContext ctx;
        ctx.store = &last;
        ctx.now = kNow + i;
        toggled.render(ctx);
        steady.render(ctx);
    }
    toggled.setLayout(withCompact);
    CHECK(toggled.geometry().slots[1].h == 88);
    WidgetContext ctx;
    ctx.store = &last;
    ctx.now = kNow + 11;
    toggled.render(ctx);
    steady.render(ctx);
    CHECK(slotPixels(toggled, 0) == slotPixels(steady, 0));
    // and the graph really holds a line: a renderer that never saw the history draws something else
    GridRenderer unseen(fonts());
    unseen.setLayout(withFull);
    unseen.render(ctx);
    CHECK(slotPixels(unseen, 0) != slotPixels(steady, 0));
}

// ---- Network: the interface being down is "Not connected", not an N/A ----

void testNetworkInterfaceDown() {
    const Layout layout = oneWidget(inst(W::Network));
    MetricStore unread;  // interface state never read: the N/A text
    MetricStore down;    // read, and down (no dongle AP and no home Wi-Fi)
    down.setEnum(metric::kNetInterfaceUp, 0, 1.0);
    MetricStore upNotConnected;
    upNotConnected.setEnum(metric::kNetInterfaceUp, 1, 1.0);
    upNotConnected.setEnum(metric::kNetConnected, 0, 1.0);
    MetricStore failed;  // the read failed: N/A with its reason
    failed.setNa(metric::kNetInterfaceUp, Reason::ReadFailed, 1.0);

    CHECK(renderPx(layout, down, "network_down.png") != renderPx(layout, unread));
    CHECK(renderPx(layout, down) != renderPx(layout, failed));
    CHECK(renderPx(layout, down) != renderPx(layout, upNotConnected));  // the link row differs
    // the same for the collapsed height
    WidgetContext ctx;
    ctx.store = &down;
    ctx.now = kNow;
    CHECK(widgetHeight(inst(W::Network), &ctx) == 64);

    // "Not connected" is in the card's language
    const auto render = [&](WidgetLanguage language) {
        GridRenderer renderer(fonts());
        renderer.setLayout(layout);
        WidgetContext c;
        c.store = &down;
        c.now = kNow;
        c.language = language;
        renderer.render(c);
        return renderer.toRgba();
    };
    CHECK(render(WidgetLanguage::En) != render(WidgetLanguage::Ja));
}

// ---- Text with broken UTF-8 doesn't upset cairo ----

void testInvalidUtf8Text() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 40);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts()};
    const Color white {1, 1, 1};
    const char* broken[] = {"a\xFF" "b", "\xC0\xAF", "end\xE3\x81", "\xED\xA0\x80", "\x80\x80"};
    for (const char* text : broken) {
        pen.text(10, 25, text, 16, white);
        CHECK(pen.measure(text, 16) > 0);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    }
    // measured as the replacement character, so layout and drawing agree
    CHECK(pen.measure("a\xFF" "b", 16) == pen.measure("a\xEF\xBF\xBD" "b", 16));
    CHECK(pen.measure(std::string("x\xE3\x81"), 16) == pen.measure(std::string("x\xEF\xBF\xBD\xEF\xBF\xBD"), 16));
    CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// ---- The segmented pill ----

/** @return the pixel as 0xRRGGBB (cairo ARGB32 is B, G, R, A bytes on little-endian) */
unsigned pixelRgb(cairo_surface_t* surface, int x, int y) {
    cairo_surface_flush(surface);
    const uint8_t* p = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4;
    return (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[1]) << 8) | p[0];
}

void testSegmentedPillColours() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 240, 70);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    const Pen pen {cr, &fonts()};
    // 200 x 48 at (10, 10): parts are 95 wide from x = 15 and x = 110. Sample 12 px inside each part's outer
    // end and mid-height, clear of the label and the check.
    for (const int selected : {0, 1}) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        drawSegmentedPill(pen, 10, 10, 200, 48, {{"On"}, {"Off"}}, selected, true);
        const unsigned left = pixelRgb(surface, 15 + 12, 34);
        const unsigned right = pixelRgb(surface, 205 - 12, 34);
        CHECK((selected == 0 ? left : right) == 0xe27dfd);   // kAccent
        CHECK((selected == 0 ? right : left) == 0x21262d);   // kControl
    }
    // unusable: no accent at full strength
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 200, 48, {{"On"}, {"Off"}}, 0, false);
    CHECK(pixelRgb(surface, 27, 34) != 0xe27dfd);

    // three parts (the attach pill: Head / L / R): 210 wide from x = 15, so 66.7 px each; the middle one is selected
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 210, 48, {{"Head"}, {"L"}, {"R"}}, 1, true);
    CHECK(pixelRgb(surface, 82 + 6, 34) == 0xe27dfd);  // the middle part's fill
    CHECK(pixelRgb(surface, 15 + 8, 34) != 0xe27dfd);
    CHECK(pixelRgb(surface, 215 - 8, 34) != 0xe27dfd);
    // pillPartLeft: the same parts, and hit areas that meet at the drawn edges and take the rim at the ends
    double l0 = 0, r0 = 0, l1 = 0, r1 = 0, l2 = 0, r2 = 0;
    CHECK(std::fabs(pillPartLeft(10, 210, 3, 0, &l0, &r0) - 15) < 1e-9);
    CHECK(std::fabs(pillPartLeft(10, 210, 3, 1, &l1, &r1) - (15 + 200.0 / 3)) < 1e-9);
    pillPartLeft(10, 210, 3, 2, &l2, &r2);
    CHECK(l0 == 10 && r2 == 220);                        // the rim at the ends
    CHECK(r0 == l1 && r1 == l2);                         // no gap, no overlap
    CHECK(std::fabs(l1 - (15 + 200.0 / 3)) < 1e-9 && std::fabs(l2 - (15 + 400.0 / 3)) < 1e-9);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// ---- The simple templates (Frame Dock) ----

/** The readings of the approved mockup: calm (90 fps, full battery) or warning (45 fps, half rate, GPU and battery bad). */
MetricStore simpleStore(bool warn, double t = 1.0) {
    MetricStore st;
    st.setF32(metric::kFrameAppFps, warn ? 45 : 90, t);
    st.setF32(metric::kFrameDisplayHz, 90, t);
    st.setF32(metric::kFrameReprojectedPct, warn ? 50 : 0, t);
    st.setEnum(metric::kFrameThrottled, warn ? 1 : 0, t);
    st.setF32(metric::kCpuUsagePct, warn ? 41 : 16, t);
    st.setF32(metric::kCpuMaxCorePct, warn ? 64 : 38, t);  // below cpu_warn_pct: the mockup's CPU isn't coloured
    st.setF32(metric::kGpuBusyPct, warn ? 97 : 62, t);
    st.setF32(metric::kRamUsedGib, 7.8, t);
    st.setF32(metric::kRamTotalGib, 15.3, t);
    st.setF32(metric::kPowerVphW, warn ? 11.9 : 4.26, t);
    st.setF32(metric::kBatteryPct, warn ? 14 : 100, t);
    st.setEnum(metric::kBatteryStatus,
               static_cast<uint32_t>(warn ? BatteryStatus::Discharging : BatteryStatus::Full), t);
    st.setEnum(metric::kBatteryChargerOnline, warn ? 0 : 1, t);
    if (warn) st.setF32(metric::kBatteryRemainMin, 25, t);  // on battery: the time left after the %
    st.setText(metric::kClock, "2026-11-03 21:19:00", t);
    return st;
}

/** Renders a layout in Korean (the app's language) and returns its pixels; writes pngName for a look by eye. */
Pixels renderKo(const Layout& layout, const MetricStore& store, const char* pngName, int* width = nullptr) {
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.language = WidgetLanguage::Ko;
    renderer.render(ctx);
    if (pngName != nullptr) renderer.writePng(pngDir() + "/" + pngName);
    if (width != nullptr) *width = renderer.geometry().textureWidth;
    return renderer.toRgba();
}

/** Frame's render size (2026-11-13): drawn on the title row, left of the graph, nothing when N/A. */
void testFrameRenderSize() {
    const auto frameStore = [](int source) {
        MetricStore st;
        st.setF32(metric::kFrameAppFps, 90, kNow);
        st.setF32(metric::kFrameDisplayHz, 90, kNow);
        st.setF32(metric::kFrameGpuMs, 8.2, kNow);
        st.setF32(metric::kFrameCpuMs, 4.1, kNow);
        st.setF32(metric::kFrameReprojectedPct, 0, kNow);
        st.setI32(metric::kFrameDropped, 0, kNow);
        if (source == 0) {
            for (uint16_t id : {metric::kFrameRenderWidth, metric::kFrameRenderHeight, metric::kFrameRenderSource}) {
                st.setNa(id, Reason::NotConnected, kNow);
            }
        } else {
            st.setI32(metric::kFrameRenderWidth, 3048, kNow);
            st.setI32(metric::kFrameRenderHeight, 3048, kNow);
            st.setEnum(metric::kFrameRenderSource, static_cast<uint32_t>(source), kNow);
        }
        return st;
    };
    const Layout frame = oneWidget(inst(W::Frame));
    int width = 0;
    const Pixels none = renderKo(frame, frameStore(0), "frame_render_none.png", &width);
    for (const WidgetLanguage language : {WidgetLanguage::Ko, WidgetLanguage::En, WidgetLanguage::Ja}) {
        for (const int source : {1, 2}) {
            GridRenderer renderer(fonts());
            renderer.setLayout(frame);
            const MetricStore st = frameStore(source);
            WidgetContext ctx;
            ctx.store = &st;
            ctx.now = kNow;
            ctx.language = language;
            renderer.render(ctx);
            if (language == WidgetLanguage::Ko && source == 2) renderer.writePng(pngDir() + "/frame_render_pc.png");
            const Pixels px = renderer.toRgba();
            CHECK(px != none || language != WidgetLanguage::Ko);
            // The graph (from the card's x 202 on) is untouched: the size stays in the text column
            if (language == WidgetLanguage::Ko) {
                const int height = static_cast<int>(px.size() / 4 / width);
                bool graphSame = true;
                for (int y = 0; y < height; ++y) {
                    for (int x = 8 + 202 - 2; x < width; ++x) {
                        const size_t k = (static_cast<size_t>(y) * width + x) * 4;
                        for (int c = 0; c < 4; ++c) graphSame = graphSame && px[k + c] == none[k + c];
                    }
                }
                CHECK(graphSame);
                // Simple A shows it too (2026-11-14), top left
                const Layout simpleA = oneWidget(inst(W::SimpleA));
                const Pixels aNone = renderKo(simpleA, frameStore(0), nullptr);
                const Pixels aSize = renderKo(simpleA, frameStore(source),
                                              source == 2 ? "simple_a_render_pc.png" : nullptr);
                CHECK(aSize != aNone);
            }
        }
    }
}

unsigned rgbOf(const Pixels& px, int width, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * width + x) * 4;
    return (unsigned(px[i]) << 16) | (unsigned(px[i + 1]) << 8) | px[i + 2];
}

void testSimpleTemplates() {
    const Layout a = oneWidget(inst(W::SimpleA));
    const Layout b = oneWidget(inst(W::SimpleB));
    // Keys, titles (Korean "간단 A" / "간단 B" in the palette), palette
    CHECK(std::string(widgetKey(W::SimpleA)) == "simple_a" && std::string(widgetKey(W::SimpleB)) == "simple_b");
    CHECK(std::string(widgetTitle(inst(W::SimpleA), WidgetLanguage::Ko)) == "간단 A");
    CHECK(std::string(widgetTitle(inst(W::SimpleB), WidgetLanguage::Ko)) == "간단 B");

    // The panel follows the layout: A is one 512 px column, B a small 250 px tile (half the width at the same text size;
    // fps, then battery)
    const GridGeometry ga = layoutGrid(a);
    CHECK(ga.width == 512 && ga.height == 248 && ga.slots.size() == 1 && ga.slots[0].w == 496);
    CHECK(std::fabs(ga.widthInColumns() - 1.0) < 1e-9);
    const GridGeometry gb = layoutGrid(b);
    CHECK(gb.width == 250 && gb.height == 116 && gb.slots[0].w == 234 && gb.slots[0].h == 100);
    CHECK(std::fabs(gb.widthInColumns() - 250.0 / 512.0) < 1e-9 && gb.textureWidth == 250 && gb.textureHeight == 116);
    // Side by side the widths add up; under a full widget B keeps its size in the wider column
    Layout both;
    both.columns = {{{inst(W::SimpleA)}}, {{inst(W::SimpleB)}}};
    const GridGeometry gab = layoutGrid(both);
    CHECK(gab.width == 762 && gab.slots[1].x == 512 + 8);
    Layout stacked = oneWidget(inst(W::Frame));
    stacked.columns[0].widgets.push_back(inst(W::SimpleB));
    const GridGeometry gs = layoutGrid(stacked);
    CHECK(gs.width == 512 && gs.slots[1].w == 234);
    CHECK(layoutGrid(defaultLayout()).width == 1024);  // the full-width widgets are unchanged

    // Normal and warning states, the mockup's values
    int wa = 0, wb = 0;
    const Pixels aCalm = renderKo(a, simpleStore(false), "simple_a.png", &wa);
    const Pixels aWarn = renderKo(a, simpleStore(true), "simple_a_warn.png");
    const Pixels bCalm = renderKo(b, simpleStore(false), "simple_b.png", &wb);
    const Pixels bWarn = renderKo(b, simpleStore(true), "simple_b_warn.png");
    CHECK(wa == 512 && wb == 250);
    CHECK(aCalm != aWarn && bCalm != bWarn);
    // The fps dot: the accent when fine, red at half the refresh rate (Perf's fps thresholds)
    CHECK(rgbOf(aCalm, wa, 28, 80) == 0xe27dfd && rgbOf(aWarn, wa, 28, 80) == 0xf85149);
    CHECK(rgbOf(bCalm, wb, 28, 40) == 0xe27dfd && rgbOf(bWarn, wb, 28, 40) == 0xf85149);
    // The state pill: green "정상", yellow "절반 제한" when SteamVR throttles the app
    CHECK(rgbOf(aCalm, wa, 484, 39) == 0x3fb950 && rgbOf(aWarn, wa, 484, 39) == 0xd29922);
    // Yellow between the thresholds (80 of 90 fps: below 95 %, above 75 %)
    MetricStore slow = simpleStore(false);
    slow.setF32(metric::kFrameAppFps, 80, 1.0);
    CHECK(rgbOf(renderKo(b, slow, "simple_b_slow.png"), wb, 28, 40) == 0xd29922);
    // B's battery row: red at 14 % (battery_crit_pct 15), yellow at 25 %, plain when full
    const auto batteryColoured = [&](const MetricStore& st, unsigned rgb) {
        const Pixels px = renderKo(b, st, nullptr);
        int n = 0;
        for (int y = 8 + 62; y < 8 + 100; ++y) {
            for (int x = 8; x < 242; ++x) n += rgbOf(px, wb, x, y) == rgb;
        }
        return n;
    };
    CHECK(batteryColoured(simpleStore(true), 0xf85149) > 20);
    CHECK(batteryColoured(simpleStore(false), 0xf85149) == 0 && batteryColoured(simpleStore(false), 0xd29922) == 0);
    MetricStore lowish = simpleStore(false);
    lowish.setF32(metric::kBatteryPct, 25, 1.0);
    CHECK(batteryColoured(lowish, 0xd29922) > 20);
    // Charging: a green bolt and "충전 중" (the % keeps its threshold colour); on battery no state at all
    MetricStore charging = simpleStore(false);
    charging.setF32(metric::kBatteryPct, 62, 1.0);
    charging.setEnum(metric::kBatteryStatus, static_cast<uint32_t>(BatteryStatus::Charging), 1.0);
    renderKo(b, charging, "simple_b_charging.png");
    renderKo(a, charging, "simple_a_charging.png");
    CHECK(batteryColoured(charging, 0x3fb950) > 20);
    MetricStore onBattery = simpleStore(false);
    onBattery.setF32(metric::kBatteryPct, 62, 1.0);
    onBattery.setEnum(metric::kBatteryStatus, static_cast<uint32_t>(BatteryStatus::Discharging), 1.0);
    onBattery.setEnum(metric::kBatteryChargerOnline, 0, 1.0);
    renderKo(b, onBattery, "simple_b_on_battery.png");
    CHECK(batteryColoured(onBattery, 0x3fb950) == 0);
    // ...and a battery reading alone redraws B (a borrowed metric)
    {
        GridRenderer r(fonts());
        r.setLayout(b);
        MetricStore st = simpleStore(false);
        WidgetContext c;
        c.store = &st;
        c.now = kNow;
        c.language = WidgetLanguage::Ko;
        r.render(c);
        st.setF32(metric::kBatteryPct, 99, 2.0);
        CHECK(r.render(c) && r.lastRedrawn() == 1);
    }

    // CPU: shows the total, coloured by the busiest core (the thresholds are for that). Count red / yellow pixels in
    // the CPU number's corner of row 2 (GPU at 62 % is blue, RAM plain)
    const auto cpuColoured = [&](double total, double busiest, unsigned rgb) {
        MetricStore st = simpleStore(false);
        st.setF32(metric::kCpuUsagePct, total, 1.0);
        st.setF32(metric::kCpuMaxCorePct, busiest, 1.0);
        const Pixels px = renderKo(a, st, nullptr);
        int n = 0;
        for (int y = 112 + 8; y < 112 + 8 + 56; ++y) {
            for (int x = 8; x < 180; ++x) n += rgbOf(px, wa, x, y) == rgb;
        }
        return n;
    };
    CHECK(cpuColoured(16, 98, 0xf85149) > 20);   // one core flat out: red, although the total is 16 %
    CHECK(cpuColoured(16, 90, 0xd29922) > 20);   // yellow between 85 and 97
    CHECK(cpuColoured(99, 50, 0xf85149) == 0 && cpuColoured(99, 50, 0xd29922) == 0);  // the total doesn't colour it
    CHECK(cpuColoured(16, 38, 0xf85149) == 0 && cpuColoured(16, 38, 0xd29922) == 0);
    // ...and a change of the busiest core alone redraws it (the signature follows the borrowed metric)
    {
        GridRenderer r(fonts());
        r.setLayout(a);
        MetricStore st = simpleStore(false);
        WidgetContext c;
        c.store = &st;
        c.now = kNow;
        c.language = WidgetLanguage::Ko;
        r.render(c);
        st.setF32(metric::kCpuMaxCorePct, 99, 2.0);
        CHECK(r.render(c) && r.lastRedrawn() == 1);
    }

    // Nothing read yet / no SteamVR: still drawn, no pill
    MetricStore empty;
    CHECK(!renderKo(a, empty, "simple_a_empty.png").empty() && !renderKo(b, empty, nullptr).empty());
    MetricStore noVr;
    noVr.setNa(metric::kFrameAppFps, Reason::NoSteamVr, 1.0);
    const Pixels aNoVr = renderKo(a, noVr, "simple_a_no_steamvr.png");
    CHECK(rgbOf(aNoVr, wa, 484, 39) != 0x3fb950 && rgbOf(aNoVr, wa, 484, 39) != 0xd29922);

    // A borrowed reading redraws the widget (its signature follows the metrics it shows, not only owned ones)
    GridRenderer renderer(fonts());
    renderer.setLayout(a);
    MetricStore live = simpleStore(false);
    WidgetContext ctx;
    ctx.store = &live;
    ctx.now = kNow;
    ctx.language = WidgetLanguage::Ko;
    renderer.render(ctx);
    CHECK(!renderer.render(ctx) && renderer.lastRedrawn() == 0);
    live.setF32(metric::kBatteryPct, 99, 2.0);
    CHECK(renderer.render(ctx) && renderer.lastRedrawn() == 1);
    live.setF32(metric::kTempCpuC, 60, 3.0);  // not shown: no redraw
    CHECK(!renderer.render(ctx));
}

}  // namespace

// The Dock's highlight: an accent border in the panel's margin while a laser points at the HUD. A change repaints
// the whole panel; a later partial redraw (a value changing) keeps it; turning it off takes it away.
void testHighlight() {
    for (const W type : {W::SimpleA, W::SimpleB}) {
        const Layout layout = oneWidget(inst(type));
        GridRenderer renderer(fonts());
        renderer.setLayout(layout);
        MetricStore calm = simpleStore(false);
        WidgetContext ctx;
        ctx.store = &calm;
        ctx.now = kNow;
        ctx.language = WidgetLanguage::Ko;
        CHECK(renderer.render(ctx));
        const int w = renderer.geometry().textureWidth, h = renderer.geometry().textureHeight;
        Pixels plain = renderer.toRgba();
        CHECK(!renderer.render(ctx));  // nothing changed
        CHECK(renderer.setHighlight(true) && !renderer.setHighlight(true));
        CHECK(renderer.render(ctx));  // the change repaints
        Pixels lit = renderer.toRgba();
        renderer.writePng(pngDir() + (type == W::SimpleA ? "/simple_a_highlight.png" : "/simple_b_highlight.png"));
        // The border: accent on the left edge's middle, the top edge's middle; the card inside untouched
        CHECK(rgbOf(lit, w, 2, h / 2) == 0xe27dfd);
        CHECK(rgbOf(lit, w, w / 2, 2) == 0xe27dfd);
        CHECK(rgbOf(plain, w, 2, h / 2) != 0xe27dfd);
        CHECK(rgbOf(lit, w, w / 2, h / 2) == rgbOf(plain, w, w / 2, h / 2));
        // A value changes: only its card is redrawn, the border stays
        MetricStore warn = simpleStore(true, 1.5);  // newer samples: the widget's inputs change
        ctx.store = &warn;
        CHECK(renderer.render(ctx));
        Pixels after = renderer.toRgba();
        CHECK(rgbOf(after, w, 2, h / 2) == 0xe27dfd);
        // Off again: gone
        CHECK(renderer.setHighlight(false));
        CHECK(renderer.render(ctx));
        CHECK(rgbOf(renderer.toRgba(), w, 2, h / 2) != 0xe27dfd);
        ctx.store = &calm;
    }
}

namespace {

// ---- Upstream PR #5's readouts Frame Dock took: compact cards stay as they were, Compact off adds rows (Frame, CPU,
// General), the process lists ----

unsigned pixelHash(cairo_surface_t* surface) {
    cairo_surface_flush(surface);
    const uint8_t* p = cairo_image_surface_get_data(surface);
    const size_t n = size_t(cairo_image_surface_get_stride(surface)) * size_t(cairo_image_surface_get_height(surface));
    unsigned h = 2166136261u;
    for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 16777619u;
    return h;
}

/** The card with its extra rows (Compact switched off). */
WidgetInstance rowsCard(W type) {
    return inst(type, false);
}

WidgetContext ctxFor(const MetricStore& store, int clockFormat = 24) {
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.clockFormat = clockFormat;
    return ctx;
}

// The default 2-column layout and every card in it: the heights it drew before, with and without a context
void testDefaultHeightsUnchanged() {
    const Layout def = defaultLayout();
    MetricStore empty;
    MetricStore connected;
    connected.setEnum(metric::kNetInterfaceUp, 1, kNow);
    connected.setEnum(metric::kNetConnected, 1, kNow);
    const WidgetContext emptyCtx = ctxFor(empty);
    const WidgetContext connectedCtx = ctxFor(connected);
    const GridGeometry plain = layoutGrid(def);
    const GridGeometry noLink = layoutGrid(def, &emptyCtx);
    const GridGeometry linked = layoutGrid(def, &connectedCtx);
    CHECK(plain.width == 1024 && plain.height == 422 && plain.columns == 2 && plain.slots.size() == 8);
    CHECK(noLink.width == 1024 && noLink.height == 422);
    CHECK(linked.width == 1024 && linked.height == 422);
    for (const GridGeometry* g : {&plain, &noLink, &linked}) {
        for (const WidgetSlot& s : g->slots) {
            int want = 0;
            switch (s.widget.type) {
                case W::Frame: case W::Power: case W::GpuRam: want = 102; break;
                case W::Temp: want = 102; break;     // compact in the default layout
                case W::Cpu: want = 64; break;
                case W::Battery: want = 88; break;   // compact
                case W::Network: want = g == &noLink ? 64 : 88; break;
                case W::General: want = 82; break;
                default: break;
            }
            CHECK(s.h == want);
        }
    }
    CHECK(widgetHeight(inst(W::Cpu), &emptyCtx) == 64);
    const WidgetContext noClock = ctxFor(empty, 0);
    CHECK(widgetHeight(inst(W::General), &emptyCtx) == 82 && widgetHeight(inst(W::General), &noClock) == 60);
    CHECK(widgetHeight(inst(W::Frame)) == 102 && widgetHeight(inst(W::Power)) == 102);
    CHECK(widgetHeight(inst(W::Temp)) == 102 && widgetHeight(inst(W::Battery)) == 88);
}

// A layout saved before (a key only on compact Temp / Battery) loads clean, writes back the same bytes and draws the
// same heights; so do the simple templates and a full Battery / Temp (no key: Frame Dock has no extra rows for them)
void testSavedLayoutsUnchanged() {
    const std::string saved =
        R"({"version":7,"columns":[[{"type":"frame"},{"type":"temp","compact":true},{"type":"battery","compact":true},)"
        R"({"type":"cpu"}],[{"type":"gpu_ram"},{"type":"power"},{"type":"network"},{"type":"general"}]]})";
    Layout layout;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(parseLayoutText(saved, layout, warnings, error) && warnings.empty());
    CHECK(layoutToJson(layout, 0) == saved);
    for (const LayoutColumn& c : layout.columns) {
        for (const WidgetInstance& w : c.widgets) CHECK(w.compact || !hasCompactView(w.type));
    }
    const GridGeometry g = layoutGrid(layout);
    CHECK(g.slots[0].h == 102 && g.slots[1].h == 102 && g.slots[2].h == 88 && g.slots[3].h == 64);
    CHECK(g.slots[7].h == 82);

    // the built-in default layout
    const std::string def = layoutToJson(defaultLayout(), 0);
    CHECK(def.find("false") == std::string::npos);
    Layout back;
    warnings.clear();
    CHECK(parseLayoutText(def, back, warnings, error) && warnings.empty() && layoutToJson(back, 0) == def);

    // the simple templates: same JSON, same size
    for (const char* text : {R"({"version":1,"columns":[[{"type":"simple_a"}]]})",
                             R"({"version":1,"columns":[[{"type":"simple_b"}]]})"}) {
        Layout simple;
        warnings.clear();
        CHECK(parseLayoutText(text, simple, warnings, error) && warnings.empty());
        CHECK(layoutToJson(simple, 0) == text);
        CHECK(layoutGrid(simple).slots[0].h == widgetHeight(inst(simple.columns[0].widgets[0].type)));
    }

    // a full Battery / Temp (no key)
    Layout full;
    CHECK(parseLayoutText(R"({"columns":[[{"type":"temp"},{"type":"battery"}]]})", full, warnings, error));
    CHECK(!full.columns[0].widgets[0].compact && !full.columns[0].widgets[1].compact);
    const GridGeometry fg = layoutGrid(full);
    CHECK(fg.slots[0].h == 154 && fg.slots[1].h == 142);
}

// Compact off: Frame, CPU and General get their extra rows
void testCompactOffHeights() {
    MetricStore empty;
    const WidgetContext ctx = ctxFor(empty);
    for (const W type : {W::Frame, W::Cpu, W::General}) {
        CHECK(hasCompactView(type));
        CHECK(widgetHeight(rowsCard(type), &ctx) > widgetHeight(inst(type), &ctx));
        CHECK(widgetHeight(rowsCard(type)) > widgetHeight(inst(type)));
    }
    CHECK(widgetHeight(rowsCard(W::Frame)) == 194);
    CHECK(widgetHeight(rowsCard(W::General), &ctx) == 82 + 3 * 22);
    // CPU: the bars, then one row per two clusters; no values yet = the Frame's four (two rows)
    CHECK(widgetHeight(rowsCard(W::Cpu)) == 188);
    MetricStore four;
    for (uint16_t k = 0; k < metric::kCpuClusterSlots; ++k) four.setF32(metric::kCpuCluster0Mhz + k, 2000, kNow);
    MetricStore one;
    one.setF32(metric::kCpuCluster0Mhz, 2000, kNow);
    for (uint16_t k = 1; k < metric::kCpuClusterSlots; ++k) one.setNa(metric::kCpuCluster0Mhz + k, Reason::SensorMissing, kNow);
    const WidgetContext fourCtx = ctxFor(four), oneCtx = ctxFor(one);
    CHECK(widgetHeight(rowsCard(W::Cpu), &fourCtx) == 188);
    CHECK(widgetHeight(rowsCard(W::Cpu), &oneCtx) == 166);
    // the rows card draws and differs from the compact one
    MetricStore store;
    store.setF32(metric::kCpuUsagePct, 20, kNow);
    store.setF32(metric::kCpuCore0Pct, 40, kNow);
    for (const W type : {W::Frame, W::Cpu, W::General}) {
        CHECK(renderPx(oneWidget(rowsCard(type)), store) != renderPx(oneWidget(inst(type)), store));
    }
}

// Compact on is the card as before: the new metrics change nothing
void testCompactOnIgnoresNewMetrics() {
    MetricStore base;
    base.setF32(metric::kCpuUsagePct, 30, kNow);
    base.setF32(metric::kFrameAppFps, 90, kNow);
    base.setF32(metric::kFrameDisplayHz, 90, kNow);
    MetricStore more = base;
    more.setF32(metric::kCpuCore0Pct, 40, kNow);
    more.setF32(metric::kCpuCluster0Mhz, 1800, kNow);
    more.setI32(metric::kUptimeS, 1000, kNow);
    more.setF32(metric::kSelfCpuPct, 0.5, kNow);
    more.setText(metric::kSceneAppName, "Beat Saber", kNow);
    for (const W type : {W::Frame, W::Cpu, W::General}) {
        const Layout layout = oneWidget(inst(type));
        CHECK(renderPx(layout, base) == renderPx(layout, more));
    }
}

// CPU / RAM items: 6 + 26 + 20 per row + 10, N/A without a scan, cairo stays happy
void testProcessListWidgets() {
    for (const W type : {W::CpuItems, W::RamItems}) {
        WidgetInstance w = inst(type);
        CHECK(widgetHeight(w) == 6 + 26 + 20 * 15 + 10);
        w.maxItems = 3;
        CHECK(widgetHeight(w) == 6 + 26 + 20 * 3 + 10);
        w.maxItems = kMaxListItems;
        CHECK(widgetHeight(w) == 6 + 26 + 20 * 50 + 10);
        CHECK(isProcessList(type) && !hasCompactView(type));
    }
    MetricStore store;
    ProcessListsView lists;
    lists.cpuValid = true;
    lists.generation = 1;
    lists.byCpu = {{"vrchat", 10, 1, 42.5, 800}, {"steamwebhelper", 0, 3, 12.0, 300}};
    lists.byRam = {{"vrchat", 10, 1, 42.5, 800}};
    store.setI32(metric::kCpuItemsRows, 2, kNow);
    store.setI32(metric::kRamItemsRows, 1, kNow);

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 512, 600);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts()};
    const WidgetHistory history;
    for (const W type : {W::CpuItems, W::RamItems}) {
        WidgetContext ctx = ctxFor(store);  // ctx.processes == nullptr: N/A text
        drawWidgetDynamic(pen, 8, 8, inst(type), ctx, history);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
        const unsigned na = pixelHash(surface);
        ctx.processes = &lists;
        drawWidgetDynamic(pen, 8, 8, inst(type), ctx, history);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
        CHECK(pixelHash(surface) != na);  // the rows drew something
    }
    // in the grid too, and a store that never saw the scan
    const Layout layout = oneWidget(inst(W::CpuItems));
    CHECK(renderPx(layout, MetricStore()) != renderPx(layout, store));
    // a new scan (generation) redraws the list, even with the same metrics
    WidgetContext a = ctxFor(store);
    a.processes = &lists;
    const uint64_t before = widgetSignature(inst(W::CpuItems), a);
    ++lists.generation;
    CHECK(widgetSignature(inst(W::CpuItems), a) != before);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// The hide_rules.json hint takes the card's last row, the card keeps its height; without Hide system it isn't drawn
void testHideRulesErrorHint() {
    MetricStore store;
    store.setI32(metric::kCpuItemsRows, 2, kNow);
    ProcessListsView lists;
    lists.cpuValid = true;
    lists.generation = 1;
    lists.byCpu = {{"vrchat", 10, 1, 42.5, 800}, {"steamwebhelper", 0, 3, 12.0, 300}};
    const auto draw = [&](bool hideSystem, bool error) {
        lists.hideRulesError = error;
        WidgetInstance card = inst(W::CpuItems);
        card.hideSystem = hideSystem;
        card.maxItems = 4;
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 512, 200);
        cairo_t* cr = cairo_create(surface);
        const Pen pen {cr, &fonts()};
        WidgetContext ctx = ctxFor(store);
        ctx.processes = &lists;
        drawWidgetDynamic(pen, 8, 8, card, ctx, WidgetHistory());
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
        const unsigned hash = pixelHash(surface);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return hash;
    };
    CHECK(draw(true, true) != draw(true, false));    // the hint is on the card
    CHECK(draw(false, true) == draw(false, false));  // Hide system off: nothing to say
    WidgetInstance card = inst(W::CpuItems);
    card.hideSystem = true;
    card.maxItems = 4;
    WidgetContext ctx = ctxFor(store);
    ctx.processes = &lists;
    lists.hideRulesError = false;
    const int plain = widgetHeight(card, &ctx);
    lists.hideRulesError = true;
    CHECK(widgetHeight(card, &ctx) == plain && plain == 6 + 26 + 20 * 4 + 10);
}

// The approved mockup's panel (2026-11-03), in Korean with made-up values, for a look by eye: Frame, CPU, General with
// Compact off; CPU / RAM items (5 rows, one row per name), GPU & RAM
void testReadoutsPng() {
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {rowsCard(W::Frame), rowsCard(W::Cpu), rowsCard(W::General)};
    WidgetInstance cpuItems = inst(W::CpuItems), ramItems = inst(W::RamItems);
    cpuItems.maxItems = ramItems.maxItems = 5;
    cpuItems.sumSameName = ramItems.sumSameName = true;
    layout.columns[1].widgets = {cpuItems, ramItems, inst(W::GpuRam)};
    ProcessListsView lists;
    lists.cpuValid = true;
    lists.byCpu = {{"text-editor", 0, 11, 42.6, 2240}, {"Xwayland", 0, 1, 7.9, 120}, {"vrserver", 0, 1, 5.9, 300},
                   {"gamescope-wl", 0, 1, 4.5, 90}, {"XRServiceLoopTh", 0, 1, 4.5, 564}};
    lists.byRam = {{"chrome", 0, 16, 1.0, 2342}, {"text-editor", 0, 11, 42.6, 2240}, {"steamwebhelper", 0, 9, 1, 1502},
                   {"vrwebhelper", 0, 9, 1, 1077}, {"XRServiceLoopTh", 0, 1, 4.5, 564}};
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    MetricStore st;
    const double cores[] = {12, 12, 22, 20, 26, 3, 5, 0};
    const double clusters[][3] = {{2265.6, 2265.6, 2265.6}, {3148.8, 3148.8, 3148.8}, {499.2, 2956.8, 2956.8},
                                  {480.0, 2419.2, 3052.8}};
    for (int k = 0; k < 40; ++k) {
        const double t = kNow - 20 + 0.5 * k;
        st.setF32(metric::kFrameAppFps, 90 - (k == 30 ? 5 : 0), t);
        st.setF32(metric::kFrameDisplayHz, 90, t);
        st.setF32(metric::kFrameCpuMs, 4.1, t);
        st.setF32(metric::kFrameGpuMs, 8.2, t);
        st.setF32(metric::kFrameCpuMaxMs, 5.2 + (k % 3) * 0.6 + (k == 30 ? 4 : 0), t);
        st.setF32(metric::kFrameGpuMaxMs, 9.3 + (k % 4) * 0.4 + (k == 30 ? 4 : 0), t);
        st.setF32(metric::kFrameReprojectedPct, 0, t);
        st.setI32(metric::kFrameDropped, 0, t);
        st.setEnum(metric::kFrameThrottled, 0, t);
        st.setF32(metric::kCpuUsagePct, 11, t);
        st.setF32(metric::kCpuMaxCorePct, 26, t);
        st.setF32(metric::kCpuFastestClockMhz, 3148.8, t);
        for (uint16_t c = 0; c < metric::kCpuCoreSlots; ++c) st.setF32(metric::kCpuCore0Pct + c, cores[c], t);
        for (uint16_t c = 0; c < metric::kCpuClusterSlots; ++c) {
            st.setF32(metric::kCpuCluster0Mhz + c, clusters[c][0], t);
            st.setF32(metric::kCpuCluster0MaxMhz + c, clusters[c][1], t);
            st.setF32(metric::kCpuCluster0HwMaxMhz + c, clusters[c][2], t);
        }
        st.setF32(metric::kCtrlLeftPct, 72, t);
        st.setEnum(metric::kCtrlLeftCharging, 0, t);
        st.setF32(metric::kCtrlRightPct, 65, t);
        st.setEnum(metric::kCtrlRightCharging, 0, t);
        st.setText(metric::kClock, "2026-11-03 21:19:00", t);
        st.setI32(metric::kUptimeS, 42 * 60 + 7, t);
        st.setI32(metric::kScenePid, 4242, t);
        st.setText(metric::kSceneAppName, "Beat Saber", t);
        st.setF32(metric::kSelfCpuPct, 0.1, t);
        st.setF32(metric::kSelfRssMib, 47, t);
        st.setF32(metric::kSelfPowerW, 0.01, t);
        st.setI32(metric::kCpuItemsRows, 5, t);
        st.setI32(metric::kRamItemsRows, 5, t);
        st.setF32(metric::kGpuClockMhz, 231, t);
        st.setF32(metric::kGpuMaxClockMhz, 903, t);
        st.setF32(metric::kRamUsedGib, 7.1, t);
        st.setF32(metric::kRamTotalGib, 15.3, t);
        ++lists.generation;
        WidgetContext ctx = ctxFor(st);
        ctx.now = t;
        ctx.processes = &lists;
        ctx.language = WidgetLanguage::Ko;
        renderer.render(ctx);
    }
    CHECK(renderer.writePng(pngDir() + "/readouts_ko.png"));
    const GridGeometry& g = renderer.geometry();
    CHECK(g.slots.size() == 6 && g.slots[0].h == 194 && g.slots[1].h == 188 && g.slots[2].h == 148);
}

}  // namespace

// ---- The controls (Frame Dock) ----

void testControls() {
    const W types[] = {W::ColorPresets, W::Volume, W::Passthrough, W::Profiles, W::Browser, W::Record};
    // Keys round-trip; a column of all six fits one texture
    Layout all;
    all.columns.resize(1);
    for (const W t : types) {
        CHECK(isControlWidget(t) && widgetKey(t) != nullptr);
        W parsed = W::None;
        CHECK(parseWidgetKey(widgetKey(t), parsed) && parsed == t);
        CHECK(widgetHeight(inst(t)) == 100 && widgetWidth(inst(t)) == kColumnWidth);
        all.columns[0].widgets.push_back(inst(t));
    }
    CHECK(!isControlWidget(W::SimpleB) && !isControlWidget(W::None));
    ControlsView v;
    v.interactive = true;
    v.colorAvailable = v.compareUsable = v.soundKnown = true;
    v.presetNames[0] = "표준";
    v.presetNames[1] = "비비드";
    v.presetNames[2] = "야간";
    v.presetInUse = 0;
    v.volume = 45;
    v.passthrough = 2;
    v.monoColorUsable = true;  // Camera Switch: 끔 | 흑백 | 컬러
    v.profileNames = {"A", "B", "C", "D", "E"};
    v.activeProfile = 1;
    // Buttons: inside the card's row, side by side, left to right, the ids the main loop acts on
    const auto row = [&](W t, const ControlsView* view) { return controlButtons(inst(t), view, 8, 200); };
    const std::vector<ControlButton> color = row(W::ColorPresets, &v);
    CHECK(color.size() == 4 && color[0].id == 0 && color[3].id == 3);
    for (size_t i = 0; i < color.size(); ++i) {
        CHECK(color[i].usable && color[i].y > 200 && color[i].y + color[i].h < 300 && color[i].h >= 56);
        if (i > 0) CHECK(color[i].x > color[i - 1].x + color[i - 1].w);
    }
    const std::vector<ControlButton> volume = row(W::Volume, &v);
    CHECK(volume.size() == 4 && volume[0].id == 0 && volume[1].id == 1 && volume[2].id == -1 && volume[3].id == 2);
    CHECK(!volume[2].usable);  // the level is shown, not pressed
    CHECK(row(W::Passthrough, &v).size() == 3);
    CHECK(row(W::Profiles, &v).size() == static_cast<size_t>(kControlProfiles));  // the first four only
    CHECK(row(W::Browser, &v).size() == 1);
    CHECK(row(W::Record, &v).size() == 1 && row(W::Record, &v)[0].usable && row(W::Record, &v)[0].id == 0);
    ControlsView saving = v;
    saving.recordState = 2;  // stopped, the file being finished: no press until it has ended
    CHECK(!row(W::Record, &saving)[0].usable);
    CHECK(std::string(widgetName(W::Record)) == "Record" && std::string(widgetKey(W::Record)) == "record");
    CHECK(recordElapsedText(0) == "● 0:00" && recordElapsedText(42) == "● 0:42" && recordElapsedText(725) == "● 12:05");
    CHECK(recordElapsedText(3723) == "● 1:02:03" && recordElapsedText(-5) == "● 0:00");
    // Not pressable: no view, in-game interaction off, mono / colour without Camera Switch, compare without the layer
    for (const ControlButton& b : row(W::ColorPresets, nullptr)) CHECK(!b.usable);
    ControlsView off = v;
    off.interactive = false;
    for (const ControlButton& b : row(W::Browser, &off)) CHECK(!b.usable);
    ControlsView noCamera = v;
    noCamera.monoColorUsable = false;
    const std::vector<ControlButton> pass = row(W::Passthrough, &noCamera);  // 끔 | 켬, as the 기기 page
    CHECK(pass.size() == 2 && pass[0].usable && pass[1].usable && pass[1].id == 1);
    ControlsView noLayer = v;
    noLayer.compareUsable = false;
    const std::vector<ControlButton> noCompare = row(W::ColorPresets, &noLayer);
    CHECK(noCompare[0].usable && !noCompare[3].usable);
    CHECK(row(W::SimpleA, &v).empty());
    // Drawn, and redrawn when what they show changes (the signature follows the view)
    MetricStore store;
    GridRenderer renderer(fonts());
    renderer.setLayout(all);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.controls = &v;
    ctx.language = WidgetLanguage::Ko;
    v.recordState = 1;  // recording: "● 0:42" beside the title, 정지 selected
    v.recordSeconds = 42;
    renderer.render(ctx);
    renderer.writePng(pngDir() + "/controls.png");
    CHECK(!renderer.render(ctx));  // nothing changed
    v.volume = 50;
    CHECK(renderer.render(ctx));
    v.recordSeconds = 43;  // the recording's clock redraws the card
    CHECK(renderer.render(ctx));
    for (const int state : {0, 2, 3, 4}) {  // idle, saving, ended by itself, another recorder runs
        v.recordState = state;
        CHECK(renderer.render(ctx));
        renderer.writePng(pngDir() + "/controls-record-" + std::to_string(state) + ".png");
    }
    v.pressedType = W::Browser;
    v.pressedButton = 0;
    CHECK(renderer.render(ctx));
}

int main() {
    testGeometry();
    testGeneralClockOffShrinks();
    testNetworkShrinksWhenNotExpanded();
    testCompactHeights();
    testDefaultHeightsUnchanged();
    testSavedLayoutsUnchanged();
    testCompactOffHeights();
    testCompactOnIgnoresNewMetrics();
    testProcessListWidgets();
    testHideRulesErrorHint();
    testReadoutsPng();
    testTitles();
    testEmptyStoreAllTypes();
    testDefaultFilled();
    testClockOffHidesTime();
    testNetworkInterfaceDown();
    testInvalidUtf8Text();
    testLanguageChangesTitles();
    testChangeDetection();
    testPartialRedraw();
    testCompactToggleKeepsGraphHistory();
    testSegmentedPillColours();
    testSimpleTemplates();
    testHighlight();
    testControls();
    testFrameRenderSize();
    if (failures == 0) std::printf("render-grid-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
