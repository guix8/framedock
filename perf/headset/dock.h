// The Dock: operating Frame Dock in a game with the other hand's laser, without the SteamVR dashboard.
//
// Collapsed, the Dock is the HUD. Pointing the pointer hand's laser at the HUD for kAimDwellSec highlights it and makes
// it clickable (the main loop turns VROverlayFlags_MakeOverlaysInteractiveIfVisible on for it, so SteamVR's laser comes
// on and the game loses that controller only while the laser is on a Frame Dock panel). The dwell keeps a ray that
// only sweeps across a panel during play (a HUD on the head is in front of both hands) from taking the controller.
// A click opens the Dock panel (quick view); the quick view's 전체 shows the dashboard tab's pages. It collapses on a
// second click on the HUD, its 접기 button, a timeout without interaction, or the HUD being hidden.
//
// Grab to move: holding the trigger on the HUD (kLongPressSec) or on the Dock panel's handle bar carries that panel
// rigidly with the controller; the release stores its new offset (the main loop puts it back instead when it was
// released where it was grabbed, or a device lost tracking). The grip (SteamVR Input) while the laser is on a panel
// does the same when SteamVR delivers it to an overlay app, which it may not do while its laser has the focus.
// A fine-adjust session (a Placement button pressed in the Dock's full view) freezes the Dock panel in the room until
// 완료, so held buttons stay under the laser while the HUD hand moves.
//
// This class is the state machine only: no OpenVR, no drawing. The main loop feeds it aim results (from the poses),
// pointer events and the grip, and acts on the events it hands back (takeEvents).
#pragma once

#include "config.h"

#include <algorithm>
#include <limits>
#include <vector>

enum class DockView {
    Collapsed,  ///< only the HUD
    Quick,      ///< the Dock panel's quick view (default when it opens)
    Full,       ///< the Dock panel shows the dashboard tab's pages
};

/** A Frame Dock panel the laser can be on. The keyboard hangs under the browser panel and moves with it. */
enum class DockTarget { None, Hud, Dock, Browser, Keyboard };

/** What started a grab, so only its own release ends it. */
enum class GrabSource { Grip, Trigger };

/** Why the Dock collapsed (log, tests). */
enum class CollapseReason { None, HudClick, Button, Timeout, HudHidden, Disabled, Browser };

/** Which hands' lasers may work the panels. */
struct PointerHands {
    bool left = false;
    bool right = false;
};

/**
 * The hands whose lasers work the HUD and the Dock panel: the setting, or (auto) the hand without the HUD, both hands
 * when the HUD is on the head.
 */
PointerHands pointerHands(DockHand setting, Attachment hud);

class DockController {
public:
    static constexpr double kAimDwellSec = 0.25;   ///< a ray must stay on a panel this long before it takes the laser
    static constexpr double kAimLingerSec = 0.3;   ///< the laser stays on this long after pointing away (Frametop)
    static constexpr double kLongPressSec = 0.6;   ///< trigger held on the HUD this long: a grab, not a click
    static constexpr double kHeldKeepSec = 1.0;    ///< a held press keeps the laser while events came this recently
    static constexpr double kGrabMaxSec = 60.0;    ///< safety: a grab whose release got lost is cancelled after this

    /** Something the main loop acts on. */
    struct Event {
        enum class Type {
            Opened,         ///< the Dock panel appears (view(): Quick or Full)
            ViewChanged,    ///< Quick ↔ Full while open
            Collapsed,      ///< the Dock panel goes (reason)
            AdjustStarted,  ///< freeze the Dock panel in the room where it is now
            AdjustEnded,    ///< attach it to its anchor again
            GrabStarted,    ///< target follows the grabbing controller
            GrabEnded,      ///< store target's new offset (and save)
            GrabCancelled,  ///< put target back where it was (no save, no "unsaved")
            Leave,          ///< target lost the laser: release what its pointer held (holds, compare, a drag)
            BrowserOpened,  ///< show the browser panel (start Chrome if needed)
            BrowserClosed,  ///< hide it (Chrome keeps running; the keyboard closes too)
            KeyboardOpened, ///< show the keyboard under the browser panel
            KeyboardClosed,
        };
        Type type;
        DockTarget target = DockTarget::None;
        CollapseReason reason = CollapseReason::None;
    };

