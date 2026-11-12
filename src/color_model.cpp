#include "color_model.h"

#include "atomic_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace framecolor {

const std::array<Preset, 3> kBuiltins{{
    {{1.00, 0.00, 1.00, 1.00, 1.00, {0.00, 0.00, 0.00}, {1.00, 1.00, 1.00}, {1.00, 1.00, 1.00}}, true},
    {{1.10, 0.30, 1.05, 1.05, 1.05, {0.30, 0.30, 0.30}, {1.08, 1.08, 1.08}, {1.00, 1.00, 1.00}}, true},
    {{0.70, 0.15, 1.05, 0.95, 0.82, {0.15, 0.15, 0.15}, {0.92, 0.92, 0.92}, {1.00, 1.00, 1.00}}, true},
}};

double clampValue(double value, double lo, double hi) { return std::max(lo, std::min(hi, value)); }

bool sameValues(const Values& a, const Values& b) {
    // The sliders move in 0.01 steps (sharpness 0.02)
    const auto same = [](double x, double y) { return std::abs(x - y) < 0.004; };
    const auto sameAll = [&](const std::array<double, 3>& x, const std::array<double, 3>& y) {
        return same(x[0], y[0]) && same(x[1], y[1]) && same(x[2], y[2]);
    };
    return same(a.panel, b.panel) && same(a.red, b.red) && same(a.green, b.green) && same(a.blue, b.blue) &&
           sameAll(a.channelContrast, b.channelContrast) && sameAll(a.gamma, b.gamma) &&
           sameAll(a.saturation, b.saturation) && same(a.sharpen, b.sharpen);
}

namespace {

double average(const std::array<double, 3>& values) { return (values[0] + values[1] + values[2]) / 3.0; }

double selectedArrayValue(const std::array<double, 3>& values, int channel) {
    return channel == 0 ? average(values) : values[channel - 1];
}

void setSelectedArrayValue(std::array<double, 3>& values, int channel, double next, double lo, double hi) {
    if (channel == 0) {
        const double delta = next - average(values);
        for (double& value : values) value = clampValue(value + delta, lo, hi);
    } else {
        values[channel - 1] = next;
    }
}

/** A loaded value: in [lo, hi], and fallback when it isn't a number at all (inf, nan) */
double loadedValue(double value, double lo, double hi, double fallback) {
    return std::isfinite(value) ? clampValue(value, lo, hi) : fallback;
}

std::filesystem::path envDir(const char* variable, const char* homeRelative) {
    const char* value = std::getenv(variable);
    if (value != nullptr && value[0] != '\0') return value;
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/tmp") / homeRelative;
}

}  // namespace

ColorPaths ColorPaths::fromEnvironment() {
    ColorPaths paths;
    paths.configDir = envDir("XDG_CONFIG_HOME", ".config") / "framecolor";
    paths.layerManifest = envDir("XDG_DATA_HOME", ".local/share") / "framecolor/layer/vkBasalt.json";
    paths.layerLibrary = envDir("XDG_DATA_HOME", ".local/share") / "framecolor/lib/libvkbasalt.so";
    return paths;
}

void sanitizeValues(Values& v, bool withPanel) {
    // The sliders' ranges (sliderRange): a hand-edited or damaged file can't put the look or SteamVR's panel
    // brightness out of what the page can show and set back
    if (withPanel) v.panel = loadedValue(v.panel, 0.40, 1.25, 1.0);
    v.red = loadedValue(v.red, 0.50, 1.50, 1.0);
    v.green = loadedValue(v.green, 0.50, 1.50, 1.0);
    v.blue = loadedValue(v.blue, 0.50, 1.50, 1.0);
    for (double& c : v.channelContrast) c = loadedValue(c, 0.0, 1.0, 0.0);
    for (double& g : v.gamma) g = loadedValue(g, 0.50, 2.50, 1.0);
    for (double& s : v.saturation) s = loadedValue(s, 0.0, 2.0, 1.0);
    v.sharpen = loadedValue(v.sharpen, 0.0, kSharpenMax, 0.0);
    v.contrast = loadedValue(v.contrast, 0.0, 1.0, 0.0);
}

ColorModel::ColorModel(ColorHost* host, ColorPaths paths) : host_(host), paths_(std::move(paths)) {}

