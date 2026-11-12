// The browser panel's bar and page, and what the laser does there (browser/browser_view.*, browser_controller.*):
// clicks and drags mapped onto the page, scrolling, a long press (selecting, the context menu), the right button, the
// bar's buttons, 즐겨찾기 (☆, the list, its file: browser/favorites.*), the keyboard typing into the page or the
// address field, the bar at 1080p. Draws the panel ($FRAMEDOCK_PNG_DIR/browser-*.png when set) with a made-up page in
// place of Chrome's.
#include "app_text.h"
#include "browser_controller.h"
#include "browser_view.h"
#include "draw.h"
#include "favorites.h"
#include "theme.h"
#include "vr_keyboard.h"

#include <cairo.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
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

using Hit = BrowserPanel::Hit;
using Request = BrowserController::Request;
using Type = PanelPointer::Type;

const char* kFont = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
const char* kBold = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";

/** Records what would go to Chrome, one line per call */
class Recorder : public BrowserInput {
public:
    std::vector<std::string> calls;
    void navigate(const std::string& text) override { calls.push_back("navigate " + text); }
    void back() override { calls.push_back("back"); }
    void forward() override { calls.push_back("forward"); }
    void reload() override { calls.push_back("reload"); }
    void mouse(BrowserSession::Mouse kind, double x, double y) override {
        char line[96];
        std::snprintf(line, sizeof(line), "%s %.0f,%.0f",
                      kind == BrowserSession::Mouse::Move ? "move" : kind == BrowserSession::Mouse::Down ? "down" : "up", x, y);
        calls.push_back(line);
    }
    void wheel(double x, double y, double dx, double dy) override {
        char line[96];
        std::snprintf(line, sizeof(line), "wheel %.0f,%.0f %.0f,%.0f", x, y, dx, dy);
        calls.push_back(line);
    }
    void insertText(const std::string& text) override { calls.push_back("text " + text); }
    void answerDialog(bool accept) override { calls.push_back(accept ? "dialog accept" : "dialog dismiss"); }
    void contextMenu(double x, double y, int seq) override {
        char line[96];
        std::snprintf(line, sizeof(line), "context %.0f,%.0f #%d", x, y, seq);
        calls.push_back(line);
    }
    void key(cdp::Key key) override {
        calls.push_back(key == cdp::Key::Backspace ? "key backspace" : key == cdp::Key::Enter ? "key enter"
                        : key == cdp::Key::SelectAll ? "key selectall" : key == cdp::Key::Left ? "key left"
                        : key == cdp::Key::Copy      ? "key copy"
                        : key == cdp::Key::Paste     ? "key paste"
                                                     : "key other");
    }
    std::string take() {
        std::string out;
        for (const std::string& c : calls) out += (out.empty() ? "" : " | ") + c;
        calls.clear();
        return out;
    }
};

struct Rig {
    FontSet fonts;
    BrowserPanel panel;
    VrKeyboard keyboard;
    Recorder input;
    BrowserController controller;
    // 즐겨찾기 with nothing saved (never written here: the tests that save use their own file)
    FavoriteStore favorites {"/nonexistent/framedock-test/favorites.json"};
    Rig() : panel(loadFonts(fonts), 1280, 720), keyboard(fonts), controller(panel, keyboard, input) {
        controller.setFavorites(&favorites);
        BrowserSession::Status ready;
        ready.phase = BrowserSession::Phase::Ready;
        ready.url = "https://www.youtube.com/results?search_query=%EA%B3%A0%EC%96%91%EC%9D%B4";
        ready.title = "고양이 - YouTube";
        ready.canBack = true;
        controller.setStatus(ready);
        draw();
    }
    static FontSet& loadFonts(FontSet& f) {
        f.load(kFont, kBold);
        return f;
    }
    void draw() { panel.renderBar(controller.barState(false, 0.45, true, true)); }
    std::vector<Request> at(Type type, double x, double y, double now = 0) {
        PanelPointer p;
        p.type = type;
        p.x = x;
        p.y = y;
        const std::vector<Request> r = controller.panelPointer(p, now);
        if (controller.takeBarDirty()) draw();
        return r;
    }
    std::vector<Request> press(Hit hit, int arg = 0) {
        double x = 0, y = 0;
        if (!panel.hitCenter(hit, arg, x, y)) {
            std::printf("  no hit area %d/%d\n", static_cast<int>(hit), arg);
            return {};
        }
        at(Type::Move, x, y);
        std::vector<Request> r = at(Type::Down, x, y);
        at(Type::Up, x, y);
        return r;
    }
    std::vector<Request> key(const std::string& id, double now = 0) {
        double x = 0, y = 0;
        if (!keyboard.keyCenter(id, x, y)) {
            std::printf("  no key %s\n", id.c_str());
            return {};
        }
        PanelPointer p;
        p.x = x;
        p.y = y;
        p.type = Type::Move;
        controller.keyboardPointer(p, now);
        p.type = Type::Down;
        std::vector<Request> r = controller.keyboardPointer(p, now);
        p.type = Type::Up;
        controller.keyboardPointer(p, now);
        return r;
    }
};

bool has(const std::vector<Request>& r, Request what) {
    for (const Request x : r) {
        if (x == what) return true;
    }
    return false;
}

/** A made-up page (a video site's results) in place of Chrome's frame */
BgraFrame fakePage(const FontSet& fonts, int w, int h) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(s);
    const Pen pen {cr, &fonts};
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.06);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_rectangle(cr, 0, 0, w, 64);
    cairo_fill(cr);
    pen.text(28, 42, "▶ VideoTube", 26, {0.95, 0.25, 0.25}, true);
    pen.color({0.18, 0.18, 0.18});
    pen.roundedRect(w * 0.3, 14, w * 0.4, 38, 19);
    cairo_fill(cr);
    pen.text(w * 0.3 + 20, 40, "고양이", 20, {0.9, 0.9, 0.9});
    const Color tints[] = {{0.85, 0.55, 0.30}, {0.35, 0.55, 0.85}, {0.45, 0.75, 0.45}, {0.75, 0.45, 0.75},
                           {0.80, 0.75, 0.35}, {0.40, 0.70, 0.75}};
    for (int i = 0; i < 6; ++i) {
        const double cw = (w - 56) / 3.0 - 24;
        const double x = 28 + (i % 3) * (w - 56) / 3.0, y = 90 + (i / 3) * (cw * 9 / 16 + 94);
        pen.color(tints[i]);
        pen.roundedRect(x, y, cw, cw * 9 / 16, 12);
        cairo_fill(cr);
        drawDisc(cr, x + cw / 2, y + cw * 9 / 32, 26, {1, 1, 1});
        pen.text(x, y + cw * 9 / 16 + 34, "귀여운 고양이 영상 " + std::to_string(i + 1), 20, {0.92, 0.92, 0.92}, true);
        pen.text(x, y + cw * 9 / 16 + 62, "조회수 " + std::to_string(12 + i * 7) + "만회 · " + std::to_string(i + 1) + "일 전",
                 16, {0.6, 0.6, 0.6});
    }
    cairo_surface_flush(s);
    BgraFrame f;
    f.width = w;
    f.height = h;
    f.pixels.resize(static_cast<size_t>(w) * h * 4);
    const unsigned char* data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y) std::memcpy(&f.pixels[static_cast<size_t>(y) * w * 4], data + y * stride, w * 4);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return f;
}

