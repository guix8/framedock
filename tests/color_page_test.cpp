// The Color page inside the dashboard tab: the fourth page of the page switch, its hit areas (inside the tab, clear
// of the header and footer buttons, not overlapping), every action through the panel's pointer events, and the
// shared language (always Korean). No SteamVR: a fake host stands in for OpenVR.
#include "color_model.h"
#include "color_page.h"
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "settings_panel.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using framecolor::ColorHit;

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

class FakeHost : public framecolor::ColorHost {
public:
    std::map<std::string, double> settings;
    framecolor::DisplayContext now;
    double getSetting(const char* key, double fallback) override {
        const auto it = settings.find(key);
        return it == settings.end() ? fallback : it->second;
    }
    std::string setSetting(const char* key, double value) override {
        settings[key] = value;
        return {};
    }
    framecolor::DisplayContext context(bool withStreaming) override {
        return {withStreaming && now.streaming, now.appKey};
    }
    std::string appName(const std::string& key) override { return key; }
};

std::vector<double> readValues(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<double> v;
    for (double x; in >> x;) v.push_back(x);
    return v;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

/** The panel, the page and a model on files in a temporary directory. */
struct Rig {
    std::filesystem::path root;
    FakeHost host;
    framecolor::ColorPaths paths;
    Config cfg;
    FontSet fonts;
    framecolor::ColorModel model;
    framecolor::ColorPage page;
    SettingsPanel panel;
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    double t = 1.0;  ///< the panel's clock (seconds)

    static framecolor::ColorPaths makePaths(const std::filesystem::path& root, bool layer) {
        framecolor::ColorPaths p;
        p.configDir = root / "framecolor";
        p.layerManifest = root / "layer/vkBasalt.json";
        std::filesystem::create_directories(p.configDir);
        std::filesystem::create_directories(p.layerManifest.parent_path());
        if (layer) std::ofstream(p.layerManifest) << "{}\n";
        return p;
    }

    explicit Rig(const std::string& name, bool layer = true)
        : root(std::filesystem::temp_directory_path() / (name + "-" + std::to_string(::getpid()))),
          paths(makePaths(root, layer)),
          model(&host, paths),
          page(model),
          panel(fonts) {
        cfg.language = Language::En;
        fonts.load(cfg.fontPath, cfg.boldFontPath);
        host.settings["analogGain"] = 1.0;
        model.start();
        panel.setColorPage(&page);
    }
    ~Rig() { std::filesystem::remove_all(root); }

    void render() { panel.render(cfg, service, {}); }

    /** A press and release at a point, then a redraw (the main loop redraws after input). */
    SettingsAction tap(double x, double y) {
        const SettingsAction action = panel.pointerDown(x, y, t);
        panel.pointerUp(x, y);
        t += 0.1;
        render();
        return action;
    }

    bool tapPage(ColorHit kind, int arg) {
        double x = 0, y = 0;
        if (!page.hitCenter(kind, arg, x, y)) return false;
        tap(x, y);
        return true;
    }

    bool tapButton(SettingsAction action) {
        double x = 0, y = 0;
        if (!panel.buttonCenter(action, x, y)) return false;
        tap(x, y);
        return true;
    }
};

// The page switch: Color is the fourth part, and the other pages still work
void testPageSwitch() {
    Rig rig("color-page-switch");
    rig.render();
    CHECK(rig.panel.page() == SettingsPage::Placement);
    double px = 0, py = 0, lx = 0, ly = 0, cx = 0, cy = 0, fx = 0, fy = 0;
    CHECK(rig.panel.buttonCenter(SettingsAction::PagePlacement, px, py));
    CHECK(rig.panel.buttonCenter(SettingsAction::PageProfiles, fx, fy));
    CHECK(rig.panel.buttonCenter(SettingsAction::PageLayout, lx, ly));
    CHECK(rig.panel.buttonCenter(SettingsAction::PageColor, cx, cy));
    CHECK(px < lx && lx < fx && fx < cx && py == cy);  // left to right, one row
    CHECK(rig.tapButton(SettingsAction::PageColor));
    CHECK(rig.panel.page() == SettingsPage::Color);
    CHECK(!rig.page.hits().empty());
    CHECK(rig.tapButton(SettingsAction::PageProfiles));
    CHECK(rig.panel.page() == SettingsPage::Profiles);
    CHECK(rig.tapButton(SettingsAction::PageColor));
    CHECK(rig.tapButton(SettingsAction::PagePlacement));
    CHECK(rig.panel.page() == SettingsPage::Placement);
    // Placement's buttons are back (the Color page placed none of its own in the panel)
    double x = 0, y = 0;
    CHECK(rig.panel.buttonCenter(SettingsAction::PresetCenterBottom, x, y));

    // A panel without a Color page draws an empty page and doesn't crash
    FontSet fonts;
    fonts.load(rig.cfg.fontPath, rig.cfg.boldFontPath);
    SettingsPanel bare(fonts);
    bare.showPage(SettingsPage::Color);
    bare.render(rig.cfg, rig.service, {});
    CHECK(bare.pointerDown(800, 300, 1.0) == SettingsAction::None);
    bare.pointerUp(800, 300);
}

// Every hit area is inside the tab, above the footer, clear of the panel's own buttons, and they don't overlap
void testHitAreas() {
    for (Language language : {Language::En, Language::Ja, Language::Ko}) {
        Rig rig("color-page-hits");
        rig.cfg.language = language;
        rig.model.state.context = {false, "steam.app.620"};  // so "this app" is hittable too
        rig.panel.showPage(SettingsPage::Color);
        rig.render();
        const auto& hits = rig.page.hits();
        // 6 sliders, 4 channels, 6 presets, save, reset, compare, auto compare, sharpen area, 3 rules
        CHECK(hits.size() == 6 + 4 + 6 + 2 + 2 + 1 + 3);
        double footerTop = 0, dummy = 0;
        CHECK(rig.panel.buttonCenter(SettingsAction::Quit, dummy, footerTop));
        footerTop -= 34;  // the footer's buttons are 68 px high
        for (size_t i = 0; i < hits.size(); ++i) {
            const auto& a = hits[i];
            CHECK(a.x >= 0 && a.y >= 0 && a.x + a.w <= rig.panel.width() && a.y + a.h < footerTop);
            for (size_t j = i + 1; j < hits.size(); ++j) {
                const auto& b = hits[j];
                const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
                if (overlap) std::printf("  overlap: %d/%d and %d/%d\n", static_cast<int>(a.kind), a.arg,
                                         static_cast<int>(b.kind), b.arg);
                CHECK(!overlap);
            }
        }
        // The page switch and the footer's buttons stay the panel's
        for (SettingsAction action : {SettingsAction::PagePlacement, SettingsAction::PageColor,
                                      SettingsAction::AutostartOn, SettingsAction::Quit}) {
            double x = 0, y = 0;
            CHECK(rig.panel.buttonCenter(action, x, y));
            CHECK(rig.page.hitAt(x, y) == ColorHit::None);
        }
    }
}

// Each control through the panel's pointer events
void testActions() {
    Rig rig("color-page-actions");
    framecolor::ColorState& ui = rig.model.state;
    rig.panel.showPage(SettingsPage::Color);
    rig.render();
    CHECK(ui.language == framecolor::Language::English);

    // A preset (Vivid) and the panel brightness it sets in SteamVR
    CHECK(rig.tapPage(ColorHit::Preset, 1));
    CHECK(near(ui.values.panel, 1.10) && near(rig.host.settings["analogGain"], 1.10));
    CHECK(near(readValues(rig.paths.current())[3], 1.08));
    // It shows as the one in use, and the line says which
    CHECK(rig.model.presetInUse(1) && !rig.model.presetInUse(0) && !rig.model.presetInUse(2));
    CHECK(ui.message == "Vivid applied" && !ui.messageIsError);

    // A slider: press at the right end, drag to the left end, release writes current.txt
    double x = 0, y = 0;
    CHECK(rig.page.hitCenter(ColorHit::Slider, framecolor::kSaturationSlider, x, y));
    double left = 1e9, right = 0;
    for (const auto& h : rig.page.hits()) {
        if (h.kind == ColorHit::Slider && h.arg == framecolor::kSaturationSlider) {
            left = h.x;
            right = h.x + h.w;
        }
    }
    CHECK(rig.panel.pointerDown(right - 1, y, rig.t) == SettingsAction::None);
    CHECK(ui.activeSlider == framecolor::kSaturationSlider && near(ui.values.saturation[0], 2.0));
    CHECK(rig.panel.pointerMove(left + 1, y));
    CHECK(near(ui.values.saturation[1], 0.0));
    rig.panel.pointerMove(left + 1, y - 200);  // off the track: still the same knob
    CHECK(ui.activeSlider == framecolor::kSaturationSlider);
    rig.panel.pointerUp(left + 1, y - 200);
    CHECK(ui.activeSlider == -1 && !ui.lookDirty);
    CHECK(near(readValues(rig.paths.current())[9], 0.0));
    CHECK(!rig.model.presetInUse(1));  // not Vivid any more

    // The channel: Red, then the brightness slider moves red only
    rig.render();
    CHECK(rig.tapPage(ColorHit::Channel, 1));
    CHECK(ui.channel == 1);
    CHECK(rig.page.hitCenter(ColorHit::Slider, framecolor::kGainSlider, x, y));
    rig.tap(x, y);  // the middle: 100 %
    CHECK(near(ui.values.red, 1.0) && near(ui.values.green, 1.05));
    CHECK(rig.tapPage(ColorHit::Channel, 0));

    // Hold to compare: on while held, off on release
    CHECK(rig.page.hitCenter(ColorHit::Compare, 0, x, y));
    rig.panel.pointerDown(x, y, rig.t);
    CHECK(ui.compare && readValues(rig.paths.current())[13] == 1.0);
    rig.panel.pointerUp(x, y);
    CHECK(!ui.compare && readValues(rig.paths.current())[13] == 0.0);
    // ...and off when the laser leaves the tab while held
    rig.panel.pointerDown(x, y, rig.t);
    CHECK(ui.compare);
    rig.panel.pointerLeave();
    CHECK(!ui.compare && readValues(rig.paths.current())[13] == 0.0);
    rig.render();

    // Auto compare on and off
    CHECK(rig.tapPage(ColorHit::AutoCompare, 0));
    CHECK(ui.autoCompare && ui.autoOriginal && readValues(rig.paths.current())[13] == 1.0);
    CHECK(rig.tapPage(ColorHit::AutoCompare, 0));
    CHECK(!ui.autoCompare && readValues(rig.paths.current())[13] == 0.0);

    // Sharpen area
    CHECK(rig.tapPage(ColorHit::SharpenArea, 0));
    CHECK(ui.sharpenGaze && readValues(rig.paths.current())[14] == 2.0);
    CHECK(rig.tapPage(ColorHit::SharpenArea, 0));
    CHECK(ui.sharpenWhole && readValues(rig.paths.current())[14] == 1.0);

    // Save mode: built-in presets can't be picked, P2 saves
    CHECK(rig.tapPage(ColorHit::SaveMode, 0));
    CHECK(ui.saveMode);
    CHECK(!rig.page.hitCenter(ColorHit::Preset, 0, x, y));  // drawn disabled
    CHECK(rig.tapPage(ColorHit::Preset, 4));
    CHECK(!ui.saveMode && ui.custom[1].valid);

    // Reset: two taps; a tap on an empty spot or a footer button in between cancels it
    CHECK(rig.tapPage(ColorHit::Reset, 0));
    CHECK(ui.resetConfirm);
    rig.tap(rig.panel.width() / 2.0, 112);  // between the header and the cards
    CHECK(!ui.resetConfirm);
    CHECK(rig.tapPage(ColorHit::Reset, 0));
    rig.tapButton(SettingsAction::AutostartOn);  // a footer button
    CHECK(!ui.resetConfirm && rig.panel.page() == SettingsPage::Color);
    CHECK(rig.tapPage(ColorHit::Reset, 0));
    CHECK(rig.tapPage(ColorHit::Reset, 0));
    CHECK(!ui.resetConfirm && near(ui.values.panel, 1.0) && near(ui.values.saturation[0], 1.0));

    // The auto preset rules: standalone cycles through the choices; "this app" needs a scene app
    CHECK(rig.tapPage(ColorHit::AutoRule, 0));
    CHECK(ui.autoRules.standalone == 0);
    CHECK(!rig.page.hitCenter(ColorHit::AutoRule, 2, x, y));
    ui.context = {false, "steam.app.620"};
    rig.render();
    CHECK(rig.tapPage(ColorHit::AutoRule, 2));
    CHECK(ui.autoRules.apps.at("steam.app.620") == 0);
    CHECK(framecolor::loadAutoRules(rig.paths.autoRules()).apps.at("steam.app.620") == 0);

    // Switching pages while compare is held lets go of it
    CHECK(rig.page.hitCenter(ColorHit::Compare, 0, x, y));
    rig.panel.pointerDown(x, y, rig.t);
    CHECK(ui.compare);
    double sx = 0, sy = 0;
    CHECK(rig.panel.buttonCenter(SettingsAction::PagePlacement, sx, sy));
    rig.panel.pointerDown(sx, sy, rig.t);  // a press on the page switch without a release in between
    // Let go of on the press itself, not only on the release that follows
    CHECK(rig.panel.page() == SettingsPage::Placement && !ui.compare);
    CHECK(readValues(rig.paths.current())[13] == 0.0);
    rig.panel.pointerUp(sx, sy);
    CHECK(rig.panel.page() == SettingsPage::Placement && !ui.compare);
}

// Without the Vulkan layer only the panel brightness, presets, channel and rules are usable
void testWithoutLayer() {
    Rig rig("color-page-nolayer", false);
    rig.panel.showPage(SettingsPage::Color);
    rig.render();
    CHECK(!rig.model.state.vulkanLayerActive);
    double x = 0, y = 0;
    CHECK(rig.page.hitCenter(ColorHit::Slider, framecolor::kPanelSlider, x, y));
    for (int slider : {framecolor::kGainSlider, framecolor::kSaturationSlider, framecolor::kContrastSlider,
                       framecolor::kGammaSlider, framecolor::kSharpnessSlider}) {
        CHECK(!rig.page.hitCenter(ColorHit::Slider, slider, x, y));
    }
    CHECK(!rig.page.hitCenter(ColorHit::Compare, 0, x, y));
    CHECK(!rig.page.hitCenter(ColorHit::AutoCompare, 0, x, y));
    CHECK(!rig.page.hitCenter(ColorHit::SharpenArea, 0, x, y));
    CHECK(rig.page.hitCenter(ColorHit::Preset, 0, x, y));
    std::string line;
    bool error = true;
    CHECK(rig.page.statusLine(line, error) && !error && line == rig.model.tr().layerOff);
}

// The preset in use: the values compared with a little slack (files, SteamVR's float), P1-P3 only when saved
void testPresetInUse() {
    framecolor::Values vivid = framecolor::kBuiltins[1].values;
    CHECK(framecolor::sameValues(vivid, framecolor::kBuiltins[1].values));
    vivid.panel = static_cast<float>(vivid.panel);  // SteamVR hands back a float
    vivid.gamma[2] += 1e-7;                         // current.txt's 9 digits
    CHECK(framecolor::sameValues(vivid, framecolor::kBuiltins[1].values));
    vivid.sharpen = 0.02;  // one step of the sharpness slider
    CHECK(!framecolor::sameValues(vivid, framecolor::kBuiltins[1].values));
    vivid = framecolor::kBuiltins[1].values;
    vivid.saturation[1] = 1.01;
    CHECK(!framecolor::sameValues(vivid, framecolor::kBuiltins[1].values));

    Rig rig("color-page-in-use");
    framecolor::ColorState& ui = rig.model.state;
    ui.values = framecolor::kBuiltins[0].values;
    CHECK(rig.model.presetInUse(0) && !rig.model.presetInUse(3));  // P1 empty: never in use, even with its zeros
    ui.custom[0] = {ui.values, true};
    CHECK(rig.model.presetInUse(0) && rig.model.presetInUse(3));    // both hold these values
    CHECK(!rig.model.presetInUse(-1) && !rig.model.presetInUse(6));
    // Korean, as the user sees it
    const framecolor::Translations& ko = framecolor::getTranslations(framecolor::Language::Korean);
    CHECK(ko.formatPresetApplied(ko.builtinPresets[1]) == "비비드 적용했어요");
    // The page draws (selected buttons, the reload line) without changing what is in use
    rig.panel.showPage(SettingsPage::Color);
    rig.render();
    CHECK(rig.model.presetInUse(3));
}

// "SteamVR must restart": vrcompositor (found by its comm) still maps the layer library as "(deleted)"
void testLayerReload() {
    const std::string lib = "/home/u/.local/share/framecolor/lib/libvkbasalt.so";
    const auto deleted = [&](const std::string& text) {
        std::istringstream maps(text);
        return framecolor::mapsLibraryDeleted(maps, lib);
    };
    CHECK(deleted("ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751                    " + lib + " (deleted)\n"));
    CHECK(!deleted("ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751                    " + lib + "\n"));
    CHECK(!deleted("ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751   /x" + lib + " (deleted)\n"));
    CHECK(!deleted("ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751   " + lib + ".old (deleted)\n"));
    CHECK(!deleted(""));
    std::istringstream any("7f00-7f01 r-xp 0 0:0 1   " + lib + " (deleted)\n");
    CHECK(!framecolor::mapsLibraryDeleted(any, ""));

    Rig rig("color-page-reload");
    const std::filesystem::path proc = rig.root / "proc";
    const auto process = [&](const std::string& pid, const std::string& comm, const std::string& maps) {
        std::filesystem::create_directories(proc / pid);
        std::ofstream(proc / pid / "comm") << comm << "\n";
        std::ofstream(proc / pid / "maps") << maps;
    };
    process("1", "systemd", "");
    process("2423", "vrcompositor", "ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751   " + lib + " (deleted)\n");
    std::filesystem::create_directories(proc / "self");
    framecolor::ColorPaths paths = rig.paths;
    paths.layerLibrary = lib;
    paths.procRoot = proc;
    framecolor::ColorModel model(&rig.host, paths);
    model.start();
    const auto now = framecolor::Clock::now();
    CHECK(model.checkLayerReload(now) && model.state.layerReloadNeeded);
    // Read again only after kLayerReloadInterval: SteamVR restarted, the new library mapped
    process("2423", "vrcompositor", "ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774752   " + lib + "\n");
    CHECK(model.checkLayerReload(now + std::chrono::seconds(1)));
    CHECK(!model.checkLayerReload(now + framecolor::kLayerReloadInterval));
    // Its PID taken by something else, and no vrcompositor: nothing to say
    process("2423", "bash", "ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751   " + lib + " (deleted)\n");
    CHECK(!model.checkLayerReload(now + 2 * framecolor::kLayerReloadInterval));
    // Found again by the scan (another PID); and never with the layer off
    process("3000", "vrcompositor", "ffffb5360000-ffffb5394000 r-xp 00000000 08:08 2774751   " + lib + " (deleted)\n");
    CHECK(model.checkLayerReload(now + 3 * framecolor::kLayerReloadInterval));
    model.state.vulkanLayerActive = false;
    CHECK(!model.checkLayerReload(now + 4 * framecolor::kLayerReloadInterval) && !model.state.layerReloadNeeded);
}

// A profile action's result on the Color page's footer: over the page's hint and messages for a while, not over its
// errors
void testFooterWithProfileStatus() {
    Rig rig("color-page-footer", false);
    rig.panel.showPage(SettingsPage::Color);
    rig.render();
    CHECK(rig.panel.footerNote() == rig.model.tr().layerOff);
    rig.panel.showActionStatus("Couldn't use Desk", true, 10.0);
    rig.render();
    CHECK(rig.panel.footerNote() == "Couldn't use Desk");
    rig.model.state.message = "Saved to P1";
    rig.model.state.messageIsError = false;
    rig.render();
    CHECK(rig.panel.footerNote() == "Couldn't use Desk");
    rig.model.state.message = "Couldn't write current.txt";
    rig.model.state.messageIsError = true;
    rig.render();
    CHECK(rig.panel.footerNote() == "Couldn't write current.txt");  // the page's error keeps its place
    rig.model.state.message.clear();
    rig.model.state.messageIsError = false;
    CHECK(rig.panel.tick(10.0 + SettingsPanel::kActionErrorSec + 1));
    rig.render();
    CHECK(rig.panel.footerNote() == rig.model.tr().layerOff);
}

// One language for the whole app: the Color page follows config.language
void testLanguage() {
    CHECK(colorLanguage(Language::Ko) == framecolor::Language::Korean);
    CHECK(colorLanguage(Language::Ja) == framecolor::Language::Japanese);
    CHECK(colorLanguage(Language::En) == framecolor::Language::English);
    Rig rig("color-page-language");
    rig.panel.showPage(SettingsPage::Color);
    rig.cfg.language = Language::Ko;
    rig.render();
    CHECK(rig.model.state.language == framecolor::Language::Korean);
    CHECK(std::string(uiText(Language::Ko).pageColor) == "색 보정");
    // Korean only: a new config is Korean, and the footer (where the language switch was) has only autostart and
    // Quit, on every page
    CHECK(Config().language == Language::Ko);
    for (const SettingsPage page : {SettingsPage::Placement, SettingsPage::Layout, SettingsPage::Profiles,
                                    SettingsPage::Color}) {
        rig.panel.showPage(page);
        rig.render();
        double qx = 0, qy = 0;
        CHECK(rig.panel.buttonCenter(SettingsAction::Quit, qx, qy));
        for (double y = qy - 30; y <= qy + 30; y += 10) {
            for (double x = 0; x < rig.panel.width(); x += 8) {
                const SettingsAction a = rig.panel.pointerDown(x, y, rig.t);
                rig.panel.pointerUp(x, y);
                CHECK(a == SettingsAction::None || a == SettingsAction::AutostartOn ||
                      a == SettingsAction::AutostartOff || a == SettingsAction::Quit);
            }
        }
        rig.panel.pointerLeave();
    }
}

}  // namespace

int main() {
    testPageSwitch();
    testHitAreas();
    testActions();
    testWithoutLayer();
    testLanguage();
    testFooterWithProfileStatus();
    testPresetInUse();
    testLayerReload();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("color page OK\n");
    return 0;
}