void ColorModel::start() {
    readValues(state.values);
    loadAdvancedValues(state.values);
    loadCustomPresets();
    state.sharpenWhole = loadSharpenWhole();
    state.sharpenGaze = !state.sharpenWhole && loadSharpenGaze();
    state.autoRules = loadAutoRules(paths_.autoRules());
    std::error_code ec;  // an unreadable folder on the way is "not enabled", not an exception out of the main loop
    state.vulkanLayerActive = std::filesystem::exists(paths_.layerManifest, ec) && !ec;
    setSetting("contrast", 0.0);
    neutralizeLegacyStreamGains();
    // Clear a compare bypass left behind if the app stopped while it was held.
    if (state.vulkanLayerActive) saveAdvancedValues();
    // The writes above leave "Applied" behind; only an error is worth showing when the page first opens
    if (!state.messageIsError) state.message.clear();
    lastLookSave_ = nextContextCheck_ = Clock::now();
    started_ = true;
}

void ColorModel::shutdown() {
    if (!started_) return;
    // Also when an earlier write failed (fileDirty_): it may have left the compare bypass on in the file
    if (state.compare || state.autoOriginal || fileDirty_) {
        state.compare = state.autoCompare = state.autoOriginal = false;
        saveAdvancedValues();
    }
}

void ColorModel::readValues(Values& values) {
    values.panel = host_ != nullptr ? host_->getSetting("analogGain", 1.0) : values.panel;
}

bool ColorModel::setSetting(const char* key, double value) {
    const std::string error = host_ != nullptr ? host_->setSetting(key, value) : std::string("no SteamVR");
    if (!error.empty()) {
        state.message = std::string(tr().errorSaveSettings) + error;
        state.messageIsError = true;
        return false;
    }
    state.message = tr().applied;
    state.messageIsError = false;
    return true;
}

void ColorModel::neutralizeLegacyStreamGains() {
    // Earlier releases wrote these stream-only gains. Leave them neutral so
    // that existing settings cannot double-apply the Vulkan LUT correction.
    for (const char* key : {"hmdDisplayColorGainR", "hmdDisplayColorGainG", "hmdDisplayColorGainB"}) {
        setSetting(key, 1.0);
    }
}

void ColorModel::loadAdvancedValues(Values& values) const {
    std::ifstream in(paths_.current());
    std::vector<double> stored;
    for (double value; in >> value;) stored.push_back(value);
    if (stored.size() >= 6) {
        std::copy_n(stored.begin(), 3, values.channelContrast.begin());
        std::copy_n(stored.begin() + 3, 3, values.gamma.begin());
        if (stored.size() >= 9) {
            values.red = stored[6];
            values.green = stored[7];
            values.blue = stored[8];
        }
        if (stored.size() >= 12) {
            for (int channel = 0; channel < 3; ++channel)
                values.saturation[channel] = clampValue(stored[9 + channel], 0.0, 2.0);
        } else if (stored.size() >= 10) {
            values.saturation.fill(clampValue(stored[9], 0.0, 2.0));
        }
        // The 16th value is the user's sharpness; the 13th is what the layer shows (0 while whole-image sharpening is paused
        // for PC streaming, see saveAdvancedValues). Files without it hold the user's value in the 13th.
        if (stored.size() >= 16) values.sharpen = clampValue(stored[15], 0.0, kSharpenMax);
        else if (stored.size() >= 13) values.sharpen = clampValue(stored[12], 0.0, kSharpenMax);
    } else {
        values.channelContrast = {values.contrast, values.contrast, values.contrast};
        values.gamma = {1.0, 1.0, 1.0};
    }
    sanitizeValues(values, false);
    values.contrast = average(values.channelContrast);
}

// The 15th value of current.txt: 1 sharpens the whole image, 2 the area around where the eyes look (gaze-following,
// 2026-11-08), 0 (or missing) the centre of each eye.
bool ColorModel::loadSharpenWhole() const {
    std::ifstream in(paths_.current());
    std::vector<double> stored;
    for (double value; in >> value;) stored.push_back(value);
    return stored.size() >= 15 && stored[14] > 0.5 && stored[14] < 1.5;
}