    /**
     * @param enabled in-game interaction is on (off: collapses, nothing takes the laser)
     * @param collapseSec collapse after this long without interaction (0 = never)
     */
    void configure(bool enabled, int collapseSec, double now);
    /**
     * How long a ray must stay on a panel before it takes the laser (default kAimDwellSec; 0 = at once). Not needed
     * when the laser is already on (the other panel, a press, a grab, the linger): the game lost the controller anyway.
     */
    void setAimDwell(double sec) { aimDwellSec_ = std::max(0.0, sec); }
    double aimDwell() const { return aimDwellSec_; }
    /**
     * Whether the HUD is there to be used: shown and the headset worn (and the feature on). Not live, everything lets
     * go at once: a grab of the HUD or the Dock panel is cancelled (collapsed or not), the Dock collapses, nothing takes
     * the laser. The browser panel has its own (setBrowserLive).
     * Call it on every pass; it does nothing while live.
     */
    void setLive(bool live, double now);
    /**
     * Whether the browser panel can be used: the feature on and the headset worn (the HUD being hidden doesn't
     * matter to it). Not live: a grab of it is cancelled and nothing on it takes the laser; it stays open (Chrome keeps
     * playing). Call it on every pass.
     */
    void setBrowserLive(bool live, double now);
    /**
     * SteamVR's keyboard, opened from the Dock panel (a profile's new name), is up: no collapse timeout while it is
     * (it counts again from when it closes). The other ways to collapse still work.
     */
    void setKeyboardUp(bool up, double now);

    DockView view() const { return view_; }
    bool expanded() const { return view_ != DockView::Collapsed; }
    bool enabled() const { return enabled_; }
    /** @return true while a fine-adjust session freezes the Dock panel in the room */
    bool adjusting() const { return adjusting_; }
    /** @return the panel being carried, or None */
    DockTarget grabbing() const { return grab_; }
    GrabSource grabSource() const { return grabSource_; }

    /**
     * Where the pointer hand's laser is now (from the poses). Call it whenever the poses were read; the caller widens
     * the margin while a panel is interactive, so the laser leaves on a wider border than it arrives.
     * @param hudAimed on the HUD (only when the HUD is shown)
     * @param dockAimed on the Dock panel (only while expanded)
     */
    void updateAim(bool hudAimed, bool dockAimed, double now) { updateAim(hudAimed, dockAimed, false, false, now); }
    /**
     * The same with the browser panel and its keyboard (each only while open; the caller passes false otherwise).
     */
    void updateAim(bool hudAimed, bool dockAimed, bool browserAimed, bool keyboardAimed, double now);

    /**
     * @return true if the panel should take SteamVR's laser now: aimed past the dwell (or within kAimLingerSec of it),
     *         carried, or a press on it still held with events coming in
     */
    bool interactive(DockTarget target, double now) const;
    /** @return true if the HUD should show its highlight border (it takes the laser) */
    bool hudHighlighted(double now) const { return interactive(DockTarget::Hud, now); }
    /** @return the panel the laser is on (the nearer panels win: keyboard, browser, Dock panel, HUD), from the last
     *          updateAim */
    DockTarget aimed() const {
        return keyboard_.aimed  ? DockTarget::Keyboard
               : browser_.aimed ? DockTarget::Browser
               : dockAimed_     ? DockTarget::Dock
               : hudAimed_      ? DockTarget::Hud
                                : DockTarget::None;
    }
    /**
     * @return when a ray now dwelling on a panel (aimed, not yet taking the laser) has stayed long enough: the caller
     *         checks the aim again then (+infinity when none dwells)
     */
    double dwellEndsAt() const;

    // ---- The HUD's pointer events (SteamVR mouse events on the HUD overlay) ----
    /** Trigger down on the HUD: a click on release, a grab when held kLongPressSec. */
    void hudDown(double now);
    /** Trigger up on the HUD: the click (toggles the Dock) or the end of a trigger grab. */
    void hudUp(double now);
    /** Any other event on the HUD (a move): counts as interaction. */
    void hudActivity(double now);
    /** The laser left the HUD (SteamVR's FocusLeave): a press on it is no click any more. */
    void hudLeave(double now);

    // ---- The Dock panel's pointer events ----
    /** A press on the Dock panel (held until dockUp / dockLeave). */
    void dockDown(double now);
    /** The release: ends a trigger grab of the Dock. */
    void dockUp(double now);
    /** A move or scroll on the Dock panel. */
    void dockActivity(double now);
    /** The laser left the Dock panel. */
    void dockLeave(double now);

    // ---- The browser panel and its keyboard (Browser / Keyboard) ----
    /** Opens the browser panel (BrowserOpened). The Dock collapses (CollapseReason::Browser): the browser is in front. */
    void openBrowser(double now);
    /** Hides it (BrowserClosed): its keyboard closes, a grab of it is cancelled. */
    void closeBrowser(double now);
    bool browserOpen() const { return browser_.open; }
    /** Shows or hides the keyboard under the browser panel (KeyboardOpened / KeyboardClosed). */
    void setKeyboardOpen(bool open, double now);
    bool keyboardOpen() const { return keyboard_.open; }
    /** A press on the browser panel or the keyboard (held until panelUp / panelLeave). */
    void panelDown(DockTarget target, double now);
    /** The release: ends a trigger grab of the browser panel (from its handle or the keyboard's). */
    void panelUp(DockTarget target, double now);
    /** A move or scroll on it. */
    void panelActivity(DockTarget target, double now);
    /** The laser left it. */
    void panelLeave(DockTarget target, double now);

