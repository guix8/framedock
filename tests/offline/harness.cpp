// Off-device check of FrameColor's logic as Frame Dock runs it: ColorModel (current.txt, presets, auto compare,
// auto preset rules) with a fake OpenVR host, streaming detection, and bench's statistics and argument checks.
// No SteamVR needed; bench.cpp links libopenvr_api.so but nothing here calls into it.
// Run: build/framecolor-harness-test (ctest runs it as framecolor-harness)
#include "auto_profile.h"
#include "bench.h"
#include "color_i18n.h"
#include "color_model.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace framecolor;

namespace {

// Not assert(): the tests build as Release (NDEBUG) too
#define CHECK(expr)                                                                        \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

/** SteamVR as the model sees it: settings in a map, a context the test sets. */
class FakeHost : public ColorHost {
public:
    std::map<std::string, double> settings;
    DisplayContext now;
    std::string failKey;  ///< setSetting on this key fails
    double getSetting(const char* key, double fallback) override {
        const auto it = settings.find(key);
        return it == settings.end() ? fallback : it->second;
    }
    std::string setSetting(const char* key, double value) override {
        if (failKey == key) return "VRSettingsError_WriteFailed";
        settings[key] = static_cast<float>(value);  // SteamVR keeps floats
        return {};
    }
    DisplayContext context(bool withStreaming) override {
        ++contextCalls;
        if (withStreaming) ++streamingCalls;
        return {withStreaming && now.streaming, now.appKey};
    }
    int streamingCalls = 0;  ///< context() calls that asked for the display mode (the /proc scan)
    int contextCalls = 0;    ///< every context() call (the scene app's key is one IPC)
    std::string appName(const std::string& key) override { return key.empty() ? "" : "Name of " + key; }
};

std::vector<double> readValues(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<double> v;
    for (double x; in >> x;) v.push_back(x);
    return v;
}

void fakeProcess(const std::filesystem::path& root, const char* pid, const std::string& cmdline) {
    std::filesystem::create_directories(root / pid);
    std::ofstream(root / pid / "cmdline", std::ios::binary) << cmdline;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("framecolor-harness-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    ColorPaths paths;
    paths.configDir = root / "framecolor";
    paths.layerManifest = root / "layer/vkBasalt.json";
    std::filesystem::create_directories(paths.layerManifest.parent_path());
    std::ofstream(paths.layerManifest) << "{}\n";
    std::filesystem::create_directories(paths.configDir);

    // --- start(): reads SteamVR and the files, neutralises old settings, clears a compare bypass ---
    FakeHost host;
    host.settings["analogGain"] = 0.9f;
    host.settings["contrast"] = 0.4f;
    host.settings["hmdDisplayColorGainR"] = 0.8f;
    std::ofstream(paths.current()) << "0.1 0.1 0.1 1.2 1.2 1.2 1 1 1 1 1 1 0.4 1 1\n";  // bypass left on
    ColorModel model(&host, paths);
    CHECK(!model.started());
    CHECK(!model.tick(Clock::now()));  // nothing before start()
    CHECK(model.nextWake(Clock::now()) == Clock::time_point::max());
    model.start();
    CHECK(model.started());
    ColorState& ui = model.state;
    CHECK(ui.vulkanLayerActive);
    CHECK(near(ui.values.panel, 0.9) && near(ui.values.sharpen, 0.4) && near(ui.values.gamma[1], 1.2));
    CHECK(near(ui.values.contrast, 0.1));
    CHECK(ui.sharpenWhole);
    CHECK(host.settings["contrast"] == 0.0 && host.settings["hmdDisplayColorGainR"] == 1.0 &&
          host.settings["hmdDisplayColorGainB"] == 1.0);
    CHECK(ui.message.empty());  // "Applied" from the writes isn't shown when the page first opens
    {
        const auto v = readValues(paths.current());
        CHECK(v.size() == 16 && v[13] == 0.0 && v[14] == 1.0 && near(v[15], 0.4));
    }

    // --- Sharpness slider and current.txt (13th value) ---
    model.updateSlider(kSharpnessSlider, 1.0);  // up to 300%
    CHECK(ui.values.sharpen == 3.0);
    model.updateSlider(kSharpnessSlider, -0.2);  // left of the track
    CHECK(ui.values.sharpen == 0.0);
    model.updateSlider(kSharpnessSlider, 0.103);  // 2% steps: 15.45 steps of 2% → 30%
    CHECK(near(ui.values.sharpen, 0.30));
    model.updateSlider(kSharpnessSlider, 0.105);  // 15.75 → 32%
    CHECK(near(ui.values.sharpen, 0.32));
    ui.channel = 2;
    model.updateSlider(kSharpnessSlider, 0.5);  // sharpening is for all channels
    CHECK(ui.values.sharpen == 1.5);
    ui.channel = 0;
    ui.sharpenWhole = false;
    CHECK(model.saveAdvancedValues());
    {
        const auto v = readValues(paths.current());
        CHECK(v.size() == 16 && v[12] == 1.5 && v[13] == 0.0 && v[14] == 0.0 && v[15] == 1.5);
    }
    Values loaded;
    model.loadAdvancedValues(loaded);
    CHECK(loaded.sharpen == 1.5);
    std::ofstream(paths.current()) << "0 0 0 1 1 1 1 1 1 1 1 1 4.5 0 0\n";  // past 300%: held at 300%
    model.loadAdvancedValues(loaded);
    CHECK(loaded.sharpen == 3.0);
    std::ofstream(paths.current()) << "0 0 0 1 1 1 1 1 1 1 1 1\n";
    Values fresh;
    model.loadAdvancedValues(fresh);
    CHECK(fresh.sharpen == 0.0);
    CHECK(!model.loadSharpenWhole());  // older files sharpen the centre of each eye

    // --- The other sliders: ranges, rounding, "All" moves the three channels together ---
    for (int i = 0; i <= kSharpnessSlider; ++i) {
        double lo = 0, hi = 0;
        ColorModel::sliderRange(i, lo, hi);
        model.updateSlider(i, 0.0);
        CHECK(near(model.sliderValue(i), lo));
        model.updateSlider(i, 1.0);
        CHECK(near(model.sliderValue(i), hi));
    }
    CHECK(near(host.settings["analogGain"], 1.25));  // panel brightness goes straight to SteamVR
    model.updateSlider(kGainSlider, 0.5);              // 100 % for all
    ui.channel = 1;
    model.updateSlider(kGainSlider, 0.2);  // red to 70 %
    CHECK(near(ui.values.red, 0.7) && near(ui.values.green, 1.0));
    ui.channel = 0;
    model.updateSlider(kGainSlider, 0.6);  // all: +0.2 on average from 0.9
    CHECK(near(ui.values.red, 0.9) && near(ui.values.green, 1.2) && near(ui.values.blue, 1.2));
    model.updateSlider(kGainSlider, 0.5);
    model.updateSlider(kGammaSlider, 0.25);
    CHECK(near(model.sliderValue(kGammaSlider), 1.0));
    model.updateSlider(kSaturationSlider, 0.5);
    model.updateSlider(kContrastSlider, 0.0);

    // --- Sharpen area: centre -> follow gaze -> whole -> centre in the 15th value (0, 2, 1); presets don't change it ---
    model.toggleSharpenArea();
    CHECK(ui.sharpenGaze && !ui.sharpenWhole && model.loadSharpenGaze() && !model.loadSharpenWhole());
    CHECK(readValues(paths.current())[14] == 2.0);
    CHECK(model.gazeFollowActive() == (ui.vulkanLayerActive && ui.values.sharpen > 0.0));
    model.toggleSharpenArea();
    CHECK(ui.sharpenWhole && !ui.sharpenGaze && model.loadSharpenWhole() && !model.loadSharpenGaze());
    CHECK(readValues(paths.current())[14] == 1.0 && !model.gazeFollowActive());
    model.pressPreset(1);
    CHECK(ui.sharpenWhole);
    model.toggleSharpenArea();
    CHECK(!ui.sharpenWhole && !ui.sharpenGaze && !model.loadSharpenWhole() && !model.loadSharpenGaze());
    CHECK(readValues(paths.current())[14] == 0.0 && !model.gazeFollowActive());
    {
        // Gaze following: only with the layer and some sharpness
        ui.sharpenGaze = true;
        const double sharpness = ui.values.sharpen;
        ui.values.sharpen = 0.5;
        CHECK(model.gazeFollowActive());
        ui.compare = true; CHECK(model.gazeFollowActive()); ui.compare = false;  // kept through compare
        ui.values.sharpen = 0.0; CHECK(!model.gazeFollowActive());
        ui.values.sharpen = 0.5; ui.vulkanLayerActive = false; CHECK(!model.gazeFollowActive()); ui.vulkanLayerActive = true;
        CHECK(model.saveAdvancedValues() && readValues(paths.current())[14] == 2.0);
        ColorModel reread(&host, paths); reread.start();
        CHECK(reread.state.sharpenGaze && !reread.state.sharpenWhole);  // kept across a restart
        ui.sharpenGaze = false; ui.values.sharpen = sharpness;
        CHECK(model.saveAdvancedValues());
    }

    // --- Auto compare: starts on the original image, a switch every 3 s, one switch after a stall, off at 60 s ---
    {
        const Clock::time_point t0 = Clock::now();
        model.toggleAutoCompare(t0);
        CHECK(ui.autoCompare && ui.autoOriginal);
        CHECK(readValues(paths.current())[13] == 1.0);
        CHECK(ui.message == model.tr().autoCompareOn);
        model.toggleAutoCompare(t0);
        CHECK(!ui.autoCompare && !ui.autoOriginal);
        CHECK(readValues(paths.current())[13] == 0.0);

        ui.saveMode = true;
        model.toggleAutoCompare(t0);
        CHECK(!ui.saveMode);  // starting auto compare leaves save mode
        CHECK(ui.autoCompareEnd - ui.autoCompareTurnEnd == kAutoCompareLength - kAutoCompareTurn);
        CHECK(!model.tickAutoCompare(t0 + std::chrono::milliseconds(2900)) && ui.autoOriginal);
        CHECK(model.tickAutoCompare(t0 + kAutoCompareTurn) && !ui.autoOriginal);
        CHECK(!model.tickAutoCompare(t0 + std::chrono::milliseconds(5900)) && !ui.autoOriginal);
        const auto late = t0 + std::chrono::seconds(16);
        CHECK(model.tickAutoCompare(late) && ui.autoOriginal);
        CHECK(!model.tickAutoCompare(late + std::chrono::milliseconds(250)) && ui.autoOriginal);
        CHECK(ui.autoCompareTurnEnd == late + kAutoCompareTurn);
        // tick() writes each switch to current.txt (the layer follows the file while the dashboard is hidden)
        CHECK(model.tick(late + kAutoCompareTurn) && !ui.autoOriginal);
        CHECK(readValues(paths.current())[13] == 0.0);
        CHECK(model.nextWake(late + kAutoCompareTurn) <= late + 2 * kAutoCompareTurn);
        CHECK(model.tick(t0 + kAutoCompareLength) && !ui.autoCompare && !ui.autoOriginal);
        CHECK(ui.message == model.tr().autoCompareEnded);
        CHECK(!model.tickAutoCompare(t0 + kAutoCompareLength + kAutoCompareTurn));
        CHECK(readValues(paths.current())[13] == 0.0);
    }

    // --- Hold to compare: the bypass flag in the 14th value; releasing re-reads the panel brightness ---
    model.pressCompare();
    CHECK(ui.compare && readValues(paths.current())[13] == 1.0);
    host.settings["analogGain"] = 0.8f;  // changed elsewhere meanwhile
    model.release();
    CHECK(!ui.compare && readValues(paths.current())[13] == 0.0 && near(ui.values.panel, 0.8));
    model.pressCompare();
    model.leave();  // the laser left the tab, or the tab was hidden
    CHECK(!ui.compare && readValues(paths.current())[13] == 0.0);
    model.pressCompare();
    model.shutdown();  // never leaves the original image on screen
    CHECK(!ui.compare && readValues(paths.current())[13] == 0.0);

    // --- Presets: save mode, P1-P3 keep sharpness, old rows load with it off, empty slots ---
    ui.values.sharpen = 0.5;
    model.toggleSaveMode();
    CHECK(ui.saveMode && ui.message == model.tr().promptSaveSlot);
    model.pressPreset(0);  // a built-in preset is no save target: nothing happens
    CHECK(ui.saveMode && !ui.custom[0].valid);
    model.pressPreset(3);
    CHECK(!ui.saveMode && ui.custom[0].valid && ui.message == model.tr().formatSavedToSlot(1));
    {
        std::string first;
        {
            std::ifstream in(paths.presets());
            std::getline(in, first);
        }
        std::ofstream(paths.presets()) << first << "\n1 1 0 1 1 1 0 0 0 1 1 1 1 1 1\n";
    }
    ColorModel reloaded(nullptr, paths);
    reloaded.loadCustomPresets();
    CHECK(reloaded.state.custom[0].valid && reloaded.state.custom[0].values.sharpen == 0.5);
    CHECK(reloaded.state.custom[1].valid && reloaded.state.custom[1].values.sharpen == 0.0);
    CHECK(!reloaded.state.custom[2].valid);
    for (const Preset& preset : kBuiltins) CHECK(preset.values.sharpen == 0.0);
    model.pressPreset(5);  // P3 is empty
    CHECK(ui.messageIsError && ui.message == model.tr().slotEmpty);
    model.pressPreset(2);  // Night
    CHECK(near(ui.values.panel, 0.7) && near(host.settings["analogGain"], 0.7) && near(ui.values.blue, 0.82));
    // The line names it, and its button shows selected (presetInUse)
    CHECK(!ui.messageIsError && ui.message == model.tr().formatPresetApplied(model.presetName(2)));
    CHECK(model.presetInUse(2) && !model.presetInUse(0) && !model.presetInUse(5));

    // --- Reset: the first press asks, the second applies Standard ---
    model.pressReset();
    CHECK(ui.resetConfirm && near(ui.values.panel, 0.7));
    model.dismiss();  // a press elsewhere cancels it
    CHECK(!ui.resetConfirm);
    model.pressReset();
    model.pressReset();
    CHECK(!ui.resetConfirm && near(ui.values.panel, 1.0) && near(ui.values.blue, 1.0));
    // A SteamVR write that fails shows its error
    host.failKey = "analogGain";
    model.updateSlider(kPanelSlider, 0.5);
    CHECK(ui.messageIsError && ui.message.find("VRSettingsError_WriteFailed") != std::string::npos);
    host.failKey.clear();
    model.pressPreset(0);

    // --- Auto rules: file round trip, priority, cycling ---
    {
        const auto autoPath = paths.autoRules();
        AutoRules rules;
        rules.standalone = 1;
        rules.streaming = -1;
        rules.apps["steam.app.620"] = 3;
        CHECK(saveAutoRules(autoPath, rules));
        AutoRules back = loadAutoRules(autoPath);
        CHECK(back.standalone == 1 && back.streaming == -1 && back.apps.at("steam.app.620") == 3);
        rules.apps["system.generated.my game.exe"] = 4;  // app keys may contain spaces
        CHECK(saveAutoRules(autoPath, rules));
        back = loadAutoRules(autoPath);
        CHECK(back.apps.size() == 2 && back.apps.at("system.generated.my game.exe") == 4);
        rules.apps.erase("system.generated.my game.exe");
        std::ofstream(autoPath) << "standalone 9\nstreaming x\napp -1 k\napp 2\nbogus 2\n";
        back = loadAutoRules(autoPath);
        CHECK(back.standalone == -1 && back.streaming == -1 && back.apps.empty());
        CHECK(!std::filesystem::exists(autoPath.string() + ".tmp"));
        CHECK(resolveAutoPreset(rules, {false, "steam.app.620"}) == 3);  // app wins
        CHECK(resolveAutoPreset(rules, {false, "other"}) == 1);          // falls back to the mode
        CHECK(resolveAutoPreset(rules, {true, ""}) == -1);
        const bool onlyP2[3] = {false, true, false};
        CHECK(nextPresetChoice(-1, onlyP2) == 0);
        CHECK(nextPresetChoice(2, onlyP2) == 4);
        CHECK(nextPresetChoice(4, onlyP2) == -1);
        std::filesystem::remove(autoPath);
    }
    {
        ColorModel rulesModel(&host, paths);  // P1 is saved (above); P2 is made empty here, P3 is
        rulesModel.loadCustomPresets();
        rulesModel.state.custom[1].valid = false;
        rulesModel.state.context = {false, "steam.app.620"};
        rulesModel.pressRule(2);
        CHECK(rulesModel.state.autoRules.apps.at("steam.app.620") == 0);
        CHECK(loadAutoRules(paths.autoRules()).apps.at("steam.app.620") == 0);
        CHECK(rulesModel.state.message == rulesModel.tr().ruleSaved);
        for (int i = 0; i < 4; ++i) rulesModel.pressRule(2);  // 0 -> 1 -> 2 -> 3 (P1) -> off (P2 / P3 empty)
        CHECK(rulesModel.state.autoRules.apps.count("steam.app.620") == 0);
        rulesModel.state.autoRules.apps["steam.app.620"] = 1;
        CHECK(rulesModel.governingRule() == 2);
        rulesModel.state.context.appKey.clear();
        CHECK(rulesModel.governingRule() == 0);
        rulesModel.state.context.streaming = true;
        CHECK(rulesModel.governingRule() == 1);
        std::filesystem::remove(paths.autoRules());
    }

    // --- Auto presets from tick(): a new context counts after two checks; leaving all rules restores ---
    {
        host.now = {false, ""};
        ColorModel autoModel(&host, paths);
        autoModel.start();
        ColorState& a = autoModel.state;
        a.autoRules.apps["steam.app.620"] = 1;  // Vivid in this app
        const Clock::time_point t0 = Clock::now() + std::chrono::seconds(2);
        autoModel.tick(t0);
        autoModel.tick(t0 + kContextInterval);
        CHECK(a.contextKnown && a.context.appKey.empty() && !a.beforeAuto);
        a.values.panel = 0.95;  // the user's own look
        host.now = {false, "steam.app.620"};
        CHECK(!autoModel.tick(t0 + 2 * kContextInterval));  // seen once: not yet
        CHECK(!autoModel.tick(t0 + 2 * kContextInterval + std::chrono::milliseconds(500)));  // not due
        CHECK(autoModel.tick(t0 + 3 * kContextInterval));  // seen twice: applied
        CHECK(a.appName == "Name of steam.app.620");
        CHECK(near(a.values.panel, 1.10) && a.beforeAuto && near(a.beforeAuto->panel, 0.95));
        CHECK(a.message == std::string(autoModel.tr().autoApplied) + autoModel.tr().builtinPresets[1]);
        // It waits while a save slot is being picked
        a.saveMode = true;
        host.now = {false, ""};
        autoModel.tick(t0 + 4 * kContextInterval);
        autoModel.tick(t0 + 5 * kContextInterval);
        CHECK(a.context.appKey == "steam.app.620");
        CHECK(autoModel.nextWake(t0 + 5 * kContextInterval) == t0 + 6 * kContextInterval);
        a.saveMode = false;
        autoModel.tick(t0 + 6 * kContextInterval);
        CHECK(autoModel.tick(t0 + 7 * kContextInterval));
        CHECK(a.context.appKey.empty() && !a.beforeAuto && near(a.values.panel, 0.95));
        CHECK(a.message == autoModel.tr().autoRestored);
        // nextWake() is never in the past, so the caller's loop can't spin
        a.lookDirty = true;
        const Clock::time_point later = t0 + std::chrono::seconds(100);
        CHECK(autoModel.nextWake(later) >= later + kMinWake);
    }

    // --- The display mode (a /proc scan) only when something needs it; the "Now:" line catches up when shown ---
    {
        using std::chrono::milliseconds;
        host.now = {false, ""};
        host.streamingCalls = 0;
        ColorModel idle(&host, paths);
        idle.start();
        ColorState& s = idle.state;
        s.autoRules = AutoRules{};
        s.values.sharpen = 0.0;  // sharpening on would follow the mode too (the next block)
        const Clock::time_point t0 = Clock::now() + std::chrono::seconds(2);
        host.contextCalls = 0;
        idle.tick(t0);
        idle.tick(t0 + kContextInterval);
        // No rule and the page hidden: no context check at all (not even the scene app), and nothing to wake for
        CHECK(!s.contextKnown && host.streamingCalls == 0 && host.contextCalls == 0 && !idle.needsStreaming());
        CHECK(!idle.needsContext() && idle.nextWake(t0) == Clock::time_point::max());
        host.now = {true, ""};  // a PC stream starts while the tab is hidden: not looked at
        idle.tick(t0 + 2 * kContextInterval);
        idle.tick(t0 + 3 * kContextInterval);
        CHECK(!s.context.streaming && host.streamingCalls == 0);
        // The tab is shown: checked at once, and the "Now:" line doesn't wait for a second look
        const Clock::time_point shown = t0 + 3 * kContextInterval + milliseconds(100);
        idle.setPageVisible(true, shown);
        CHECK(idle.needsStreaming() && idle.nextWake(shown) <= shown + kMinWake);
        CHECK(idle.tick(shown));
        CHECK(s.context.streaming && host.streamingCalls == 1);
        // Hidden again: not looked at, unless a rule depends on the mode
        idle.setPageVisible(false, shown);
        idle.tick(shown + kContextInterval);
        CHECK(host.streamingCalls == 1);
        s.autoRules.streaming = 0;
        CHECK(idle.needsStreaming());
        idle.tick(shown + 2 * kContextInterval);
        CHECK(host.streamingCalls == 2);
        host.now = {false, ""};
    }

    // --- PC streaming pauses whole-image sharpening: the layer gets 0 (13th value), the user's sharpness stays
    //     (16th); the centre area stays on ---
    {
        host.now = {false, ""};
        ColorModel paused(&host, paths);
        paused.start();
        ColorState& p = paused.state;
        p.autoRules = AutoRules{};
        p.values.sharpen = 0.6;
        // The centre area (the default) stays on while streaming: nothing to follow for it
        p.sharpenWhole = false;
        CHECK(!paused.needsStreaming());
        p.contextKnown = true; p.context.streaming = true;
        CHECK(!paused.sharpenPausedForStreaming());
        CHECK(paused.saveAdvancedValues());
        {
            const auto v = readValues(paths.current());  // the centre area keeps its sharpness during a stream
            CHECK(v.size() == 16 && near(v[12], 0.6) && v[14] == 0.0 && near(v[15], 0.6));
        }
        paused.toggleSharpenArea();  // centre -> follow gaze: still sharpening during the stream
        {
            const auto v = readValues(paths.current());
            CHECK(p.sharpenGaze && !paused.sharpenPausedForStreaming() && near(v[12], 0.6) && v[14] == 2.0);
        }
        paused.toggleSharpenArea();  // to the whole image during the stream: the layer gets 0 at once
        {
            const auto v = readValues(paths.current());
            CHECK(p.sharpenWhole && paused.sharpenPausedForStreaming() && v[12] == 0.0 && v[14] == 1.0 && near(v[15], 0.6));
        }
        paused.toggleSharpenArea();  // and back to the centre: the sharpness at once
        {
            const auto v = readValues(paths.current());
            CHECK(!p.sharpenWhole && !paused.sharpenPausedForStreaming() && near(v[12], 0.6) && v[14] == 0.0);
        }
        p.contextKnown = false; p.context.streaming = false;
        // Whole-image sharpening pauses during a stream
        p.sharpenWhole = true;
        CHECK(paused.needsStreaming() && paused.needsContext());  // followed with the tab hidden and no rule
        const Clock::time_point t0 = Clock::now() + std::chrono::seconds(2);
        paused.tick(t0);
        paused.tick(t0 + kContextInterval);
        CHECK(p.contextKnown && !p.context.streaming && !paused.sharpenPausedForStreaming());
        CHECK(paused.saveAdvancedValues());
        {
            const auto v = readValues(paths.current());
            CHECK(v.size() == 16 && near(v[12], 0.6) && near(v[15], 0.6));
        }
        host.now = {true, ""};  // a PC stream starts
        CHECK(paused.tick(t0 + 2 * kContextInterval));
        CHECK(p.context.streaming && paused.sharpenPausedForStreaming() && near(p.values.sharpen, 0.6));
        {
            const auto v = readValues(paths.current());
            CHECK(v.size() == 16 && v[12] == 0.0 && near(v[15], 0.6) && near(v[3], p.values.gamma[0]));
        }
        Values reread;  // a restart during the stream still knows the user's sharpness
        paused.loadAdvancedValues(reread);
        CHECK(near(reread.sharpen, 0.6));
        // A slider moved during the stream: the user's value changes, the layer still gets 0
        paused.updateSlider(kSharpnessSlider, 0.3);
        CHECK(paused.saveAdvancedValues());
        {
            const auto v = readValues(paths.current());
            CHECK(v[12] == 0.0 && near(v[15], p.values.sharpen) && p.values.sharpen > 0.0);
        }
        const double chosen = p.values.sharpen;
        host.now = {false, ""};  // the stream ends: the layer gets the sharpness back
        CHECK(paused.tick(t0 + 3 * kContextInterval));
        CHECK(!p.context.streaming && !paused.sharpenPausedForStreaming());
        {
            const auto v = readValues(paths.current());
            CHECK(near(v[12], chosen) && near(v[15], chosen));
        }
        // A save slot left half picked when the tab is hidden: cancelled, so the pause still follows
        p.saveMode = true;
        paused.setPageVisible(true, t0 + std::chrono::seconds(29));
        paused.setPageVisible(false, t0 + std::chrono::seconds(29));
        CHECK(!p.saveMode);
        host.now = {true, ""};
        CHECK(paused.tick(t0 + std::chrono::seconds(30)));
        CHECK(paused.sharpenPausedForStreaming() && readValues(paths.current())[12] == 0.0);
        host.now = {false, ""};
        paused.tick(t0 + std::chrono::seconds(31));
        CHECK(!paused.sharpenPausedForStreaming() && near(readValues(paths.current())[12], chosen));
        p.saveMode = false;
        // Sharpening off: nothing to pause, and the mode isn't followed for it
        p.values.sharpen = 0.0;
        CHECK(!paused.needsStreaming());
        host.now = {true, ""};
        paused.tick(t0 + 4 * kContextInterval);
        CHECK(!paused.sharpenPausedForStreaming());
        // No layer: nothing to pause either
        p.values.sharpen = 0.6;
        p.vulkanLayerActive = false;
        CHECK(!paused.needsStreaming() && !paused.sharpenPausedForStreaming());
        host.now = {false, ""};
    }

    // --- A failing write of current.txt from tick() is retried after 1 s, then less often (not every 20 ms) ---
    {
        using std::chrono::milliseconds;
        using std::chrono::seconds;
        ColorPaths broken = paths;
        broken.configDir = root / "not-a-folder";
        std::ofstream(broken.configDir) << "a file, so nothing can be written under it\n";
        ColorModel failing(&host, broken);
        failing.start();
        ColorState& f = failing.state;
        f.autoRules = AutoRules{};
        f.lookDirty = true;
        const Clock::time_point t0 = Clock::now() + seconds(2);
        CHECK(failing.tick(t0));  // the error is shown
        CHECK(f.lookDirty && f.messageIsError && f.message == failing.tr().errorSaveAdvanced);
        CHECK(failing.nextWake(t0) >= t0 + kSaveRetryFirst);
        f.message.clear();
        failing.tick(t0 + milliseconds(500));
        CHECK(f.message.empty());  // no new attempt yet
        failing.tick(t0 + seconds(1));
        CHECK(f.message == failing.tr().errorSaveAdvanced);  // retried after 1 s
        f.message.clear();
        failing.tick(t0 + seconds(2));
        CHECK(f.message.empty());  // the next wait is 2 s
        CHECK(failing.nextWake(t0 + seconds(2)) >= t0 + seconds(2) + kMinWake);
        failing.tick(t0 + seconds(3));
        CHECK(f.message == failing.tr().errorSaveAdvanced);
        // Writable again: written at the next attempt, and the waiting starts over
        std::filesystem::remove(broken.configDir);
        std::filesystem::create_directories(broken.configDir);
        failing.tick(t0 + seconds(7));
        CHECK(!f.lookDirty && readValues(broken.current()).size() == 16);
        f.lookDirty = true;
        failing.tick(t0 + seconds(7) + kLookSaveInterval);
        CHECK(!f.lookDirty);
    }

    // --- A failed write from release() / leave() (not tick's own) is retried too: the bypass doesn't stay on ---
    {
        using std::chrono::seconds;
        ColorPaths broken = paths;
        broken.configDir = root / "not-a-folder-2";
        std::filesystem::create_directories(broken.configDir);
        ColorModel m(&host, broken);
        m.start();
        ColorState& f = m.state;
        f.autoRules = AutoRules{};
        m.pressCompare();  // writes the bypass on
        CHECK(readValues(broken.current()).size() == 16 && readValues(broken.current())[13] == 1.0);
        CHECK(!m.fileDirty());
        // The folder becomes a file: the release can't write the bypass off
        std::filesystem::remove_all(broken.configDir);
        std::ofstream(broken.configDir) << "not a folder\n";
        m.release();
        CHECK(m.fileDirty() && f.messageIsError);
        const Clock::time_point t0 = Clock::now() + seconds(2);
        CHECK(m.nextWake(t0) <= t0 + kLookSaveInterval);  // a retry is planned (nothing else is)
        m.tick(t0);  // fails again: the backoff starts
        CHECK(m.fileDirty() && m.nextWake(t0) >= t0 + kSaveRetryFirst);
        // Writable again: the retry writes the bypass off without any other change
        std::filesystem::remove(broken.configDir);
        std::filesystem::create_directories(broken.configDir);
        m.tick(t0 + seconds(1));
        CHECK(!m.fileDirty());
        const auto v = readValues(broken.current());
        CHECK(v.size() == 16 && v[13] == 0.0);
        // leave() during compare, failing, then shutdown(): shutdown tries once more
        m.pressCompare();
        std::filesystem::remove_all(broken.configDir);
        std::ofstream(broken.configDir) << "not a folder\n";
        m.leave();
        CHECK(m.fileDirty());
        std::filesystem::remove(broken.configDir);
        std::filesystem::create_directories(broken.configDir);
        m.shutdown();
        CHECK(!m.fileDirty() && readValues(broken.current())[13] == 0.0);
    }

    // --- A held compare with no pointer events for kCompareIdleStop: the pressing hand's trigger decides ---
    {
        ColorPaths held = paths;
        held.configDir = root / "held";
        std::filesystem::create_directories(held.configDir);
        ColorModel m(&host, held);
        m.start();
        m.state.autoRules = AutoRules{};
        TriggerState trigger = TriggerState::Pressed;
        int reads = 0;
        m.setTriggerProbe([&] {
            ++reads;
            return trigger;
        });
        // A deliberate still hold, the trigger readable: kept as long as it reads pressed
        m.pressCompare();
        CHECK(reads == 1);  // read at the press
        const Clock::time_point t0 = Clock::now();
        m.pointerSeen(t0);
        CHECK(m.nextWake(t0) <= t0 + kCompareIdleStop);
        m.tick(t0 + std::chrono::seconds(5));
        CHECK(m.state.compare && reads == 1);  // not read before kCompareIdleStop
        m.pointerSeen(t0 + std::chrono::seconds(8));  // still pointing
        m.tick(t0 + std::chrono::seconds(12));
        CHECK(m.state.compare && reads == 1);
        Clock::time_point t = t0 + std::chrono::seconds(18);
        CHECK(!m.tick(t) && m.state.compare && reads == 2);
        // Read again kCompareTriggerPoll later (the next wake), not every tick
        CHECK(m.nextWake(t) <= t + kCompareTriggerPoll);
        m.tick(t + std::chrono::milliseconds(500));
        CHECK(reads == 2);
        for (int s = 19; s <= 120; ++s) m.tick(t0 + std::chrono::seconds(s));  // two minutes, held still
        CHECK(m.state.compare && readValues(held.current())[13] == 1.0);
        // Released (its up event lost): let go at the next reading
        trigger = TriggerState::Released;
        CHECK(m.tick(t0 + std::chrono::seconds(121)));
        CHECK(!m.state.compare && readValues(held.current())[13] == 0.0);
        const int readsAfter = reads;
        m.tick(t0 + std::chrono::seconds(140));
        CHECK(reads == readsAfter);  // nothing held: nothing read

        // Unreadable: kept until kCompareUnreadableStop after the last pointer event, then let go
        trigger = TriggerState::Unreadable;
        m.pressCompare();
        const Clock::time_point t1 = Clock::now();
        m.pointerSeen(t1);
        m.tick(t1 + std::chrono::seconds(30));
        CHECK(m.state.compare);
        m.tick(t1 + kCompareUnreadableStop - std::chrono::seconds(1));
        CHECK(m.state.compare);
        CHECK(m.tick(t1 + kCompareUnreadableStop));
        CHECK(!m.state.compare && readValues(held.current())[13] == 0.0);

        // Reads "released" from the start (an all-zero state: no input focus): never believed, the 60 s limit holds
        trigger = TriggerState::Released;
        m.pressCompare();
        const Clock::time_point t2 = Clock::now();
        m.pointerSeen(t2);
        m.tick(t2 + std::chrono::seconds(15));
        CHECK(m.state.compare);
        CHECK(m.tick(t2 + kCompareUnreadableStop + std::chrono::seconds(1)) && !m.state.compare);

        // Unreadable at the press but readable later: a pressed reading makes its later "released" count
        trigger = TriggerState::Unreadable;
        m.pressCompare();
        const Clock::time_point t3 = Clock::now();
        m.pointerSeen(t3);
        trigger = TriggerState::Pressed;
        m.tick(t3 + std::chrono::seconds(11));
        CHECK(m.state.compare);
        trigger = TriggerState::Released;
        CHECK(m.tick(t3 + std::chrono::seconds(13)) && !m.state.compare);

        // No probe at all: as unreadable
        m.setTriggerProbe(nullptr);
        m.pressCompare();
        const Clock::time_point t4 = Clock::now();
        m.pointerSeen(t4);
        m.tick(t4 + std::chrono::seconds(30));
        CHECK(m.state.compare);
        CHECK(m.tick(t4 + kCompareUnreadableStop) && !m.state.compare);

        // Leaving (the laser left the tab, the dashboard hidden) still lets go at once, whatever the trigger
        m.setTriggerProbe([] { return TriggerState::Pressed; });
        m.pressCompare();
        m.leave();
        CHECK(!m.state.compare && readValues(held.current())[13] == 0.0);
    }

    // --- Loaded values are brought into the sliders' ranges (a hand-edited or damaged file) ---
    {
        ColorPaths odd = paths;
        odd.configDir = root / "odd";
        std::filesystem::create_directories(odd.configDir);
        std::ofstream(odd.current()) << "5 -1 0.5 9 0.1 1 3 0.2 1 7 -2 1 4 1 0\n";
        std::ofstream(odd.presets()) << "1 3 0.5 9 0.1 1 2 -1 0.5 9 0.1 1 7 -2 1 4\n"
                                     << "1 0.1 0 1 1 1 0 0 0 1 1 1 1 1 1 0\n";
        ColorModel m(&host, odd);
        Values v;
        m.loadAdvancedValues(v);
        CHECK(v.channelContrast[0] == 1.0 && v.channelContrast[1] == 0.0 && v.channelContrast[2] == 0.5);
        CHECK(v.gamma[0] == 2.5 && v.gamma[1] == 0.5 && v.gamma[2] == 1.0);
        CHECK(v.red == 1.5 && v.green == 0.5 && v.blue == 1.0);
        CHECK(v.saturation[0] == 2.0 && v.saturation[1] == 0.0 && v.sharpen == 3.0);  // 4 is past 300%
        m.loadCustomPresets();
        const Values& p = m.state.custom[0].values;
        CHECK(m.state.custom[0].valid && p.panel == 1.25 && p.contrast == 0.5 && p.red == 1.5 && p.green == 0.5);
        CHECK(p.channelContrast[0] == 1.0 && p.channelContrast[1] == 0.0 && p.gamma[0] == 2.5 && p.gamma[2] == 1.0);
        CHECK(p.saturation[0] == 2.0 && p.saturation[1] == 0.0 && p.sharpen == 3.0);
        CHECK(m.state.custom[1].valid && m.state.custom[1].values.panel == 0.40);
        // presets.txt and auto.txt are written whole (atomically, fsync'd): no .tmp left behind
        m.state.custom[2] = {kBuiltins[1].values, true};
        CHECK(m.saveCustomPresets());
        CHECK(!std::filesystem::exists(odd.presets().string() + ".tmp"));
        AutoRules rules;
        rules.standalone = 1;
        rules.apps["a b"] = 4;
        CHECK(saveAutoRules(odd.autoRules(), rules));
        const AutoRules back = loadAutoRules(odd.autoRules());
        CHECK(back.standalone == 1 && back.apps.at("a b") == 4 && !std::filesystem::exists(odd.autoRules().string() + ".tmp"));
        // A layer manifest under a folder we can't look into is "not enabled", not an exception
        ColorPaths hidden = odd;
        std::filesystem::create_directories(root / "locked/layer");
        hidden.layerManifest = root / "locked/layer/vkBasalt.json";
        std::filesystem::permissions(root / "locked", std::filesystem::perms::none);
        ColorModel h(&host, hidden);
        h.start();  // exists() on it fails with EACCES (unless we are root)
        CHECK(!h.state.vulkanLayerActive || ::geteuid() == 0);
        std::filesystem::permissions(root / "locked", std::filesystem::perms::owner_all);
    }

    // --- StreamingProbe: the vrlink found last by its PID; a full /proc scan only every kRescanSec ---
    {
        const std::filesystem::path proc = root / "proc-probe";
        fakeProcess(proc, "200", std::string("bash") + '\0');
        StreamingProbe probe(proc);
        CHECK(!probe.streaming(100.0) && probe.scans() == 1);
        fakeProcess(proc, "201", std::string("/opt/steamvr/bin/vrlink") + '\0');
        CHECK(!probe.streaming(101.0) && probe.scans() == 1);  // not rescanned yet
        CHECK(probe.streaming(100.0 + StreamingProbe::kRescanSec) && probe.scans() == 2);
        for (int i = 0; i < 10; ++i) CHECK(probe.streaming(106.0 + i));  // by its PID: no scan
        CHECK(probe.scans() == 2);
        // Its PID now belongs to something else: rescanned at once, nothing found
        std::ofstream(proc / "201" / "cmdline", std::ios::binary) << std::string("cat") + '\0';
        CHECK(!probe.streaming(120.0) && probe.scans() == 3);
        std::filesystem::remove_all(proc / "201");
        CHECK(!probe.streaming(121.0) && probe.scans() == 3);
    }

    // --- Streaming detection from /proc ---
    {
        const std::filesystem::path proc = root / "proc";
        fakeProcess(proc, "100", std::string("bash") + '\0');
        fakeProcess(proc, "101", std::string("/usr/bin/vrlinkx") + '\0');
        std::filesystem::create_directories(proc / "self");
        CHECK(!isStreaming(proc));
        fakeProcess(proc, "102", std::string("/opt/steamvr/bin/vrlink") + '\0' + "--serve" + '\0');
        CHECK(isStreaming(proc));
        CHECK(!isStreaming(root / "does-not-exist"));

        // --- Bench statistics and /proc readers ---
        CHECK(percentile({}, 0.5) == 0.0);
        CHECK(percentile({3, 1, 2}, 0.5) == 2.0);
        CHECK(percentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 0.95) == 10.0);
        {
            std::vector<FrameSample> s;
            for (uint32_t i = 1; i <= 90; ++i)
                s.push_back({i, i / 90.0, i == 90 ? 6.0f : 3.0f, 1.0f, i == 45 ? 1u : 0u, 0, i % 30 == 0});
            s.push_back(s[10]);      // duplicates from overlapping reads
            std::swap(s[0], s[50]);  // out of order
            const BenchSummary b = summarize(s);
            CHECK(b.frames == 90);
            CHECK(b.gpuP50 == 3.0 && b.gpuMax == 6.0);
            CHECK(b.dropped == 1 && b.reprojected == 3);
            CHECK(b.refreshHz > 89.9 && b.refreshHz < 90.1);
        }
        CHECK(summarize({}).frames == 0 && summarize({}).refreshHz == 0.0);
        std::filesystem::create_directories(proc / "200");
        std::ofstream(proc / "200" / "comm") << "vrcompositor\n";
        std::ofstream(proc / "200" / "status") << "Name:\tvrcompositor\nVmRSS:\t  409600 kB\nThreads:\t12\n";
        std::filesystem::create_directories(proc / "201");
        std::ofstream(proc / "201" / "comm") << "framedock\n";
        CHECK(findProcess(proc, "vrcompositor") == 200);
        CHECK(findProcess(proc, "framedock", 201) == -1);
        CHECK(findProcess(proc, "framedock") == 201);
        CHECK(readRssKb(proc, 200) == 409600);
        CHECK(readRssKb(proc, 201) == -1 && readRssKb(proc, -1) == -1);
        // DRM fdinfo, as the msm driver writes it: each client id counts once, other fds are skipped
        std::filesystem::create_directories(proc / "200" / "fdinfo");
        std::ofstream(proc / "200" / "fdinfo" / "0") << "pos:\t0\nflags:\t02\n";
        std::ofstream(proc / "200" / "fdinfo" / "67")
            << "drm-driver:\tmsm\ndrm-client-id:\t116\ndrm-engine-gpu:\t0 ns\ndrm-cycles-gpu:\t0\n";
        std::ofstream(proc / "200" / "fdinfo" / "69")
            << "drm-driver:\tmsm\ndrm-client-id:\t118\ndrm-engine-gpu:\t171714985445 ns\ndrm-cycles-gpu:\t154256749949\n"
               "drm-maxfreq-gpu:\t903000000 Hz\ndrm-total-memory:\t880204 KiB\ndrm-resident-memory:\t880204 KiB\n";
        std::ofstream(proc / "200" / "fdinfo" / "70")  // a second fd for client 118
            << "drm-driver:\tmsm\ndrm-client-id:\t118\ndrm-engine-gpu:\t171714985445 ns\ndrm-cycles-gpu:\t154256749949\n"
               "drm-resident-memory:\t880204 KiB\n";
        std::ofstream(proc / "200" / "fdinfo" / "71")  // a client without engine time; memory in MiB
            << "drm-driver:\tmsm\ndrm-client-id:\t119\ndrm-resident-memory:\t10 MiB\n";
        const DrmGpuUsage drm = readDrmGpuUsage(proc, 200);
        CHECK(drm.engineNs.size() == 2 && drm.engineNs.at("118") == 171714985445ull && drm.engineNs.at("116") == 0);
        CHECK(drm.residentKb == 880204 + 10 * 1024);
        CHECK(readDrmGpuUsage(proc, 201).engineNs.empty() && readDrmGpuUsage(proc, -1).engineNs.empty());
        CHECK(drmMemoryKb("880204 KiB") == 880204 && drmMemoryKb("10 MiB") == 10240 && drmMemoryKb("4096") == 4);
        CHECK(drmMemoryKb("0") == 0 && drmMemoryKb("") == 0);
        DrmGpuUsage before, after;
        before.engineNs = {{"116", 100}, {"118", 1000}, {"120", 50}};
        after.engineNs = {{"116", 150}, {"118", 1600}, {"121", 99999}};
        CHECK(drmEngineDeltaNs(before, after) == 650u);
        CHECK(!drmEngineDeltaNs(DrmGpuUsage{}, after));
    }

    // --- bench --compare: paired statistics, its arguments and the current.txt it writes ---
    {
        const PairStats p = pairedStats({{2.0, 2.4}, {1.8, 2.3}, {2.2, 2.5}});
        CHECK(p.pairs == 3 && std::abs(p.meanA - 2.0) < 1e-9 && std::abs(p.meanB - 2.4) < 1e-9);
        CHECK(std::abs(p.meanDiff - 0.4) < 1e-9);
        // differences 0.4, 0.5, 0.3: sample sd 0.1; 95% interval t(2) * 0.1 / sqrt(3)
        CHECK(std::abs(p.ci95 - 4.303 * 0.1 / std::sqrt(3.0)) < 1e-9);
        CHECK(pairedStats({}).pairs == 0 && pairedStats({{1.0, 2.0}}).ci95 == 0.0);
        CHECK(studentT95(0) == 0.0 && studentT95(11) == 2.201 && studentT95(30) == 2.042);
        CHECK(studentT95(59) < 2.042 && studentT95(59) > 1.98);
        CHECK(parseIntArg("0", 0, 100) == 0 && parseIntArg("60", 0, 100) == 60 && parseIntArg("100", 0, 100) == 100);
        for (const char* bad : {"", "-5", "101", "010", "0100", "0.6", "6a", "99999999999999999999"})
            CHECK(!parseIntArg(bad, 0, 100));
        CHECK(!parseIntArg("4", 5, 60) && parseIntArg("5", 5, 60) == 5 && !parseIntArg(nullptr, 0, 100));
        const auto with = currentWithSharpness("0 0 0 1.12 1.12 1.12 1 1 1 1.2 1.2 1.2 0.6 0 1\n", 0.35);
        CHECK(with && *with == "0 0 0 1.12 1.12 1.12 1 1 1 1.2 1.2 1.2 0.35 0 1\n");
        CHECK(*currentWithSharpness("0 0 0 1 1 1 1 1 1 1 1 1\n", 1.0) == "0 0 0 1 1 1 1 1 1 1 1 1 1\n");
        CHECK(!currentWithSharpness("0 0 0 1 1 1\n", 0.5) && !currentWithSharpness("", 0.5));
        CHECK(!currentWithSharpness("0 0 0 1 1 1 1 1 1 1 1 nan\n", 0.5));
        // The command line: bad --bench-compare arguments exit 2 before anything connects; other commands
        // (and Perf's `--dump-png PATH --bench N`) aren't bench commands
        const auto run = [](std::vector<const char*> args) {
            args.insert(args.begin(), "framedock");
            return runBenchCommand(static_cast<int>(args.size()), const_cast<char**>(args.data()));
        };
        CHECK(run({"--bench-compare", "0"}) == 2 && run({"--bench-compare", "0", "101"}) == 2);
        CHECK(run({"--bench-compare", "0", "60", "4"}) == 2 && run({"--bench-compare", "0", "60", "12", "1"}) == 2);
        CHECK(!run({}) && !run({"--dump-png", "x.png", "--bench", "5"}) && !run({"--help"}));
        CHECK(!run({"--bench", "5", "--dump-png", "x.png"}));
    }

    // --- Every string of every language is set (a null one would crash std::string) ---
    for (Language language : availableLanguages()) {
        const auto& t = getTranslations(language);
        CHECK(t.code == languageCode(language));
        const std::vector<const char*> strings{
            t.code, t.builtinPresets[0], t.builtinPresets[1], t.builtinPresets[2], t.emptySlotSuffix, t.save,
            t.selectSlot, t.reset, t.resetConfirm, t.panelBrightness, t.adjustmentChannel, t.channelNames[0],
            t.channelNames[1], t.channelNames[2], t.channelNames[3], t.brightness, t.saturation, t.contrast, t.gamma,
            t.sharpness, t.compare, t.compareHeld, t.autoCompare, t.autoCompareShowingOriginal,
            t.autoCompareShowingCorrected, t.autoCompareOn, t.autoCompareOff, t.autoCompareEnded, t.sharpenCenter, t.sharpenGaze, t.gazeFit, t.gazeFitRunning, t.gazeFitHint[0], t.gazeFitHint[1], t.gazeFitReviewHint[0], t.gazeFitReviewHint[1], t.gazeFitFailed, t.gazeFitFailReason[0], t.gazeFitFailReason[1],
            t.gazeFitFailReason[2], t.gazeFitFailReason[3],
            t.sharpenWhole, t.presets, t.autoPreset, t.standalone, t.streaming, t.thisApp, t.autoOff, t.noApp,
            t.nowPrefix, t.sharpenPausedStreaming, t.autoApplied, t.autoRestored, t.ruleSaved, t.layerOff, t.layerReload, t.errorSaveSettings, t.applied,
            t.errorSaveAdvanced, t.errorSavePreset, t.slotEmpty, t.presetApplied, t.promptSaveSlot, t.saveCanceled,
            t.savedToSlotPrefix, t.savedToSlotSuffix};
        for (const char* s : strings) CHECK(s != nullptr);
        // The struct has no other members: a new field must be added to the list above
        static_assert(sizeof(Translations) == 66 * sizeof(const char*), "update the string list");
    }

    std::filesystem::remove_all(root);
    std::printf("framecolor harness OK\n");
    return 0;
}
