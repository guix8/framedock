// The Dock panel's pictures and hit areas: the quick view (the battery row, profile quick switch, colour presets,
// compare, the 소리 · 패스스루 card (output, volume, passthrough: a DevicePage on the shared DeviceModel), 브라우저 (the
// browser panel), 전체 and 접기) and the full view (a bar with 빠른 / 완료 / 접기 over a second
// SettingsPanel's image: the dashboard tab's pages, usable in a game). Both have a handle bar on top: holding the
// trigger on it carries the panel (DockController::startGrab).
//
// With framecorder installed the bottom row has 녹화 too (between 브라우저 and 전체): a tap starts or stops a recording,
// which then shows its time ("● 0:42"); a recorder that ended by itself, or another one running, is said on the note
// line. While framecorder's clips are on and nothing is being recorded, that part is split: 녹화 | 클립 (save the last
// seconds; "saved" or framecorder's no on the note line).
//
// While Chrome runs, 브라우저 shares its third with 끄기 (Chrome 끄기, as the Browser page's): the first tap arms it
// (a red ring, "한 번 더 누르면 Chrome을 꺼요" on the note line), a second one within SettingsPanel::kQuitConfirmSec asks
// the main loop to close Chrome.
//
// The quick switch follows the footer's rules (SettingsPanel): one tap uses the profile; when that would throw away
// unsaved changes (ProfilesUiState::Item::useLoses) the first tap arms it and says so, and a second tap on the same
// button, still standing for the same profile, within SettingsPanel::kQuitConfirmSec uses it. The colour buttons work
// the shared ColorModel directly, like the Color page does.
#pragma once

#include "device_page.h"
#include "profiles_ui.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

class FontSet;
class SettingsPanel;
struct Pen;
namespace framecolor {
class ColorModel;
}
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** The quick view's battery row: the headset and both controllers (NaN %: not read). */
struct DockBattery {
    struct Hand {
        bool present = false;
        double pct = std::numeric_limits<double>::quiet_NaN();
        bool charging = false;
        bool operator==(const Hand& o) const {
            return present == o.present && charging == o.charging &&
                   (pct == o.pct || (std::isnan(pct) && std::isnan(o.pct)));
        }
    };
    double headsetPct = std::numeric_limits<double>::quiet_NaN();
    bool headsetCharging = false;  ///< charging (green)
    std::string headsetState;      ///< not on battery: "충전 중", "충전 완료", "전원 연결" (instead of the time left)
    std::string minutesText;       ///< "약 40분" ("" = unknown)
    Hand left, right;
    double warnPct = 30, critPct = 15;                     ///< the headset's colours (Perf's battery thresholds)
    double controllerWarnPct = 20, controllerCritPct = 10;  ///< the controllers' colours
    bool operator==(const DockBattery& o) const {
        return (headsetPct == o.headsetPct || (std::isnan(headsetPct) && std::isnan(o.headsetPct))) &&
               headsetCharging == o.headsetCharging && headsetState == o.headsetState && minutesText == o.minutesText && left == o.left &&
               right == o.right && warnPct == o.warnPct && critPct == o.critPct &&
               controllerWarnPct == o.controllerWarnPct && controllerCritPct == o.controllerCritPct;
    }
    bool operator!=(const DockBattery& o) const { return !(*this == o); }
};

/** The 녹화 button: framecorder's recorder (framecorder.h). */
struct DockRecord {
    bool available = false;  ///< framecorder is installed (else no button)
    int state = 0;           ///< ControlsView::recordState's numbers (0 idle, 1 recording, 2 saving, 3 failed, 4 busy,
                             ///< 5 clip saved, 6 refused)
    int seconds = 0;         ///< while recording
    bool clips = false;      ///< framecorder's clips are on: 녹화 | 클립
    bool clipUsable = false;
    bool operator==(const DockRecord& o) const {
        return available == o.available && state == o.state && seconds == o.seconds && clips == o.clips &&
               clipUsable == o.clipUsable;
    }
};