    // ---- Commands (the Dock panel's buttons, the grip) ----
    void openQuick(double now);
    void openFull(double now);
    void collapse(CollapseReason reason, double now);
    /** A Placement button was pressed in the full view: freeze the Dock panel (no-op unless Full). */
    void startAdjust(double now);
    /** 완료: attach the Dock panel again. */
    void finishAdjust(double now);
    /**
     * Starts carrying a panel. The Dock panel only while expanded; the HUD only while the laser is on it or it was
     * pressed. Ends a fine-adjust session first when the Dock panel is grabbed.
     * @return false if not started
     */
    bool startGrab(DockTarget target, GrabSource source, double now);
    /** Ends a grab without keeping where it went (the controller or the anchor lost tracking): GrabCancelled. */
    void cancelGrab(double now) { endGrab(now, true); }
    /** The grip (SteamVR Input) went down or up. Down on a panel taking the laser grabs it; up ends a grip grab. */
    void grip(bool down, double now);
    /**
     * The grip can't be read any more (no longer watched, SteamVR made the action inactive): no release came, so a
     * grip grab is cancelled rather than kept where it happens to be.
     */
    void gripLost(double now);
    /** The HUD went away (hidden, the headset off): collapse, cancel any grab. */
    void hudHidden(double now);

    /**
     * Time-driven changes: long press → grab, the laser's linger running out (Leave), the collapse timeout, the grab
     * safety (cancelled after kGrabMaxSec).
     */
    void tick(double now);
    /**
     * @return the next time after now when tick() has something to do (+infinity when nothing is pending). Always
     *         later than now: a deadline that already passed is due, and the main loop runs tick() on its next pass
     *         anyway (it checks the aim every kDockActivePollSec while anything here is pending), so it is never
     *         returned (a past wake would make the loop spin)
     */
    double nextWake(double now) const;

    /** @return the events since the last call, oldest first */
    std::vector<Event> takeEvents();

    /** @return the last interaction (tests) */
    double lastInteraction() const { return lastInteraction_; }

private:
    bool enabled_ = true;
    int collapseSec_ = 20;
    double aimDwellSec_ = kAimDwellSec;
    bool keyboardUp_ = false;
    DockView view_ = DockView::Collapsed;
    bool adjusting_ = false;
    DockTarget grab_ = DockTarget::None;
    GrabSource grabSource_ = GrabSource::Grip;
    double grabStartedAt_ = 0;
    bool hudAimed_ = false, dockAimed_ = false;
    double hudAimUntil_ = -1, dockAimUntil_ = -1;  ///< aimed past the dwell, plus the linger
    double hudAimSince_ = -1, dockAimSince_ = -1;  ///< aimed without a break since (-1: not aimed)
    bool hudPressed_ = false;                      ///< trigger down on the HUD, not yet a click or a grab
    double hudPressedAt_ = 0;
    bool dockHeld_ = false;                        ///< trigger down on the Dock panel
    double hudEventAt_ = -1e9, dockEventAt_ = -1e9;
    bool hudWasInteractive_ = false, dockWasInteractive_ = false;
    /** The browser panel and the keyboard: their own aim, press and Leave state (the Dock's are the fields above) */
    struct Panel {
        bool open = false;
        bool aimed = false;
        double aimUntil = -1, aimSince = -1;
        bool held = false;
        double eventAt = -1e9;
        bool wasInteractive = false;
    };
    Panel browser_, keyboard_;
    Panel& panel(DockTarget t) { return t == DockTarget::Keyboard ? keyboard_ : browser_; }
    const Panel& panel(DockTarget t) const { return t == DockTarget::Keyboard ? keyboard_ : browser_; }
    static bool isExtra(DockTarget t) { return t == DockTarget::Browser || t == DockTarget::Keyboard; }
    /** @return true while any panel takes the laser */
    bool anyInteractive(double now) const;
    double lastInteraction_ = 0;
    std::vector<Event> events_;

    void emit(Event::Type type, DockTarget target = DockTarget::None, CollapseReason reason = CollapseReason::None) {
        events_.push_back({type, target, reason});
    }
    void touch(double now) { lastInteraction_ = std::max(lastInteraction_, now); }
    void endGrab(double now, bool cancelled);
    /** Emits Leave for a panel that just stopped taking the laser. */
    void checkLeave(double now);
};
