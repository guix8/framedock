// The Dock's state machine. See dock.h.
#include "dock.h"

#include <cmath>

PointerHands pointerHands(DockHand setting, Attachment hud) {
    switch (setting) {
        case DockHand::Left: return {true, false};
        case DockHand::Right: return {false, true};
        case DockHand::Auto: break;
    }
    if (hud == Attachment::LeftWrist) return {false, true};
    if (hud == Attachment::RightWrist) return {true, false};
    return {true, true};
}

void DockController::configure(bool enabled, int collapseSec, double now) {
    collapseSec_ = std::max(0, collapseSec);
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (!enabled_) {
        if (grab_ != DockTarget::None) endGrab(now, true);
        collapse(CollapseReason::Disabled, now);
        // Nothing can point at the browser panel either: it goes (Chrome keeps running)
        closeBrowser(now);
        hudAimed_ = dockAimed_ = false;
        hudAimUntil_ = dockAimUntil_ = -1;
        hudAimSince_ = dockAimSince_ = -1;
        hudPressed_ = dockHeld_ = false;
        checkLeave(now);
    }
}

void DockController::setLive(bool live, double now) {
    if (!live) hudHidden(now);
}

void DockController::setKeyboardUp(bool up, double now) {
    if (up == keyboardUp_) return;
    keyboardUp_ = up;
    touch(now);  // the timeout counts from when it closed
}

bool DockController::anyInteractive(double now) const {
    return interactive(DockTarget::Hud, now) || interactive(DockTarget::Dock, now) ||
           interactive(DockTarget::Browser, now) || interactive(DockTarget::Keyboard, now);
}

void DockController::updateAim(bool hudAimed, bool dockAimed, bool browserAimed, bool keyboardAimed, double now) {
    if (!enabled_) hudAimed = dockAimed = browserAimed = keyboardAimed = false;
    if (!expanded()) dockAimed = false;
    if (!browser_.open) browserAimed = false;
    if (!keyboard_.open) keyboardAimed = false;
    // The laser is on already (another panel, a press, a grab): the game has lost the controller, no dwell needed
    const bool laserOn = anyInteractive(now);
    const auto aim = [&](bool aimed, double& since, double& until) {
        if (!aimed) {
            since = -1;
            return;
        }
        if (since < 0) since = now;
        // A ray that only sweeps across the panel doesn't take the controller from the game: it must stay a moment
        if (laserOn || now < until || now - since >= aimDwellSec_ - 1e-9) until = now + kAimLingerSec;
    };
    aim(hudAimed, hudAimSince_, hudAimUntil_);
    aim(dockAimed, dockAimSince_, dockAimUntil_);
    aim(browserAimed, browser_.aimSince, browser_.aimUntil);
    aim(keyboardAimed, keyboard_.aimSince, keyboard_.aimUntil);
    hudAimed_ = hudAimed;
    dockAimed_ = dockAimed;
    browser_.aimed = browserAimed;
    keyboard_.aimed = keyboardAimed;
    // Pointing at an open Dock (or the HUD that opened it) counts as using it: the timeout runs from pointing away
    if (expanded() && (hudAimed || dockAimed)) touch(now);
    checkLeave(now);
}

bool DockController::interactive(DockTarget target, double now) const {
    if (!enabled_ || target == DockTarget::None) return false;
    if (grab_ == target) return true;
    if (isExtra(target)) {
        const Panel& p = panel(target);
        if (!p.open) return false;
        if (grab_ == DockTarget::Browser) return true;  // carried by either handle: both keep the laser
        return now < p.aimUntil || (p.held && now - p.eventAt < kHeldKeepSec);
    }
    if (target == DockTarget::Hud) {
        return now < hudAimUntil_ || (hudPressed_ && now - hudEventAt_ < kHeldKeepSec);
    }
    if (!expanded()) return false;
    return now < dockAimUntil_ || (dockHeld_ && now - dockEventAt_ < kHeldKeepSec);
}

void DockController::checkLeave(double now) {
    const bool hud = interactive(DockTarget::Hud, now);
    const bool dock = interactive(DockTarget::Dock, now);
    if (hudWasInteractive_ && !hud) {
        hudPressed_ = false;  // no click from a press the laser left
        emit(Event::Type::Leave, DockTarget::Hud);
    }
    if (dockWasInteractive_ && !dock) {
        dockHeld_ = false;
        emit(Event::Type::Leave, DockTarget::Dock);
    }
    hudWasInteractive_ = hud;
    dockWasInteractive_ = dock;
    for (const DockTarget t : {DockTarget::Browser, DockTarget::Keyboard}) {
        Panel& p = panel(t);
        const bool on = interactive(t, now);
        if (p.wasInteractive && !on) {
            p.held = false;
            emit(Event::Type::Leave, t);
        }
        p.wasInteractive = on;
    }
}