bool ColorModel::loadSharpenGaze() const {
    std::ifstream in(paths_.current());
    std::vector<double> stored;
    for (double value; in >> value;) stored.push_back(value);
    return stored.size() >= 15 && stored[14] > 1.5 && stored[14] < 2.5;
}

bool ColorModel::saveAdvancedValues() {
    const auto path = paths_.current();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.string() + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) {
        state.message = tr().errorSaveAdvanced;
        state.messageIsError = true;
        fileDirty_ = true;
        return false;
    }
    const Values& v = state.values;
    out << std::setprecision(9)
        << v.channelContrast[0] << ' ' << v.channelContrast[1] << ' ' << v.channelContrast[2] << ' '
        << v.gamma[0] << ' ' << v.gamma[1] << ' ' << v.gamma[2] << ' '
        << v.red << ' ' << v.green << ' ' << v.blue << ' '
        << v.saturation[0] << ' ' << v.saturation[1] << ' ' << v.saturation[2] << ' '
        << (sharpenPausedForStreaming() ? 0.0 : v.sharpen) << ' '
        << (state.compare || state.autoOriginal ? 1 : 0) << ' ' << (state.sharpenWhole ? 1 : state.sharpenGaze ? 2 : 0) << ' '
        << v.sharpen
        << '\n';
    out.close();
    if (out) std::filesystem::rename(temporary, path, ec);
    if (!out || ec) {
        std::filesystem::remove(temporary, ec);
        state.message = tr().errorSaveAdvanced;
        state.messageIsError = true;
        fileDirty_ = true;  // tick() tries again (with the backoff): the file may say "compare" still
        return false;
    }
    fileDirty_ = false;
    saveBackoff_ = Clock::duration::zero();
    saveRetryAt_ = Clock::time_point{};
    return true;
}

bool ColorModel::tickAutoCompare(Clock::time_point now) {
    if (!state.autoCompare) return false;
    if (now >= state.autoCompareEnd) {
        state.autoCompare = state.autoOriginal = false;
        state.message = tr().autoCompareEnded;
        state.messageIsError = false;
        return true;
    }
    if (now < state.autoCompareTurnEnd) return false;
    state.autoOriginal = !state.autoOriginal;
    // From now, not from the missed deadline: after a stall this switches once, not in a burst.
    state.autoCompareTurnEnd = now + kAutoCompareTurn;
    return true;
}

void ColorModel::loadCustomPresets() {
    std::ifstream in(paths_.presets());
    for (Preset& preset : state.custom) {
        std::string line;
        if (!std::getline(in, line)) break;
        std::istringstream row(line);
        int valid = 0;
        Values& v = preset.values;
        if (!(row >> valid >> v.panel >> v.contrast >> v.red >> v.green >> v.blue)) break;
        if (!(row >> v.channelContrast[0] >> v.channelContrast[1] >> v.channelContrast[2] >> v.gamma[0] >> v.gamma[1] >>
              v.gamma[2])) {
            v.channelContrast = {v.contrast, v.contrast, v.contrast};
            v.gamma = {1.0, 1.0, 1.0};
        }
        std::vector<double> storedSaturation;
        for (double value; row >> value;) storedSaturation.push_back(value);
        if (storedSaturation.size() >= 3) {
            for (int channel = 0; channel < 3; ++channel)
                v.saturation[channel] = clampValue(storedSaturation[channel], 0.0, 2.0);
        } else if (!storedSaturation.empty()) {
            v.saturation.fill(clampValue(storedSaturation[0], 0.0, 2.0));
        }
        if (storedSaturation.size() >= 4) v.sharpen = clampValue(storedSaturation[3], 0.0, kSharpenMax);
        sanitizeValues(v, true);
        preset.valid = valid != 0;
    }
}

