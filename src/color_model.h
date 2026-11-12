// FrameColor's display controls without the drawing: the values, the files the Vulkan layer and the
// dashboard share (current.txt, presets.txt, auto.txt), presets, auto compare and the auto preset rules.
// OpenVR is reached through ColorHost, so all of this runs in tests without SteamVR.
#pragma once

#include "auto_profile.h"
#include "color_i18n.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>

namespace framecolor {

using Clock = std::chrono::steady_clock;

/** What the Color page needs from OpenVR. Frame Dock's is OpenVrColorHost; tests use a fake. */
class ColorHost {
public:
    virtual ~ColorHost() = default;
    /** A float in SteamVR's "steamvr" settings section, or fallback when it can't be read. */
    virtual double getSetting(const char* key, double fallback) = 0;
    /** Sets a float in the "steamvr" section. @return "" on success, otherwise the error's name */
    virtual std::string setSetting(const char* key, double value) = 0;
    /**
     * The display mode and the scene app's OpenVR key ("" when it has none).
     * @param withStreaming false: skip the display mode (a /proc scan), streaming is then false
     */
    virtual DisplayContext context(bool withStreaming) = 0;
    /** The app's display name, or the key when it has none. */
    virtual std::string appName(const std::string& key) = 0;
};

struct Values {
    double panel = 1.0;
    double contrast = 0.0;
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    std::array<double, 3> channelContrast{{0.0, 0.0, 0.0}};
    std::array<double, 3> gamma{{1.0, 1.0, 1.0}};
    std::array<double, 3> saturation{{1.0, 1.0, 1.0}};
    double sharpen = 0.0;
};

struct Preset {
    Values values;
    bool valid = false;
};

/** Standard, Vivid, Night. */
extern const std::array<Preset, 3> kBuiltins;

/**
 * @return true if a and b are the same look and panel brightness (the averaged contrast aside: it follows the
 *         channels). Off by less than half a slider step counts as the same (a file's rounding, SteamVR's float)
 */
bool sameValues(const Values& a, const Values& b);

// Auto compare switches between the original and the corrected image so that it can be
// judged in a game with the dashboard closed. Short turns make small differences easier
// to see and leave the scene less time to change; it stops by itself so the switching
// cannot be left running. The translations mention both durations.
constexpr auto kAutoCompareTurn = std::chrono::seconds(3);
constexpr auto kAutoCompareLength = std::chrono::seconds(60);
/** While a slider is dragged, current.txt is written at most this often. */
constexpr auto kLookSaveInterval = std::chrono::milliseconds(150);
/** The display mode and scene app are checked this often. */
constexpr auto kContextInterval = std::chrono::seconds(1);
/** A failed write of current.txt from tick() is retried after this, then after twice as long each time... */
constexpr auto kSaveRetryFirst = std::chrono::seconds(1);
/** ...up to this. */
constexpr auto kSaveRetryMax = std::chrono::seconds(16);
/**
 * Compare held this long with no pointer event (a still hand sends none; or its release was missed): the pressing
 * hand's trigger is read (ColorModel::setTriggerProbe). Read as released, compare lets go; read as pressed, it stays.
 */
constexpr auto kCompareIdleStop = std::chrono::seconds(10);
/** ...and read again this often while it stays. */
constexpr auto kCompareTriggerPoll = std::chrono::seconds(1);
/** The trigger can't be read (or never read as pressed during this hold): compare lets go this long after the last
 * pointer event, a safety limit for a missed release. */
constexpr auto kCompareUnreadableStop = std::chrono::seconds(60);

/**
 * The trigger of the hand that pressed compare, as read now. Unreadable: no device, no answer, or SteamVR gives this
 * app no controller state (input focus elsewhere, the action system). A reading counts only once it has read Pressed
 * during the hold (an all-zero state from an app without input focus reads "released" forever).
 */
enum class TriggerState { Unreadable, Pressed, Released };
/** nextWake() is never sooner than this after now. */
constexpr auto kMinWake = std::chrono::milliseconds(20);
/** While the colour is drawn, vrcompositor's layer is checked (has it been replaced on disk?) at most this often. */
constexpr auto kLayerReloadInterval = std::chrono::seconds(30);

/** Slider indices (FrameColor's order; current.txt does not depend on it). */
enum Slider { kPanelSlider = 0, kGainSlider = 1, kContrastSlider = 2, kGammaSlider = 3, kSaturationSlider = 4,
              kSharpnessSlider = 5 };

/** Sharpness 0–3 (0–300%): 1 is CAS at its strongest, above that the layer pushes past it (exaggerated on purpose,
 *  the user's choice 2026-10-30). The slider moves in 2% steps. */
constexpr double kSharpenMax = 3.0;

/** Where FrameColor keeps its files. */
struct ColorPaths {
    std::filesystem::path configDir;      ///< ~/.config/framecolor
    std::filesystem::path layerManifest;  ///< ~/.local/share/framecolor/layer/vkBasalt.json (exists = layer on)
    std::filesystem::path layerLibrary;   ///< ~/.local/share/framecolor/lib/libvkbasalt.so ("" = not checked)
    std::filesystem::path procRoot = "/proc";  ///< where vrcompositor is looked for (tests: a fake one)

