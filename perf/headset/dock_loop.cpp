// The main loop's Dock wiring. See dock_loop.h.
#include "dock_loop.h"

#include "dock_view.h"
#include "settings_panel.h"

#include <algorithm>
#include <cmath>

namespace dockloop {

double sleepSeconds(double wakeAt, double now) {
    if (!std::isfinite(wakeAt)) return wakeAt > 0 ? wakeAt : kMinSleepSec;  // +infinity: until something happens
    return std::max(wakeAt - now, kMinSleepSec);
}

GripEdges::Change GripEdges::update(bool readable, bool down) {
    if (!readable) {
        const bool lost = watched_ && down_;
        watched_ = down_ = false;
        return lost ? Change::Lost : Change::None;
    }
    if (!watched_) {  // the watch begins: whatever is held now is no press
        watched_ = true;
        down_ = down;
        return Change::None;
    }
    if (down == down_) return Change::None;
    down_ = down;
    return down ? Change::Pressed : Change::Released;
}

bool grabTracked(const DockPoses& poses, Attachment anchor, bool grabLeft, uint32_t grabDevice) {
    const DockPoses::Device& hand = poses.hand(grabLeft);
    return poses.anchor(anchor).valid && hand.valid && hand.index == grabDevice;
}

bool pressingHandLeft(const DockPoses& poses, uint32_t pressDevice, bool aimLeft) {
    if (poses.left.valid && poses.left.index == pressDevice) return true;
    if (poses.right.valid && poses.right.index == pressDevice) return false;
    return aimLeft;
}

GrabRelease releaseGrab(bool ended, const DockPoses& poses, Attachment anchor, bool grabLeft, uint32_t grabDevice,
                        const dockmath::Mat34& startWorld, const dockmath::Mat34& grab) {
    GrabRelease r;
    if (!ended) return r;  // Cancelled
    if (!grabTracked(poses, anchor, grabLeft, grabDevice)) {
        r.outcome = GrabRelease::Outcome::Untracked;
        return r;
    }
    const DockPoses::Device& hand = poses.hand(grabLeft);
    // A slow click (the trigger held past kLongPressSec) or a grip let go in place: nothing moved, nothing changes
    if (!dockmath::movedByGrab(startWorld, dockmath::multiply(hand.pose, grab))) {
        r.outcome = GrabRelease::Outcome::InPlace;
        return r;
    }
    r.outcome = GrabRelease::Outcome::Keep;
    r.local = dockmath::localAfterGrab(poses.anchor(anchor).pose, hand.pose, grab);
    return r;
}

bool applyLeave(const DockController::Event& event, DockPanel& view, SettingsPanel& full) {
    if (event.type != DockController::Event::Type::Leave || event.target != DockTarget::Dock) return false;
    bool dirty = view.pointerLeave();
    dirty |= full.pointerLeave();
    return dirty;
}

void renderDock(DockView view, bool adjusting, DockPanel& panel, SettingsPanel& full, const Config& config,
                const AutostartStatus& autostart, const ProfilesUiState& profiles, double now) {
    switch (view) {
        case DockView::Quick: panel.renderQuick(profiles, now); break;
        case DockView::Full:
            full.render(config, autostart, profiles);
            panel.renderFull(full, adjusting);
            break;
        case DockView::Collapsed: break;
    }
}

void runExitSteps(const ExitSteps& steps) {
    for (const std::function<void()>* step : {&steps.releaseInput, &steps.hideOverlays, &steps.saveConfig,
                                              &steps.colorOff, &steps.vrOff, &steps.browserOff}) {
        if (*step) (*step)();
    }
}

}  // namespace dockloop