bool ColorModel::saveCustomPresets() {
    std::ostringstream out;
    out << std::setprecision(9);
    for (const Preset& preset : state.custom) {
        const Values& v = preset.values;
        out << (preset.valid ? 1 : 0) << ' ' << v.panel << ' ' << v.contrast << ' '
            << v.red << ' ' << v.green << ' ' << v.blue << ' '
            << v.channelContrast[0] << ' ' << v.channelContrast[1] << ' ' << v.channelContrast[2] << ' '
            << v.gamma[0] << ' ' << v.gamma[1] << ' ' << v.gamma[2] << ' '
            << v.saturation[0] << ' ' << v.saturation[1] << ' ' << v.saturation[2] << ' '
            << v.sharpen << '\n';
    }
    // Written rarely (a slot saved): fsync'd, so a power loss right after doesn't leave an empty presets.txt
    std::string error;
    if (!frame_perf_overlay::atomicWriteFile(paths_.presets().string(), out.str(), error)) {
        std::fprintf(stderr, "[color] Couldn't save the presets: %s\n", error.c_str());
        state.message = tr().errorSavePreset;
        state.messageIsError = true;
        return false;
    }
    return true;
}

void ColorModel::applyPreset(const Preset& preset) {
    if (!preset.valid) {
        state.message = tr().slotEmpty;
        state.messageIsError = true;
        return;
    }
    state.values = preset.values;
    setSetting("analogGain", state.values.panel);
    state.lookDirty = true;
    saveAdvancedValues();
    readValues(state.values);
    if (!state.messageIsError) state.message.clear();
}

std::string ColorModel::presetName(int index) const {
    if (index < 0) return tr().autoOff;
    if (index < 3) return tr().builtinPresets[index];
    return "P" + std::to_string(index - 2);
}

const Preset& ColorModel::presetAt(int index) const {
    return index < 3 ? kBuiltins[index] : state.custom[index - 3];
}

bool ColorModel::presetInUse(int index) const {
    if (index < 0 || index > 5) return false;
    const Preset& preset = presetAt(index);
    return preset.valid && sameValues(preset.values, state.values);
}

int ColorModel::governingRule() const {
    if (!state.context.appKey.empty() && state.autoRules.apps.count(state.context.appKey)) return 2;
    return state.context.streaming ? 1 : 0;
}

void ColorModel::applyAutoPreset() {
    const int index = resolveAutoPreset(state.autoRules, state.context);
    if (index < 0) {
        if (!state.beforeAuto) return;
        applyPreset({*state.beforeAuto, true});
        state.beforeAuto.reset();
        if (!state.messageIsError) state.message = tr().autoRestored;
        return;
    }
    if (!presetAt(index).valid) {
        state.message = std::string(tr().autoApplied) + presetName(index) + tr().emptySlotSuffix;
        state.messageIsError = true;
        return;
    }
    if (!state.beforeAuto) state.beforeAuto = state.values;
    applyPreset(presetAt(index));
    if (state.messageIsError) return;
    state.message = std::string(tr().autoApplied) + presetName(index);
}

int ColorModel::ruleValue(int rule) const {
    if (rule == 0) return state.autoRules.standalone;
    if (rule == 1) return state.autoRules.streaming;
    const auto it = state.autoRules.apps.find(state.context.appKey);
    return it == state.autoRules.apps.end() ? -1 : it->second;
}

void ColorModel::cycleRule(int rule) {
    const bool valid[3] = {state.custom[0].valid, state.custom[1].valid, state.custom[2].valid};
    const int next = nextPresetChoice(ruleValue(rule), valid);
    const AutoRules previous = state.autoRules;
    if (rule == 0) state.autoRules.standalone = next;
    else if (rule == 1) state.autoRules.streaming = next;
    else if (next < 0) state.autoRules.apps.erase(state.context.appKey);
    else state.autoRules.apps[state.context.appKey] = next;
    // Do not apply now: stepping through the choices would replace the
    // current look several times and drop unsaved edits.
    if (!saveAutoRules(paths_.autoRules(), state.autoRules)) {
        state.autoRules = previous;
        state.message = tr().errorSavePreset;
        state.messageIsError = true;
        return;
    }
    state.message = tr().ruleSaved;
    state.messageIsError = false;
}

double ColorModel::selectedGain() const {
    if (state.channel == 1) return state.values.red;
    if (state.channel == 2) return state.values.green;
    if (state.channel == 3) return state.values.blue;
    return (state.values.red + state.values.green + state.values.blue) / 3.0;
}

double ColorModel::sliderValue(int index) const {
    switch (index) {
        case kPanelSlider: return state.values.panel;
        case kGainSlider: return selectedGain();
        case kContrastSlider: return selectedArrayValue(state.values.channelContrast, state.channel);
        case kGammaSlider: return selectedArrayValue(state.values.gamma, state.channel);
        case kSaturationSlider: return selectedArrayValue(state.values.saturation, state.channel);
        case kSharpnessSlider: return state.values.sharpen;
        default: return 0.0;
    }
}