    std::filesystem::path presets() const { return configDir / "presets.txt"; }
    std::filesystem::path current() const { return configDir / "current.txt"; }
    std::filesystem::path autoRules() const { return configDir / "auto.txt"; }

    /** From XDG_CONFIG_HOME / XDG_DATA_HOME / HOME, like FrameColor. */
    static ColorPaths fromEnvironment();
};

/** Everything the page shows and edits (FrameColor's UiState without the menus). */
struct ColorState {
    Values values;
    std::array<Preset, 3> custom{};
    int channel = 0;       ///< 0=all, 1=red, 2=green, 3=blue
    int activeSlider = -1;
    bool resetConfirm = false;
    Language language = Language::English;
    bool saveMode = false;
    bool lookDirty = false;
    bool vulkanLayerActive = false;
    bool layerReloadNeeded = false;  ///< vrcompositor runs a layer since replaced on disk (checkLayerReload)
    std::string message;
    bool messageIsError = false;
    bool compare = false;          ///< compare button held: the layer shows the original image
    bool autoCompare = false;      ///< auto compare is running
    bool autoOriginal = false;     ///< auto compare is showing the original image now
    Clock::time_point autoCompareTurnEnd{}, autoCompareEnd{};
    bool sharpenWhole = false;     ///< sharpen the whole image instead of the centre of each eye
    bool sharpenGaze = false;      ///< centre the sharpened area where the eyes look (never with sharpenWhole)
                                   ///< (a global setting like the language, not part of presets)
    int gazeFitStep = -1;          ///< the gaze fit's dot being shown (from 0), -1: none running (set by the main loop)
    int gazeFitSteps = 0;
    int gazeFitOutcome = 0;        ///< 0 none, 1 done, 2 failed, 3 stopped (GazeFeed::FitOutcome), shown for a while
    bool gazeFitPressed = false;   ///< the gaze fit button was pressed; the main loop takes it
    bool gazeFitReviewing = false; ///< a fit was just done: its tint stays a few seconds (the page says so)
    int gazeFitReviewLeft = 0;     ///< ... this many seconds more
    int gazeFitFailure = 0;        ///< with outcome 2: why (GazeFeed::FitFailure, 1 no gaze .. 4 out of order)
    AutoRules autoRules;
    DisplayContext context;
    bool contextKnown = false;
    std::string appName;
    DisplayContext pendingContext;  ///< a new context must be seen twice before it is used
    int pendingCount = 0;
    std::optional<Values> beforeAuto;  ///< values to restore when no auto rule applies any more
};

/**
 * FrameColor's dashboard logic. `state` is public on purpose (the page draws it, tests set it up);
 * the functions keep FrameColor's behaviour and file formats.
 */
class ColorModel {
public:
    /** @param host OpenVR, or null offline (nothing is written to SteamVR then) */
    ColorModel(ColorHost* host, ColorPaths paths);

    ColorState state;

    const ColorPaths& paths() const { return paths_; }
    const Translations& tr() const { return getTranslations(state.language); }

    /**
     * After SteamVR is connected: read the panel brightness and the files, neutralise the settings that
     * older releases wrote, and clear a compare bypass left behind by a crash.
     */
    void start();
    /** @return true once start() ran (until then nothing touches SteamVR or the files) */
    bool started() const { return started_; }
    /** On exit: never leave the layer showing the original image. */
    void shutdown();

    // ---- FrameColor's functions ----
    void readValues(Values& values);
    bool setSetting(const char* key, double value);
    void neutralizeLegacyStreamGains();
    void loadAdvancedValues(Values& values) const;
    bool loadSharpenWhole() const;
    bool loadSharpenGaze() const;
    bool saveAdvancedValues();
    void loadCustomPresets();
    bool saveCustomPresets();
    void applyPreset(const Preset& preset);
    std::string presetName(int index) const;
    const Preset& presetAt(int index) const;
    /** @return true if preset `index` (0-5) is valid and holds the values in use now (sameValues) */
    bool presetInUse(int index) const;
    /** Which rule decides the preset now: 0=standalone, 1=streaming, 2=this app. */
    int governingRule() const;
    void applyAutoPreset();
    int ruleValue(int rule) const;
    void cycleRule(int rule);
    /** Advance auto compare to `now`. @return true when current.txt needs to be written */
    bool tickAutoCompare(Clock::time_point now);

    // ---- Slider values ----
    double selectedGain() const;
    /** The value slider `index` shows for the selected channel. */
    double sliderValue(int index) const;
    /** Slider `index`'s range. */
    static void sliderRange(int index, double& lo, double& hi);
    /** Set slider `index` from a track position (0 = left end, 1 = right end), rounded like FrameColor. */
    void updateSlider(int index, double fraction);