void DockController::hudDown(double now) {
    if (!enabled_) return;
    hudEventAt_ = now;
    touch(now);
    if (grab_ != DockTarget::None) return;
    hudPressed_ = true;
    hudPressedAt_ = now;
}

void DockController::hudUp(double now) {
    if (!enabled_) return;
    hudEventAt_ = now;
    touch(now);
    if (grab_ == DockTarget::Hud && grabSource_ == GrabSource::Trigger) {
        endGrab(now, false);
        return;
    }
    if (!hudPressed_) return;
    hudPressed_ = false;
    if (expanded()) {
        collapse(CollapseReason::HudClick, now);
    } else {
        openQuick(now);
    }
}

void DockController::hudActivity(double now) {
    if (!enabled_) return;
    hudEventAt_ = now;
    touch(now);
}

double DockController::dwellEndsAt() const {
    double next = std::numeric_limits<double>::infinity();
    if (!enabled_) return next;
    if (hudAimed_ && hudAimSince_ >= 0 && hudAimSince_ + kAimLingerSec > hudAimUntil_) {
        next = std::min(next, hudAimSince_ + aimDwellSec_);
    }
    if (dockAimed_ && dockAimSince_ >= 0 && dockAimSince_ + kAimLingerSec > dockAimUntil_) {
        next = std::min(next, dockAimSince_ + aimDwellSec_);
    }
    for (const Panel* p : {&browser_, &keyboard_}) {
        if (p->aimed && p->aimSince >= 0 && p->aimSince + kAimLingerSec > p->aimUntil) {
            next = std::min(next, p->aimSince + aimDwellSec_);
        }
    }
    return next;
}

void DockController::hudLeave(double now) {
    hudEventAt_ = now;
    // A carried HUD keeps its press (the laser can slip off for a moment); a plain press is no click any more
    if (grab_ != DockTarget::Hud) hudPressed_ = false;
}

void DockController::dockDown(double now) {
    if (!enabled_ || !expanded()) return;
    dockHeld_ = true;
    dockEventAt_ = now;
    touch(now);
}

void DockController::dockUp(double now) {
    dockEventAt_ = now;
    dockHeld_ = false;
    touch(now);
    if (grab_ == DockTarget::Dock && grabSource_ == GrabSource::Trigger) endGrab(now, false);
}

void DockController::dockActivity(double now) {
    if (!expanded()) return;
    dockEventAt_ = now;
    touch(now);
}

void DockController::dockLeave(double now) {
    dockEventAt_ = now;
    if (grab_ != DockTarget::Dock) dockHeld_ = false;
}

void DockController::openBrowser(double now) {
    if (!enabled_ || browser_.open) return;
    browser_ = Panel {};
    browser_.open = true;
    emit(Event::Type::BrowserOpened, DockTarget::Browser);
    // The browser panel takes the Dock panel's place in front of the wrist
    collapse(CollapseReason::Browser, now);
}

void DockController::closeBrowser(double now) {
    if (!browser_.open) return;
    if (grab_ == DockTarget::Browser) endGrab(now, true);
    setKeyboardOpen(false, now);
    browser_.open = false;
    browser_.aimed = false;
    browser_.aimUntil = browser_.aimSince = -1;
    browser_.held = false;
    emit(Event::Type::BrowserClosed, DockTarget::Browser);
    checkLeave(now);
}

void DockController::setKeyboardOpen(bool open, double now) {
    if (open && !browser_.open) return;
    if (open == keyboard_.open) return;
    const bool wasInteractive = keyboard_.wasInteractive;
    keyboard_ = Panel {};
    keyboard_.open = open;
    keyboard_.wasInteractive = wasInteractive;
    emit(open ? Event::Type::KeyboardOpened : Event::Type::KeyboardClosed, DockTarget::Keyboard);
    checkLeave(now);
}

void DockController::panelDown(DockTarget target, double now) {
    if (!enabled_ || !isExtra(target) || !panel(target).open) return;
    Panel& p = panel(target);
    p.held = true;
    p.eventAt = now;
}

