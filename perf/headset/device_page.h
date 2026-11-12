// The 기기 page of the dashboard tab (also the Dock's full view) and the Dock quick view's 소리 · 패스스루 card:
// drawing and hit areas. DeviceModel holds the state and queues what the controls ask for. The words are app_text's
// (uiLanguage()).
//
// Page: 소리 (the output as a segmented toggle, the volume slider and 음소거) and 패스스루 (끔 | 흑백 | 컬러) on the
// left, PC 계정 맞추기 (Frame Sync: the PC's line, the big button, the question / message line, the other accounts)
// on the right. Without Frame Sync set up (SyncState::configured false: no ~/.config/frame-sync/config.json or key)
// that card isn't there and 소리 and 패스스루 take the whole width. The slider is the Color page's (drawSlider), but a
// press alone doesn't move it (a laser brushing the track must not set the volume): a drag moves it from where it is by the distance dragged. − / ＋ beside it step 5 %
// and repeat while held (HoldRepeat, like the Placement page's buttons).
#pragma once

#include "device_model.h"
#include "hold_repeat.h"

#include <map>
#include <string>
#include <vector>

struct Pen;
typedef struct _cairo_surface cairo_surface_t;

/** What a spot on the page does. */
enum class DeviceHit {
    None,
    Output,       ///< arg: the output's index in AudioState::outputs
    Volume,       ///< the slider (a drag moves it; a press alone doesn't)
    VolumeDown,   ///< − : kVolumeStep down, repeats while held
    VolumeUp,     ///< ＋ : kVolumeStep up, repeats while held
    Mute,
    Passthrough,  ///< arg: PassthroughMode
    SyncBig,      ///< the big button: the Frame's account
    SyncRow,      ///< arg: the row (another account's 선택)
    Shutdown,     ///< the PC's 끄기
    AnswerYes,    ///< 계속 (a game runs) / 끄기 (the PC's 끄기)
    AnswerNo,     ///< 취소
};

/** Where the page may draw (px). */
struct DevicePageArea {
    double left = 0, top = 0, width = 0, height = 0;
};

/**
 * The part of a segmented pill (drawSegmentedPill's parts, n of them) at x: the parts split the inside evenly and
 * the rim belongs to the end parts (pillPartLeft). @return the index, -1 outside the pill
 */
int segmentAt(double x, double pillX, double pillW, size_t n);

/** The − / ＋ step (%). */
constexpr int kVolumeStep = 5;
/** How far (px) the laser must move from the press before the slider follows (a press's jitter moves nothing). */
constexpr double kVolumeDragStartPx = 8;

/**
 * A volume slider drag: the volume when the drag began plus the distance moved since (the track's width is 100 %),
 * rounded and clamped to 0-100. Where on the track it began doesn't matter.
 */
int draggedVolume(int startVolume, double startX, double x, double trackW);

/** The − / ＋ step from a volume: to the next multiple of kVolumeStep down (dir < 0) or up, clamped to 0-100. */
int steppedVolume(int volume, int dir);

class DevicePage {
public:
    explicit DevicePage(DeviceModel& model) : model_(&model) {}
    ~DevicePage();
    DevicePage(const DevicePage&) = delete;
    DevicePage& operator=(const DevicePage&) = delete;

    DeviceModel& model() { return *model_; }
    /** Works on another model (the Dock panel's default one until main.cpp gives the shared one). */
    void setModel(DeviceModel& model) {
        pointerLeave();
        model_ = &model;
    }

    /** The whole page (the dashboard tab, 1544 × 468 under the header). Lays out its hit areas. */
    void render(const Pen& pen, const DevicePageArea& area);

    /** The Dock quick view's card rows: 출력, 음량, 패스스루 (x, y: the first row's top left; w: the card's inside). */
    void renderQuick(const Pen& pen, double x, double y, double w);
    /** The rows' height in renderQuick (three rows of kQuickRowH and the gaps). */
    static constexpr double kQuickRowH = 76;
    static constexpr double kQuickRowGap = 10;
    static constexpr double kQuickRowsH = 3 * kQuickRowH + 2 * kQuickRowGap;

