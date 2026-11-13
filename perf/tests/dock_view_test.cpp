// The Dock's views: the dashboard tab's Dock page (settings for in-game use) and its actions. Writes PNGs to
// $FRAMEDOCK_PNG_DIR when set (README images). No SteamVR.
#include "app_text.h"
#include "battery_watch.h"
#include "color_model.h"
#include "color_page.h"
#include "config.h"
#include "dashboard_preview.h"
#include "device_page.h"
#include "dock.h"
#include "dock_loop.h"
#include "dock_view.h"
#include "draw.h"
#include "i18n.h"
#include "notice_view.h"
#include "settings_panel.h"

#include <cairo.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using Kind = DockPanel::Hit::Kind;

class FakeHost : public framecolor::ColorHost {
public:
    std::map<std::string, double> settings;
    double getSetting(const char* key, double fallback) override {
        const auto it = settings.find(key);
        return it == settings.end() ? fallback : it->second;
    }
    std::string setSetting(const char* key, double value) override {
        settings[key] = value;
        return {};
    }
    framecolor::DisplayContext context(bool) override { return {}; }
    std::string appName(const std::string& key) override { return key; }
};

/** A colour model on files in a temporary folder (the Vulkan layer "installed"), fonts, a Dock panel. */
struct Rig {
    std::filesystem::path root;
    FakeHost host;
    framecolor::ColorPaths paths;
    FontSet fonts;
    framecolor::ColorModel model;
    DockPanel dock;
    DeviceModel device;  ///< the 소리 · 패스스루 card's (made-up state: dashboard_preview)
    ProfilesUiState profiles;
    double t = 10.0;

    static framecolor::ColorPaths makePaths(const std::filesystem::path& root) {
        framecolor::ColorPaths p;
        p.configDir = root / "framecolor";
        p.layerManifest = root / "layer/vkBasalt.json";
        std::filesystem::create_directories(p.configDir);
        std::filesystem::create_directories(p.layerManifest.parent_path());
        std::ofstream(p.layerManifest) << "{}\n";
        return p;
    }

    explicit Rig(const std::string& name)
        : root(std::filesystem::temp_directory_path() / (name + "-" + std::to_string(::getpid()))),
          paths(makePaths(root)),
          model(&host, paths),
          dock(fonts, &model) {
        const Config cfg;
        fonts.load(cfg.fontPath, cfg.boldFontPath);
        host.settings["analogGain"] = 1.0;
        model.state.language = framecolor::Language::Korean;
        model.start();
        model.state.custom[0] = {model.state.values, true};  // P1 saved, P2 and P3 empty
        model.state.custom[0].values.sharpen = 0.5;
        for (int i = 0; i < 4; ++i) {
            ProfilesUiState::Item item {"profile-" + std::to_string(i + 1), i == 0 ? "간단 A" : i == 1 ? "간단 B" : i == 2 ? "Racing" : "Standalone", i < 2};
            profiles.items.push_back(item);
        }
        profiles.activeId = "profile-3";
        previewDeviceState(device, "");
        dock.setDevice(device);
        // The battery row: the headset on battery with its time left, the right controller low
        DockBattery battery;
        battery.headsetPct = 46;
        battery.minutesText = "약 40분";
        battery.left = {true, 80, false};
        battery.right = {true, 15, false};
        dock.setBattery(battery);
    }
    ~Rig() { std::filesystem::remove_all(root); }

    void render() { dock.renderQuick(profiles, t); }

    DockPress tap(Kind kind, int arg = 0) {
        double x = 0, y = 0;
        if (!dock.hitCenter(kind, arg, x, y)) return DockPress {};
        dock.setProfiles(profiles);
        const DockPress press = dock.pointerDown(x, y, t);
        dock.pointerUp(x, y);
        t += 0.1;
        render();
        return press;
    }
};

/** @return $FRAMEDOCK_PNG_DIR/name (the folder is created), or "" when the variable isn't set */
std::string pngPath(const char* name) {
    const char* dir = std::getenv("FRAMEDOCK_PNG_DIR");
    if (dir == nullptr || dir[0] == '\0') return "";
    std::filesystem::create_directories(dir);
    return std::string(dir) + "/" + name;
}

/** Presses a button on the panel by its action. @return what the press returned */
SettingsAction press(SettingsPanel& panel, SettingsAction action, double now) {
    double x = 0, y = 0;
    if (!panel.buttonCenter(action, x, y)) return SettingsAction::None;
    const SettingsAction got = panel.pointerDown(x, y, now);
    panel.pointerUp(x, y);
    return got;
}