/** What a press on the Dock panel asks the main loop to do. */
struct DockPress {
    enum class Kind {
        None,          ///< nothing for the caller (handled inside, missed, or the first tap of an armed switch)
        UseProfile,    ///< use profileId (the quick switch)
        ShowFull,      ///< 전체
        ShowQuick,     ///< ← 빠른 (full view)
        Collapse,      ///< 접기
        Done,          ///< 완료: end the fine-adjust session
        GrabHandle,    ///< the handle bar: carry the panel while the trigger is held
        ColorChanged,  ///< a colour button acted on the model (redraw)
        OpenBrowser,   ///< 브라우저: the browser panel
        QuitChrome,    ///< 끄기 tapped twice: close Chrome
        DeviceChanged, ///< the 소리 · 패스스루 card acted on the DeviceModel (redraw; its actions are queued there)
        ToggleRecord,  ///< 녹화: start or stop the recording
        SaveClip,      ///< 클립: framecorder saves the last seconds
    };
    Kind kind = Kind::None;
    std::string profileId;
};

class DockPanel {
public:
    static constexpr int kQuickWidth = 1000;  ///< quick view texture (px)
    static constexpr int kQuickHeight = 1164;
    static constexpr int kBarHeight = 84;     ///< the handle bar on top of both views
    static constexpr size_t kQuickProfiles = 6;

    /**
     * @param fonts outlives this
     * @param color the Color page's model (null: the colour buttons are off)
     */
    DockPanel(const FontSet& fonts, framecolor::ColorModel* color);
    ~DockPanel();
    DockPanel(const DockPanel&) = delete;
    DockPanel& operator=(const DockPanel&) = delete;

    // ---- The quick view ----
    /**
     * Draws the quick view.
     * @param profiles the profiles (the first kQuickProfiles are the quick switch; the active one selected)
     * @param now monotonic seconds (the armed tap's and the status line's expiry)
     */
    void renderQuick(const ProfilesUiState& profiles, double now);
    /** @return true if the hover changed (redraw) */
    bool pointerMove(double x, double y);
    /**
     * A press on the quick view. The colour buttons act on the model at once; compare holds until pointerUp /
     * pointerLeave. The quick switch asks for a second tap when its profile's useLoses says so (setProfiles first).
     */
    DockPress pointerDown(double x, double y, double now);
    /** @return true if a press ended (redraw) */
    bool pointerUp(double x, double y);
    /** The laser left (or the panel collapsed): compare lets go, hover clears. @return true (redraw) if anything did */
    bool pointerLeave();
    /** The battery row's values (drawn on the next renderQuick). @return true if they changed (redraw) */
    bool setBattery(const DockBattery& battery) {
        if (battery == battery_) return false;
        battery_ = battery;
        return true;
    }
    /** Chrome runs (끄기 next to 브라우저). @return true if that changed (redraw) */
    bool setChromeRunning(bool running) {
        if (running == chromeRunning_) return false;
        chromeRunning_ = running;
        if (!running) quitArmedUntil_ = -1;
        return true;
    }
    bool quitArmed() const { return quitArmedUntil_ >= 0; }
    /** The 녹화 button's state. @return true if that changed (redraw) */
    bool setRecord(const DockRecord& record) {
        if (record == record_) return false;
        record_ = record;
        return true;
    }
    /** The profiles' state right now, before pointerDown (a reload may have changed what a tap would lose). */
    void setProfiles(const ProfilesUiState& profiles) { profiles_ = profiles; }
    /** A profile action's result on the bottom line, like the footer (5 s, an error 10 s in red). */
    void showStatus(const std::string& text, bool error, double now);
    /** Expiry of the armed tap and the status; a held − / ＋ of the volume. @return true if a redraw is needed */
    bool tick(double now);
    /** A pointer event came in: a held − / ＋'s safety stop (HoldRepeat::kIdleStopSec) counts from now. */
    void pointerSeen(double now) { devicePage_.pointerSeen(now); }
    /** @return when tick() has something to do (+infinity: nothing) */
    double nextWake() const;
    /** @return true while a quick-switch button waits for its second tap */
    bool armed() const { return armedIndex_ >= 0; }
    /** @return the quick view's bottom line as last drawn (tests) */
    const std::string& note() const { return note_; }
    /** The 소리 · 패스스루 card works on this model (shared with the 기기 pages); until then on a default one. */
    void setDevice(DeviceModel& model) { devicePage_.setModel(model); }
    /** @return the 소리 · 패스스루 card (its hit areas as last drawn; tests) */
    const DevicePage& devicePage() const { return devicePage_; }