    // ---- Button actions (each closes the reset confirmation like FrameColor's) ----
    void selectChannel(int channel);
    void toggleSaveMode();
    void pressPreset(int index);
    void pressCompare();
    void toggleAutoCompare(Clock::time_point now);
    void toggleSharpenArea();
    void pressRule(int rule);
    void pressReset();
    /** A press that hit nothing on the page. */
    void dismiss() { state.resetConfirm = false; }
    /** Pointer released: stop dragging and comparing, re-read the panel brightness, write current.txt. */
    void release();
    /** The pointer left the tab (or it was hidden): stop dragging and comparing. */
    void leave();
    /**
     * A pointer event reached a page that can hold compare (the tab, the Dock panel). kCompareIdleStop after the last
     * one the pressing hand's trigger is read: released (a release SteamVR never sent) lets go, pressed keeps it, and
     * unreadable lets go kCompareUnreadableStop after the last one.
     */
    void pointerSeen(Clock::time_point now) { compareSeenAt_ = now; }
    /** How to read the pressing hand's trigger (none: always Unreadable). Asked at the press and after kCompareIdleStop. */
    void setTriggerProbe(std::function<TriggerState()> probe) { triggerProbe_ = std::move(probe); }

    /**
     * Time-driven work, also while the dashboard is hidden: write current.txt while a slider moves,
     * switch auto compare, follow the display mode and scene app (FrameColor's main loop).
     * @return true if the page needs a redraw
     */
    bool tick(Clock::time_point now);
    /** When tick() next has something to do (at least kMinWake after now; time_point::max() before start()). */
    Clock::time_point nextWake(Clock::time_point now) const;

    /**
     * The dashboard tab was shown or hidden. While it is hidden and no auto preset rule depends on the display mode,
     * tick() leaves out the display mode check (a scan of /proc); when it is shown again, the next tick() checks at
     * once, so the "Now:" line is up to date.
     */
    void setPageVisible(bool visible, Clock::time_point now);
    /**
     * @return true when the context check needs the display mode (the tab is visible, a mode rule is set, or
     *         whole-image sharpening is on with the layer)
     */
    bool needsStreaming() const;
    /**
     * @return true while PC streaming pauses whole-image sharpening (the centre area stays on): current.txt then gives
     *         the layer 0 as the 13th value and
     *         keeps the user's sharpness as the 16th, so the colour stays and the sharpness comes back after the stream
     */
    bool sharpenPausedForStreaming() const;
    /** @return true while the gaze-following area is in use (GazeFeed then publishes the gaze, at 90 Hz) */
    bool gazeFollowActive() const;
    /**
     * @return true when the context check (the scene app, and the display mode per needsStreaming) is needed at all:
     *         the page is shown, a rule is set, or an auto preset is in effect (its rule may still go away)
     */
    bool needsContext() const;
    /** @return true while a write of current.txt failed and tick() is to try again */
    bool fileDirty() const { return fileDirty_; }

    /**
     * Whether SteamVR must restart before a newly installed layer applies: vrcompositor still maps the layer library
     * as "(deleted)" (an install replaced it). Called by what draws the colour, so it runs only while that is shown;
     * reads /proc at most every kLayerReloadInterval (vrcompositor's maps, a few ms) and returns the last answer in
     * between. @return state.layerReloadNeeded
     */
    bool checkLayerReload(Clock::time_point now);

private:
    ColorHost* host_;
    ColorPaths paths_;
    bool started_ = false;
    Clock::time_point lastLookSave_{};
    Clock::time_point nextContextCheck_{};
    bool pageVisible_ = false;
    Clock::duration saveBackoff_{};     ///< zero, or the wait after the last failed write from tick()
    Clock::time_point saveRetryAt_{};   ///< tick() doesn't write current.txt again before this
    bool fileDirty_ = false;            ///< the last write of current.txt failed (any caller): tick() retries it
    Clock::time_point compareSeenAt_{}; ///< compare pressed or the last pointer event since
    Clock::time_point compareCheckAt_{}; ///< the trigger isn't read again before this
    bool triggerReadable_ = false;       ///< the trigger read as pressed during this hold: its "released" counts
    std::function<TriggerState()> triggerProbe_;
    Clock::time_point layerReloadCheckAt_{};  ///< checkLayerReload() doesn't read /proc again before this
    long compositorPid_ = 0;                  ///< vrcompositor as found last (0: look for it)
};

/**
 * @param maps a /proc/<pid>/maps text
 * @return true if it maps `library` as deleted: the file was replaced (or removed) after the process loaded it
 */
bool mapsLibraryDeleted(std::istream& maps, const std::string& library);

/** Brings loaded values into the sliders' ranges (non-numbers to the neutral value); withPanel: the brightness too. */
void sanitizeValues(Values& values, bool withPanel);

/** Clamp, as FrameColor wrote it. */
double clampValue(double value, double lo, double hi);

}  // namespace framecolor