void testDockPage() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    panel.render(cfg, service);
    // The fifth page in the page switch
    CHECK(press(panel, SettingsAction::PageDock, 1) == SettingsAction::None);
    CHECK(panel.page() == SettingsPage::Dock);
    panel.render(cfg, service);
    // Its buttons come back as actions; applySettingsAction changes config.dock
    for (const SettingsAction a : {SettingsAction::DockOn, SettingsAction::DockOff, SettingsAction::DockHandAuto,
                                   SettingsAction::DockHandLeft, SettingsAction::DockHandRight,
                                   SettingsAction::DockSizeDown, SettingsAction::DockSizeUp,
                                   SettingsAction::DockTimeoutDown, SettingsAction::DockTimeoutUp,
                                   SettingsAction::DockResetPlace}) {
        CHECK(press(panel, a, 2) == a);
    }
    Config c;
    CHECK(applySettingsAction(SettingsAction::DockOff, c) && !c.dock.enabled);
    CHECK(!applySettingsAction(SettingsAction::DockOff, c));
    CHECK(applySettingsAction(SettingsAction::DockHandRight, c) && c.dock.pointerHand == DockHand::Right);
    CHECK(applySettingsAction(SettingsAction::DockSizeUp, c) && std::fabs(c.dock.widthM - 0.35) < 1e-9);
    for (int i = 0; i < 20; ++i) applySettingsAction(SettingsAction::DockSizeUp, c);
    CHECK(std::fabs(c.dock.widthM - 0.60) < 1e-9);
    for (int i = 0; i < 20; ++i) applySettingsAction(SettingsAction::DockSizeDown, c);
    CHECK(std::fabs(c.dock.widthM - 0.15) < 1e-9);
    // The timeout: 20 → 30 → 60 → 120 → never, and back down to 10
    c.dock.collapseSec = 20;
    const int up[] = {30, 60, 120, 0, 0};
    for (const int expected : up) {
        applySettingsAction(SettingsAction::DockTimeoutUp, c);
        CHECK(c.dock.collapseSec == expected);
    }
    const int down[] = {120, 60, 30, 20, 10, 10};
    for (const int expected : down) {
        applySettingsAction(SettingsAction::DockTimeoutDown, c);
        CHECK(c.dock.collapseSec == expected);
    }
    c.dock.collapseSec = 45;  // a hand-edited value: the next step either way
    applySettingsAction(SettingsAction::DockTimeoutUp, c);
    CHECK(c.dock.collapseSec == 60);
    c.dock.collapseSec = 45;
    applySettingsAction(SettingsAction::DockTimeoutDown, c);
    CHECK(c.dock.collapseSec == 30);
    // The Dock panel's place back to the defaults (the HUD's stays)
    c.dock.leftWrist.x = 0.3;
    c.dock.head.z = -1.2;
    c.posX = 0.1;
    CHECK(applySettingsAction(SettingsAction::DockResetPlace, c));
    CHECK(!(c.dock.leftWrist != defaultDockWrist()) && !(c.dock.head != HeadPose()) && c.posX == 0.1);

    // On the Dock's full view, 끔 (in-game use) and the Placement page's 표시 끔 take the Dock away: two taps, the first
    // says on the bottom line what happens and how to get it back. The tab's own panel (above) does them at once
    {
        SettingsPanel dockFull(fonts);
        dockFull.setInDock(true);
        dockFull.showPage(SettingsPage::Dock);
        dockFull.render(cfg, service);
        CHECK(press(dockFull, SettingsAction::DockOff, 10) == SettingsAction::None);
        dockFull.render(cfg, service);
        CHECK(dockFull.footerNote() == uiText(cfg.language).dockOffConfirmNote);
        CHECK(dockFull.footerNote().find("SteamVR 대시보드") != std::string::npos);
        const std::string dockOff = pngPath("dock-full-dock-off-armed.png");
        if (!dockOff.empty()) CHECK(dockFull.writePng(dockOff));
        CHECK(press(dockFull, SettingsAction::DockOff, 11) == SettingsAction::DockOff);
        CHECK(press(dockFull, SettingsAction::DockOn, 12) == SettingsAction::DockOn);  // 켬 at once
        // Waiting past kQuitConfirmSec: the next tap only arms again, and tick() clears the note
        CHECK(press(dockFull, SettingsAction::DockOff, 20) == SettingsAction::None);
        CHECK(dockFull.tick(20 + SettingsPanel::kQuitConfirmSec + 0.1));
        dockFull.render(cfg, service);
        CHECK(dockFull.footerNote() != uiText(cfg.language).dockOffConfirmNote);
        CHECK(press(dockFull, SettingsAction::DockOff, 30) == SettingsAction::None);
        CHECK(press(dockFull, SettingsAction::DockOff, 34) == SettingsAction::None);  // too late: armed again
        dockFull.showPage(SettingsPage::Placement);
        dockFull.render(cfg, service);
        CHECK(press(dockFull, SettingsAction::ShowOff, 40) == SettingsAction::None);
        dockFull.render(cfg, service);
        CHECK(dockFull.footerNote() == uiText(cfg.language).showOffConfirmNote);
        const std::string showOff = pngPath("dock-full-show-off-armed.png");
        if (!showOff.empty()) CHECK(dockFull.writePng(showOff));
        CHECK(press(dockFull, SettingsAction::ShowOff, 41) == SettingsAction::ShowOff);
        CHECK(press(dockFull, SettingsAction::ShowOn, 42) == SettingsAction::ShowOn);
        // The tab's panel: 표시 끔 at once; the Placement page's reset takes two taps everywhere
        SettingsPanel tab(fonts);
        tab.showPage(SettingsPage::Placement);
        tab.render(cfg, service);
        CHECK(press(tab, SettingsAction::ShowOff, 50) == SettingsAction::ShowOff);
        CHECK(press(tab, SettingsAction::Reset, 51) == SettingsAction::None);
        tab.render(cfg, service);
        CHECK(tab.footerNote() == uiText(cfg.language).resetConfirmNote);
        const std::string reset = pngPath("placement-reset-armed.png");
        if (!reset.empty()) CHECK(tab.writePng(reset));
        CHECK(press(tab, SettingsAction::Reset, 52) == SettingsAction::Reset);
        // The Profiles page's 덮어쓰기 over unsaved changes, armed
        ProfilesUiState unsaved;
        unsaved.items = {{"profile-1", "간단 A", false}, {"profile-2", "Racing", false}};
        unsaved.activeId = "profile-2";
        unsaved.unsaved = true;
        tab.showPage(SettingsPage::Profiles);
        tab.selectProfile("profile-2");
        tab.render(cfg, service, unsaved);
        CHECK(press(tab, SettingsAction::ProfileSave, 60) == SettingsAction::None);
        tab.render(cfg, service, unsaved);
        const std::string save = pngPath("profiles-save-armed.png");
        if (!save.empty()) CHECK(tab.writePng(save));
        CHECK(press(tab, SettingsAction::ProfileSave, 61) == SettingsAction::ProfileSave);
    }

    // Every page keeps the footer and the page switch apart: the Dock page's buttons sit inside the tab
    for (const SettingsAction a : {SettingsAction::DockOn, SettingsAction::DockResetPlace, SettingsAction::PageDock}) {
        double x = 0, y = 0;
        CHECK(panel.buttonCenter(a, x, y) && x > 0 && x < panel.width() && y > 0 && y < panel.height());
    }
    cfg.dock.collapseSec = 0;  // "안 함" fits
    panel.render(cfg, service);
    const std::string path = pngPath("ko-dock-never.png");
    if (!path.empty()) CHECK(panel.writePng(path));
}