    /** @return true if a redraw is needed (hover changed, the volume moved) */
    bool pointerMove(double x, double y);
    /** A press (now: monotonic seconds, for − / ＋ held). @return true if it hit something (redraw) */
    bool pointerDown(double x, double y, double now);
    /** @return true if a press ended (a drag or a held − / ＋ lets go: the final volume is queued) */
    bool pointerUp();
    /** The laser left / the page was switched away: a drag lets go where it is. @return true (redraw) if anything did */
    bool pointerLeave();

    /** A held − / ＋ steps again when due (call at nextRepeatAt() or later). @return true if it stepped */
    bool takeRepeat(double now);
    /** @return when a held − / ＋ steps next (+infinity when none is held) */
    double nextRepeatAt() const { return repeat_.nextAt(); }
    /** A pointer event came in: the held button's safety stop (HoldRepeat::kIdleStopSec) counts from now. */
    void pointerSeen(double now) { repeat_.touch(now); }

    /** One spot that does something, as last drawn. */
    struct Hit {
        double x, y, w, h;
        DeviceHit kind;
        int arg;
    };
    DeviceHit hitAt(double x, double y, int* arg = nullptr) const;
    /** The centre of a hit area as last drawn (tests). @return false if it isn't there (or can't be pressed) */
    bool hitCenter(DeviceHit kind, int arg, double& x, double& y) const;
    const std::vector<Hit>& hits() const { return hits_; }
    /** The volume slider's track as last drawn (tests). */
    void volumeTrack(double& x, double& w) const {
        x = trackX_;
        w = trackW_;
    }

private:
    DeviceModel* model_;
    std::vector<Hit> hits_;
    double px_ = -1, py_ = -1;  ///< the pointer (-1 = off)
    bool pressing_ = false;
    DeviceHit pressedKind_ = DeviceHit::None;
    int pressedArg_ = -1;
    bool dragging_ = false;     ///< this page's press holds the volume knob
    bool dragMoved_ = false;    ///< the drag went past kVolumeDragStartPx (the slider follows it)
    double dragStartX_ = 0;     ///< where the press on the slider began, then where the drag passed kVolumeDragStartPx
    int dragStartVolume_ = 0;   ///< the volume when the drag passed it
    HoldRepeat repeat_;         ///< a held − / ＋
    double trackX_ = 0, trackW_ = 1;
    std::vector<std::string> rowIds_;  ///< the list's accounts as last drawn
    std::string bigId_;                ///< the big button's account as last drawn
    std::map<std::string, cairo_surface_t*> avatars_;  ///< PNG path -> image (null: unreadable)

    const Hit* hitTest(double x, double y) const;
    bool hovered(double x, double y, double w, double h) const;
    bool isPressed(DeviceHit kind, int arg) const { return pressing_ && pressedKind_ == kind && pressedArg_ == arg; }
    /** This page's press holds the volume (the slider, a held − / ＋): its release queues the final value. */
    bool holdsVolume() const {
        return dragging_ || (pressing_ && (pressedKind_ == DeviceHit::VolumeDown || pressedKind_ == DeviceHit::VolumeUp));
    }

    void button(const Pen& pen, double x, double y, double w, double h, const std::string& label, DeviceHit kind,
                int arg, bool selected, bool usable, double size);
    /** A segmented pill whose parts may be disabled one by one (drawn faded, not hittable). */
    void segments(const Pen& pen, double x, double y, double w, double h, const std::vector<std::string>& labels,
                  const std::vector<bool>& usable, int selected, bool allUsable, DeviceHit kind,
                  const std::vector<int>& args, double size);

    void outputRow(const Pen& pen, double labelX, double controlX, double right, double y, double h, double size);
    void volumeRow(const Pen& pen, double labelX, double controlX, double right, double y, double h, double size,
                   double muteW);
    void passthroughRow(const Pen& pen, double labelX, double controlX, double pillW, double right, double y,
                        double h, double size, const char* label);
    void syncCard(const Pen& pen, double x, double y, double w, double h);
    void drawAvatar(const Pen& pen, const std::string& steamId, const std::string& name, double cx, double cy,
                    double r);
};