void DockController::panelUp(DockTarget target, double now) {
    if (!isExtra(target)) return;
    Panel& p = panel(target);
    p.eventAt = now;
    p.held = false;
    if (grab_ == DockTarget::Browser && grabSource_ == GrabSource::Trigger) endGrab(now, false);
}

void DockController::panelActivity(DockTarget target, double now) {
    if (!isExtra(target) || !panel(target).open) return;
    panel(target).eventAt = now;
}

void DockController::panelLeave(DockTarget target, double now) {
    if (!isExtra(target)) return;
    Panel& p = panel(target);
    p.eventAt = now;
    if (grab_ != DockTarget::Browser) p.held = false;
}

void DockController::openQuick(double now) {
    if (!enabled_) return;
    touch(now);
    if (adjusting_) {
        adjusting_ = false;
        emit(Event::Type::AdjustEnded);
    }
    const bool wasOpen = expanded();
    view_ = DockView::Quick;
    emit(wasOpen ? Event::Type::ViewChanged : Event::Type::Opened);
}

void DockController::openFull(double now) {
    if (!enabled_) return;
    touch(now);
    const bool wasOpen = expanded();
    if (view_ == DockView::Full) return;
    view_ = DockView::Full;
    emit(wasOpen ? Event::Type::ViewChanged : Event::Type::Opened);
}

void DockController::collapse(CollapseReason reason, double now) {
    if (!expanded()) return;
    if (grab_ == DockTarget::Dock) endGrab(now, true);
    if (adjusting_) {
        adjusting_ = false;
        emit(Event::Type::AdjustEnded);
    }
    view_ = DockView::Collapsed;
    dockAimed_ = false;
    dockAimUntil_ = -1;
    dockAimSince_ = -1;
    dockHeld_ = false;
    emit(Event::Type::Collapsed, DockTarget::None, reason);
    checkLeave(now);  // the Dock panel's pointer lets go of what it held
}

void DockController::startAdjust(double now) {
    touch(now);
    if (view_ != DockView::Full || adjusting_) return;
    adjusting_ = true;
    emit(Event::Type::AdjustStarted);
}

void DockController::finishAdjust(double now) {
    touch(now);
    if (!adjusting_) return;
    adjusting_ = false;
    emit(Event::Type::AdjustEnded);
}

bool DockController::startGrab(DockTarget target, GrabSource source, double now) {
    if (target == DockTarget::Keyboard) target = DockTarget::Browser;  // the keyboard moves with the browser panel
    if (!enabled_ || grab_ != DockTarget::None || target == DockTarget::None) return false;
    if (target == DockTarget::Dock && !expanded()) return false;
    if (target == DockTarget::Browser) {
        if (!browser_.open) return false;
        if (!interactive(DockTarget::Browser, now) && !interactive(DockTarget::Keyboard, now) && !browser_.held &&
            !keyboard_.held) {
            return false;
        }
    }
    if (target == DockTarget::Hud && !interactive(DockTarget::Hud, now) && !hudPressed_) return false;
    if (target == DockTarget::Dock && adjusting_) {
        // Moving the frozen panel by hand ends the session: it goes back to following its anchor at the new offset
        adjusting_ = false;
        emit(Event::Type::AdjustEnded);
    }
    grab_ = target;
    grabSource_ = source;
    grabStartedAt_ = now;
    hudPressed_ = false;  // the press became the grab, not a click
    touch(now);
    emit(Event::Type::GrabStarted, target);
    return true;
}

void DockController::endGrab(double now, bool cancelled) {
    if (grab_ == DockTarget::None) return;
    const DockTarget target = grab_;
    grab_ = DockTarget::None;
    touch(now);
    if (target == DockTarget::Hud) hudEventAt_ = now;
    if (target == DockTarget::Dock) {
        dockEventAt_ = now;
        dockHeld_ = false;
    }
    if (target == DockTarget::Browser) {
        for (Panel* p : {&browser_, &keyboard_}) {
            p->eventAt = now;
            p->held = false;
        }
    }
    emit(cancelled ? Event::Type::GrabCancelled : Event::Type::GrabEnded, target);
}