// The Browser page: the sixth page in the switch, its buttons, the steps of its settings, Chrome's line
void testBrowserPage() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    panel.render(cfg, service);
    CHECK(press(panel, SettingsAction::PageBrowser, 1) == SettingsAction::None);
    CHECK(panel.page() == SettingsPage::Browser);
    BrowserPageInfo info;
    info.url = "https://www.youtube.com/watch?v=abc";
    info.status = "실행 중 (PID 4242)";
    info.running = true;
    CHECK(panel.setBrowserInfo(info));
    CHECK(!panel.setBrowserInfo(info));
    panel.render(cfg, service);
    const SettingsAction all[] = {SettingsAction::BrowserFpsDown,      SettingsAction::BrowserFpsUp,
                                  SettingsAction::Browser720p,         SettingsAction::Browser1080p,
                                  SettingsAction::BrowserHeadless,     SettingsAction::BrowserWindow,
                                  SettingsAction::BrowserKeepAudioOn,  SettingsAction::BrowserKeepAudioOff,
                                  SettingsAction::BrowserRemovedKeepOn, SettingsAction::BrowserRemovedPause,
                                  SettingsAction::BrowserIdleDown,     SettingsAction::BrowserIdleUp,
                                  SettingsAction::BrowserAnchorAuto,   SettingsAction::BrowserAnchorLeft,
                                  SettingsAction::BrowserAnchorRight,  SettingsAction::BrowserAnchorHead,
                                  SettingsAction::BrowserResetPlace,   SettingsAction::BrowserHomeFromPage};
    for (const SettingsAction a : all) {
        double x = 0, y = 0;
        CHECK(panel.buttonCenter(a, x, y) && x > 0 && x < panel.width() && y > 0 && y < panel.height());
        CHECK(press(panel, a, 2) == a);
    }
    // Chrome 끄기 while it runs: the first tap arms it, the second quits
    double sx = 0, sy = 0;
    CHECK(panel.buttonCenter(SettingsAction::BrowserStopChrome, sx, sy));
    CHECK(!panel.buttonCenter(SettingsAction::BrowserStartChrome, sx, sy));
    CHECK(press(panel, SettingsAction::BrowserStopChrome, 3) == SettingsAction::None);
    CHECK(press(panel, SettingsAction::BrowserStopChrome, 3.5) == SettingsAction::BrowserStopChrome);
    // Chrome off: Chrome 켜기 in its place, one tap
    info.running = false;
    info.status = "꺼짐";
    CHECK(panel.setBrowserInfo(info));
    panel.render(cfg, service);
    double kx = 0, ky = 0;
    CHECK(!panel.buttonCenter(SettingsAction::BrowserStopChrome, kx, ky));
    CHECK(panel.buttonCenter(SettingsAction::BrowserStartChrome, kx, ky) && kx == sx && ky == sy);
    CHECK(press(panel, SettingsAction::BrowserStartChrome, 4) == SettingsAction::BrowserStartChrome);
    const std::string startPath = pngPath("browser-page-chrome-off.png");
    if (!startPath.empty()) CHECK(panel.writePng(startPath));
    info.running = true;
    CHECK(panel.setBrowserInfo(info));
    panel.render(cfg, service);
    // All seven page buttons fit in the header, apart
    double px = -1;
    for (const SettingsAction a : {SettingsAction::PagePlacement, SettingsAction::PageLayout, SettingsAction::PageProfiles,
                                   SettingsAction::PageColor, SettingsAction::PageDock, SettingsAction::PageBrowser,
                                   SettingsAction::PageDevice}) {
        double x = 0, y = 0;
        CHECK(panel.buttonCenter(a, x, y) && x > px);
        px = x;
    }
    // The steps
    Config c;
    // 벗으면 영상: 멈춤 by default
    CHECK(c.browser.pauseWhenRemoved);
    CHECK(applySettingsAction(SettingsAction::BrowserRemovedKeepOn, c) && !c.browser.pauseWhenRemoved);
    CHECK(applySettingsAction(SettingsAction::BrowserRemovedPause, c) && c.browser.pauseWhenRemoved);
    CHECK(applySettingsAction(SettingsAction::BrowserFpsUp, c) && c.browser.fps == 45);
    CHECK(applySettingsAction(SettingsAction::BrowserFpsDown, c) && c.browser.fps == 30);
    // Resolution: 720p (the default) or 1080p
    CHECK(c.browser.viewportWidth == 1280 && c.browser.viewportHeight == 720);
    CHECK(applySettingsAction(SettingsAction::Browser1080p, c));
    CHECK(c.browser.viewportWidth == 1920 && c.browser.viewportHeight == 1080);
    CHECK(!applySettingsAction(SettingsAction::Browser1080p, c));  // already
    CHECK(applySettingsAction(SettingsAction::Browser720p, c));
    CHECK(c.browser.viewportWidth == 1280 && c.browser.viewportHeight == 720);
    applySettingsAction(SettingsAction::BrowserIdleUp, c);
    CHECK(c.browser.idleShutdownMin == 15);
    for (int i = 0; i < 5; ++i) applySettingsAction(SettingsAction::BrowserIdleUp, c);
    CHECK(c.browser.idleShutdownMin == 0);  // never, the last step
    applySettingsAction(SettingsAction::BrowserIdleDown, c);
    CHECK(c.browser.idleShutdownMin == 60);
    applySettingsAction(SettingsAction::BrowserWindow, c);
    applySettingsAction(SettingsAction::BrowserKeepAudioOff, c);
    applySettingsAction(SettingsAction::BrowserAnchorHead, c);
    CHECK(!c.browser.headless && !c.browser.keepAudio && c.browser.anchor == BrowserAnchor::Head);
    c.browser.leftWrist.x = 0.3;
    c.browser.widthM = 0.9;
    CHECK(applySettingsAction(SettingsAction::BrowserResetPlace, c));
    CHECK(!(c.browser.leftWrist != defaultBrowserWrist()) && std::fabs(c.browser.widthM - 0.45) < 1e-9);
    CHECK(!applySettingsAction(SettingsAction::BrowserHomeFromPage, c));  // the caller's
    const std::string path = pngPath("ko-browser.png");
    if (!path.empty()) CHECK(panel.writePng(path));
}