void ColorModel::sliderRange(int index, double& lo, double& hi) {
    switch (index) {
        case kPanelSlider: lo = 0.40; hi = 1.25; return;
        case kGainSlider: lo = 0.50; hi = 1.50; return;
        case kGammaSlider: lo = 0.50; hi = 2.50; return;
        case kSaturationSlider: lo = 0.0; hi = 2.0; return;
        case kSharpnessSlider: lo = 0.0; hi = kSharpenMax; return;
        default: lo = 0.0; hi = 1.0; return;
    }
}

void ColorModel::updateSlider(int index, double fraction) {
    fraction = clampValue(fraction, 0.0, 1.0);
    if (index == kPanelSlider) {
        state.values.panel = std::round((0.40 + fraction * 0.85) * 100.0) / 100.0;
        setSetting("analogGain", state.values.panel);
    } else if (index == kGainSlider) {
        const double next = std::round((0.50 + fraction) * 100.0) / 100.0;
        if (state.channel == 0) {
            const double delta = next - selectedGain();
            state.values.red = clampValue(state.values.red + delta, 0.50, 1.50);
            state.values.green = clampValue(state.values.green + delta, 0.50, 1.50);
            state.values.blue = clampValue(state.values.blue + delta, 0.50, 1.50);
        } else {
            double* value = state.channel == 1 ? &state.values.red
                            : (state.channel == 2 ? &state.values.green : &state.values.blue);
            *value = next;
        }
        state.lookDirty = true;
    } else if (index == kContrastSlider) {
        const double next = std::round(fraction * 100.0) / 100.0;
        setSelectedArrayValue(state.values.channelContrast, state.channel, next, 0.0, 1.0);
        state.lookDirty = true;
    } else if (index == kGammaSlider) {
        const double next = std::round((0.50 + fraction * 2.0) * 100.0) / 100.0;
        setSelectedArrayValue(state.values.gamma, state.channel, next, 0.50, 2.50);
        state.lookDirty = true;
    } else if (index == kSaturationSlider) {
        const double next = std::round(fraction * 200.0) / 100.0;
        setSelectedArrayValue(state.values.saturation, state.channel, next, 0.0, 2.0);
        state.lookDirty = true;
    } else if (index == kSharpnessSlider) {
        // Sharpening applies to all channels, like panel brightness. Steps of 2%
        state.values.sharpen = std::round(fraction * kSharpenMax * 50.0) * 2.0 / 100.0;
        state.lookDirty = true;
    }
}

void ColorModel::selectChannel(int channel) {
    state.channel = std::clamp(channel, 0, 3);
    state.resetConfirm = false;
    state.message.clear();
}

void ColorModel::toggleSaveMode() {
    state.saveMode = !state.saveMode;
    state.message = state.saveMode ? tr().promptSaveSlot : tr().saveCanceled;
    state.messageIsError = false;
    state.resetConfirm = false;
}

void ColorModel::pressPreset(int index) {
    state.resetConfirm = false;
    if (state.saveMode) {
        if (index >= 3) {
            state.custom[index - 3] = {state.values, true};
            if (saveCustomPresets()) {
                state.message = tr().formatSavedToSlot(index - 2);
                state.messageIsError = false;
            }
            state.saveMode = false;
        }
    } else {
        const Preset& preset = index < 3 ? kBuiltins[index] : state.custom[index - 3];
        applyPreset(preset);
        // "비비드 적용했어요": which one, not just that one was
        if (preset.valid && !state.messageIsError) state.message = tr().formatPresetApplied(presetName(index));
    }
}

void ColorModel::pressCompare() {
    state.compare = true;
    compareSeenAt_ = Clock::now();
    compareCheckAt_ = compareSeenAt_ + kCompareIdleStop;
    // Read at the press, while the trigger is surely down: if it reads pressed now, its "released" later is believed
    triggerReadable_ = triggerProbe_ && triggerProbe_() == TriggerState::Pressed;
    state.resetConfirm = false;
    saveAdvancedValues();
}

