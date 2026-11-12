// The main loop's browser-panel wiring that needs no SteamVR (out of main.cpp so the tests can run it): where the
// panel hangs and how the keyboard sits under it, who may point at it, Chrome's options from config.json, and the
// status line the Browser page shows.
#pragma once

#include "browser_session.h"
#include "config.h"
#include "dock.h"
#include "dock_math.h"

#include <string>

namespace browserloop {

/** The keyboard's tilt under the panel (top edge at the panel, bottom edge toward the user, like a laptop's) */
constexpr double kKeyboardTiltDeg = 30;
/** The gap between the panel's lower edge and the keyboard's upper edge (m) */
constexpr double kKeyboardGapM = 0.015;
/** The panel's width steps (its − / ＋) and range (m) */
constexpr double kWidthStepM = 0.05;
constexpr double kMinWidthM = 0.25;
constexpr double kMaxWidthM = 1.5;

/** @return ~/.local/share/framedock/browser (XDG_DATA_HOME when absolute): the profile and Chrome's log live here */
std::string defaultBrowserDir();

/** @return ~/.config/framedock/favorites.json (XDG_CONFIG_HOME when absolute): the browser panel's 즐겨찾기 */
std::string defaultFavoritesPath();

/** @return where the panel hangs: its anchor setting, auto = the HUD's attachment */
Attachment browserAttachment(const Config& config);
/** @return the panel's offset from its anchor (config.browser's head or wrist pose) */
dockmath::Mat34 browserLocal(const Config& config);
/** Stores a carried panel's new offset (relative to its anchor) as its head or wrist pose. */
void setBrowserPlacementFrom(const dockmath::Mat34& local, Config& config);

/**
 * @return the browser overlay's width (m) for the panel's size (width_m): the width, but for 9:16 the height, so a tall
 * panel isn't a 25 cm wide tower at the smallest size. panelWidth × panelHeight: its texture (the bar included)
 */
double panelWidthM(const BrowserSettings& browser, int panelWidth, int panelHeight);

/** @return the keyboard's width for a panel this wide: 0.9 × it, kept between 0.35 and 0.6 m (keys stay hittable) */
double keyboardWidthM(double browserWidthM);
/**
 * @return the keyboard relative to the browser panel (both in the panel's own axes): centred under it, its top edge
 *         kKeyboardGapM below the panel's lower edge, tilted kKeyboardTiltDeg with its lower edge toward the user
 */
dockmath::Mat34 keyboardUnderBrowser(double browserHeightM, double keyboardHeightM);

/**
 * The hands whose lasers work the browser panel: the Dock's pointing hands without the hand the panel hangs on (it
 * can't point at itself). If that leaves none, the other hand. Fixed in the room (pinned) it hangs on no hand: both
 * hands point (and so either can carry it).
 */
PointerHands browserHands(const Config& config, bool pinned);

/** @return the panel's width after a − / ＋ step */
double stepWidth(double widthM, int direction);

/**
 * @return true if the browser panel is really in front of the user: open, the headset worn with the feature on
 *         (live) and on its anchor (placed: its controller tracked, or fixed in the room). Only then does Chrome
 *         screencast and the overlay get textures; otherwise the session is told it is hidden (screencast off, media
 *         paused per keep_audio, the idle timer running).
 */
bool panelShowing(bool open, bool live, bool placed);

/** Tells the session when the panel starts or stops being seen: one setVisible per change, never repeated. */
class SessionVisibility {
public:
    /** @return true if showing changed since the last call (then call setVisible(showing)) */
    bool update(bool showing) {
        if (showing == showing_) return false;
        showing_ = showing;
        return true;
    }
    bool showing() const { return showing_; }

private:
    bool showing_ = false;  ///< a new session starts hidden
};

/** The panel counts as out of view this long after it left the field of view (or turned away), not at once */
constexpr double kOutOfViewSec = 0.5;
/** Half the field of view a panel must reach into to count as in view (the Frame's is about 110° across) */
constexpr double kInViewHalfFovDeg = 60;

/**
 * Whether the browser panel can be seen: its front faces the head (not edge-on, not its back) and some of it is within
 * kInViewHalfFovDeg of where the head looks (the wrist's facing logic, plus a cone for the field of view).
 * @param panel its pose in the room; widthM, heightM its size
 * @param head the HMD's pose in the room
 */
bool panelInView(const dockmath::Mat34& panel, double widthM, double heightM, const dockmath::Mat34& head);

/** In view at once; out of view only after kOutOfViewSec of it (a glance away doesn't slow the picture). */
class InViewTimer {
public:
    /** @return true if inView() changed (then tell Chrome: BrowserSession::setInView) */
    bool update(bool seen, double now) {
        if (seen) {
            outSince_ = -1;
            if (inView_) return false;
            inView_ = true;
            return true;
        }
        if (!inView_) return false;
        if (outSince_ < 0) outSince_ = now;
        if (now - outSince_ < kOutOfViewSec) return false;
        inView_ = false;
        return true;
    }
    /**
     * The check can't run (the panel isn't aimable, the headset is off, no HMD pose): forget a countdown that was
     * running. Without this, changesAt() would stay in the past with no reading to move it, and the loop would wake
     * at its minimum sleep until the check ran again. inView() stays as it was.
     */
    void pause() { outSince_ = -1; }
    bool inView() const { return inView_; }
    /** @return when update() could next change it (+infinity: only on a new reading, or paused) */
    double changesAt() const { return inView_ && outSince_ >= 0 ? outSince_ + kOutOfViewSec : 1e300; }

private:
    bool inView_ = true;
    double outSince_ = -1;
};

/**
 * @return the next aim check, moved up to when an out-of-view countdown ends (the in-view check runs only with the
 *         aim check, so the loop wakes for both at once instead of waking at changesAt() for a check that isn't due).
 *         Call it right after the check ran at now (changesAt() is then after now, or +infinity).
 */
inline double alignAimWithInView(double nextAim, const InViewTimer& timer, double now) {
    const double ends = timer.changesAt();
    return ends > now && ends < nextAim ? ends : nextAim;
}

/** @return Chrome's options from config.json's "browser" (profile and log under dir) */
SessionOptions sessionOptions(const BrowserSettings& browser, const std::string& dir);

/** @return the Browser page's line about Chrome ("실행 중 (PID 1234)", "꺼짐", …) */
std::string statusLine(const BrowserSession::Status& status, pid_t pid);

/** @return true if url may become the home page (http or https) */
bool usableHome(const std::string& url);

}  // namespace browserloop