// The quick view: its buttons inside the panel and apart, the quick switch with the footer's two-tap rule, the
// colour buttons on the shared model, compare let go when the laser leaves, the browser button
void testQuickView() {
    Rig rig("dock-quick");
    rig.render();
    CHECK(rig.dock.width() == DockPanel::kQuickWidth && rig.dock.height() == DockPanel::kQuickHeight);
    // Every button is on the panel and none overlaps another
    std::vector<std::pair<Kind, int>> all = {{Kind::Handle, 0}, {Kind::Compare, 0}, {Kind::AutoCompare, 0},
                                             {Kind::Full, 0}, {Kind::Collapse, 0}};
    for (int i = 0; i < 4; ++i) all.push_back({Kind::Profile, i});
    for (int i = 0; i < 4; ++i) all.push_back({Kind::Preset, i});
    for (const auto& [kind, arg] : all) {
        double x = 0, y = 0;
        CHECK(rig.dock.hitCenter(kind, arg, x, y));
        CHECK(x > 0 && x < DockPanel::kQuickWidth && y > 0 && y < DockPanel::kQuickHeight);
    }
    double x = 0, y = 0;
    CHECK(rig.dock.hitCenter(Kind::Browser, 0, x, y));    // the browser panel (Phase 3)
    CHECK(rig.tap(Kind::Browser, 0).kind == DockPress::Kind::OpenBrowser);
    CHECK(!rig.dock.hitCenter(Kind::Preset, 4, x, y));    // P2 empty: nothing to apply
    CHECK(!rig.dock.hitCenter(Kind::Profile, 4, x, y));   // four profiles

    // The quick switch: one tap when nothing would be lost
    DockPress press = rig.tap(Kind::Profile, 0);
    CHECK(press.kind == DockPress::Kind::UseProfile && press.profileId == "profile-1");
    // A profile whose use would lose unsaved changes: the first tap arms (and says so), the second uses it
    rig.profiles.items[3].useLoses.placement = true;
    rig.render();
    press = rig.tap(Kind::Profile, 3);
    CHECK(press.kind == DockPress::Kind::None && rig.dock.armed());
    CHECK(rig.dock.nextWake() < rig.t + SettingsPanel::kQuitConfirmSec + 0.1);
    std::string armedPng = pngPath("dock-quick-armed.png");
    press = rig.tap(Kind::Profile, 3);
    CHECK(press.kind == DockPress::Kind::UseProfile && press.profileId == "profile-4" && !rig.dock.armed());
    // Another press disarms; so does the timeout
    rig.tap(Kind::Profile, 3);
    CHECK(rig.dock.armed());
    rig.tap(Kind::Full);
    CHECK(!rig.dock.armed());
    rig.tap(Kind::Profile, 3);
    CHECK(rig.dock.armed());
    rig.t += SettingsPanel::kQuitConfirmSec + 0.5;
    CHECK(rig.dock.tick(rig.t) && !rig.dock.armed());
    press = rig.tap(Kind::Profile, 3);  // armed again, not used
    CHECK(press.kind == DockPress::Kind::None);
    // The armed button stands for a profile: if the list changed under it, the second tap arms the new one
    rig.profiles.items[3].id = "profile-9";
    rig.render();
    press = rig.tap(Kind::Profile, 3);
    CHECK(press.kind == DockPress::Kind::None && rig.dock.armed());
    rig.dock.tick(rig.t + 10);

    // A result on the status line, like the footer's
    rig.dock.showStatus("간단 A 사용 중", false, rig.t);
    CHECK(rig.dock.tick(rig.t + SettingsPanel::kActionStatusSec + 0.1));

    // Colour: a preset applies (never saves into the slot, even if the Color page was in save mode)
    rig.model.state.saveMode = true;
    const framecolor::Preset p1 = rig.model.state.custom[0];
    press = rig.tap(Kind::Preset, 3);
    CHECK(press.kind == DockPress::Kind::ColorChanged && !rig.model.state.saveMode);
    CHECK(std::fabs(rig.model.state.values.sharpen - 0.5) < 1e-9 && rig.model.state.custom[0].valid);
    rig.tap(Kind::Preset, 0);  // 표준
    CHECK(std::fabs(rig.model.state.values.sharpen - 0.5) > 1e-9 || p1.values.sharpen == 0.0);

    // Hold to compare: the original while held, back on release
    CHECK(rig.dock.hitCenter(Kind::Compare, 0, x, y));
    rig.dock.pointerDown(x, y, rig.t);
    CHECK(rig.model.state.compare);
    rig.dock.pointerUp(x, y);
    CHECK(!rig.model.state.compare);
    // Pointing away while holding lets go (the main loop calls pointerLeave on the Dock's Leave)
    rig.render();
    rig.dock.pointerDown(x, y, rig.t);
    CHECK(rig.model.state.compare);
    CHECK(rig.dock.pointerLeave());
    CHECK(!rig.model.state.compare);
    // Auto compare toggles
    press = rig.tap(Kind::AutoCompare);
    CHECK(press.kind == DockPress::Kind::ColorChanged && rig.model.state.autoCompare);
    rig.tap(Kind::AutoCompare);
    CHECK(!rig.model.state.autoCompare);

    // The rest
    CHECK(rig.tap(Kind::Full).kind == DockPress::Kind::ShowFull);
    CHECK(rig.tap(Kind::Collapse).kind == DockPress::Kind::Collapse);
    CHECK(rig.tap(Kind::Handle).kind == DockPress::Kind::GrabHandle);

    // PNGs: the usual view, then a pressed / armed one (compare held, a quick switch waiting for its second tap)
    rig.profiles.items[3].id = "profile-4";
    rig.profiles.unsaved = true;
    rig.model.state.message.clear();
    rig.dock.pointerLeave();  // no hover left from the taps above
    rig.render();
    const std::string normal = pngPath("dock-quick.png");
    if (!normal.empty()) CHECK(rig.dock.writePng(normal));
    rig.dock.setProfiles(rig.profiles);
    CHECK(rig.dock.hitCenter(Kind::Profile, 3, x, y));
    rig.dock.pointerDown(x, y, rig.t);
    rig.dock.pointerUp(x, y);
    CHECK(rig.dock.armed());
    CHECK(rig.dock.hitCenter(Kind::Compare, 0, x, y));
    rig.dock.pointerMove(x, y);
    rig.dock.pointerDown(x, y, rig.t);  // a press elsewhere disarms...
    CHECK(!rig.dock.armed());
    rig.dock.pointerLeave();
    // ...so arm last: hold compare first, then tap the profile with the other... the PNG shows both states
    rig.render();
    CHECK(rig.dock.hitCenter(Kind::Profile, 3, x, y));
    rig.dock.pointerDown(x, y, rig.t);
    rig.dock.pointerUp(x, y);
    rig.model.pressCompare();  // compare held (as by the other laser / a held press)
    rig.render();
    CHECK(rig.dock.armed());
    if (!armedPng.empty()) CHECK(rig.dock.writePng(armedPng));
    rig.model.leave();

    // While Chrome runs, 끄기 beside 브라우저: the first tap arms it (red ring, the note line), the second closes Chrome
    rig.dock.pointerLeave();
    CHECK(!rig.dock.hitCenter(Kind::QuitChrome, 0, x, y));
    CHECK(rig.dock.setChromeRunning(true));
    rig.render();
    CHECK(rig.dock.hitCenter(Kind::Browser, 0, x, y) && rig.dock.hitCenter(Kind::QuitChrome, 0, x, y));
    const std::string running = pngPath("dock-quick-chrome.png");
    if (!running.empty()) CHECK(rig.dock.writePng(running));
    press = rig.tap(Kind::QuitChrome, 0);
    CHECK(press.kind == DockPress::Kind::None && rig.dock.quitArmed());
    CHECK(rig.dock.note() == "한 번 더 누르면 Chrome을 꺼요");
    const std::string quitArmed = pngPath("dock-quick-quit-armed.png");
    if (!quitArmed.empty()) CHECK(rig.dock.writePng(quitArmed));
    CHECK(rig.tap(Kind::QuitChrome, 0).kind == DockPress::Kind::QuitChrome && !rig.dock.quitArmed());
    rig.tap(Kind::QuitChrome, 0);
    CHECK(rig.tap(Kind::Full, 0).kind == DockPress::Kind::ShowFull && !rig.dock.quitArmed());  // another press: gone

    // 녹화 only with framecorder installed: between 브라우저 and 전체, the time while recording, its news on the note
    rig.dock.pointerLeave();
    CHECK(!rig.dock.hitCenter(Kind::Record, 0, x, y));
    DockRecord record;
    record.available = true;
    CHECK(rig.dock.setRecord(record) && !rig.dock.setRecord(record));
    rig.render();
    double bx = 0, by = 0, fx = 0, fy = 0;
    CHECK(rig.dock.hitCenter(Kind::Record, 0, x, y) && rig.dock.hitCenter(Kind::QuitChrome, 0, bx, by) &&
          rig.dock.hitCenter(Kind::Full, 0, fx, fy));
    CHECK(bx < x && x < fx && y == fy);
    CHECK(rig.tap(Kind::Record, 0).kind == DockPress::Kind::ToggleRecord);
    record.state = 1;
    record.seconds = 42;
    CHECK(rig.dock.setRecord(record));
    rig.render();
    const std::string recording = pngPath("dock-quick-record.png");
    if (!recording.empty()) CHECK(rig.dock.writePng(recording));
    CHECK(rig.tap(Kind::Record, 0).kind == DockPress::Kind::ToggleRecord);
    record.state = 2;  // saving: no press until it has ended
    rig.dock.setRecord(record);
    rig.render();
    CHECK(!rig.dock.hitCenter(Kind::Record, 0, x, y) && rig.dock.note() == "저장 중…");
    record.state = 3;
    rig.dock.setRecord(record);
    rig.render();
    CHECK(rig.dock.hitCenter(Kind::Record, 0, x, y) && rig.dock.note() == "녹화가 멈췄어요");
    record.state = 4;
    rig.dock.setRecord(record);
    rig.render();
    CHECK(rig.dock.note() == "framecorder가 이미 녹화 중이에요");
    const std::string busy = pngPath("dock-quick-record-busy.png");
    if (!busy.empty()) CHECK(rig.dock.writePng(busy));
    // Without Chrome too: 브라우저 alone in its part
    rig.dock.setChromeRunning(false);
    record.state = 0;
    rig.dock.setRecord(record);
    rig.render();
    CHECK(rig.dock.hitCenter(Kind::Browser, 0, bx, by) && rig.dock.hitCenter(Kind::Record, 0, x, y) && bx < x);
    // framecorder's clips on: 녹화 | 클립 in the same part (전체 where it was), 클립 dim until a clip can be saved
    CHECK(!rig.dock.hitCenter(Kind::Clip, 0, x, y));
    double wholeX = 0, cx = 0, cy = 0;
    rig.dock.hitCenter(Kind::Record, 0, wholeX, y);
    double fullX = 0;
    rig.dock.hitCenter(Kind::Full, 0, fullX, fy);
    record.clips = true;
    CHECK(rig.dock.setRecord(record));
    rig.render();
    CHECK(rig.dock.hitCenter(Kind::Record, 0, x, y) && !rig.dock.hitCenter(Kind::Clip, 0, cx, cy));
    record.clipUsable = true;
    CHECK(rig.dock.setRecord(record));
    rig.render();
    double fullAfter = 0;
    CHECK(rig.dock.hitCenter(Kind::Clip, 0, cx, cy) && rig.dock.hitCenter(Kind::Full, 0, fullAfter, fy));
    CHECK(x < wholeX && wholeX < cx && cx < fullAfter && fullAfter == fullX && cy == y);
    const std::string clips = pngPath("dock-quick-record-clips.png");
    if (!clips.empty()) CHECK(rig.dock.writePng(clips));
    CHECK(rig.tap(Kind::Clip, 0).kind == DockPress::Kind::SaveClip);
    CHECK(rig.tap(Kind::Record, 0).kind == DockPress::Kind::ToggleRecord);
    record.state = 5;
    rig.dock.setRecord(record);
    rig.render();
    CHECK(rig.dock.note() == "클립을 저장했어요");
    record.state = 6;
    rig.dock.setRecord(record);
    rig.render();
    CHECK(rig.dock.note() == "framecorder가 하지 못했어요");
}