void ColorModel::toggleAutoCompare(Clock::time_point now) {
    state.resetConfirm = false;
    state.saveMode = false;
    state.autoCompare = !state.autoCompare;
    // Start with the original image so the first switch shows up at once.
    state.autoOriginal = state.autoCompare;
    state.autoCompareTurnEnd = now + kAutoCompareTurn;
    state.autoCompareEnd = now + kAutoCompareLength;
    state.message = state.autoCompare ? tr().autoCompareOn : tr().autoCompareOff;
    state.messageIsError = false;
    saveAdvancedValues();
}

void ColorModel::toggleSharpenArea() {
    const bool wasPaused = sharpenPausedForStreaming();
    state.resetConfirm = false;
    state.saveMode = false;
    // Centre -> follow gaze -> whole image -> centre
    if (state.sharpenWhole) {
        state.sharpenWhole = false;
        state.sharpenGaze = false;
    } else if (state.sharpenGaze) {
        state.sharpenGaze = false;
        state.sharpenWhole = true;
    } else {
        state.sharpenGaze = true;
    }
    if (wasPaused != sharpenPausedForStreaming())
        std::fprintf(stderr, "[color] sharpening %s for PC streaming (area changed)\n",
                     sharpenPausedForStreaming() ? "paused" : "resumed");
    saveAdvancedValues();
}

void ColorModel::pressRule(int rule) {
    state.resetConfirm = false;
    state.saveMode = false;
    cycleRule(rule);
}

void ColorModel::pressReset() {
    if (!state.resetConfirm) {
        state.resetConfirm = true;
        state.saveMode = false;
    } else {
        applyPreset(kBuiltins[0]);
        state.resetConfirm = false;
        state.saveMode = false;
    }
}

void ColorModel::release() {
    state.activeSlider = -1;
    state.compare = false;
    readValues(state.values);
    if (saveAdvancedValues()) state.lookDirty = false;
}

void ColorModel::leave() {
    state.activeSlider = -1;
    if (state.compare) {
        state.compare = false;
        saveAdvancedValues();
    }
}