    // ---- The full view: the bar over a SettingsPanel ----
    /**
     * Draws the full view: the bar, then the panel's last render under it.
     * @param panel the Dock's own SettingsPanel, rendered already
     * @param adjusting a fine-adjust session runs (the bar shows 완료 and says the panel stays put)
     */
    void renderFull(const SettingsPanel& panel, bool adjusting);
    /** Pointer on the full view's bar (y < kBarHeight). @return true if the hover changed */
    bool barMove(double x, double y);
    /** A press on the bar. */
    DockPress barDown(double x, double y);
    /** @return true if a press on the bar ended */
    bool barUp();

    // ---- Both ----
    /** @return the last view drawn: premultiplied ARGB32 (cairo's), width × height × 4 */
    const uint8_t* pixels() const;
    int width() const;
    int height() const;
    /** @return the last view as non-premultiplied RGBA */
    const std::vector<uint8_t>& toRgba();
    bool writePng(const std::string& path) const;

    /** A spot on the panel that does something, as last drawn. */
    struct Hit {
        enum class Kind {
            Handle, Profile, Preset, Compare, AutoCompare, Browser, QuitChrome, Record, Clip, Full, Quick, Collapse,
            Done
        };
        Kind kind;
        int arg;  ///< the quick-switch index, the preset index
        double x, y, w, h;
        bool usable;
    };
    /** @return the centre of a hit area as last drawn (tests). False if it isn't there or can't be pressed */
    bool hitCenter(Hit::Kind kind, int arg, double& x, double& y) const;

private:
    const FontSet& fonts_;
    framecolor::ColorModel* color_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;
    std::vector<uint8_t> rgba_;
    std::vector<Hit> hits_;
    ProfilesUiState profiles_;
    std::vector<std::string> quickIds_;  ///< the quick switch's profiles as last drawn
    double px_ = -1, py_ = -1;           ///< the pointer (-1 = off)
    Hit::Kind pressedKind_ = Hit::Kind::Handle;  ///< the press (kind and arg: hits_ is rebuilt by each render)
    int pressedArg_ = -1;
    bool pressing_ = false;
    bool colorPressed_ = false;          ///< a colour button was pressed: the model gets release() on the release
    int armedIndex_ = -1;                ///< the quick-switch button waiting for its second tap
    bool chromeRunning_ = false;
    double quitArmedUntil_ = -1;         ///< 끄기 waits for its second tap until then (−1: not armed)
    std::string armedId_;
    double armedUntil_ = 0;
    std::string status_;
    bool statusError_ = false;
    double statusUntil_ = 0;
    std::string note_;
    DeviceModel ownDevice_;              ///< the card's model until setDevice
    DevicePage devicePage_ {ownDevice_};  ///< the 소리 · 패스스루 card (its own pointer state)
    bool quickShown_ = false;            ///< the quick view is the last view drawn (the card takes the pointer)
    DockBattery battery_;                ///< the battery row
    DockRecord record_;                  ///< the 녹화 button

    void ensureSurface(int w, int h);
    void clear();
    const Hit* hitAt(double x, double y) const;
    bool hovered(const Hit& hit) const;
    bool isPressed(Hit::Kind kind, int arg) const { return pressing_ && pressedKind_ == kind && pressedArg_ == arg; }
    void drawButton(const Pen& pen, Hit hit, const std::string& label, bool selected, double size = 26);
    /** The handle bar, width wide; its hit area (pressing it carries the panel) is the first hitWidth px. */
    void drawHandle(const Pen& pen, double width, double hitWidth, const std::string& title, const std::string& hint);
};