std::string pngPath(const std::string& name) {
    const char* dir = std::getenv("FRAMEDOCK_PNG_DIR");
    if (dir == nullptr || *dir == '\0') return {};
    std::filesystem::create_directories(dir);
    return std::string(dir) + "/" + name;
}

/** The panel with the keyboard hanging under it, as in the headset (gap of 24 px) */
bool writeTogether(const BrowserPanel& panel, const VrKeyboard& keyboard, const std::string& path) {
    const int gap = 24;
    const int w = panel.width(), h = panel.height() + gap + VrKeyboard::kHeight;
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(out);
    cairo_surface_t* a = cairo_image_surface_create_for_data(const_cast<uint8_t*>(panel.pixels()), CAIRO_FORMAT_ARGB32,
                                                             panel.width(), panel.height(), panel.width() * 4);
    cairo_set_source_surface(cr, a, 0, 0);
    cairo_paint(cr);
    cairo_surface_t* b = cairo_image_surface_create_for_data(const_cast<uint8_t*>(keyboard.pixels()), CAIRO_FORMAT_ARGB32,
                                                             VrKeyboard::kWidth, VrKeyboard::kHeight, VrKeyboard::kWidth * 4);
    cairo_set_source_surface(cr, b, (w - VrKeyboard::kWidth) / 2.0, panel.height() + gap);
    cairo_paint(cr);
    const bool ok = cairo_surface_write_to_png(out, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(a);
    cairo_surface_destroy(b);
    cairo_destroy(cr);
    cairo_surface_destroy(out);
    return ok;
}

void testPage() {
    Rig rig;
    CHECK(rig.panel.width() == 1280 && rig.panel.height() == BrowserPanel::kBarHeight + 720);
    CHECK(rig.panel.barHeight() == BrowserPanel::kBarHeight);
    const double top = BrowserPanel::kBarHeight;
    // A tap: the click at the page point (texture px → CSS px), sent on the release
    rig.at(Type::Move, 640, top + 360);
    CHECK(rig.input.take() == "move 640,360");
    rig.at(Type::Down, 640, top + 360);
    CHECK(rig.input.take().empty());
    rig.at(Type::Move, 645, top + 362);  // a wobble under the drag distance
    rig.at(Type::Up, 645, top + 362);
    CHECK(rig.input.take() == "move 640,360 | down 640,360 | up 640,360");
    // A drag: the page follows the laser (moving up scrolls down), no click
    rig.at(Type::Down, 400, top + 500);
    rig.at(Type::Move, 400, top + 470);
    rig.at(Type::Move, 400, top + 400);
    rig.at(Type::Up, 400, top + 400);
    CHECK(rig.input.take() == "wheel 400,500 0,30 | wheel 400,500 0,70");
    // The laser leaving mid-press: no click
    rig.at(Type::Down, 100, top + 100);
    rig.at(Type::Leave, 0, 0);
    rig.at(Type::Up, 100, top + 100);
    CHECK(rig.input.take().empty());
    // The thumbstick (SteamVR's scroll events): a wheel where the laser is
    rig.at(Type::Move, 300, top + 200);
    rig.input.take();
    PanelPointer scroll;
    scroll.type = Type::Scroll;
    scroll.scrollDy = -0.5;  // down
    rig.controller.panelPointer(scroll, 0);
    CHECK(rig.input.take() == "wheel 300,200 0,60");
    // Drag-to-scroll off: a real mouse drag (sliders, selecting text)
    rig.press(Hit::DragScroll);
    CHECK(!rig.controller.dragScroll());
    rig.input.take();
    rig.at(Type::Down, 200, top + 650);
    CHECK(rig.input.take() == "move 200,650 | down 200,650");
    rig.at(Type::Move, 260, top + 650);
    CHECK(rig.input.take() == "move 260,650");
    rig.at(Type::Up, 300, top + 650);
    CHECK(rig.input.take() == "up 300,650");
    rig.at(Type::Down, 200, top + 650);
    rig.at(Type::Leave, 0, 0);  // pointed away while holding: released where it last was
    CHECK(rig.input.take() == "move 200,650 | down 200,650 | up 200,650");
    // The bar is no page
    rig.at(Type::Move, 640, 30);
    CHECK(rig.input.take().empty());
}

void testDragDistance() {
    // The same 8 mm on the panel whatever its width in the room or its resolution: a move just short of it is a tap
    // (one click where it was pressed), just past it a scroll
    struct Case {
        double widthM;
        int viewportW, viewportH;
    };
    for (const Case c : {Case {0.25, 1280, 720}, Case {0.45, 1280, 720}, Case {0.45, 960, 540}, Case {1.0, 1920, 1080},
                         Case {1.5, 1280, 720}}) {
        Rig rig;
        rig.panel.setViewport(c.viewportW, c.viewportH);
        rig.controller.setPanelWidthM(c.widthM);
        rig.draw();
        const double px = rig.controller.dragStartPx();
        const double expected = std::max(BrowserController::kMinDragStartPx, 0.008 / c.widthM * c.viewportW);
        CHECK(std::fabs(px - expected) < 1e-9);
        CHECK(std::fabs(px * c.widthM / c.viewportW - 0.008) < 1e-9 || px == BrowserController::kMinDragStartPx);
        const double top = rig.panel.barHeight();
        const double x0 = c.viewportW / 2.0, y0 = top + c.viewportH / 2.0;
        rig.at(Type::Down, x0, y0);
        rig.at(Type::Move, x0, y0 - (px - 0.5));
        rig.at(Type::Up, x0, y0 - (px - 0.5));
        const std::string tap = rig.input.take();
        CHECK(tap.find("down ") != std::string::npos && tap.find("wheel") == std::string::npos);
        rig.at(Type::Down, x0, y0);
        rig.at(Type::Move, x0, y0 - (px + 0.5));
        rig.at(Type::Up, x0, y0 - (px + 0.5));
        const std::string drag = rig.input.take();
        CHECK(drag.find("wheel") != std::string::npos && drag.find("down ") == std::string::npos);
        std::printf("drag distance at %.2f m, %d px: %.1f px\n", c.widthM, c.viewportW, px);
    }
    // At the default 45 cm and 1280 px it is about 23 px (the old fixed 14 px was under 5 mm there)
    Rig rig;
    CHECK(std::fabs(rig.controller.dragStartPx() - 0.008 / 0.45 * 1280) < 1e-9);
}

void testBar() {
    Rig rig;
    CHECK(has(rig.press(Hit::Handle), Request::Grab));
    rig.press(Hit::Back);
    rig.press(Hit::Reload);
    CHECK(rig.input.take() == "back | reload");
    double x, y;
    CHECK(!rig.panel.hitCenter(Hit::Forward, 0, x, y));  // nothing to go forward to
    // 번역 asks the main loop; 보기 opens its list, whose presses ask for the setting and keep it open (a 비율
    // closes it: Chrome starts again)
    CHECK(has(rig.press(Hit::Translate), Request::Translate));
    rig.press(Hit::View);
    CHECK(rig.controller.viewOpen());
    CHECK(has(rig.press(Hit::ViewMobile, 1), Request::MobileOn) && rig.controller.viewOpen());
    CHECK(has(rig.press(Hit::ZoomIn), Request::ZoomIn) && has(rig.press(Hit::ZoomReset), Request::ZoomReset));
    rig.controller.setView(true, 125, 0);
    rig.draw();
    CHECK(rig.controller.barState(false, 0.45, true, true).zoomPct == 125);
    const std::string viewPath = pngPath("browser-view-list.png");
    if (!viewPath.empty()) CHECK(rig.panel.writePng(viewPath));
    CHECK(rig.press(Hit::Aspect, 0).empty());  // the 비율 it has: nothing
    CHECK(has(rig.press(Hit::Aspect, 3), Request::AspectTall) && !rig.controller.viewOpen());
    rig.press(Hit::View);
    rig.press(Hit::Back);  // a press elsewhere closes it
    CHECK(!rig.controller.viewOpen());
    rig.input.take();
    CHECK(has(rig.press(Hit::SizeUp), Request::SizeUp));
    CHECK(has(rig.press(Hit::SizeDown), Request::SizeDown));
    CHECK(has(rig.press(Hit::Pin), Request::TogglePin));
    CHECK(has(rig.press(Hit::Close), Request::Close));
    // 끄기: the first tap arms it (한 번 더), any other press lets it go, a second tap closes Chrome; it lapses by itself
    const auto quitArmed = [&] { return rig.controller.barState(false, 0.45, true, true).quitArmed; };
    CHECK(!has(rig.press(Hit::Quit), Request::QuitChrome) && quitArmed());
    const std::string quitPath = pngPath("browser-quit-armed.png");
    if (!quitPath.empty()) CHECK(rig.panel.writePng(quitPath));
    rig.press(Hit::Back);
    CHECK(!quitArmed());
    rig.input.take();
    rig.press(Hit::Quit);
    CHECK(has(rig.press(Hit::Quit), Request::QuitChrome) && !quitArmed());
    rig.press(Hit::Quit);
    rig.controller.tick(BrowserController::kQuitConfirmSec + 1);
    CHECK(!quitArmed());
    // Hit areas: every button's centre hits it, none overlaps another
    for (const Hit h : {Hit::Handle, Hit::Back, Hit::Reload, Hit::Address, Hit::Favorites, Hit::Keyboard, Hit::SizeDown,
                        Hit::SizeUp, Hit::Pin, Hit::DragScroll, Hit::Quit, Hit::Close, Hit::Translate, Hit::View}) {
        CHECK(rig.panel.hitCenter(h, 0, x, y));
        CHECK(rig.panel.hitAt(x, y) == h);
        CHECK(y < BrowserPanel::kBarHeight);
    }
    // The first row's buttons are at least 60 px tall, and 끄기 · 닫기 stand apart at the end: 30 px of nothing before
    for (const Hit h : {Hit::SizeDown, Hit::SizeUp, Hit::Pin, Hit::DragScroll, Hit::Quit, Hit::Close}) {
        CHECK(rig.panel.hitCenter(h, 0, x, y));
        CHECK(rig.panel.hitAt(x, y - 30) == h && rig.panel.hitAt(x, y + 30) == h);
    }
    CHECK(rig.panel.hitCenter(Hit::Quit, 0, x, y));
    double quitLeft = x;
    while (rig.panel.hitAt(quitLeft - 1, y) == Hit::Quit) --quitLeft;
    for (double gx = quitLeft - 30; gx < quitLeft; gx += 1) CHECK(rig.panel.hitAt(gx, y) == Hit::None);
    CHECK(rig.panel.hitCenter(Hit::Close, 0, x, y));
    double closeLeft = x;
    while (rig.panel.hitAt(closeLeft - 1, y) == Hit::Close) --closeLeft;
    CHECK(closeLeft + 2 * (x - closeLeft) >= 1280 - 20);  // its right edge near the panel's
    // Not ready: back, ☆ and 번역 can't be pressed; reload can (it starts Chrome again)
    BrowserSession::Status failed;
    failed.phase = BrowserSession::Phase::Failed;
    failed.error = "Chrome을 시작하지 못했어요: not found: flatpak";
    rig.controller.setStatus(failed);
    rig.draw();
    CHECK(!rig.panel.hasPage());
    CHECK(!rig.panel.hitCenter(Hit::Back, 0, x, y));
    CHECK(!rig.panel.hitCenter(Hit::Star, 0, x, y));
    CHECK(!rig.panel.hitCenter(Hit::Translate, 0, x, y));
    CHECK(rig.panel.hitCenter(Hit::Reload, 0, x, y));
    CHECK(browserStatusText(failed).find("새로고침을 누르면 다시 시도해요") != std::string::npos);
}

void testDialog() {
    Rig rig;
    BrowserSession::Status asking;
    asking.phase = BrowserSession::Phase::Ready;
    asking.url = "https://forms.example/";
    asking.title = "설문";
    asking.dialogType = "confirm";
    asking.dialogMessage = "정말 삭제할까요?\n되돌릴 수 없습니다";
    rig.controller.setStatus(asking);
    rig.draw();
    // The question takes the second row: 확인 and 취소, the navigation buttons are gone until it is answered
    double x, y;
    CHECK(rig.panel.hitCenter(Hit::DialogAccept, 0, x, y) && y < BrowserPanel::kBarHeight);
    CHECK(rig.panel.hitCenter(Hit::DialogDismiss, 0, x, y));
    CHECK(!rig.panel.hitCenter(Hit::Back, 0, x, y));
    CHECK(!rig.panel.hitCenter(Hit::Address, 0, x, y));
    CHECK(rig.panel.hitCenter(Hit::Close, 0, x, y));  // the panel's own row stays
    CHECK(browserDialogText(asking) == "페이지의 질문: 정말 삭제할까요? 되돌릴 수 없습니다");
    const std::string path = pngPath("browser-dialog.png");
    if (!path.empty()) CHECK(rig.panel.writePng(path));
    rig.press(Hit::DialogDismiss);
    rig.press(Hit::DialogAccept);
    CHECK(rig.input.take() == "dialog dismiss | dialog accept");
    asking.dialogType = "beforeunload";
    CHECK(browserDialogText(asking).find("떠날까요") != std::string::npos);
    asking.dialogType = "prompt";
    CHECK(browserDialogText(asking).find("기본값") != std::string::npos);
    // Answered: the usual row is back
    asking.dialogType.clear();
    rig.controller.setStatus(asking);
    rig.draw();
    CHECK(!rig.panel.hitCenter(Hit::DialogAccept, 0, x, y));
    CHECK(rig.panel.hitCenter(Hit::Address, 0, x, y));
    // A notice (a page that kept crashing Chrome) shows in the title's place
    asking.notice = "Chrome이 계속 종료되어 홈을 열었어요: crash.example/";
    rig.controller.setStatus(asking);
    rig.draw();
    const std::string noticePath = pngPath("browser-notice.png");
    if (!noticePath.empty()) CHECK(rig.panel.writePng(noticePath));
}

double wallNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void testHugePageText() {
    // A page controls its URL, title and dialog text: a megabyte of each is drawn in bounded time (was O(n²))
    Rig rig;
    std::string huge;
    huge.reserve(1u << 20);
    while (huge.size() < (1u << 20)) huge += "가나다abc/";  // multi-byte characters too
    BrowserSession::Status st;
    st.phase = BrowserSession::Phase::Ready;
    st.url = "https://huge.example/" + huge;
    st.title = huge;
    st.dialogType = "confirm";
    st.dialogMessage = huge;
    const double t0 = wallNow();
    rig.controller.setStatus(st);
    rig.draw();
    st.dialogType.clear();
    rig.controller.setStatus(st);
    rig.draw();
    const double took = wallNow() - t0;
    CHECK(took < 1.0);
    // The cut itself: valid UTF-8, ends in "…", fits, and is the longest that fits
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* cr = cairo_create(s);
    const Pen pen {cr, &rig.fonts};
    const double t1 = wallNow();
    const std::string cut = ellipsize(pen, huge, 22, 400, false);
    CHECK(wallNow() - t1 < 0.2);
    CHECK(cut.size() > 3 && cut.compare(cut.size() - 3, 3, "…") == 0);
    CHECK(pen.measure(cut, 22) <= 400);
    CHECK(huge.compare(0, cut.size() - 3, cut, 0, cut.size() - 3) == 0);
    // One character more would not fit
    size_t chars = 0;  // characters before the "…"
    for (size_t i = 0; i + 3 < cut.size(); ++i) chars += (static_cast<unsigned char>(cut[i]) & 0xC0) != 0x80 ? 1 : 0;
    const size_t longer = utf8Prefix(huge, chars + 1);
    CHECK(pen.measure(huge.substr(0, longer) + "…", 22) > 400);
    CHECK(utf8Prefix("가나다", 2) == 6 && utf8Prefix("가나다", 9) == 9 && utf8Prefix("ab", 0) == 0);
    CHECK(ellipsize(pen, "short", 22, 400, false) == "short");
    CHECK(ellipsize(pen, huge, 22, 1, false) == "…");  // nothing else fits
    // Text over the character limit that would fit anyway is still cut (with "…")
    const std::string many(1000, 'i');
    const std::string cutMany = ellipsize(pen, many, 4, 1e9, false);
    CHECK(cutMany == std::string(kEllipsizeMaxChars, 'i') + "…");
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

void testKeyboard() {
    Rig rig;
    // The address field: the page's address to edit (the caret at its end), its ✕ clears it, the keyboard types at
    // the caret (← → move it); enter searches and closes the keyboard
    CHECK(has(rig.press(Hit::Address), Request::Keyboard));
    CHECK(rig.controller.keyboardOpen() && rig.controller.addressEditing());
    const std::string url = "https://www.youtube.com/results?search_query=고양이";  // decoded, as Chrome's bar shows it
    CHECK(rig.controller.addressText() == url);
    CHECK(rig.controller.barState(false, 0.45, true, true).addressCaret == url.size());
    const std::string editPath = pngPath("browser-address-edit.png");
    if (!editPath.empty()) CHECK(rig.panel.writePng(editPath));
    rig.press(Hit::AddressClear);
    CHECK(rig.controller.addressText().empty() && rig.controller.addressEditing());
    for (const char* k : {"ㄱ", "ㅗ", "ㅇ", "ㅑ", "ㅇ", "ㅇ", "ㅣ"}) rig.key(k);
    CHECK(rig.controller.addressText() == "고양");
    CHECK(rig.controller.barState(false, 0.45, true, true).addressPreedit == "이");
    CHECK(rig.input.take().empty());  // nothing reached the page
    rig.key("left");  // the syllable first, then the caret before it
    CHECK(rig.controller.addressText() == "고양이" &&
          rig.controller.barState(false, 0.45, true, true).addressCaret == std::string("고양").size());
    rig.key("backspace");  // the one before the caret
    CHECK(rig.controller.addressText() == "고이");
    // A press in the field while editing puts the caret there: at the text's start, then past its end
    {
        double cx = 0, cy = 0;
        CHECK(rig.panel.hitCenter(Hit::Address, 0, cx, cy));
        double left = cx;
        while (left > 0 && rig.panel.hitAt(left - 1, cy) == Hit::Address) left -= 1;
        rig.at(Type::Move, left + 26, cy);
        rig.at(Type::Down, left + 26, cy);
        rig.at(Type::Up, left + 26, cy);
        CHECK(rig.controller.addressEditing() && rig.controller.barState(false, 0.45, true, true).addressCaret == 0);
        rig.press(Hit::Address);
        CHECK(rig.controller.barState(false, 0.45, true, true).addressCaret == std::string("고이").size());
        CHECK(rig.input.take().empty());
    }
    rig.key("right");
    rig.key("ㅇ");
    rig.key("ㅏ");
    CHECK(has(rig.key("enter"), Request::Keyboard));
    CHECK(rig.input.take() == "navigate 고이아");
    CHECK(!rig.controller.keyboardOpen() && !rig.controller.addressEditing());
    // The keyboard for the page: text goes in with insertText, keys as key events
    rig.press(Hit::Keyboard);
    CHECK(rig.controller.keyboardOpen() && !rig.controller.addressEditing());
    for (const char* k : {"ㅎ", "ㅏ", "ㄴ", "ㄱ", "ㅡ", "ㄹ"}) rig.key(k);
    CHECK(rig.input.take() == "text 한");
    rig.key("backspace");  // composing: a jamo back, nothing sent
    CHECK(rig.input.take().empty());
    rig.key("space");
    rig.key("backspace");
    rig.key("enter");
    rig.key("left");
    rig.key("clear");
    CHECK(rig.input.take() == "text 그  | key backspace | key enter | key left | key selectall | key backspace");
    // Closing the keyboard finishes the syllable into the page
    rig.key("ㅁ");
    rig.key("ㅏ");
    rig.press(Hit::Keyboard);
    CHECK(!rig.controller.keyboardOpen());
    CHECK(rig.input.take() == "text 마");
    // Its handle carries the panel; its close key closes it
    rig.press(Hit::Keyboard);
    CHECK(has(rig.key("handle"), Request::Grab));
    CHECK(has(rig.key("close"), Request::Keyboard));
    CHECK(!rig.controller.keyboardOpen());
    // Escapes that wouldn't be readable stay escaped: an overlong '/', a surrogate, a right-to-left override
    BrowserSession::Status odd;
    odd.phase = BrowserSession::Phase::Ready;
    odd.url = "https://x.example/%C0%AF/%ED%A0%80/%E2%80%AE/%ED%95%9C";
    rig.controller.setStatus(odd);
    rig.press(Hit::Address);
    CHECK(rig.controller.addressText() == "https://x.example/%C0%AF/%ED%A0%80/%E2%80%AE/한");
    rig.key("close");
    // Address editing, then 번역: the edit is dropped and 번역 asked
    rig.press(Hit::Address);
    rig.key("ㄴ");
    CHECK(has(rig.press(Hit::Translate), Request::Translate));
    CHECK(!rig.controller.keyboardOpen());
    CHECK(rig.input.take().empty());
    // A held key's repeat goes to the same place
    rig.press(Hit::Keyboard);
    double x, y;
    rig.keyboard.keyCenter("backspace", x, y);
    PanelPointer p;
    p.type = Type::Down;
    p.x = x;
    p.y = y;
    rig.controller.keyboardPointer(p, 10.0);
    rig.controller.tick(10.4);
    rig.controller.tick(10.5);
    p.type = Type::Up;
    rig.controller.keyboardPointer(p, 10.55);
    CHECK(rig.input.take() == "key backspace | key backspace | key backspace");
}

void testPictures() {
    Rig rig;
    const BgraFrame page = fakePage(rig.fonts, 1280, 720);
    rig.panel.showFrame(page);
    rig.draw();
    CHECK(rig.panel.hasPage());
    // The page is in the page area 1:1
    const uint8_t* p = rig.panel.pixels();
    const size_t at = (static_cast<size_t>(BrowserPanel::kBarHeight + 400) * 1280 + 640) * 4;
    CHECK(std::memcmp(p + at, &page.pixels[(400 * 1280 + 640) * 4], 4) == 0);
    double x, y;
    rig.panel.hitCenter(Hit::Keyboard, 0, x, y);
    rig.at(Type::Move, x, y);  // hovered
    const std::string path = pngPath("browser-panel.png");
    if (!path.empty()) CHECK(rig.panel.writePng(path));
    // Fixed in the room: its button says how to undo it
    rig.panel.renderBar(rig.controller.barState(true, 0.45, true, true));
    const std::string pinnedPath = pngPath("browser-pinned.png");
    if (!pinnedPath.empty()) CHECK(rig.panel.writePng(pinnedPath));

    // Typing a search into the address field, the keyboard under the panel
    rig.at(Type::Move, -1, -1);
    rig.press(Hit::Address);
    for (const char* k : {"ㄱ", "ㅗ", "ㅇ", "ㅑ", "ㅇ", "ㅇ", "ㅣ"}) rig.key(k);
    rig.draw();
    rig.keyboard.render();
    const std::string together = pngPath("browser-keyboard.png");
    if (!together.empty()) CHECK(writeTogether(rig.panel, rig.keyboard, together));

    // A smaller frame of the same shape (a resize on the way): scaled up to fill the page area
    const BgraFrame small = fakePage(rig.fonts, 640, 360);
    rig.panel.showFrame(small);
    CHECK(rig.panel.hasPage());
    const cdp::ViewMap smallMap = rig.panel.viewMap();
    CHECK(smallMap.areaW == 1280 && smallMap.areaH == 720 && smallMap.viewportW == 640 && smallMap.viewportH == 360);

    // Starting: the page area says so
    Rig starting;
    BrowserSession::Status s;
    s.phase = BrowserSession::Phase::Starting;
    starting.controller.setStatus(s);
    starting.draw();
    const std::string startPath = pngPath("browser-starting.png");
    if (!startPath.empty()) CHECK(starting.panel.writePng(startPath));
    s.phase = BrowserSession::Phase::Restarting;
    s.restarts = 2;
    s.error = "Chrome이 종료됐어요 (signal 11)";
    starting.controller.setStatus(s);
    starting.draw();
    const std::string restartPath = pngPath("browser-restarting.png");
    if (!restartPath.empty()) CHECK(starting.panel.writePng(restartPath));
}


std::filesystem::path testDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("framedock-browser-view-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    return dir;
}

std::string readFile(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

void testFavoriteStore() {
    const std::filesystem::path dir = testDir();
    const std::string path = (dir / "framedock" / "favorites.json").string();  // its folder doesn't exist yet
    using Change = FavoriteStore::Change;
    {
        FavoriteStore store(path);
        CHECK(store.load() && store.items().empty());  // no file: nothing saved yet
        CHECK(store.add("고양이 - YouTube", "https://www.youtube.com/results?q=1") == Change::Done);
        CHECK(store.add("A \"quoted\" title", "https://a.example/") == Change::Done);
        CHECK(store.add("again", "https://a.example/") == Change::None);  // saved already
        CHECK(store.find("https://a.example/") == 1 && store.find("https://b.example/") == -1);
        CHECK(!std::filesystem::exists(path + ".tmp"));
    }
    {
        FavoriteStore store(path);  // read back: the same list, in order
        CHECK(store.load() && store.items().size() == 2);
        CHECK(store.items()[0].name == "고양이 - YouTube" && store.items()[1].name == "A \"quoted\" title");
        CHECK(store.items()[1].url == "https://a.example/");
        CHECK(store.remove(0) == Change::Done && store.remove(5) == Change::None);
        for (size_t i = store.items().size(); i < FavoriteStore::kMax; ++i) {
            CHECK(store.add("p", "https://p.example/" + std::to_string(i)) == Change::Done);
        }
        CHECK(store.items().size() == FavoriteStore::kMax);
        CHECK(store.add("one more", "https://more.example/") == Change::Full);
        CHECK(store.find("https://more.example/") == -1);
    }
    {
        FavoriteStore store(path);
        CHECK(store.load() && store.items().size() == FavoriteStore::kMax && store.items()[0].url == "https://a.example/");
    }
    // A file that can't be read: none loaded, the file kept until the first change moves it aside
    writeFile(path, "{\"favorites\": [ broken");
    {
        FavoriteStore store(path);
        CHECK(!store.load() && store.items().empty() && !store.error().empty());
        CHECK(readFile(path) == "{\"favorites\": [ broken");
        CHECK(store.add("new", "https://new.example/") == Change::Done);
        CHECK(readFile(path + ".bad") == "{\"favorites\": [ broken");
        FavoriteStore again(path);
        CHECK(again.load() && again.items().size() == 1);
    }
    // Can't write (its folder is a file): kept in memory, says so
    {
        writeFile(dir / "blocker", "x");
        FavoriteStore store((dir / "blocker" / "favorites.json").string());
        store.load();
        CHECK(store.add("t", "https://t.example/") == Change::Unsaved && !store.error().empty());
        CHECK(store.items().size() == 1);
    }
    std::filesystem::remove_all(dir);
}

void testFavoritesOnTheBar() {
    const std::filesystem::path dir = testDir();
    FavoriteStore store((dir / "favorites.json").string());
    CHECK(store.load());
    Rig rig;
    rig.controller.setFavorites(&store);
    rig.draw();
    double x, y;
    // ☆ saves the page showing (its title), ★ removes it
    CHECK(rig.panel.hitCenter(Hit::Star, 0, x, y) && rig.panel.hitAt(x, y) == Hit::Star && y < rig.panel.barHeight());
    CHECK(!rig.controller.barState(false, 0.45, true, true).starred);
    rig.press(Hit::Star);
    CHECK(store.items().size() == 1 && store.items()[0].name == "고양이 - YouTube");
    CHECK(rig.controller.barState(false, 0.45, true, true).starred);
    rig.press(Hit::Star);
    CHECK(store.items().empty());
    rig.press(Hit::Star);
    BrowserSession::Status other;
    other.phase = BrowserSession::Phase::Ready;
    other.url = "https://www.google.com/";  // no title yet: the address stands in
    rig.controller.setStatus(other);
    rig.draw();
    rig.press(Hit::Star);
    CHECK(store.items().size() == 2 && store.items()[1].name == "www.google.com/");
    CHECK(rig.input.take().empty());
    // 즐겨찾기 opens the list: a row opens its page and closes the list
    rig.press(Hit::Favorites);
    CHECK(rig.controller.favoritesOpen());
    int arg = -1;
    CHECK(rig.panel.hitCenter(Hit::FavoriteOpen, 1, x, y) && rig.panel.hitAt(x, y, &arg) == Hit::FavoriteOpen && arg == 1);
    CHECK(y > rig.panel.barHeight());  // over the page
    rig.press(Hit::FavoriteOpen, 0);
    CHECK(rig.input.take() == "navigate https://www.youtube.com/results?search_query=%EA%B3%A0%EC%96%91%EC%9D%B4");
    CHECK(!rig.controller.favoritesOpen());
    // ✕ removes a row and the list stays; the button again closes it
    rig.press(Hit::Favorites);
    rig.press(Hit::FavoriteRemove, 0);
    CHECK(store.items().size() == 1 && rig.controller.favoritesOpen());
    CHECK(!rig.panel.hitCenter(Hit::FavoriteOpen, 1, x, y));
    rig.press(Hit::Favorites);
    CHECK(!rig.controller.favoritesOpen());
    // A tap on the page while it is open only closes it (no click under it); hovering it sends no page moves
    rig.press(Hit::Favorites);
    rig.panel.hitCenter(Hit::FavoriteOpen, 0, x, y);
    rig.at(Type::Move, x, y);
    CHECK(rig.input.take().empty());
    rig.at(Type::Down, 100, rig.panel.barHeight() + 600);
    rig.at(Type::Up, 100, rig.panel.barHeight() + 600);
    CHECK(rig.input.take().empty() && !rig.controller.favoritesOpen());
    // Full: nothing added, the list opens with a note
    for (size_t i = store.items().size(); i < FavoriteStore::kMax; ++i) store.add("p", "https://p.example/" + std::to_string(i));
    other.url = "https://full.example/";
    rig.controller.setStatus(other);
    rig.draw();
    rig.press(Hit::Star);
    CHECK(rig.controller.favoritesOpen() && store.find("https://full.example/") == -1);
    CHECK(rig.controller.barState(false, 0.45, true, true).favoritesNote.find("10개") != std::string::npos);
    // Not a web page (about:blank): ☆ can't be pressed
    other.url = "about:blank";
    rig.controller.setStatus(other);
    rig.draw();
    CHECK(!rig.panel.hitCenter(Hit::Star, 0, x, y));
    // Closing the panel closes the list
    rig.press(Hit::Close);
    CHECK(!rig.controller.favoritesOpen());

    // The picture: ★ on a saved page, the list open over the page
    Rig pic;
    FavoriteStore shown((dir / "shown.json").string());
    shown.load();
    shown.add("고양이 - YouTube", "https://www.youtube.com/results?search_query=%EA%B3%A0%EC%96%91%EC%9D%B4");
    shown.add("네이버 날씨", "https://weather.naver.com/");
    shown.add("Steam Frame 지원 - 아주 긴 제목이 들어가면 끝이 말줄임표로 잘립니다 (Valve Corporation)",
              "https://help.steampowered.com/");
    pic.controller.setFavorites(&shown);
    pic.panel.showFrame(fakePage(pic.fonts, 1280, 720));
    pic.press(Hit::Favorites);
    pic.panel.hitCenter(Hit::FavoriteOpen, 1, x, y);
    pic.at(Type::Move, x, y);
    const std::string path = pngPath("browser-favorites.png");
    if (!path.empty()) CHECK(pic.panel.writePng(path));
    // A new frame keeps the list on top; closing it puts the page back
    const BgraFrame page = fakePage(pic.fonts, 1280, 720);
    pic.panel.showFrame(page);
    pic.panel.hitCenter(Hit::FavoriteOpen, 0, x, y);
    const size_t at = (static_cast<size_t>(y) * 1280 + static_cast<size_t>(x)) * 4;
    const size_t pageAt = (static_cast<size_t>(y - pic.panel.barHeight()) * 1280 + static_cast<size_t>(x)) * 4;
    CHECK(std::memcmp(pic.panel.pixels() + at, &page.pixels[pageAt], 4) != 0);
    uint8_t listPixel[4];
    std::memcpy(listPixel, pic.panel.pixels() + at, 4);
    pic.panel.showFrame(fakePage(pic.fonts, 1280, 720));  // the cached list again, the same pixels
    CHECK(std::memcmp(pic.panel.pixels() + at, listPixel, 4) == 0);
    pic.press(Hit::Favorites);
    CHECK(!pic.controller.favoritesOpen());
    CHECK(std::memcmp(pic.panel.pixels() + at, &page.pixels[pageAt], 4) == 0);
    std::filesystem::remove_all(dir);
}

void testLongPressAndMenu() {
    Rig rig;
    const double top = rig.panel.barHeight();
    using Mi = MenuItem;
    // Held without moving, then let go: the right click there and the menu (복사, 붙여넣기, 전체 선택)
    rig.at(Type::Down, 300, top + 200, 10.0);
    CHECK(std::fabs(rig.controller.nextWake() - 10.5) < 1e-9);  // wake for the ring
    rig.controller.tick(10.3);
    CHECK(!rig.controller.barState(false, 0.45, true, true).pressRing);
    rig.controller.tick(10.5);
    CHECK(rig.controller.takeBarDirty());
    BrowserBarState st = rig.controller.barState(false, 0.45, true, true);
    CHECK(st.pressRing && st.ringX == 300 && st.ringY == top + 200);
    rig.draw();
    rig.at(Type::Move, 303, top + 202, 10.6);  // a wobble
    rig.at(Type::Up, 303, top + 202, 10.7);
    CHECK(rig.input.take() == "context 300,200 #1");
    CHECK(rig.controller.menuOpen());
    st = rig.controller.barState(false, 0.45, true, true);
    CHECK(!st.pressRing && st.menu == (std::vector<Mi> {Mi::Copy, Mi::Paste, Mi::SelectAll}));
    // The link under the point comes back: 링크 열기 at the end
    BrowserSession::Status s;
    s.phase = BrowserSession::Phase::Ready;
    s.url = "https://www.youtube.com/results?search_query=%EA%B3%A0%EC%96%91%EC%9D%B4";
    s.title = "고양이 - YouTube";
    s.canBack = true;
    s.contextSeq = 1;
    s.contextLink = "https://www.youtube.com/watch?v=cat";
    rig.controller.setStatus(s);
    rig.draw();
    CHECK(rig.controller.barState(false, 0.45, true, true).menu.size() == 4);
    double x, y;
    for (const Mi m : {Mi::Copy, Mi::Paste, Mi::SelectAll, Mi::OpenLink}) {
        int arg = -1;
        CHECK(rig.panel.hitCenter(Hit::MenuRow, static_cast<int>(m), x, y));
        CHECK(rig.panel.hitAt(x, y, &arg) == Hit::MenuRow && arg == static_cast<int>(m));
        CHECK(y > top && x < 300);  // over the page, left of the point
    }
    rig.panel.hitCenter(Hit::MenuRow, static_cast<int>(Mi::Copy), x, y);
    rig.at(Type::Move, x, y);
    const BgraFrame page = fakePage(rig.fonts, 1280, 720);
    rig.panel.showFrame(page);
    const std::string path = pngPath("browser-context-menu.png");
    if (!path.empty()) CHECK(rig.panel.writePng(path));
    rig.press(Hit::MenuRow, static_cast<int>(Mi::Copy));
    CHECK(rig.input.take() == "key copy" && !rig.controller.menuOpen());
    // Each row: paste, select all, open the link
    for (const auto& [item, call] : {std::pair<Mi, std::string> {Mi::Paste, "key paste"},
                                     {Mi::SelectAll, "key selectall"},
                                     {Mi::OpenLink, "navigate https://www.youtube.com/watch?v=cat"}}) {
        rig.at(Type::Down, 300, top + 200, 20.0);
        rig.at(Type::Up, 300, top + 200, 20.6);
        s.contextSeq = 1 + static_cast<int>(item);  // #2, #3, #4: each menu its own request
        rig.controller.setStatus(s);
        rig.draw();
        rig.input.take();
        rig.press(Hit::MenuRow, static_cast<int>(item));
        CHECK(rig.input.take() == call);
    }
    // An answer for an older menu doesn't add the link to a newer one
    rig.at(Type::Down, 300, top + 200, 30.0);
    rig.at(Type::Up, 300, top + 200, 30.6);
    s.contextSeq = 4;
    s.contextLink = "https://old.example/";
    rig.controller.setStatus(s);
    CHECK(rig.controller.barState(false, 0.45, true, true).menu.size() == 3);
    // A tap on the page while it is open only closes it
    rig.input.take();
    rig.at(Type::Down, 900, top + 500, 31.0);
    rig.at(Type::Up, 900, top + 500, 31.1);
    CHECK(rig.input.take().empty() && !rig.controller.menuOpen());
    // A navigation closes it too
    rig.at(Type::Down, 300, top + 200, 32.0);
    rig.at(Type::Up, 300, top + 200, 32.6);
    s.url = "https://elsewhere.example/";
    rig.controller.setStatus(s);
    CHECK(!rig.controller.menuOpen());
    rig.input.take();

    // Held, then dragged: a mouse press where it went down, the drag selects, the release ends it
    rig.at(Type::Down, 100, top + 100, 40.0);
    rig.at(Type::Move, 102, top + 100, 40.2);
    rig.at(Type::Move, 400, top + 100, 40.6);
    rig.at(Type::Move, 500, top + 120, 40.7);
    rig.at(Type::Up, 500, top + 120, 40.8);
    CHECK(rig.input.take() == "move 100,100 | down 100,100 | move 400,100 | move 500,120 | up 500,120");
    // Dragged before it was long: a scroll as before (even if the release comes late)
    rig.at(Type::Down, 100, top + 400, 50.0);
    rig.at(Type::Move, 100, top + 300, 50.2);
    rig.at(Type::Up, 100, top + 300, 51.0);
    CHECK(rig.input.take() == "wheel 100,400 0,100");
    // Leaving mid-press: no menu
    rig.at(Type::Down, 100, top + 100, 60.0);
    rig.controller.tick(60.6);
    rig.at(Type::Leave, 0, 0, 60.7);
    rig.at(Type::Up, 100, top + 100, 60.8);
    CHECK(rig.input.take().empty() && !rig.controller.menuOpen());
    CHECK(!rig.controller.barState(false, 0.45, true, true).pressRing);

    // The right button (the thumbstick click): the menu at once where the laser is; on the bar, nothing
    PanelPointer right;
    right.type = Type::SecondaryDown;
    right.x = 640;
    right.y = top + 360;
    rig.controller.panelPointer(right, 70.0);
    CHECK(rig.input.take().rfind("context 640,360 #", 0) == 0 && rig.controller.menuOpen());
    right.type = Type::SecondaryUp;
    rig.controller.panelPointer(right, 70.1);
    CHECK(rig.controller.menuOpen());
    rig.controller.closePopups();
    right.type = Type::SecondaryDown;
    right.y = 30;
    rig.controller.panelPointer(right, 71.0);
    CHECK(rig.input.take().empty() && !rig.controller.menuOpen());
    // Drag-to-scroll off: presses go straight to the page, a long one stays a press (no menu)
    rig.press(Hit::DragScroll);
    rig.input.take();
    rig.at(Type::Down, 200, top + 200, 80.0);
    rig.at(Type::Up, 200, top + 200, 81.0);
    CHECK(rig.input.take() == "move 200,200 | down 200,200 | up 200,200" && !rig.controller.menuOpen());
}

void test1080p() {
    // The bar keeps its size on the panel at 1920×1080: everything × 1.5, the page 1:1 below it
    Rig small;
    Rig big;
    big.panel.setViewport(1920, 1080);
    big.draw();
    CHECK(big.panel.width() == 1920 && big.panel.barHeight() == 240 && big.panel.height() == 240 + 1080);
    CHECK(std::fabs(big.panel.scale() - 1.5) < 1e-9);
    for (const Hit h : {Hit::Handle, Hit::Back, Hit::Reload, Hit::Star, Hit::Address, Hit::Favorites, Hit::Keyboard,
                        Hit::Pin, Hit::Close}) {
        double sx, sy, bx, by;
        CHECK(small.panel.hitCenter(h, 0, sx, sy) && big.panel.hitCenter(h, 0, bx, by));
        CHECK(std::fabs(bx - sx * 1.5) < 1e-6 && std::fabs(by - sy * 1.5) < 1e-6);
        CHECK(big.panel.hitAt(bx, by) == h);
    }
    const cdp::ViewMap map = big.panel.viewMap();
    CHECK(map.areaY == 240 && map.areaW == 1920 && map.viewportW == 1920 && map.viewportH == 1080);
    big.at(Type::Down, 960, 240 + 540);
    big.at(Type::Up, 960, 240 + 540);
    CHECK(big.input.take() == "move 960,540 | down 960,540 | up 960,540");
    const BgraFrame page = fakePage(big.fonts, 1920, 1080);
    big.panel.showFrame(page);
    const size_t at = (static_cast<size_t>(240 + 700) * 1920 + 1500) * 4;
    CHECK(std::memcmp(big.panel.pixels() + at, &page.pixels[(700 * 1920 + 1500) * 4], 4) == 0);
    // The menu over it, scaled the same
    big.at(Type::Down, 1200, 240 + 400, 1.0);
    big.at(Type::Up, 1200, 240 + 400, 1.6);
    double x, y;
    CHECK(big.panel.hitCenter(Hit::MenuRow, static_cast<int>(MenuItem::Paste), x, y) && y > 240 && x < 1200);
    // Back to 720p with the menu open: it goes (its place was in 1080p texture px), the bar is at scale 1 again
    CHECK(big.controller.menuOpen());
    big.controller.setViewport(1280, 720);
    CHECK(!big.controller.menuOpen() && big.panel.barHeight() == BrowserPanel::kBarHeight);
    big.draw();
    CHECK(!big.panel.hitCenter(Hit::MenuRow, static_cast<int>(MenuItem::Paste), x, y));
    big.controller.setViewport(1920, 1080);
    big.draw();
    const std::string path = pngPath("browser-1080p.png");
    if (!path.empty()) CHECK(big.panel.writePng(path));
}

}  // namespace

// The panel's words in English and Japanese: the bar, the 보기 list, the keyboard under it ($FRAMEDOCK_PNG_DIR)
void testLanguages() {
    for (const Language language : {Language::En, Language::Ja}) {
        setUiLanguage(language);
        Rig rig;
        rig.panel.showFrame(fakePage(rig.fonts, 1280, 720));
        rig.draw();
        CHECK(browserStatusText(BrowserSession::Status {}) == tr(Tx::BrowserStatusOff));
        CHECK(std::string(menuItemLabel(MenuItem::Copy)) == tr(Tx::BrowserMenuCopy));
        rig.press(Hit::View);
        const std::string code = language == Language::En ? "en" : "ja";
        const std::string viewPath = pngPath("browser-view-list-" + code + ".png");
        if (!viewPath.empty()) CHECK(rig.panel.writePng(viewPath));
        rig.press(Hit::View);
        rig.press(Hit::Address);
        rig.draw();
        rig.keyboard.render();
        const std::string keyboardPath = pngPath("browser-keyboard-" + code + ".png");
        if (!keyboardPath.empty()) CHECK(rig.keyboard.writePng(keyboardPath));
        const std::string panelPath = pngPath("browser-panel-" + code + ".png");
        if (!panelPath.empty()) CHECK(rig.panel.writePng(panelPath));
    }
    setUiLanguage(Language::Ko);
    // Chrome speaks the app's language: --lang, --accept-lang, and 번역's target
    CHECK(std::string(chromeLocale(Language::Ko)) == "ko-KR" && std::string(chromeLocale(Language::En)) == "en-US" &&
          std::string(chromeLocale(Language::Ja)) == "ja-JP");
    CHECK(acceptLanguages("ko-KR") == "ko-KR,ko,en-US,en" && acceptLanguages("en-US") == "en-US,en" &&
          acceptLanguages("ja-JP") == "ja-JP,ja,en-US,en");
    CHECK(std::string(translateTarget(Language::En)) == "en" && std::string(translateTarget(Language::Ja)) == "ja" &&
          std::string(translateTarget(Language::Ko)) == "ko");
    const std::string script = translateScript("ja");
    CHECK(script.find("includedLanguages:'ja'") != std::string::npos &&
          script.find("c.value='ja'") != std::string::npos);
    ChromeLaunch launch;
    launch.locale = "en-US";
    const std::vector<std::string> argv = chromeArgv(launch);
    CHECK(std::find(argv.begin(), argv.end(), "--lang=en-US") != argv.end() &&
          std::find(argv.begin(), argv.end(), "--accept-lang=en-US,en") != argv.end());
}

void testOffSizeFrame() {
    // The 4:3 panel (1108×832) while Chrome still sends 16:9 (1280×720): shown with its shape, at the top, black
    // under it; the laser maps through where it is, so a press lands on what it shows (it used to be stretched to
    // 1108×832 while presses went to the 832-tall viewport: off more and more towards the bottom)
    Rig rig;
    rig.controller.setViewport(1108, 832);
    rig.draw();
    const int bar = rig.panel.barHeight();
    BgraFrame wide = fakePage(rig.fonts, 1280, 720);
    wide.cssWidth = 1280;
    wide.cssHeight = 720;
    rig.panel.showFrame(wide);
    const cdp::ViewMap map = rig.panel.viewMap();
    const double fit = 1108.0 / 1280;
    CHECK(std::fabs(map.areaW - 1108) < 1e-9 && std::fabs(map.areaH - 720 * fit) < 1e-9);
    CHECK(map.areaX == 0 && map.areaY == bar && map.viewportW == 1280 && map.viewportH == 720);
    // The middle and the bottom of the picture are the page's middle and bottom
    rig.at(Type::Down, 554, bar + 360 * fit);
    rig.at(Type::Up, 554, bar + 360 * fit);
    CHECK(rig.input.take() == "move 640,360 | down 640,360 | up 640,360");
    rig.at(Type::Down, 1108 - 1, bar + 720 * fit - 1);
    rig.at(Type::Up, 1108 - 1, bar + 720 * fit - 1);
    CHECK(rig.input.take() == "move 1279,719 | down 1279,719 | up 1279,719");  // the page's last pixel
    // Under the picture: not the page
    rig.at(Type::Down, 554, bar + 800);
    rig.at(Type::Up, 554, bar + 800);
    CHECK(rig.panel.hitAt(554, bar + 800) == Hit::None);
    CHECK(rig.input.take().empty());
    const size_t under = (static_cast<size_t>(bar + 800) * 1108 + 554) * 4;
    CHECK(rig.panel.pixels()[under] == 0 && rig.panel.pixels()[under + 1] == 0 && rig.panel.pixels()[under + 2] == 0);
    CHECK(rig.panel.pixels()[under + 3] == 255);  // black, not a hole in the panel
    // A frame at the page size again: the whole page area, 1:1
    rig.panel.showFrame(fakePage(rig.fonts, 1108, 832));
    const cdp::ViewMap back = rig.panel.viewMap();
    CHECK(back.areaH == 832 && back.viewportW == 1108 && back.viewportH == 832);
    // A tall frame on the 16:9 panel: pillarboxed, centred
    Rig tall;
    tall.panel.showFrame(fakePage(tall.fonts, 720, 1280));
    const cdp::ViewMap side = tall.panel.viewMap();
    CHECK(std::fabs(side.areaH - 720) < 1e-9 && std::fabs(side.areaW - 405) < 1e-9 &&
          std::fabs(side.areaX - (1280 - 405) / 2.0) < 1e-9 && side.viewportW == 720 && side.viewportH == 1280);
}

int main() {
    testPage();
    testDragDistance();
    testBar();
    testDialog();
    testHugePageText();
    testKeyboard();
    testPictures();
    testFavoriteStore();
    testFavoritesOnTheBar();
    testLongPressAndMenu();
    test1080p();
    testOffSizeFrame();
    testLanguages();
    if (failures == 0) std::printf("browser-view: all passed\n");
    return failures == 0 ? 0 : 1;
}