bool ColorModel::tick(Clock::time_point now) {
    if (!started_) return false;
    bool redraw = false;
    // A look to write, or a write that failed anywhere (release, leave, a preset): tried again with the backoff, so
    // a compare bypass the failed write left in the file doesn't stay on
    if (((state.vulkanLayerActive && state.lookDirty) || fileDirty_) && now >= saveRetryAt_ &&
        (state.activeSlider < 0 || now - lastLookSave_ >= kLookSaveInterval)) {
        if (saveAdvancedValues()) {
            state.lookDirty = false;
            lastLookSave_ = now;
        } else {
            // A full disk or a read-only folder doesn't go away in 20 ms: retry after 1 s, then less and less often
            saveBackoff_ = saveBackoff_ == Clock::duration::zero()
                               ? Clock::duration(kSaveRetryFirst)
                               : std::min<Clock::duration>(saveBackoff_ * 2, kSaveRetryMax);
            saveRetryAt_ = now + saveBackoff_;
            redraw = true;  // the error message
        }
    }
    // A compare held with no pointer event for kCompareIdleStop: a hand held still, or its release was missed (the
    // laser's up event lost). The trigger decides: released lets go, pressed keeps it; unreadable (or never seen
    // pressed in this hold) lets go only kCompareUnreadableStop after the last pointer event
    if (state.compare && now - compareSeenAt_ >= kCompareIdleStop && now >= compareCheckAt_) {
        const TriggerState trigger = triggerProbe_ ? triggerProbe_() : TriggerState::Unreadable;
        if (trigger == TriggerState::Pressed) triggerReadable_ = true;
        const auto quietSec = std::chrono::duration_cast<std::chrono::seconds>(now - compareSeenAt_).count();
        if (triggerReadable_ && trigger == TriggerState::Released) {
            std::fprintf(stderr, "[color] compare held with no pointer event for %lld s, the trigger reads released: "
                                 "let go\n", static_cast<long long>(quietSec));
            leave();
            redraw = true;
        } else if (trigger != TriggerState::Pressed && now - compareSeenAt_ >= kCompareUnreadableStop) {
            std::fprintf(stderr, "[color] compare held with no pointer event for %lld s and the trigger can't be read: "
                                 "let go\n", static_cast<long long>(quietSec));
            leave();
            redraw = true;
        } else {
            compareCheckAt_ = now + kCompareTriggerPoll;
        }
    }
    // Auto compare keeps switching while the dashboard is hidden, which is the point of it.
    if (tickAutoCompare(now)) {
        saveAdvancedValues();
        redraw = true;
    }
    // Follow the display mode and the scene app once a second. Wait while the
    // user drags a slider, holds compare, runs auto compare or picks a save slot,
    // so an auto preset never fights them. A new context must be seen twice in a
    // row, so a short gap while an app starts does not flip presets.
    // Nothing to follow (no rule, the page not shown): no check at all, not even the scene app's key (one IPC)
    if (!needsContext()) nextContextCheck_ = std::max(nextContextCheck_, now + kContextInterval);
    if (now >= nextContextCheck_ && state.activeSlider < 0 && !state.compare && !state.autoCompare &&
        !state.saveMode && host_ != nullptr) {
        nextContextCheck_ = now + kContextInterval;
        const bool wasPaused = sharpenPausedForStreaming();
        const bool withStreaming = needsStreaming();
        DisplayContext context = host_->context(withStreaming);
        const bool modeRules = state.autoRules.standalone >= 0 || state.autoRules.streaming >= 0;
        if (!withStreaming) {
            // Not looked at: nothing depends on it now, so it counts as unchanged
            context.streaming = state.contextKnown ? state.context.streaming : false;
        } else if (!modeRules && state.contextKnown && context.appKey == state.context.appKey &&
                   context.streaming != state.context.streaming) {
            // Only the "Now:" line shows the mode when no rule uses it: no preset to apply, no need to see it twice
            state.context.streaming = context.streaming;
            redraw = true;
        }
        const bool anyRule = modeRules || !state.autoRules.apps.empty() || state.beforeAuto.has_value();
        if (!anyRule && !(state.contextKnown && context == state.context)) {
            // No rule could apply: only the "Now:" line shows it, no need to see it twice
            state.pendingCount = 0;
            if (!state.contextKnown || context.appKey != state.context.appKey) state.appName = host_->appName(context.appKey);
            state.context = context;
            state.contextKnown = true;
            redraw = true;
        } else if (state.contextKnown && context == state.context) {
            state.pendingCount = 0;
        } else {
            state.pendingCount = state.pendingCount > 0 && context == state.pendingContext ? state.pendingCount + 1 : 1;
            state.pendingContext = context;
            if (state.pendingCount >= 2) {
                state.pendingCount = 0;
                state.context = context;
                state.contextKnown = true;
                state.appName = host_->appName(context.appKey);
                // Visible with journalctl --user -u framedock.service.
                std::fprintf(stderr, "[color] context: %s, app key \"%s\" (%s)\n",
                             context.streaming ? "PC streaming" : "standalone", context.appKey.c_str(),
                             state.appName.c_str());
                applyAutoPreset();
                redraw = true;
            }
        }
        // PC streaming came or went: the layer's sharpness follows (current.txt's 13th value)
        if (wasPaused != sharpenPausedForStreaming()) {
            std::fprintf(stderr, "[color] sharpening %s for PC streaming\n",
                         sharpenPausedForStreaming() ? "paused" : "resumed");
            saveAdvancedValues();
            redraw = true;
        }
    }
    return redraw;
}

bool ColorModel::gazeFollowActive() const {
    // The worker reads the gaze (and keeps the eye tracker at 90 Hz) only while it changes what is on screen
    // Not dropped while compare shows the original: auto compare would toggle the tracker rate every 3 s
    return state.vulkanLayerActive && state.sharpenGaze && !state.sharpenWhole && state.values.sharpen > 0.0;
}

bool ColorModel::sharpenPausedForStreaming() const {
    // The layer's sharpening pass costs the headset's compositor time it doesn't budget for. Over the whole image it
    // made it miss frames while compositing a PC stream (2026-11-08, Titanfall 2 over Steam Link: 25-31%), so that
    // area pauses during a stream; the centre area (the layer's smaller default since then) stays on (1-3%, as with
    // sharpening off). The colour always stays; the user's sharpness comes back after the stream.
    return state.vulkanLayerActive && state.sharpenWhole && state.contextKnown && state.context.streaming &&
           state.values.sharpen > 0.0;
}