void DockController::grip(bool down, double now) {
    if (!enabled_) return;
    if (down) {
        if (grab_ != DockTarget::None) return;
        // The panel the laser is on now (or still lingers on): the Dock panel first, it sits in front of the HUD. A ray
        // still dwelling doesn't count (it hasn't taken the laser)
        DockTarget target = DockTarget::None;
        if (keyboard_.open && now < keyboard_.aimUntil) {
            target = DockTarget::Browser;  // the keyboard's grip carries the browser panel it hangs under
        } else if (browser_.open && now < browser_.aimUntil) {
            target = DockTarget::Browser;
        } else if (expanded() && now < dockAimUntil_) {
            target = DockTarget::Dock;
        } else if (now < hudAimUntil_) {
            target = DockTarget::Hud;
        }
        if (target != DockTarget::None) startGrab(target, GrabSource::Grip, now);
        return;
    }
    if (grab_ != DockTarget::None && grabSource_ == GrabSource::Grip) endGrab(now, false);
}

void DockController::gripLost(double now) {
    if (grab_ != DockTarget::None && grabSource_ == GrabSource::Grip) endGrab(now, true);
}

void DockController::setBrowserLive(bool live, double now) {
    if (live || !browser_.open) return;
    if (grab_ == DockTarget::Browser) endGrab(now, true);
    for (Panel* p : {&browser_, &keyboard_}) {
        p->aimed = false;
        p->aimUntil = p->aimSince = -1;
        p->held = false;
    }
    checkLeave(now);
}

void DockController::hudHidden(double now) {
    // The HUD or the Dock panel, collapsed or not: nothing is kept (the browser panel has its own liveness)
    if (grab_ == DockTarget::Hud || grab_ == DockTarget::Dock) endGrab(now, true);
    hudPressed_ = false;
    hudAimed_ = false;
    hudAimUntil_ = -1;
    hudAimSince_ = -1;
    collapse(CollapseReason::HudHidden, now);
    checkLeave(now);
}

void DockController::tick(double now) {
    if (!enabled_) return;
    // A press held on the HUD becomes a grab
    if (hudPressed_ && grab_ == DockTarget::None && now - hudPressedAt_ >= kLongPressSec) {
        startGrab(DockTarget::Hud, GrabSource::Trigger, now);
    }
    // Safety: a grab whose release never came is put back (where the hand happens to be is no choice)
    if (grab_ != DockTarget::None && now - grabStartedAt_ >= kGrabMaxSec) endGrab(now, true);
    // Not used for a while: collapse (never while carrying a panel, holding a press on it, or typing on the keyboard
    // opened from it)
    const bool busy =
        grab_ != DockTarget::None || keyboardUp_ || (dockHeld_ && now - dockEventAt_ < kHeldKeepSec);
    if (busy) touch(now);  // the timeout counts from when it's let go
    if (expanded() && collapseSec_ > 0 && !busy && now - lastInteraction_ >= collapseSec_) {
        collapse(CollapseReason::Timeout, now);
    }
    checkLeave(now);
}

double DockController::nextWake(double now) const {
    double next = std::numeric_limits<double>::infinity();
    if (!enabled_) return next;
    // Only deadlines still ahead: one that passed is due (tick() acts on it on the next pass) or moot
    const auto at = [&](double deadline) {
        if (deadline > now) next = std::min(next, deadline);
    };
    if (hudPressed_ && grab_ == DockTarget::None) at(hudPressedAt_ + kLongPressSec);
    if (grab_ != DockTarget::None) at(grabStartedAt_ + kGrabMaxSec);
    if (expanded() && collapseSec_ > 0 && !keyboardUp_) at(lastInteraction_ + collapseSec_);
    // The linger and a held press running out (Leave). A held press decides nothing while its panel is carried or
    // still aimed at (it stays interactive either way), so its deadline is left out then: with a steady press no
    // event comes for a while, and the deadline would be long past
    if (hudWasInteractive_) {
        at(hudAimUntil_);
        if (hudPressed_ && grab_ != DockTarget::Hud && !hudAimed_) at(hudEventAt_ + kHeldKeepSec);
    }
    if (dockWasInteractive_) {
        at(dockAimUntil_);
        if (dockHeld_ && grab_ != DockTarget::Dock && !dockAimed_) at(dockEventAt_ + kHeldKeepSec);
    }
    for (const Panel* p : {&browser_, &keyboard_}) {
        if (!p->wasInteractive) continue;
        at(p->aimUntil);
        if (p->held && grab_ != DockTarget::Browser && !p->aimed) at(p->eventAt + kHeldKeepSec);
    }
    return next;
}

std::vector<DockController::Event> DockController::takeEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}
