// The main loop's Dock wiring that needs no SteamVR, out of main.cpp so the tests can run it: the loop's sleep, the
// grip's edges, whether a grab still has its devices, what a release keeps, the Leave that lets go of the Dock panel's
// pointer, and drawing the Dock panel's current view.
#pragma once

#include "dock.h"
#include "dock_math.h"

#include <cstdint>
#include <functional>

class DockPanel;
class SettingsPanel;
struct AutostartStatus;
struct ProfilesUiState;

namespace dockloop {

/** The main loop never sleeps less than this, whatever deadline it computed */
constexpr double kMinSleepSec = 0.01;

/**
 * @param wakeAt the earliest deadline (+infinity: none)
 * @return how long to sleep: wakeAt − now, but at least kMinSleepSec, so a deadline that is already past (a bug
 *         anywhere) costs a short sleep instead of spinning a core; +infinity stays +infinity
 */
double sleepSeconds(double wakeAt, double now);

/**
 * The grip as the Dock sees it: presses and releases while it is watched (a laser on a panel, a panel carried).
 * A grip already held when the watch begins is no press (a game's "hold the object" while pointing). When it can't be
 * read any more (the watch ends, SteamVR makes the action inactive) while it was down, that is Lost, not Released:
 * no release came, so a grip grab is cancelled (DockController::gripLost), not kept.
 */
class GripEdges {
public:
    enum class Change { None, Pressed, Released, Lost };
    /**
     * @param readable the grip is watched and was read
     * @param down its state (ignored unless readable)
     */
    Change update(bool readable, bool down);
    bool down() const { return down_; }

private:
    bool watched_ = false;
    bool down_ = false;
};

/**
 * @param grabLeft the hand carrying the panel
 * @param grabDevice its device index at the grab (a role moving to another device counts as lost)
 * @return true while the carrying controller and the anchor (the HUD's attachment) are both tracked
 */
bool grabTracked(const DockPoses& poses, Attachment anchor, bool grabLeft, uint32_t grabDevice);

/**
 * The hand a trigger grab belongs to: the controller whose laser pressed (the Down event's device), not the hand whose
 * ray is nearest (with both hands pointing at a pinned browser panel the other ray may be the nearer one).
 * @param pressDevice the pressing controller's device index (invalid: unknown)
 * @param aimLeft the nearest ray's hand, used when pressDevice is neither hand
 * @return true for the left hand
 */
bool pressingHandLeft(const DockPoses& poses, uint32_t pressDevice, bool aimLeft);

/** What a grab's end does with the panel. */
struct GrabRelease {
    enum class Outcome {
        Keep,       ///< store local as its new offset (and save)
        Cancelled,  ///< the controller cancelled it (hidden, safety, grip lost): put back
        Untracked,  ///< the carrying controller or the anchor isn't tracked: put back
        InPlace,    ///< released where it was grabbed (kGrabDeadZoneM / kGrabDeadZoneDeg): put back, nothing changes
    };
    Outcome outcome = Outcome::Cancelled;
    dockmath::Mat34 local {};  ///< Keep: the panel relative to its anchor
    bool keep() const { return outcome == Outcome::Keep; }
};

/**
 * @param ended GrabEnded (true) or GrabCancelled (false)
 * @param poses read at the release
 * @param startWorld the panel's pose in the room when the grab started
 * @param grab the panel relative to the carrying controller (dockmath::grabOffset)
 */
GrabRelease releaseGrab(bool ended, const DockPoses& poses, Attachment anchor, bool grabLeft, uint32_t grabDevice,
                        const dockmath::Mat34& startWorld, const dockmath::Mat34& grab);

/**
 * The controller's Leave: the panel it names stopped taking the laser, so its pointer lets go of what it held (hold to
 * repeat, compare, an editor drag). The Dock panel's quick view and full view both let go; the HUD has nothing to.
 * @return true if a redraw is needed
 */
bool applyLeave(const DockController::Event& event, DockPanel& view, SettingsPanel& full);

/**
 * Draws the Dock panel's current view (Quick: its own buttons; Full: the bar over the full view's SettingsPanel),
 * which also rebuilds its hit areas. Called on a view switch at once, so the rest of that batch of pointer events hits
 * the new view's buttons, not the old one's.
 */
void renderDock(DockView view, bool adjusting, DockPanel& panel, SettingsPanel& full, const Config& config,
                const AutostartStatus& autostart, const ProfilesUiState& profiles, double now);

/** After SteamVR's laser was taken off an overlay (or the dashboard closed), its events are still polled this long:
 * an Up or a Leave sent just before must not wait for the 2 Hz poll. */
constexpr double kEventDrainSec = 1.0;

/**
 * Which of our overlays a wake polls for events (each poll is an IPC per overlay). Between the 2 Hz full polls only
 * the overlays SteamVR's laser can be on: interactive ones, every one while the dashboard is open (its own laser
 * reaches them), and for kEventDrainSec after either ended (the last Up / Leave).
 */
class EventPolling {
public:
    enum Overlay { Hud = 0, Dock, Browser, Keyboard, kCount };
    /** The overlay's MakeOverlaysInteractiveIfVisible changed: on, polled every wake; off, kEventDrainSec more. */
    void setInteractive(Overlay overlay, bool on, double now) {
        if (interactive_[overlay] && !on) drainUntil_[overlay] = now + kEventDrainSec;
        interactive_[overlay] = on;
    }
    /** The dashboard's state as read this wake: open polls every overlay, and kEventDrainSec after it closed. */
    void setDashboardOpen(bool open, double now) {
        if (dashboardOpen_ && !open) allUntil_ = now + kEventDrainSec;
        dashboardOpen_ = open;
    }
    /** @return true if this wake polls the overlay (full: the 2 Hz tick polls every one) */
    bool polls(Overlay overlay, bool full, double now) const {
        return full || allNow(now) || interactive_[overlay] || now < drainUntil_[overlay];
    }
    /** @return true if some overlay is polled this wake for a reason of its own (the dashboard, a drain) */
    bool wanted(double now) const { return dashboardOpen_ || draining(now); }
    /** @return true while a drain runs (after interactive ended, after the dashboard closed): the loop wakes often
     *          enough to deliver its last events */
    bool draining(double now) const {
        if (now < allUntil_) return true;
        for (int i = 0; i < kCount; ++i) {
            if (now < drainUntil_[i]) return true;
        }
        return false;
    }

private:
    bool allNow(double now) const { return dashboardOpen_ || now < allUntil_; }
    bool interactive_[kCount] = {};
    double drainUntil_[kCount] = {-1, -1, -1, -1};
    bool dashboardOpen_ = false;
    double allUntil_ = -1;
};

/** A panel hidden this long (closed, collapsed) gives back its GPU texture; the next submit makes a new one. */
constexpr double kIdleTextureSec = 30.0;

/**
 * @param hiddenAt when a HideOverlay last succeeded; negative: not hidden by one (never shown, or a show was asked for
 *        and ShowOverlay keeps failing), which never counts as idle: the texture isn't freed and made again each tick
 * @return true when a hidden overlay's texture is due to be freed (it has one, and was hidden kIdleTextureSec ago)
 */
inline bool idleTextureDue(bool shown, bool hasTexture, double hiddenAt, double now) {
    return !shown && hasTexture && hiddenAt >= 0 && now - hiddenAt >= kIdleTextureSec;
}

/**
 * The app's exit, in the order that keeps the user safe whatever happens next: the controllers go back to the game and
 * our overlays disappear first, the colour layer is set back (no compare bypass left on) and SteamVR let go, and only
 * then Chrome is closed (which may take seconds). Every way out of the main loop runs these.
 */
struct ExitSteps {
    std::function<void()> releaseInput;   ///< no overlay interactive, the HUD's laser input off
    std::function<void()> hideOverlays;   ///< the Dock panel, the browser panel and its keyboard hidden
    std::function<void()> saveConfig;     ///< a held button's or a grab's change not written yet
    std::function<void()> colorOff;       ///< ColorModel::shutdown (the compare bypass off in current.txt)
    std::function<void()> vrOff;          ///< VrOverlay::shutdown
    std::function<void()> browserOff;     ///< Chrome closed, its thread joined
};
/** Runs the steps in that order (an empty one is skipped). */
void runExitSteps(const ExitSteps& steps);

/** The exit code when the Vulkan device was lost: not 0 or 3, so systemd's Restart=always starts us again. */
constexpr int kExitCodeDeviceLost = 4;

}  // namespace dockloop