// The full view: the bar's buttons (완료 only in a fine-adjust session), the pages under it at the bar's height
void testFullView() {
    Rig rig("dock-full");
    Config cfg;
    SettingsPanel panel(rig.fonts);
    framecolor::ColorPage page(rig.model);
    panel.setColorPage(&page);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    panel.render(cfg, service, rig.profiles);
    rig.dock.renderFull(panel, false);
    CHECK(rig.dock.width() == panel.width() && rig.dock.height() == panel.height() + DockPanel::kBarHeight);
    double x = 0, y = 0;
    CHECK(rig.dock.hitCenter(Kind::Quick, 0, x, y) && y < DockPanel::kBarHeight);
    CHECK(rig.dock.hitCenter(Kind::Collapse, 0, x, y) && y < DockPanel::kBarHeight);
    CHECK(!rig.dock.hitCenter(Kind::Done, 0, x, y));
    CHECK(rig.dock.barDown(x, y).kind == DockPress::Kind::Collapse);
    rig.dock.barUp();
    CHECK(rig.dock.hitCenter(Kind::Quick, 0, x, y));
    CHECK(rig.dock.barDown(x, y).kind == DockPress::Kind::ShowQuick);
    rig.dock.barUp();
    CHECK(rig.dock.barDown(60, 40).kind == DockPress::Kind::GrabHandle);  // the handle: left of the buttons
    rig.dock.barUp();
    // A fine-adjust session: 완료 appears
    rig.dock.renderFull(panel, true);
    CHECK(rig.dock.hitCenter(Kind::Done, 0, x, y) && y < DockPanel::kBarHeight);
    CHECK(rig.dock.barDown(x, y).kind == DockPress::Kind::Done);
    rig.dock.barUp();
    // The pages work as on the tab (their own pointer state): a Placement button returns its action
    double bx = 0, by = 0;
    CHECK(panel.buttonCenter(SettingsAction::SizeUp, bx, by));
    CHECK(panel.pointerDown(bx, by, 1.0) == SettingsAction::SizeUp && panel.repeating());
    // ...and the Dock's Leave lets go of the held button
    panel.pointerLeave();
    CHECK(!panel.repeating());
    panel.pointerUp(bx, by);
    const std::string adjust = pngPath("dock-full-adjust.png");
    if (!adjust.empty()) {
        panel.render(cfg, service, rig.profiles);
        rig.dock.renderFull(panel, true);
        CHECK(rig.dock.writePng(adjust));
    }
    // The Color page in the full view (the shared model, the narrower header since the fifth page)
    const std::string color = pngPath("dock-full-color.png");
    panel.showPage(SettingsPage::Color);
    panel.render(cfg, service, rig.profiles);
    rig.dock.renderFull(panel, false);
    double cx = 0, cy = 0;
    CHECK(page.hitCenter(framecolor::ColorHit::Compare, 0, cx, cy));
    CHECK(page.hitCenter(framecolor::ColorHit::AutoCompare, 0, cx, cy));
    if (!color.empty()) CHECK(rig.dock.writePng(color));
}