Clock::time_point ColorModel::nextWake(Clock::time_point now) const {
    if (!started_) return Clock::time_point::max();
    // The context check waits while the user drags, compares, runs auto compare or picks a save slot; those end
    // with a pointer event (which wakes the caller anyway) or with auto compare's own deadline
    const bool contextWaits =
        state.activeSlider >= 0 || state.compare || state.autoCompare || state.saveMode || host_ == nullptr;
    // No rule and the page not shown: no context check to wake for (setPageVisible brings it back at once)
    Clock::time_point next = !needsContext() ? Clock::time_point::max()
                             : contextWaits  ? now + kContextInterval
                                             : nextContextCheck_;
    if (state.autoCompare) next = std::min({next, state.autoCompareTurnEnd, state.autoCompareEnd});
    if (state.compare) next = std::min(next, std::max(compareSeenAt_ + kCompareIdleStop, compareCheckAt_));
    if ((state.vulkanLayerActive && state.lookDirty) || fileDirty_)
        next = std::min(next, std::max(lastLookSave_ + kLookSaveInterval, saveRetryAt_));
    if (next == Clock::time_point::max()) return next;
    // Never sooner than kMinWake: a deadline already passed (a failed write retried, say) must not spin the loop
    return std::max(next, now + kMinWake);
}

void ColorModel::setPageVisible(bool visible, Clock::time_point now) {
    if (visible && !pageVisible_ && started_) nextContextCheck_ = std::min(nextContextCheck_, now);
    // A save slot left half picked ends with the tab: it would hold the context check (and with it the streaming
    // sharpness pause) while nothing on screen can finish it
    if (!visible) state.saveMode = false;
    pageVisible_ = visible;
}

bool ColorModel::needsContext() const {
    return needsStreaming() || !state.autoRules.apps.empty() || state.beforeAuto.has_value();
}

bool ColorModel::needsStreaming() const {
    // Whole-image sharpening on with the layer: PC streaming pauses it (sharpenPausedForStreaming), so the mode is followed
    return pageVisible_ || state.autoRules.standalone >= 0 || state.autoRules.streaming >= 0 ||
           (state.vulkanLayerActive && state.sharpenWhole && state.values.sharpen > 0.0);
}

namespace {

/** @return true if /proc/<pid>/comm says vrcompositor */
bool isCompositor(const std::filesystem::path& procRoot, const std::string& pid) {
    std::ifstream comm(procRoot / pid / "comm");
    std::string name;
    return std::getline(comm, name) && name == "vrcompositor";
}

}  // namespace

bool mapsLibraryDeleted(std::istream& maps, const std::string& library) {
    if (library.empty()) return false;
    // "<range> <perms> <offset> <dev> <inode>   /path (deleted)": the path is last, after spaces
    const std::string tail = " " + library + " (deleted)";
    for (std::string line; std::getline(maps, line);) {
        if (line.size() >= tail.size() && line.compare(line.size() - tail.size(), tail.size(), tail) == 0) return true;
    }
    return false;
}

bool ColorModel::checkLayerReload(Clock::time_point now) {
    if (now < layerReloadCheckAt_) return state.layerReloadNeeded;
    layerReloadCheckAt_ = now + kLayerReloadInterval;
    state.layerReloadNeeded = false;
    if (!state.vulkanLayerActive || paths_.layerLibrary.empty()) return false;
    // The vrcompositor found last (one small read: its PID could be reused by something else), else a scan of /proc
    if (compositorPid_ > 0 && !isCompositor(paths_.procRoot, std::to_string(compositorPid_))) compositorPid_ = 0;
    if (compositorPid_ == 0) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(paths_.procRoot, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] < '0' || name[0] > '9') continue;
            if (isCompositor(paths_.procRoot, name)) {
                compositorPid_ = std::strtol(name.c_str(), nullptr, 10);
                break;
            }
        }
    }
    if (compositorPid_ == 0) return false;
    std::ifstream maps(paths_.procRoot / std::to_string(compositorPid_) / "maps");
    state.layerReloadNeeded = mapsLibraryDeleted(maps, paths_.layerLibrary.string());
    return state.layerReloadNeeded;
}

}  // namespace framecolor