// The main loop's wiring for the Dock panel: the controller's Leave lets go of both views' pointers (compare on the
// quick view, a held button on the full view), and a view switch draws the new view at once so the next event in the
// same batch hits its buttons
void testLoopWiring() {
    Rig rig("dock-wiring");
    Config cfg;
    SettingsPanel full(rig.fonts);
    framecolor::ColorPage page(rig.model);
    full.setColorPage(&page);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};

    // The quick view drawn: its own buttons, not the full view's bar
    dockloop::renderDock(DockView::Quick, false, rig.dock, full, cfg, service, rig.profiles, rig.t);
    double x = 0, y = 0;
    CHECK(rig.dock.hitCenter(Kind::Profile, 0, x, y) && rig.dock.hitCenter(Kind::Full, 0, x, y));
    CHECK(!rig.dock.hitCenter(Kind::Quick, 0, x, y));
    // 전체 pressed: the controller switches, the switch draws the full view at once
    DockController dock;
    dock.configure(true, 20, 0);
    dock.openQuick(1);
    CHECK(rig.dock.hitCenter(Kind::Full, 0, x, y));
    CHECK(rig.dock.pointerDown(x, y, rig.t).kind == DockPress::Kind::ShowFull);
    dock.openFull(1.1);
    dockloop::renderDock(dock.view(), dock.adjusting(), rig.dock, full, cfg, service, rig.profiles, rig.t);
    CHECK(rig.dock.width() == full.width() && rig.dock.height() == full.height() + DockPanel::kBarHeight);
    CHECK(rig.dock.hitCenter(Kind::Quick, 0, x, y) && y < DockPanel::kBarHeight);
    CHECK(!rig.dock.hitCenter(Kind::Profile, 0, x, y) && !rig.dock.hitCenter(Kind::Full, 0, x, y));
    CHECK(rig.dock.barDown(x, y).kind == DockPress::Kind::ShowQuick);  // the next press hits the new view
    rig.dock.barUp();
    // Collapsed draws nothing (the hit areas stay as they were)
    dockloop::renderDock(DockView::Collapsed, false, rig.dock, full, cfg, service, rig.profiles, rig.t);
    CHECK(rig.dock.hitCenter(Kind::Quick, 0, x, y));

    // Leave on the Dock panel: a held button on the full view stops, compare on the quick view lets go
    double bx = 0, by = 0;
    CHECK(full.buttonCenter(SettingsAction::SizeUp, bx, by));
    CHECK(full.pointerDown(bx, by, 1.0) == SettingsAction::SizeUp && full.repeating());
    const DockController::Event hudLeave {DockController::Event::Type::Leave, DockTarget::Hud};
    const DockController::Event dockLeave {DockController::Event::Type::Leave, DockTarget::Dock};
    const DockController::Event opened {DockController::Event::Type::Opened, DockTarget::None};
    CHECK(!dockloop::applyLeave(hudLeave, rig.dock, full) && full.repeating());  // the HUD's: not this panel
    CHECK(!dockloop::applyLeave(opened, rig.dock, full) && full.repeating());
    CHECK(dockloop::applyLeave(dockLeave, rig.dock, full) && !full.repeating());
    dockloop::renderDock(DockView::Quick, false, rig.dock, full, cfg, service, rig.profiles, rig.t);
    CHECK(rig.dock.hitCenter(Kind::Compare, 0, x, y));
    rig.dock.pointerDown(x, y, rig.t);
    CHECK(rig.model.state.compare);
    CHECK(dockloop::applyLeave(dockLeave, rig.dock, full) && !rig.model.state.compare);
    // From the controller itself: pointing away from a held press ends in that Leave
    dock.openQuick(2);
    dock.updateAim(false, true, 2);
    dock.updateAim(false, true, 2 + DockController::kAimDwellSec);
    dock.dockDown(2.3);
    rig.dock.pointerDown(x, y, rig.t);
    CHECK(rig.model.state.compare);
    dock.updateAim(false, false, 2.4);
    dock.tick(4);
    bool released = false;
    for (const DockController::Event& e : dock.takeEvents()) released |= dockloop::applyLeave(e, rig.dock, full);
    CHECK(released && !rig.model.state.compare);
}

// The README's pictures of the dashboard tab (every page), with the same made-up state as --dump-settings-png,
// written only when FRAMEDOCK_PNG_DIR is set: Korean under the README's names (ko-*.png), English and Japanese
// (en-*.png, ja-*.png) to check the longer words fit. The 기기 page as it is without Frame Sync (ko-device.png) and
// with it (*-device-sync.png)
void testDashboardPngs() {
    if (pngPath("x").empty()) return;
    FontSet fonts;
    fonts.load(Config().fontPath, Config().boldFontPath);
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("dock-dashboard-" + std::to_string(::getpid()));
    framecolor::ColorModel model(nullptr, Rig::makePaths(root));  // no host, not started: no files read or written
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    const struct {
        SettingsPage page;
        const char* name;
        bool busy;
        bool sync;
    } pages[] = {{SettingsPage::Placement, "placement", false, true}, {SettingsPage::Layout, "layout", false, true},
                 {SettingsPage::Profiles, "profiles", false, true},   {SettingsPage::Color, "color", false, true},
                 {SettingsPage::Color, "color-busy", true, true},     {SettingsPage::Dock, "dock", false, true},
                 {SettingsPage::Browser, "browser-page", false, true}, {SettingsPage::Device, "device", false, false},
                 {SettingsPage::Device, "device-sync", false, true}};
    for (const Language language : {Language::Ko, Language::En, Language::Ja}) {
        setUiLanguage(language);
        Config cfg;
        cfg.language = language;
        const std::string prefix = std::string(languageCode(language)) + "-";
        for (const auto& p : pages) {
            // (ko-browser.png is testBrowserPage's)
            if (language == Language::Ko && p.page == SettingsPage::Browser) continue;
            previewColorState(model, colorLanguage(language), p.busy);
            framecolor::ColorPage page(model);
            DeviceModel device;
            previewDeviceState(device, "");
            device.sync.present = device.sync.configured = p.sync;
            DevicePage devicePage(device);
            SettingsPanel panel(fonts);
            panel.setColorPage(&page);
            panel.setDevicePage(&devicePage);
            panel.showPage(p.page);
            const ProfilesUiState profiles = previewProfiles(language, p.page == SettingsPage::Profiles);
            if (p.page == SettingsPage::Profiles) panel.selectProfile(profiles.activeId);
            panel.render(cfg, service, profiles);
            CHECK(panel.writePng(pngPath((prefix + p.name + ".png").c_str())));
            if (p.page == SettingsPage::Placement && language == Language::Ko) {
                // The footer alone (its last 124 px): the quick switch
                const int h = 124;
                cairo_surface_t* crop = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel.width(), h);
                cairo_t* cr = cairo_create(crop);
                cairo_set_source_surface(cr, panel.surface(), 0, h - panel.height());
                cairo_paint(cr);
                cairo_destroy(cr);
                CHECK(cairo_surface_write_to_png(crop, pngPath("ko-quick-switch.png").c_str()) == CAIRO_STATUS_SUCCESS);
                cairo_surface_destroy(crop);
            }
        }
        // The Dock panel's quick view and the first-run guide in each language
        if (language != Language::Ko) {
            Rig rig("dock-lang");
            rig.model.state.language = colorLanguage(language);
            const frame_perf_overlay::WidgetText& wt = frame_perf_overlay::widgetText(widgetLanguage(language));
            rig.profiles.items[0].name = wt.simpleA;
            rig.profiles.items[1].name = wt.simpleB;
            DockBattery battery;
            battery.headsetPct = 46;
            battery.minutesText = minutesText(40);
            battery.left = {true, 80, false};
            battery.right = {true, 15, true};
            rig.dock.setBattery(battery);
            rig.render();
            CHECK(rig.dock.writePng(pngPath((prefix + "dock-quick.png").c_str())));
            // 녹화 (framecorder installed), recording, with Chrome running: the narrowest bottom row
            DockRecord record;
            record.available = true;
            record.state = 1;
            record.seconds = 3723;
            rig.dock.setRecord(record);
            rig.dock.setChromeRunning(true);
            rig.render();
            CHECK(rig.dock.writePng(pngPath((prefix + "dock-quick-record.png").c_str())));
        }
        GuideView guide(fonts);
        guide.render();
        CHECK(guide.writePng(pngPath((std::string("guide-") + languageCode(language) + ".png").c_str())));
    }
    setUiLanguage(Language::Ko);
    std::filesystem::remove_all(root);
}

// The UI languages: the string table, the header's language button (auto -> ko -> en -> ja -> auto, applied at once
// and on both the tab and the Dock's full view), the 기기 page without Frame Sync
void testLanguages() {
    std::string problem;
    CHECK(textTableComplete(problem));
    if (!problem.empty()) std::printf("text table: %s\n", problem.c_str());
    // Every string in each language, the Color page's too
    for (const framecolor::Language l : framecolor::availableLanguages()) {
        const framecolor::Translations& t = framecolor::getTranslations(l);
        const char* const* first = reinterpret_cast<const char* const*>(&t);
        const size_t suffix = offsetof(framecolor::Translations, savedToSlotSuffix) / sizeof(const char*);
        for (size_t i = 0; i < sizeof(framecolor::Translations) / sizeof(const char*); ++i) {
            // English has no suffix after "Saved to P1" on purpose
            CHECK(first[i] != nullptr && (first[i][0] != '\0' || (i == suffix && l == framecolor::Language::English)));
        }
    }
    CHECK(std::string(tr(Tx::DeviceSound, Language::En)) == "Sound" &&
          std::string(tr(Tx::DeviceSound, Language::Ja)) == "サウンド");
    CHECK(formatText("{1} / {0} {2}", {"a", "b"}) == "b / a {2}");
    setUiLanguage(Language::En);
    CHECK(trf(Tx::SyncMore, {"3"}) == "3 more" && uiLanguage() == Language::En);
    setUiLanguage(Language::Ko);

    // The button: between the page switch and the status pill, on every page but Color; each press is the next choice
    FontSet fonts;
    fonts.load(Config().fontPath, Config().boldFontPath);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    for (const bool inDock : {false, true}) {
        Config cfg;
        cfg.languageAuto = true;
        cfg.language = systemLanguage();
        SettingsPanel panel(fonts);
        panel.setInDock(inDock);
        panel.render(cfg, service);
        double x = 0, y = 0, px = 0, py = 0;
        CHECK(panel.buttonCenter(SettingsAction::LanguageCycle, x, y));
        CHECK(panel.buttonCenter(SettingsAction::PageDevice, px, py) && x > px && std::fabs(y - py) < 1);
        const Language expect[] = {Language::Ko, Language::En, Language::Ja};
        for (int i = 0; i < 4; ++i) {
            const SettingsAction action = press(panel, SettingsAction::LanguageCycle, 1 + i);
            CHECK(action == SettingsAction::LanguageCycle);
            CHECK(applySettingsAction(action, cfg));  // changed: the caller saves it
            if (i < 3) CHECK(!cfg.languageAuto && cfg.language == expect[i]);
            panel.render(cfg, service);
        }
        CHECK(cfg.languageAuto && cfg.language == systemLanguage());
        panel.showPage(SettingsPage::Color);
        panel.render(cfg, service);
        CHECK(!panel.buttonCenter(SettingsAction::LanguageCycle, x, y));  // the compare buttons' place
    }

    // 기기 without Frame Sync: no PC 계정 맞추기 at all, 소리 and 패스스루 across the whole page
    DeviceModel device;
    previewDeviceState(device, "");
    device.sync.present = device.sync.configured = false;
    DevicePage page(device);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1600, 738);
    cairo_t* cr = cairo_create(surface);
    page.render(Pen {cr, &fonts}, {28, 124, 1544, 468});
    double x = 0, y = 0;
    CHECK(!page.hitCenter(DeviceHit::SyncBig, 0, x, y) && !page.hitCenter(DeviceHit::Shutdown, 0, x, y) &&
          !page.hitCenter(DeviceHit::SyncRow, 0, x, y));
    CHECK(page.hitCenter(DeviceHit::Mute, 0, x, y) && page.hitCenter(DeviceHit::Passthrough, 2, x, y));
    double right = 0;
    for (const DevicePage::Hit& hit : page.hits()) right = std::max(right, hit.x + hit.w);
    CHECK(right > 1400);  // the volume row reaches the far side
    // Set up: the card is there, on the right
    device.sync.present = device.sync.configured = true;
    page.render(Pen {cr, &fonts}, {28, 124, 1544, 468});
    CHECK(page.hitCenter(DeviceHit::SyncBig, 0, x, y) && x > 900);
    // Files there but unreadable (e.g. no agentUrl): the card keeps its place (and says so), so 소리 stays narrow
    device.sync.configured = false;
    page.render(Pen {cr, &fonts}, {28, 124, 1544, 468});
    right = 0;
    for (const DevicePage::Hit& hit : page.hits()) {
        if (hit.kind == DeviceHit::Mute || hit.kind == DeviceHit::Passthrough) right = std::max(right, hit.x + hit.w);
    }
    CHECK(right > 0 && right < 900);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// The 기기 controls on the Dock: the quick view's 소리 · 패스스루 card (output, volume drag, passthrough) on the shared
// model, apart from the Dock's own buttons; the full view's 기기 page. PNGs of each when FRAMEDOCK_PNG_DIR is set
void testDeviceViews() {
    Rig rig("dock-device");
    rig.render();
    const DevicePage& card = rig.dock.devicePage();
    // The card's controls are inside the panel, between the colour card and the bottom row, and apart from the Dock's
    double cx = 0, cy = 0, bx = 0, by = 0;
    CHECK(rig.dock.hitCenter(Kind::AutoCompare, 0, bx, by) && rig.dock.hitCenter(Kind::Full, 0, cx, cy));
    const double colourBottom = by, bottomRow = cy;
    for (const DevicePage::Hit& hit : card.hits()) {
        CHECK(hit.x >= 0 && hit.x + hit.w <= DockPanel::kQuickWidth);
        CHECK(hit.y > colourBottom + 38 && hit.y + hit.h < bottomRow - 38);
    }
    double x = 0, y = 0;
    for (int i = 0; i < 2; ++i) CHECK(card.hitCenter(DeviceHit::Output, i, x, y));
    for (int i = 0; i < 3; ++i) CHECK(card.hitCenter(DeviceHit::Passthrough, i, x, y));
    CHECK(card.hitCenter(DeviceHit::Mute, 0, x, y) && card.hitCenter(DeviceHit::Volume, 0, x, y));
    CHECK(!card.hitCenter(DeviceHit::SyncBig, 0, x, y));  // Frame Sync only on the 기기 page
    // A press: the shared model changes, the main loop hears DeviceChanged (redraw both)
    rig.device.takeActions();
    card.hitCenter(DeviceHit::Output, 0, x, y);
    CHECK(rig.dock.pointerDown(x, y, rig.t).kind == DockPress::Kind::DeviceChanged);
    CHECK(rig.dock.pointerUp(x, y));
    CHECK(rig.device.audio.defaultId == rig.device.audio.outputs[0].id);
    // The volume: a press changes nothing; a drag moves it by the distance, the laser leaves -> the final value
    rig.render();
    double trackX = 0, trackW = 0;
    card.volumeTrack(trackX, trackW);
    card.hitCenter(DeviceHit::Volume, 0, x, y);
    CHECK(rig.device.audio.volume == 45);
    CHECK(rig.dock.pointerDown(trackX, y, rig.t).kind == DockPress::Kind::DeviceChanged && rig.device.audio.volume == 45);
    rig.dock.pointerMove(trackX + kVolumeDragStartPx, y);  // the drag starts here
    CHECK(rig.dock.pointerMove(trackX + kVolumeDragStartPx + trackW * 0.3, y) && rig.device.audio.volume == 75);
    CHECK(rig.dock.pointerLeave() && !rig.device.volumeDragging());
    auto actions = rig.device.takeActions();
    CHECK(actions.size() == 3 && actions.back().kind == DeviceAction::Kind::SetVolume && actions.back().final);  // + the output
    // ＋ held: a step now, more from tick() at nextWake(); the release lets go
    rig.render();
    CHECK(card.hitCenter(DeviceHit::VolumeUp, 0, x, y) && card.hitCenter(DeviceHit::VolumeDown, 0, x, y));
    card.hitCenter(DeviceHit::VolumeUp, 0, x, y);
    CHECK(rig.dock.pointerDown(x, y, rig.t).kind == DockPress::Kind::DeviceChanged && rig.device.audio.volume == 80);
    CHECK(rig.dock.nextWake() == rig.t + HoldRepeat::kDelaySec);
    CHECK(rig.dock.tick(rig.t + HoldRepeat::kDelaySec) && rig.device.audio.volume == 85);
    CHECK(rig.dock.pointerUp(x, y) && !rig.device.volumeDragging() && std::isinf(rig.dock.nextWake()));
    actions = rig.device.takeActions();
    CHECK(actions.size() == 3 && actions.back().final && actions.back().value == 85);
    // A press on the Dock's own buttons still works
    CHECK(rig.tap(Kind::Full).kind == DockPress::Kind::ShowFull);
    // Camera Switch off: 끔 | 켬 (no 컬러), 켬 turns it on without a colour
    rig.device.passthrough.cameraSwitchAvailable = false;
    rig.render();
    CHECK(card.hitCenter(DeviceHit::Passthrough, 0, x, y) && card.hitCenter(DeviceHit::Passthrough, 1, x, y) &&
          !card.hitCenter(DeviceHit::Passthrough, 2, x, y));

    // PNGs: the quick view with the card (two outputs, colour passthrough; one output, muted, unavailable)
    rig.dock.pointerLeave();
    previewDeviceState(rig.device, "");
    rig.render();
    const std::string quick = pngPath("dock-quick-device.png");
    if (!quick.empty()) CHECK(rig.dock.writePng(quick));
    previewDeviceState(rig.device, "single");
    rig.device.passthrough.cameraSwitchAvailable = false;
    rig.render();
    const std::string single = pngPath("dock-quick-device-single.png");
    if (!single.empty()) CHECK(rig.dock.writePng(single));

    // The full view's 기기 page: its own DevicePage on the same model
    previewDeviceState(rig.device, "");
    Config cfg;
    SettingsPanel panel(rig.fonts);
    framecolor::ColorPage page(rig.model);
    DevicePage fullPage(rig.device);
    panel.setColorPage(&page);
    panel.setDevicePage(&fullPage);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    panel.render(cfg, service, rig.profiles);
    CHECK(press(panel, SettingsAction::PageDevice, 1) == SettingsAction::None && panel.page() == SettingsPage::Device);
    panel.pointerUp(0, 0);
    panel.render(cfg, service, rig.profiles);
    rig.dock.renderFull(panel, false);
    CHECK(fullPage.hitCenter(DeviceHit::SyncBig, 0, x, y) && fullPage.hitCenter(DeviceHit::Output, 1, x, y));
    // A press on the page goes to it (no action for the caller), the shared model changes
    CHECK(fullPage.hitCenter(DeviceHit::Passthrough, 1, x, y));
    CHECK(panel.pointerDown(x, y, 2) == SettingsAction::None && rig.device.passthrough.mode == PassthroughMode::Mono);
    panel.pointerUp(x, y);
    // ...the volume drags through the panel too, and leaving the page lets go
    panel.render(cfg, service, rig.profiles);
    fullPage.volumeTrack(trackX, trackW);
    fullPage.hitCenter(DeviceHit::Volume, 0, x, y);
    panel.pointerDown(trackX + trackW, y, 3);
    CHECK(!rig.device.volumeDragging() && rig.device.audio.volume == 45);
    panel.pointerMove(trackX + trackW - kVolumeDragStartPx, y);
    CHECK(panel.pointerMove(trackX + trackW * 0.7 - kVolumeDragStartPx, y) && rig.device.audio.volume == 15);
    CHECK(press(panel, SettingsAction::PageColor, 4) == SettingsAction::None && !rig.device.volumeDragging());
    // ...and − held repeats through the panel's takeRepeat / nextRepeatAt (the main loop's), until the tab hides
    CHECK(press(panel, SettingsAction::PageDevice, 5) == SettingsAction::None);
    panel.pointerUp(0, 0);
    panel.render(cfg, service, rig.profiles);
    fullPage.hitCenter(DeviceHit::VolumeDown, 0, x, y);
    CHECK(panel.pointerDown(x, y, 6) == SettingsAction::None && rig.device.audio.volume == 10);
    CHECK(panel.nextRepeatAt() == 6 + HoldRepeat::kDelaySec);
    CHECK(panel.takeRepeat(6 + HoldRepeat::kDelaySec, [] { return true; }) == SettingsAction::None &&
          rig.device.audio.volume == 5);
    CHECK(panel.takeRepeat(7, [] { return false; }) == SettingsAction::None && rig.device.audio.volume == 5);
    CHECK(std::isinf(panel.nextRepeatAt()) && !rig.device.volumeDragging());
    previewDeviceState(rig.device, "sync-confirm");
    panel.showPage(SettingsPage::Device);
    panel.pointerLeave();  // no hover left on the page switch
    panel.render(cfg, service, rig.profiles);
    rig.dock.renderFull(panel, false);
    const std::string full = pngPath("dock-full-device.png");
    if (!full.empty()) CHECK(rig.dock.writePng(full));
}

}  // namespace

int main() {
    testDockPage();
    testBrowserPage();
    testQuickView();
    testFullView();
    testLoopWiring();
    testDeviceViews();
    testLanguages();
    testDashboardPngs();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("dock-view: all passed\n");
    return 0;
}
