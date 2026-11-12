// The Dock (in-game interaction): its geometry (a laser against a panel, grab offsets on a wrist and on the head, the
// pose decompositions back into config.json's offsets) and its state machine (collapsed / quick / full, the frozen
// fine-adjust session, grabs by grip or by a held trigger, the collapse timeout, the HUD hidden, the laser's dwell and
// linger and the Leave that lets go of holds, the wake times) and the main loop's wiring (dock_loop: the sleep, the
// grip's edges, what a grab's release keeps). No SteamVR.
#include "browser_loop.h"
#include "dock.h"
#include "dock_loop.h"
#include "dock_manifest.h"
#include "dock_math.h"
#include "json.h"
#include "log_limiter.h"
#include "placement.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using dockmath::Mat34;
using Ev = DockController::Event::Type;

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

bool samePose(const Mat34& a, const Mat34& b, double eps = 1e-4) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::fabs(static_cast<double>(a.m[i][j]) - b.m[i][j]) > eps) return false;
        }
    }
    return true;
}

/** A pose: rotation from the head convention plus a position. */
Mat34 pose(double x, double y, double z, double yaw, double pitch, double roll) {
    HeadPose h;
    h.x = x, h.y = y, h.z = z, h.yaw = yaw, h.pitch = pitch, h.roll = roll;
    return dockmath::headTransform(h);
}

/** @return the reason of the (last) Collapsed event, None if none */
CollapseReason collapseReason(const std::vector<DockController::Event>& events) {
    CollapseReason reason = CollapseReason::None;
    for (const auto& e : events) {
        if (e.type == Ev::Collapsed) reason = e.reason;
    }
    return reason;
}

bool has(const std::vector<DockController::Event>& events, Ev type, DockTarget target = DockTarget::None) {
    for (const auto& e : events) {
        if (e.type == type && (target == DockTarget::None || e.target == target)) return true;
    }
    return false;
}

void testInverseAndDecompose() {
    const Mat34 m = pose(0.3, -0.2, 1.1, 35, -20, 70);
    CHECK(samePose(dockmath::multiply(m, dockmath::inverseRigid(m)), dockmath::identity()));
    CHECK(samePose(dockmath::multiply(dockmath::inverseRigid(m), m), dockmath::identity()));

    // Angles back from the rotation, over the range config.json allows
    for (double yaw : {-170.0, -45.0, 0.0, 30.0, 179.0}) {
        for (double pitch : {-85.0, -10.0, 0.0, 45.0, 89.0}) {
            for (double roll : {-120.0, 0.0, 15.0, 175.0}) {
                double r[3][3];
                panelRotation(yaw, pitch, roll, r);
                double y = 0, p = 0, rl = 0;
                dockmath::yawPitchRoll(r, y, p, rl);
                CHECK(near(y, yaw, 1e-6) && near(p, pitch, 1e-6) && near(rl, roll, 1e-6));
            }
        }
    }
    // Straight up: roll folds into yaw, the rotation stays the same
    double r[3][3];
    panelRotation(40, 90, 25, r);
    double y = 0, p = 0, rl = 0;
    dockmath::yawPitchRoll(r, y, p, rl);
    double back[3][3];
    panelRotation(y, p, rl, back);
    bool same = true;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) same = same && near(back[i][j], r[i][j], 1e-6);
    }
    CHECK(same);
    CHECK(near(p, 90) && near(rl, 0));

    // A wrist pose back from wristTransform (rounded to 1 mm and 0.1°)
    for (double pitch : {-170.0, -115.0, -90.0, -30.0, -5.0}) {
        WristPose w;
        w.x = 0.012, w.y = 0.087, w.z = -0.103, w.pitch = pitch, w.yaw = 12.5, w.roll = -33.3;
        const WristPose back = dockmath::wristPoseFrom(wristTransform(w));
        CHECK(!(back != w));
    }
    // Out of config.json's range: clamped (±0.5 m on a wrist, ±5 m on the head)
    const WristPose far = dockmath::wristPoseFrom(pose(0.9, -0.7, 0.2, 0, 0, 0));
    CHECK(near(far.x, 0.5) && near(far.y, -0.5) && near(far.z, 0.2));
    HeadPose h;
    h.x = -0.12, h.y = 0.03, h.z = -0.48, h.yaw = -15, h.pitch = 12.3, h.roll = 4;
    CHECK(!(dockmath::headPoseFrom(dockmath::headTransform(h)) != h));
}

void testRayHit() {
    // A panel 1 m ahead (−z), facing the viewer (+z), 0.3 m wide, 0.2 m tall (texture 600×400)
    const Mat34 panel = pose(0, 0, -1, 0, 0, 0);
    dockmath::Ray ray;  // from the origin along −z
    dockmath::PanelHit hit = dockmath::hitPanel(panel, ray);
    CHECK(hit.hit && near(hit.u, 0) && near(hit.v, 0) && near(hit.distance, 1));
    double x = 0, y = 0;
    CHECK(dockmath::hitPixels(hit, 0.3, 0.2, 600, 400, x, y) && near(x, 300) && near(y, 200));

    // Off centre: u right, v up; pixels from the top left
    ray.origin[0] = -0.15 + 0.03;  // 3 cm in from the left edge
    ray.origin[1] = 0.1 - 0.02;    // 2 cm down from the top
    hit = dockmath::hitPanel(panel, ray);
    CHECK(dockmath::hitPixels(hit, 0.3, 0.2, 600, 400, x, y) && near(x, 60) && near(y, 40));
    CHECK(dockmath::onPanel(hit, 0.3, 0.2, 0));

    // Just past the edge: off without a margin, on with one (leaving takes a wider margin than arriving)
    ray.origin[0] = 0.16;
    ray.origin[1] = 0;
    hit = dockmath::hitPanel(panel, ray);
    CHECK(!dockmath::onPanel(hit, 0.3, 0.2, 0.0));
    CHECK(dockmath::onPanel(hit, 0.3, 0.2, 0.02));
    CHECK(!dockmath::hitPixels(hit, 0.3, 0.2, 600, 400, x, y));

    // Pointing away, or along the plane: no hit
    dockmath::Ray away;
    away.dir[2] = 1;
    CHECK(!dockmath::hitPanel(panel, away).hit);
    dockmath::Ray along;
    along.dir[0] = 1, along.dir[2] = 0;
    CHECK(!dockmath::hitPanel(panel, along).hit);

    // A controller's pose: its −z is the ray. Yawed 90° left, it points along −x, at a panel facing +x
    const Mat34 controller = pose(0.5, 1.2, 0.0, 90, 0, 0);  // yaw +90: its +z faces +x, so −z points to −x
    const dockmath::Ray r = dockmath::rayFromPose(controller);
    CHECK(near(r.dir[0], -1) && near(r.dir[1], 0) && near(r.dir[2], 0));
    const Mat34 side = pose(-0.5, 1.25, 0.04, 90, 0, 0);  // facing +x, 1 m along the ray
    hit = dockmath::hitPanel(side, r);
    CHECK(hit.hit && near(hit.distance, 1) && near(hit.v, -0.05) && near(hit.u, 0.04));

    // A tilted panel (the HUD on a wrist): the hit point maps back onto the panel's own axes
    const Mat34 wrist = pose(0.2, 1.0, -0.4, 20, -50, 10);
    Mat34 target = dockmath::multiply(wrist, pose(0.05, -0.03, 0, 0, 0, 0));  // a point on the panel
    dockmath::Ray toPoint;
    const double from[3] = {0.0, 1.6, 0.0};
    double len = 0;
    for (int i = 0; i < 3; ++i) {
        toPoint.origin[i] = from[i];
        toPoint.dir[i] = target.m[i][3] - from[i];
        len += toPoint.dir[i] * toPoint.dir[i];
    }
    for (double& d : toPoint.dir) d /= std::sqrt(len);
    hit = dockmath::hitPanel(wrist, toPoint);
    CHECK(hit.hit && near(hit.u, 0.05) && near(hit.v, -0.03));
}

void testGrabWrist() {
    // HUD on the left wrist at its WristPose; the right hand's controller grabs it, moves and turns, lets go
    WristPose w;  // the defaults: on the wrist, flat
    const Mat34 hudLocal = wristTransform(w);
    const Mat34 left0 = pose(-0.2, 1.0, -0.3, 10, -30, 5);
    const Mat34 right0 = pose(0.15, 1.1, -0.25, -40, -10, 0);
    const Mat34 hudWorld0 = dockmath::multiply(left0, hudLocal);
    const Mat34 grab = dockmath::grabOffset(right0, hudWorld0);
    // Not moved: back where it was
    CHECK(samePose(dockmath::localAfterGrab(left0, right0, grab), hudLocal));
    // The right hand moves and turns, the left wrist moves too: the HUD lands where the right hand carried it
    const Mat34 right1 = dockmath::multiply(pose(0.05, 0.02, -0.03, 0, 0, 0), dockmath::multiply(right0, pose(0, 0, 0, 25, 10, -5)));
    const Mat34 left1 = pose(-0.18, 0.95, -0.32, 15, -35, 0);
    const Mat34 local1 = dockmath::localAfterGrab(left1, right1, grab);
    CHECK(samePose(dockmath::multiply(left1, local1), dockmath::multiply(right1, grab)));
    // Into the config: the selected wrist's pose, and back to the same transform (to 1 mm / 0.1°)
    Config config;
    config.attachment = Attachment::LeftWrist;
    const WristPose rightBefore = config.rightWrist;
    dockmath::setHudPlacementFrom(local1, config);
    CHECK(samePose(wristTransform(config.leftWrist), local1, 2e-3));
    CHECK(!(config.rightWrist != rightBefore));  // the other wrist stays
    CHECK(near(config.posX, Config().posX));     // and the head position
}

void testGrabHead() {
    Config config;  // the HUD on the head
    const Mat34 hudLocal = attachmentTransform(config);
    const Mat34 head0 = pose(0, 1.6, 0, 5, 0, 0);
    const Mat34 hand0 = pose(0.2, 1.2, -0.3, 0, 0, 0);
    const Mat34 grab = dockmath::grabOffset(hand0, dockmath::multiply(head0, hudLocal));
    const Mat34 hand1 = dockmath::multiply(pose(-0.1, 0.15, 0, 0, 0, 0), hand0);
    const Mat34 head1 = pose(0, 1.6, 0, -10, 3, 0);
    const Mat34 local = dockmath::localAfterGrab(head1, hand1, grab);
    dockmath::setHudPlacementFrom(local, config);
    CHECK(samePose(attachmentTransform(config), local, 2e-3));
    // The wrists' poses are untouched by a head grab
    CHECK(!(config.leftWrist != WristPose()));
    // The Dock panel's own head offset uses the same math
    const HeadPose dock = dockmath::headPoseFrom(local);
    CHECK(samePose(dockmath::headTransform(dock), local, 2e-3));
}

void testPointerHands() {
    PointerHands h = pointerHands(DockHand::Auto, Attachment::LeftWrist);
    CHECK(!h.left && h.right);
    h = pointerHands(DockHand::Auto, Attachment::RightWrist);
    CHECK(h.left && !h.right);
    h = pointerHands(DockHand::Auto, Attachment::Head);
    CHECK(h.left && h.right);
    h = pointerHands(DockHand::Left, Attachment::LeftWrist);
    CHECK(h.left && !h.right);
    h = pointerHands(DockHand::Right, Attachment::Head);
    CHECK(!h.left && h.right);
}

void testClickOpenCollapse() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);  // the dwell has its own test
    double t = 1.0;
    // Not aimed: nothing takes the laser
    d.updateAim(false, false, t);
    CHECK(!d.interactive(DockTarget::Hud, t) && !d.hudHighlighted(t));
    // Aimed: the HUD highlights and takes the laser
    d.updateAim(true, false, t);
    CHECK(d.interactive(DockTarget::Hud, t) && d.hudHighlighted(t));
    // A click (short press) opens the quick view
    d.hudDown(t);
    d.hudUp(t + 0.2);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(has(ev, Ev::Opened) && d.view() == DockView::Quick);
    // Full and back
    d.openFull(t + 1);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::ViewChanged) && d.view() == DockView::Full);
    d.openQuick(t + 1.5);
    CHECK(d.view() == DockView::Quick && has(d.takeEvents(), Ev::ViewChanged));
    // Clicking the HUD again collapses
    t += 2;
    d.updateAim(true, false, t);
    d.hudDown(t);
    d.hudUp(t + 0.1);
    ev = d.takeEvents();
    CHECK(d.view() == DockView::Collapsed && has(ev, Ev::Collapsed));
    CHECK(collapseReason(ev) == CollapseReason::HudClick);
    // The 접기 button
    d.openQuick(t + 1);
    d.collapse(CollapseReason::Button, t + 2);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::Collapsed) && collapseReason(ev) == CollapseReason::Button);
    // A press the laser left isn't a click
    d.updateAim(true, false, t + 3);
    d.hudDown(t + 3);
    d.hudLeave(t + 3.1);
    d.hudUp(t + 3.2);
    CHECK(d.view() == DockView::Collapsed);
}

void testLingerAndLeave() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);  // the dwell has its own test
    double t = 10;
    d.updateAim(true, false, t);
    d.hudDown(t);
    d.hudUp(t + 0.1);
    d.takeEvents();
    // On the Dock panel, a press held (a hold-to-repeat button, compare)
    d.updateAim(false, true, t + 0.5);
    CHECK(d.interactive(DockTarget::Dock, t + 0.5));
    d.dockDown(t + 0.5);
    // Pointing away: the laser lingers kAimLingerSec, then the panel lets it go and the held press is released
    d.updateAim(false, false, t + 0.6);
    CHECK(d.interactive(DockTarget::Dock, t + 0.8));  // linger (and the press still has recent events)
    d.tick(t + 0.85);
    CHECK(!has(d.takeEvents(), Ev::Leave, DockTarget::Dock));
    // The press keeps it only while events come in (kHeldKeepSec after the last)
    d.tick(t + 0.5 + DockController::kHeldKeepSec + 0.01);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(has(ev, Ev::Leave, DockTarget::Dock));
    CHECK(!d.interactive(DockTarget::Dock, t + 2));
    // Without a held press the linger alone decides (it counts from the last time the laser was on it)
    d.updateAim(false, true, t + 3);
    d.updateAim(false, false, t + 3.1);
    d.tick(t + 3 + DockController::kAimLingerSec - 0.01);
    CHECK(!has(d.takeEvents(), Ev::Leave, DockTarget::Dock));
    d.tick(t + 3 + DockController::kAimLingerSec + 0.01);
    CHECK(has(d.takeEvents(), Ev::Leave, DockTarget::Dock));
    // The HUD the same way
    d.updateAim(true, false, t + 5);
    d.updateAim(false, false, t + 5.1);
    d.tick(t + 5.5);
    CHECK(has(d.takeEvents(), Ev::Leave, DockTarget::Hud));
    CHECK(!d.hudHighlighted(t + 5.5));
    // nextWake points at the linger's end while it runs
    d.updateAim(true, false, t + 6);
    d.updateAim(false, false, t + 6.05);
    CHECK(near(d.nextWake(t + 6.1), t + 6 + DockController::kAimLingerSec, 1e-9));
}

void testTimeoutAndHidden() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);  // the dwell has its own test
    d.updateAim(true, false, 100);
    d.hudDown(100);
    d.hudUp(100.1);
    d.updateAim(false, false, 100.2);
    d.takeEvents();
    // Pointing at it keeps it open
    d.updateAim(false, true, 115);
    d.updateAim(false, false, 115.1);
    d.tick(130);
    CHECK(d.expanded());
    CHECK(near(d.nextWake(130), 135, 1e-6));
    d.tick(135.2);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(!d.expanded() && has(ev, Ev::Collapsed) && collapseReason(ev) == CollapseReason::Timeout);
    // Never with collapse_sec 0
    d.configure(true, 0, 200);
    d.openQuick(200);
    d.tick(10000);
    CHECK(d.expanded());
    // The HUD hidden collapses
    d.hudHidden(10001);
    ev = d.takeEvents();
    CHECK(!d.expanded() && has(ev, Ev::Collapsed) && collapseReason(ev) == CollapseReason::HudHidden);
    // Turning the feature off collapses, and nothing takes the laser
    d.configure(true, 20, 10002);
    d.openQuick(10002);
    d.updateAim(true, true, 10002);
    d.configure(false, 20, 10003);
    ev = d.takeEvents();
    CHECK(!d.expanded() && has(ev, Ev::Collapsed));
    CHECK(!d.interactive(DockTarget::Hud, 10003) && !d.interactive(DockTarget::Dock, 10003));
    d.updateAim(true, false, 10004);
    CHECK(!d.hudHighlighted(10004));
    d.hudDown(10004);
    d.hudUp(10004.1);
    CHECK(!d.expanded());
}

void testAdjust() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);  // the dwell has its own test
    // Only in the full view
    d.openQuick(1);
    d.startAdjust(2);
    CHECK(!d.adjusting());
    d.openFull(3);
    d.takeEvents();
    d.startAdjust(4);
    CHECK(d.adjusting() && has(d.takeEvents(), Ev::AdjustStarted));
    d.startAdjust(4.5);  // once
    CHECK(d.takeEvents().empty());
    // 완료 attaches it again
    d.finishAdjust(5);
    CHECK(!d.adjusting() && has(d.takeEvents(), Ev::AdjustEnded));
    // Back to the quick view ends it
    d.startAdjust(6);
    d.openQuick(7);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(!d.adjusting() && has(ev, Ev::AdjustEnded));
    // Collapse ends it (first), then collapses
    d.openFull(8);
    d.startAdjust(9);
    d.takeEvents();
    d.collapse(CollapseReason::Button, 10);
    ev = d.takeEvents();
    CHECK(ev.size() >= 2 && ev[0].type == Ev::AdjustEnded && ev[1].type == Ev::Collapsed);
    // The timeout too
    d.openFull(20);
    d.startAdjust(21);
    d.takeEvents();
    d.tick(41.5);
    ev = d.takeEvents();
    CHECK(!d.adjusting() && has(ev, Ev::AdjustEnded) && has(ev, Ev::Collapsed));
    // Grabbing the frozen panel ends the session (it follows its anchor again at the new offset)
    d.openFull(50);
    d.startAdjust(51);
    d.updateAim(false, true, 52);
    d.takeEvents();
    d.grip(true, 52.1);
    ev = d.takeEvents();
    CHECK(!d.adjusting() && ev.size() == 2 && ev[0].type == Ev::AdjustEnded && ev[1].type == Ev::GrabStarted);
}

void testGrabs() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);  // the dwell has its own test
    // Grip on the HUD
    d.updateAim(true, false, 1);
    d.grip(true, 1.1);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::Hud && has(ev, Ev::GrabStarted, DockTarget::Hud));
    // The laser may slip off while carrying: it stays interactive, a trigger release doesn't end a grip grab
    d.updateAim(false, false, 1.5);
    d.tick(3);
    CHECK(d.interactive(DockTarget::Hud, 3));
    d.hudUp(3.1);
    CHECK(d.grabbing() == DockTarget::Hud);
    d.grip(false, 3.2);
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabEnded, DockTarget::Hud));
    CHECK(!d.expanded());  // a grab is no click

    // Grip with nothing aimed: nothing
    d.updateAim(false, false, 10);
    d.tick(11);
    d.takeEvents();
    d.grip(true, 11);
    CHECK(d.grabbing() == DockTarget::None);
    d.grip(false, 11.1);

    // Holding the trigger on the HUD: a grab after kLongPressSec, the release ends it, no click
    d.updateAim(true, false, 20);
    d.hudDown(20);
    CHECK(near(d.nextWake(20), 20 + DockController::kAimLingerSec, 1e-9));  // the linger first
    CHECK(near(d.nextWake(20.35), 20 + DockController::kLongPressSec, 1e-9));
    d.tick(20 + DockController::kLongPressSec - 0.05);
    CHECK(d.grabbing() == DockTarget::None);
    d.tick(20 + DockController::kLongPressSec + 0.01);
    CHECK(d.grabbing() == DockTarget::Hud && d.grabSource() == GrabSource::Trigger);
    d.grip(false, 20.8);  // a grip release doesn't end a trigger grab
    CHECK(d.grabbing() == DockTarget::Hud);
    d.hudUp(21.5);
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabEnded, DockTarget::Hud) && !d.expanded());

    // The Dock panel: the grip while aimed at it (it wins over the HUD), or the handle bar (trigger)
    d.updateAim(true, false, 30);
    d.hudDown(30);
    d.hudUp(30.1);
    d.updateAim(true, true, 30.5);
    d.takeEvents();
    d.grip(true, 30.6);
    CHECK(d.grabbing() == DockTarget::Dock);
    d.grip(false, 31);
    CHECK(has(d.takeEvents(), Ev::GrabEnded, DockTarget::Dock));
    d.dockDown(32);
    CHECK(d.startGrab(DockTarget::Dock, GrabSource::Trigger, 32));
    d.dockUp(33);
    CHECK(d.grabbing() == DockTarget::None && has(d.takeEvents(), Ev::GrabEnded, DockTarget::Dock));
    // Collapsing while the Dock panel is carried cancels the grab (no save)
    d.startGrab(DockTarget::Dock, GrabSource::Grip, 34);
    d.collapse(CollapseReason::Button, 35);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::GrabCancelled, DockTarget::Dock) && d.grabbing() == DockTarget::None);
    // The Dock panel can't be grabbed while collapsed
    CHECK(!d.startGrab(DockTarget::Dock, GrabSource::Grip, 36));
    // No collapse timeout while carrying something
    d.updateAim(true, false, 40);
    d.hudDown(40);
    d.hudUp(40.1);
    d.updateAim(false, true, 40.2);
    d.grip(true, 40.3);
    d.tick(70);
    CHECK(d.expanded() && d.grabbing() == DockTarget::Dock);
    // Safety: a grab whose release never came is cancelled after kGrabMaxSec (put back, nothing kept)
    d.takeEvents();
    d.tick(40.3 + DockController::kGrabMaxSec + 0.1);
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabCancelled, DockTarget::Dock));
    CHECK(!has(ev, Ev::GrabEnded));
    // The HUD hidden while carrying the HUD: cancelled
    d.updateAim(true, false, 200);
    d.grip(true, 200.1);
    d.takeEvents();
    d.hudHidden(201);
    CHECK(has(d.takeEvents(), Ev::GrabCancelled, DockTarget::Hud));
}

// The dwell: a ray must stay on a panel kAimDwellSec before it takes the laser, so one that only sweeps across the HUD
// during play (on the head, in front of both hands) doesn't take the controller from the game
void testDwell() {
    DockController d;
    d.configure(true, 20, 0);
    CHECK(near(d.aimDwell(), DockController::kAimDwellSec, 1e-12));
    CHECK(DockController::kAimDwellSec >= 0.2 && DockController::kAimDwellSec <= 0.3);
    const double dwell = DockController::kAimDwellSec;
    // A sweep: aimed on one check, gone on the next. Never interactive, no highlight, no Leave
    double t = 10;
    d.updateAim(true, false, t);
    CHECK(!d.interactive(DockTarget::Hud, t) && !d.hudHighlighted(t));
    CHECK(near(d.dwellEndsAt(), t + dwell, 1e-9));
    d.updateAim(false, false, t + 0.05);
    CHECK(!d.interactive(DockTarget::Hud, t + 0.05));
    CHECK(std::isinf(d.dwellEndsAt()));
    d.tick(t + 1);
    CHECK(d.takeEvents().empty());
    // Staying: not before the dwell's end, then on
    t = 20;
    for (double dt = 0; dt < dwell - 0.01; dt += 0.05) {
        d.updateAim(true, false, t + dt);
        CHECK(!d.interactive(DockTarget::Hud, t + dt));
    }
    d.updateAim(true, false, t + dwell);
    CHECK(d.interactive(DockTarget::Hud, t + dwell) && d.hudHighlighted(t + dwell));
    CHECK(std::isinf(d.dwellEndsAt()));
    // Pointing away and back within the linger: no new dwell (the laser never went off)
    d.updateAim(true, false, t + 0.9);
    d.updateAim(false, false, t + 1);
    d.updateAim(true, false, t + 0.9 + DockController::kAimLingerSec - 0.05);
    CHECK(d.interactive(DockTarget::Hud, t + 0.9 + DockController::kAimLingerSec - 0.05));
    // After the linger ran out: the dwell again
    d.updateAim(false, false, t + 2);
    d.tick(t + 3);
    CHECK(has(d.takeEvents(), Ev::Leave, DockTarget::Hud));
    d.updateAim(true, false, t + 4);
    CHECK(!d.interactive(DockTarget::Hud, t + 4));
    // The grip during the dwell grabs nothing (the ray hasn't taken the laser)
    d.grip(true, t + 4.05);
    CHECK(d.grabbing() == DockTarget::None);
    d.grip(false, t + 4.1);
    d.updateAim(true, false, t + 4 + dwell);
    CHECK(d.interactive(DockTarget::Hud, t + 4 + dwell));
    // Open: moving from the HUD to the Dock panel needs no dwell (the laser is on already)
    d.hudDown(t + 4.3);
    d.hudUp(t + 4.4);
    CHECK(d.expanded());
    d.updateAim(false, true, t + 4.5);
    CHECK(d.interactive(DockTarget::Dock, t + 4.5));
    // ...but coming back to it from outside does
    d.updateAim(false, false, t + 5);
    d.tick(t + 6);
    d.takeEvents();
    d.updateAim(false, true, t + 7);
    CHECK(!d.interactive(DockTarget::Dock, t + 7));
    d.updateAim(false, true, t + 7 + dwell);
    CHECK(d.interactive(DockTarget::Dock, t + 7 + dwell));
    // Configurable
    DockController e;
    e.configure(true, 20, 0);
    e.setAimDwell(0.5);
    e.updateAim(true, false, 1);
    e.updateAim(true, false, 1.3);
    CHECK(!e.interactive(DockTarget::Hud, 1.3) && near(e.dwellEndsAt(), 1.5, 1e-9));
    e.updateAim(true, false, 1.5);
    CHECK(e.interactive(DockTarget::Hud, 1.5));
    e.setAimDwell(-1);
    CHECK(e.aimDwell() == 0);
}

/**
 * Runs the controller the way the main loop does (aim checks every 0.05 s, tick, nextWake) over [from, to) and checks
 * nextWake never returns a time at or before now (the main loop would spin on it).
 * @return the number of wakes that weren't in the future
 */
int pastWakes(DockController& d, double from, double to, bool hudAimed, bool dockAimed) {
    int bad = 0;
    for (double t = from; t < to; t += 0.05) {
        d.updateAim(hudAimed, dockAimed, t);
        d.tick(t);
        d.takeEvents();
        const double next = d.nextWake(t + 0.003);  // a little later, as at the end of a loop
        if (!(next > t + 0.003)) ++bad;
    }
    return bad;
}

// nextWake while a panel is carried or a press is held steady (no events come for a while): always in the future
void testWakeNeverPast() {
    // The Dock panel carried by its handle (a steady trigger: no MouseMove for seconds), aimed or not
    for (const bool aimed : {true, false}) {
        DockController d;
        d.configure(true, 20, 0);
        d.setAimDwell(0);
        d.updateAim(true, false, 1);
        d.hudDown(1);
        d.hudUp(1.1);
        d.updateAim(false, true, 1.2);
        d.dockDown(1.2);
        CHECK(d.startGrab(DockTarget::Dock, GrabSource::Trigger, 1.2));
        CHECK(pastWakes(d, 1.25, 8, false, aimed) == 0);
        CHECK(d.grabbing() == DockTarget::Dock);
    }
    // A steady press on the Dock panel (hold to repeat, compare) while aimed at it
    {
        DockController d;
        d.configure(true, 20, 0);
        d.setAimDwell(0);
        d.updateAim(true, false, 1);
        d.hudDown(1);
        d.hudUp(1.1);
        d.updateAim(false, true, 1.2);
        d.dockDown(1.2);
        CHECK(pastWakes(d, 1.25, 8, false, true) == 0);
        CHECK(d.interactive(DockTarget::Dock, 8));
        // Pointing away with the press still held: it lets go after the linger, and the wakes stay ahead
        CHECK(pastWakes(d, 8, 10, false, false) == 0);
        CHECK(!d.interactive(DockTarget::Dock, 10));
    }
    // The HUD carried by the grip, by the trigger (a long press), aimed or not; the collapse timeout running
    for (const bool aimed : {true, false}) {
        DockController d;
        d.configure(true, 20, 0);
        d.setAimDwell(0);
        d.updateAim(true, false, 1);
        d.grip(true, 1.05);
        CHECK(d.grabbing() == DockTarget::Hud);
        CHECK(pastWakes(d, 1.1, 6, aimed, false) == 0);
        d.grip(false, 6);
        d.updateAim(true, false, 7);
        d.hudDown(7);
        CHECK(pastWakes(d, 7.05, 12, aimed, false) == 0);
        CHECK(d.grabbing() == DockTarget::Hud && d.grabSource() == GrabSource::Trigger);
    }
    // The dwell, the open Dock's timeout, a press on the HUD before it becomes a grab
    {
        DockController d;
        d.configure(true, 20, 0);
        CHECK(pastWakes(d, 1, 3, true, false) == 0);
        d.hudDown(3);
        d.hudUp(3.1);
        CHECK(d.expanded());
        CHECK(pastWakes(d, 3.2, 30, false, false) == 0);
        CHECK(!d.expanded());
    }
}

// Not live (the HUD hidden, the headset off): any grab is cancelled, also with the Dock collapsed
void testLiveAndGripLost() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);
    // A HUD grab with the Dock collapsed (the case the old wiring missed)
    d.updateAim(true, false, 1);
    d.grip(true, 1.1);
    CHECK(d.grabbing() == DockTarget::Hud && !d.expanded());
    d.takeEvents();
    d.setLive(true, 2);
    CHECK(d.grabbing() == DockTarget::Hud && d.takeEvents().empty());
    d.setLive(false, 3);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabCancelled, DockTarget::Hud) && !has(ev, Ev::GrabEnded));
    CHECK(!d.interactive(DockTarget::Hud, 3));
    d.setLive(false, 3.5);  // again: nothing more
    CHECK(d.takeEvents().empty());
    // A trigger grab of the HUD, the same
    d.updateAim(true, false, 4);
    d.hudDown(4);
    d.tick(4 + DockController::kLongPressSec + 0.01);
    CHECK(d.grabbing() == DockTarget::Hud);
    d.takeEvents();
    d.setLive(false, 5);
    CHECK(has(d.takeEvents(), Ev::GrabCancelled, DockTarget::Hud));
    // The Dock panel carried: cancelled, and the Dock collapses
    d.updateAim(true, false, 6);
    d.hudDown(6);
    d.hudUp(6.1);
    d.updateAim(false, true, 6.2);
    d.grip(true, 6.3);
    CHECK(d.grabbing() == DockTarget::Dock);
    d.takeEvents();
    d.setLive(false, 7);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::GrabCancelled, DockTarget::Dock) && collapseReason(ev) == CollapseReason::HudHidden);
    CHECK(!d.expanded());

    // The grip lost (no release came): a grip grab is cancelled, a trigger grab doesn't care
    d.updateAim(true, false, 10);
    d.grip(true, 10.1);
    d.takeEvents();
    d.gripLost(10.5);
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabCancelled, DockTarget::Hud));
    d.updateAim(true, false, 11);
    d.hudDown(11);
    d.tick(11 + DockController::kLongPressSec + 0.01);
    CHECK(d.grabbing() == DockTarget::Hud);
    d.gripLost(12);
    CHECK(d.grabbing() == DockTarget::Hud);
}

// SteamVR's keyboard opened from the Dock panel: no collapse timeout while it is up
void testKeyboardKeepsOpen() {
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);
    d.updateAim(true, false, 1);
    d.hudDown(1);
    d.hudUp(1.1);
    d.updateAim(false, false, 1.2);
    d.openFull(2);
    d.setKeyboardUp(true, 3);
    d.tick(100);
    CHECK(d.expanded());
    CHECK(d.nextWake(100) > 100);
    // Closed: the timeout counts from then
    d.setKeyboardUp(false, 200);
    d.tick(219);
    CHECK(d.expanded());
    CHECK(near(d.nextWake(219), 220, 1e-9));
    d.tick(220.1);
    CHECK(!d.expanded() && collapseReason(d.takeEvents()) == CollapseReason::Timeout);
    // The other ways still collapse it
    d.openQuick(300);
    d.setKeyboardUp(true, 300);
    d.collapse(CollapseReason::Button, 301);
    CHECK(!d.expanded());
    d.setKeyboardUp(false, 302);
}

// The main loop's wiring: the minimum sleep, the grip's edges, a grab's tracking, what a release keeps
void testBrowserPanel() {
    DockController d;
    d.configure(true, 20, 0);
    double t = 1;
    // From the Dock's quick view: the browser opens, the Dock collapses (the browser takes its place)
    d.setAimDwell(0);
    d.updateAim(true, false, t);
    d.hudDown(t);
    d.hudUp(t + 0.1);
    d.takeEvents();
    d.openBrowser(t + 0.5);
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(d.browserOpen() && has(ev, Ev::BrowserOpened, DockTarget::Browser));
    CHECK(!d.expanded() && collapseReason(ev) == CollapseReason::Browser);
    // Pointing at it takes the laser after the dwell like any panel; the keyboard only while open
    d.setAimDwell(DockController::kAimDwellSec);
    t = 10;
    d.updateAim(false, false, true, true, t);
    CHECK(!d.interactive(DockTarget::Browser, t));
    CHECK(!d.interactive(DockTarget::Keyboard, t + 1));  // closed: never
    CHECK(near(d.dwellEndsAt(), t + DockController::kAimDwellSec, 1e-9));
    d.updateAim(false, false, true, false, t + DockController::kAimDwellSec);
    CHECK(d.interactive(DockTarget::Browser, t + DockController::kAimDwellSec));
    CHECK(d.aimed() == DockTarget::Browser);
    d.setKeyboardOpen(true, t + 0.3);
    CHECK(d.keyboardOpen() && has(d.takeEvents(), Ev::KeyboardOpened, DockTarget::Keyboard));
    // Moving from the panel to its keyboard: no dwell (the laser is on already)
    d.updateAim(false, false, false, true, t + 0.4);
    CHECK(d.interactive(DockTarget::Keyboard, t + 0.4));
    CHECK(d.aimed() == DockTarget::Keyboard);
    // A held key keeps the laser while events come; pointing away lets go after the linger (Leave)
    d.panelDown(DockTarget::Keyboard, t + 0.5);
    d.updateAim(false, false, false, false, t + 0.6);
    d.tick(t + 1.0);
    CHECK(d.interactive(DockTarget::Keyboard, t + 1.0));
    d.tick(t + 0.5 + DockController::kHeldKeepSec + 0.01);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::Leave, DockTarget::Keyboard) && has(ev, Ev::Leave, DockTarget::Browser));
    CHECK(!d.interactive(DockTarget::Keyboard, t + 2));
    CHECK(std::isinf(d.nextWake(t + 2)));  // nothing pending once it let go
    // The handle (panel or keyboard) carries the browser panel; the trigger's release ends it
    t = 20;
    d.setAimDwell(0);
    d.updateAim(false, false, false, true, t);
    d.panelDown(DockTarget::Keyboard, t);
    CHECK(d.startGrab(DockTarget::Keyboard, GrabSource::Trigger, t));
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::Browser && has(ev, Ev::GrabStarted, DockTarget::Browser));
    d.updateAim(false, false, false, false, t + 1);  // slipping off while carrying keeps both
    d.tick(t + 2);
    CHECK(d.interactive(DockTarget::Browser, t + 2) && d.interactive(DockTarget::Keyboard, t + 2));
    CHECK(!has(d.takeEvents(), Ev::Leave));
    d.panelUp(DockTarget::Keyboard, t + 2.5);
    ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabEnded, DockTarget::Browser));
    // The grip on the browser panel grabs it too
    d.updateAim(false, false, true, false, t + 3);
    d.grip(true, t + 3.1);
    CHECK(d.grabbing() == DockTarget::Browser && d.grabSource() == GrabSource::Grip);
    // The HUD hidden: nothing changes for the browser panel. The headset off: the grab is cancelled, it stays open
    d.setLive(false, t + 3.5);
    CHECK(d.grabbing() == DockTarget::Browser);
    d.setBrowserLive(true, t + 3.6);
    CHECK(d.grabbing() == DockTarget::Browser);
    d.setBrowserLive(false, t + 4);
    ev = d.takeEvents();
    CHECK(has(ev, Ev::GrabCancelled, DockTarget::Browser) && d.browserOpen());
    CHECK(!d.interactive(DockTarget::Browser, t + 4));
    // Closing it closes the keyboard and cancels a grab of it
    d.updateAim(false, false, true, false, t + 5);
    d.panelDown(DockTarget::Browser, t + 5);
    CHECK(d.startGrab(DockTarget::Browser, GrabSource::Trigger, t + 5));
    d.takeEvents();
    d.closeBrowser(t + 5.5);
    ev = d.takeEvents();
    CHECK(!d.browserOpen() && !d.keyboardOpen());
    CHECK(has(ev, Ev::GrabCancelled, DockTarget::Browser) && has(ev, Ev::KeyboardClosed) && has(ev, Ev::BrowserClosed));
    CHECK(!d.startGrab(DockTarget::Browser, GrabSource::Trigger, t + 6));  // closed: nothing to carry
    d.setKeyboardOpen(true, t + 6);
    CHECK(!d.keyboardOpen());  // no keyboard without the browser
    // Turning in-game use off closes it
    d.openBrowser(t + 7);
    d.takeEvents();
    d.configure(false, 20, t + 8);
    CHECK(!d.browserOpen() && has(d.takeEvents(), Ev::BrowserClosed));
    d.openBrowser(t + 9);
    CHECK(!d.browserOpen());  // stays closed while off
    // The Dock's timeout doesn't close the browser
    d.configure(true, 10, t + 10);
    d.openBrowser(t + 10);
    d.tick(t + 100);
    CHECK(d.browserOpen());
}

void testBrowserLoop() {
    Config c;
    // Where it hangs: auto follows the HUD
    c.attachment = Attachment::LeftWrist;
    CHECK(browserloop::browserAttachment(c) == Attachment::LeftWrist);
    c.browser.anchor = BrowserAnchor::Head;
    CHECK(browserloop::browserAttachment(c) == Attachment::Head);
    CHECK(samePose(browserloop::browserLocal(c), dockmath::headTransform(c.browser.head)));
    c.browser.anchor = BrowserAnchor::RightWrist;
    CHECK(samePose(browserloop::browserLocal(c), wristTransform(c.browser.rightWrist)));
    // A carried panel's new offset round-trips into its wrist pose (1 mm, 0.1°)
    WristPose w;
    w.x = 0.02, w.y = 0.15, w.z = -0.04, w.pitch = -130, w.yaw = 12, w.roll = -5;
    browserloop::setBrowserPlacementFrom(wristTransform(w), c);
    CHECK(!(c.browser.rightWrist != w));
    // Who points: never the hand it hangs on
    c.attachment = Attachment::Head;
    c.dock.pointerHand = DockHand::Auto;
    PointerHands h = browserloop::browserHands(c, false);
    CHECK(h.left && !h.right);
    c.browser.anchor = BrowserAnchor::Head;
    h = browserloop::browserHands(c, false);
    CHECK(h.left && h.right);
    c.attachment = Attachment::LeftWrist;
    c.browser.anchor = BrowserAnchor::Auto;  // on the HUD's (left) wrist: the right hand points
    h = browserloop::browserHands(c, false);
    CHECK(!h.left && h.right);
    c.dock.pointerHand = DockHand::Left;  // the setting names the panel's own hand: the other one points instead
    h = browserloop::browserHands(c, false);
    CHECK(!h.left && h.right);
    // Fixed in the room it hangs on no hand: either hand points (and carries it)
    h = browserloop::browserHands(c, true);
    CHECK(h.left && h.right);
    c.dock.pointerHand = DockHand::Auto;
    h = browserloop::browserHands(c, true);
    CHECK(h.left && h.right);
    // The keyboard under the panel: its top edge right under the panel's lower edge, its face turned up
    const double bh = 0.28, kh = 0.19;
    const Mat34 k = browserloop::keyboardUnderBrowser(bh, kh);
    const double topY = k.m[1][1] * kh / 2 + k.m[1][3], topZ = k.m[2][1] * kh / 2 + k.m[2][3];
    CHECK(near(topY, -bh / 2 - browserloop::kKeyboardGapM, 1e-6) && near(topZ, 0, 1e-6));
    const double bottomZ = -k.m[2][1] * kh / 2 + k.m[2][3];
    CHECK(bottomZ > 0.05);       // its lower edge toward the user
    CHECK(k.m[1][2] > 0.4);      // its face (+z) turned up
    CHECK(near(browserloop::keyboardWidthM(0.45), 0.405, 1e-9));
    CHECK(near(browserloop::keyboardWidthM(1.2), 0.6, 1e-9) && near(browserloop::keyboardWidthM(0.25), 0.35, 1e-9));
    // The panel's size is its width, but a 9:16 panel's height (its overlay is narrower)
    {
        BrowserSettings b;
        b.widthM = 0.45;
        CHECK(near(browserloop::panelWidthM(b, 1280, 802), 0.45, 1e-9));
        b.aspect = BrowserAspect::Tall;
        CHECK(near(browserloop::panelWidthM(b, 720, 1360), 0.45 * 720 / 1360, 1e-9));
    }
    // Size steps
    CHECK(near(browserloop::stepWidth(0.45, 1), 0.5, 1e-9) && near(browserloop::stepWidth(0.25, -1), 0.25, 1e-9));
    CHECK(near(browserloop::stepWidth(1.48, 1), 1.5, 1e-9));
    // Chrome's options
    BrowserSettings b;
    b.idleShutdownMin = 15;
    b.chromeCommand = "  ";
    b.headless = false;
    const SessionOptions o = browserloop::sessionOptions(b, "/home/u/.local/share/framedock/browser");
    CHECK(o.launch.profileDir == "/home/u/.local/share/framedock/browser/profile");
    CHECK(o.launch.logPath == "/home/u/.local/share/framedock/browser/chrome.log");
    CHECK(o.launch.command.size() == 3 && o.launch.command[0] == "flatpak");  // an empty command: the default
    CHECK(!o.launch.headless && near(o.idleShutdownSec, 900, 1e-9) && o.fpsCap == 30);
    CHECK(o.launch.transport == DevToolsTransport::Port);  // a flatpak Chrome never gets the pipe
    CHECK((o.launch.privateDirs ==
           std::vector<std::string> {"/home/u/.local/share/framedock", "/home/u/.local/share/framedock/browser"}));
    b.chromeCommand = "/usr/bin/chromium";
    CHECK(browserloop::sessionOptions(b, "/x/framedock/browser").launch.transport == DevToolsTransport::Pipe);
    b.transportPipe = false;
    CHECK(browserloop::sessionOptions(b, "/x/framedock/browser").launch.transport == DevToolsTransport::Port);
    CHECK(browserloop::usableHome("https://x.example/") && !browserloop::usableHome("about:blank"));
    BrowserSession::Status st;
    st.phase = BrowserSession::Phase::Ready;
    CHECK(browserloop::statusLine(st, 42) == "실행 중 (PID 42)");
    st.channel = "pipe";
    CHECK(browserloop::statusLine(st, 42) == "실행 중 (PID 42, 파이프)");
    st.channel = "port";
    CHECK(browserloop::statusLine(st, 42) == "실행 중 (PID 42, 포트)");
    // Seen only when open, live (headset worn) and placed (on its anchor)
    CHECK(browserloop::panelShowing(true, true, true));
    CHECK(!browserloop::panelShowing(false, true, true));
    CHECK(!browserloop::panelShowing(true, false, true));
    CHECK(!browserloop::panelShowing(true, true, false));
    // Chrome hears each change once: opened, headset off, back on, anchor lost, back, closed
    browserloop::SessionVisibility v;
    CHECK(!v.showing());
    CHECK(!v.update(browserloop::panelShowing(false, true, true)));  // closed from the start: nothing to say
    std::vector<bool> told;
    const auto step = [&](bool open, bool live, bool placed) {
        if (v.update(browserloop::panelShowing(open, live, placed))) told.push_back(v.showing());
    };
    step(true, true, true);    // opened
    step(true, true, true);    // still: nothing
    step(true, false, true);   // headset off
    step(true, false, true);
    step(true, true, true);    // worn again
    step(true, true, false);   // the anchor's controller lost
    step(true, true, true);    // tracked again
    step(false, true, true);   // closed
    step(false, false, false);
    CHECK((told == std::vector<bool> {true, false, true, false, true, false}));
}

void testLoopWiring() {
    // Sleep: never less than the minimum, +infinity stays
    CHECK(near(dockloop::sleepSeconds(10.5, 10), 0.5, 1e-12));
    CHECK(near(dockloop::sleepSeconds(10, 10), dockloop::kMinSleepSec, 1e-12));
    CHECK(near(dockloop::sleepSeconds(5, 10), dockloop::kMinSleepSec, 1e-12));
    CHECK(near(dockloop::sleepSeconds(10.001, 10), dockloop::kMinSleepSec, 1e-12));
    CHECK(std::isinf(dockloop::sleepSeconds(std::numeric_limits<double>::infinity(), 10)));
    CHECK(near(dockloop::sleepSeconds(std::nan(""), 10), dockloop::kMinSleepSec, 1e-12));
    CHECK(dockloop::kMinSleepSec >= 0.01 && dockloop::kMinSleepSec <= 0.02);

    // The grip: held when the watch begins is no press; a press, a release; lost while down
    using Change = dockloop::GripEdges::Change;
    dockloop::GripEdges g;
    CHECK(g.update(true, true) == Change::None);  // already held (a game's object)
    CHECK(g.update(true, true) == Change::None);
    CHECK(g.update(true, false) == Change::Released);  // let go: harmless (no grip grab started)
    CHECK(g.update(true, true) == Change::Pressed);
    CHECK(g.update(false, false) == Change::Lost);  // the watch ended while down: no release
    CHECK(g.update(false, false) == Change::None);
    CHECK(g.update(true, false) == Change::None);
    CHECK(g.update(false, true) == Change::None);  // not down: nothing lost
    // Lost cancels a grip grab through the controller
    DockController d;
    d.configure(true, 20, 0);
    d.setAimDwell(0);
    d.updateAim(true, false, 1);
    dockloop::GripEdges edges;
    const auto feed = [&](bool readable, bool down, double t) {
        switch (edges.update(readable, down)) {
            case Change::Pressed: d.grip(true, t); break;
            case Change::Released: d.grip(false, t); break;
            case Change::Lost: d.gripLost(t); break;
            case Change::None: break;
        }
    };
    feed(true, false, 1);
    feed(true, true, 1.1);
    CHECK(d.grabbing() == DockTarget::Hud);
    d.takeEvents();
    feed(false, false, 1.2);  // SteamVR made the action inactive: no release, cancelled
    std::vector<DockController::Event> ev = d.takeEvents();
    CHECK(d.grabbing() == DockTarget::None && has(ev, Ev::GrabCancelled) && !has(ev, Ev::GrabEnded));

    // Tracking during a grab: the carrying controller (the same device) and the anchor
    DockPoses poses;
    poses.hmd = {0, true, pose(0, 1.6, 0, 0, 0, 0), {}};
    poses.left = {3, true, pose(-0.2, 1.0, -0.3, 10, -30, 5), {}};
    poses.right = {4, true, pose(0.15, 1.1, -0.25, -40, -10, 0), {}};
    CHECK(dockloop::grabTracked(poses, Attachment::LeftWrist, false, 4));
    CHECK(!dockloop::grabTracked(poses, Attachment::LeftWrist, false, 5));  // the role moved to another device
    // A trigger grab goes to the hand that pressed, whichever ray is nearer; unknown: the nearest ray's hand
    CHECK(!dockloop::pressingHandLeft(poses, 4, true) && dockloop::pressingHandLeft(poses, 3, false));
    CHECK(dockloop::pressingHandLeft(poses, 0xffffffff, true) && !dockloop::pressingHandLeft(poses, 9, false));
    DockPoses lost = poses;
    lost.left.valid = false;
    CHECK(!dockloop::grabTracked(lost, Attachment::LeftWrist, false, 4));  // the anchor
    CHECK(dockloop::grabTracked(lost, Attachment::Head, false, 4));
    lost = poses;
    lost.right.valid = false;
    CHECK(!dockloop::grabTracked(lost, Attachment::Head, false, 4));  // the carrying hand

    // A release: the HUD on the left wrist, carried by the right hand
    using Outcome = dockloop::GrabRelease::Outcome;
    const Mat34 hudWorld = dockmath::multiply(poses.left.pose, wristTransform(WristPose()));
    const Mat34 grab = dockmath::grabOffset(poses.right.pose, hudWorld);
    // Cancelled: put back whatever the poses say
    CHECK(dockloop::releaseGrab(false, poses, Attachment::LeftWrist, false, 4, hudWorld, grab).outcome ==
          Outcome::Cancelled);
    // Let go in place (a slow click): put back, nothing to store
    CHECK(dockloop::releaseGrab(true, poses, Attachment::LeftWrist, false, 4, hudWorld, grab).outcome ==
          Outcome::InPlace);
    DockPoses jitter = poses;  // 3 mm and 0.5°: still in place
    jitter.right.pose =
        dockmath::multiply(pose(0.003, 0, 0, 0, 0, 0), dockmath::multiply(poses.right.pose, pose(0, 0, 0, 0.5, 0, 0)));
    jitter.left.pose = dockmath::multiply(pose(0, 0.02, 0, 0, 0, 0), poses.left.pose);  // the anchor wrist moved
    CHECK(dockloop::releaseGrab(true, jitter, Attachment::LeftWrist, false, 4, hudWorld, grab).outcome ==
          Outcome::InPlace);
    // Moved 1 cm: kept, at the anchor-relative pose
    DockPoses moved = poses;
    moved.right.pose = dockmath::multiply(pose(0.01, 0, 0, 0, 0, 0), poses.right.pose);
    dockloop::GrabRelease r = dockloop::releaseGrab(true, moved, Attachment::LeftWrist, false, 4, hudWorld, grab);
    CHECK(r.keep() && samePose(r.local, dockmath::localAfterGrab(poses.left.pose, moved.right.pose, grab)));
    // Turned 2° in place: kept
    moved.right.pose = dockmath::multiply(poses.right.pose, pose(0, 0, 0, 0, 2, 0));
    CHECK(dockloop::releaseGrab(true, moved, Attachment::LeftWrist, false, 4, hudWorld, grab).keep());
    // Moved but not tracked: put back
    moved.left.valid = false;
    CHECK(dockloop::releaseGrab(true, moved, Attachment::LeftWrist, false, 4, hudWorld, grab).outcome ==
          Outcome::Untracked);
    // The dead zone itself
    CHECK(!dockmath::movedByGrab(hudWorld, hudWorld));
    CHECK(near(dockmath::angleBetweenDeg(pose(0, 0, 0, 0, 0, 0), pose(0, 0, 0, 30, 0, 0)), 30, 1e-3));
    CHECK(near(dockmath::angleBetweenDeg(pose(0, 0, 0, 10, 20, 0), pose(0, 0, 0, 10, 20, 0)), 0, 1e-3));
    CHECK(near(dockmath::angleBetweenDeg(pose(0, 0, 0, 10, 20, 0), pose(0, 0, 0, 10, 20.3, 0)), 0.3, 2e-3));
    CHECK(dockmath::movedByGrab(pose(0, 0, 0, 0, 0, 0), pose(0.0051, 0, 0, 0, 0, 0)));
    CHECK(!dockmath::movedByGrab(pose(0, 0, 0, 0, 0, 0), pose(0.0049, 0, 0, 0, 0.9, 0)));
    CHECK(dockmath::movedByGrab(pose(0, 0, 0, 0, 0, 0), pose(0, 0, 0, 0, 0, 1.1)));
}

// SteamVR Input's manifest for the grip: valid JSON, the grab action in its set, the Frame controller's grip bound
// for both hands, written once and then left alone while unchanged
void testManifest() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("dock-manifest-" + std::to_string(::getpid())) / "input";
    int written = 0;
    std::string error;
    const std::string manifest = dockinput::writeManifest(dir.string(), written, error);
    CHECK(!manifest.empty() && written == static_cast<int>(dockinput::manifestFiles().size()));
    CHECK(manifest == (dir / "actions.json").string());
    for (const auto& file : dockinput::manifestFiles()) {
        std::ifstream in(dir / file.name);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        JsonValue root;
        std::string parseError;
        CHECK(parseJson(text, root, parseError) && root.isObject());
        if (file.name == "actions.json") {
            const JsonValue* actions = root.get("actions");
            CHECK(actions != nullptr && actions->type == JsonValue::Type::Array && actions->items.size() == 1);
            CHECK(text.find(dockinput::kGrabAction) != std::string::npos);
            CHECK(text.find("bindings_frame_controller.json") != std::string::npos);
        }
        if (file.name == "bindings_frame_controller.json") {
            CHECK(text.find("/user/hand/left/input/grip") != std::string::npos);
            CHECK(text.find("/user/hand/right/input/grip") != std::string::npos);
            CHECK(text.find("/input/trigger") == std::string::npos);  // never the trigger: the game keeps it
        }
    }
    CHECK(!dockinput::writeManifest(dir.string(), written, error).empty() && written == 0);
    std::ofstream(dir / "actions.json") << "{}";
    CHECK(!dockinput::writeManifest(dir.string(), written, error).empty() && written == 1);
    std::filesystem::remove_all(dir.parent_path());
}

}  // namespace

void testReviewHelpers() {
    // The browser panel in view: facing the head and within the field of view
    const Mat34 head = pose(0, 0, 0, 0, 0, 0);  // looking down −z
    CHECK(browserloop::panelInView(pose(0, 0, -1, 0, 0, 0), 0.45, 0.3, head));
    CHECK(!browserloop::panelInView(pose(0, 0, -1, 180, 0, 0), 0.45, 0.3, head));  // its back to us
    CHECK(!browserloop::panelInView(pose(0, 0, 1, 180, 0, 0), 0.45, 0.3, head));   // behind the head
    const double a = 50 * M_PI / 180;
    CHECK(browserloop::panelInView(pose(std::sin(a), 0, -std::cos(a), -50, 0, 0), 0.45, 0.3, head));  // 50° aside
    CHECK(!browserloop::panelInView(pose(1, 0, 0, -90, 0, 0), 0.45, 0.3, head));  // 90° aside, facing us
    CHECK(!browserloop::panelInView(pose(0, -1, -0.05, 0, -88, 0), 0.45, 0.3, head));  // flat under the chin
    // In view at once, out of view only after kOutOfViewSec of it
    browserloop::InViewTimer timer;
    CHECK(timer.inView());
    CHECK(!timer.update(false, 10.0));
    CHECK(timer.inView() && timer.changesAt() == 10.0 + browserloop::kOutOfViewSec);
    CHECK(!timer.update(true, 10.2));  // a glance away: nothing changed
    CHECK(!timer.update(false, 10.3));
    CHECK(!timer.update(false, 10.79));
    CHECK(timer.update(false, 10.8) && !timer.inView());
    CHECK(timer.changesAt() > 1e299);
    CHECK(timer.update(true, 10.81) && timer.inView());

    // The check can't run (headset off, no HMD pose) with a countdown running: no deadline in the past is left
    // behind, and the countdown starts over once readings come again
    CHECK(!timer.update(false, 20.0));
    CHECK(timer.changesAt() == 20.0 + browserloop::kOutOfViewSec);
    timer.pause();
    CHECK(timer.changesAt() > 1e299 && timer.inView());
    CHECK(!timer.update(false, 30.0) && timer.inView());  // a new reading: a new countdown, not an instant change
    CHECK(timer.changesAt() == 30.0 + browserloop::kOutOfViewSec);
    // The next aim check moves up to the countdown's end, never back before now, never later than it was
    CHECK(browserloop::alignAimWithInView(30.1, timer, 30.0) == 30.1);
    CHECK(browserloop::alignAimWithInView(31.0, timer, 30.0) == 30.0 + browserloop::kOutOfViewSec);
    CHECK(browserloop::alignAimWithInView(31.0, timer, 31.0) == 31.0);  // an ended countdown is no deadline
    timer.pause();
    CHECK(browserloop::alignAimWithInView(31.0, timer, 30.0) == 31.0);

    // The main loop's wakes for it, simulated: the panel goes out of view, then the headset comes off for a minute.
    // The in-view check runs only with the aim check (at nextAim); when it can't run the countdown is paused. Even
    // with changesAt() in the wake set, every wake must be in the future (no 10 ms wake loop) and the wakes few.
    {
        browserloop::InViewTimer sim;
        double nextAim = 0, t = 0;
        int wakes = 0, pastWakes = 0;
        bool changed = false;
        while (t < 70.0) {
            const bool worn = t < 0.3 || t >= 60.0;  // headset off from 0.3 s to 60 s
            if (worn && t >= nextAim) {
                changed |= sim.update(t < 0.2, t);  // looked away at 0.2 s
                nextAim = browserloop::alignAimWithInView(t + 0.1, sim, t);
            } else if (!worn) {
                sim.pause();
            }
            double wakeAt = t + 1.0;  // some other once-a-second deadline
            if (worn) wakeAt = std::min(wakeAt, nextAim);  // as the loop: the aim check only while it can run
            wakeAt = std::min(wakeAt, sim.changesAt());
            if (wakeAt <= t) {
                ++pastWakes;
                wakeAt = t + 0.01;  // the loop's minimum sleep
            }
            t = wakeAt;
            ++wakes;
        }
        CHECK(pastWakes == 0);
        CHECK(wakes < 60 + 2 * 10 * 10 + 20);  // ~1 Hz while off, ~10 Hz worn: never ~100 Hz
        CHECK(changed && !sim.inView());       // out of view once the headset came back and kOutOfViewSec passed
    }

    // A failure retried every 0.1 s logs once a minute, a new message at once, counting what it left out
    LogLimiter log;
    int left = -1;
    CHECK(log.allow("SetOverlayAlpha", "VROverlayError_InvalidHandle", 0.0, left) && left == 0);
    for (int i = 1; i <= 50; ++i) CHECK(!log.allow("SetOverlayAlpha", "VROverlayError_InvalidHandle", i * 0.1, left));
    CHECK(log.allow("ShowOverlay", "VROverlayError_InvalidHandle", 5.0, left));  // another key: its own
    CHECK(log.allow("SetOverlayAlpha", "VROverlayError_RequestFailed", 6.0, left) && left == 0);  // another message
    CHECK(!log.allow("SetOverlayAlpha", "VROverlayError_RequestFailed", 7.0, left));
    CHECK(log.allow("SetOverlayAlpha", "VROverlayError_RequestFailed", 6.0 + LogLimiter::kRepeatSec, left) && left == 1);
    log.clear("SetOverlayAlpha");  // worked again: the next failure is logged at once
    CHECK(log.allow("SetOverlayAlpha", "VROverlayError_RequestFailed", 70.0, left));

    // A hidden panel's texture is freed after kIdleTextureSec
    CHECK(!dockloop::idleTextureDue(true, true, 0, 1000));   // shown
    CHECK(!dockloop::idleTextureDue(false, false, 0, 1000));  // none
    CHECK(!dockloop::idleTextureDue(false, true, 100, 100 + dockloop::kIdleTextureSec - 1));
    CHECK(dockloop::idleTextureDue(false, true, 100, 100 + dockloop::kIdleTextureSec));
    // Not hidden by a HideOverlay that worked (never shown, ShowOverlay failing): never idle, not freed every tick
    CHECK(!dockloop::idleTextureDue(false, true, -1, 1000));
    CHECK(dockloop::idleTextureDue(false, true, 0, dockloop::kIdleTextureSec));  // hidden at time 0 still counts

    // The exit: input and overlays first, the colour layer and SteamVR next, Chrome last
    std::string order;
    dockloop::ExitSteps steps;
    steps.browserOff = [&] { order += "browser "; };
    steps.vrOff = [&] { order += "vr "; };
    steps.colorOff = [&] { order += "color "; };
    steps.saveConfig = [&] { order += "save "; };
    steps.hideOverlays = [&] { order += "hide "; };
    steps.releaseInput = [&] { order += "input "; };
    dockloop::runExitSteps(steps);
    CHECK(order == "input hide save color vr browser ");
    steps.saveConfig = nullptr;  // an empty step is skipped
    order.clear();
    dockloop::runExitSteps(steps);
    CHECK(order == "input hide color vr browser ");
    CHECK(dockloop::kExitCodeDeviceLost != 0 && dockloop::kExitCodeDeviceLost != 3);
}

void testEventPolling() {
    using dockloop::EventPolling;
    const double drain = dockloop::kEventDrainSec;
    EventPolling polling;
    // Nothing interactive, the dashboard closed: only the full (2 Hz) poll looks at our overlays
    CHECK(!polling.wanted(0) && !polling.draining(0));
    CHECK(!polling.polls(EventPolling::Dock, false, 0) && polling.polls(EventPolling::Dock, true, 0));
    // The laser on the Dock panel: polled every wake, the others not
    polling.setInteractive(EventPolling::Dock, true, 1.0);
    CHECK(polling.polls(EventPolling::Dock, false, 1.1));
    CHECK(!polling.polls(EventPolling::Browser, false, 1.1) && !polling.polls(EventPolling::Hud, false, 1.1));
    CHECK(!polling.wanted(1.1));  // interactive is the caller's own reason (dockInput), not an extra one
    // Taken away: still polled for kEventDrainSec (the Up / Leave SteamVR sent last), with wakes for it, then not
    polling.setInteractive(EventPolling::Dock, false, 2.0);
    CHECK(polling.polls(EventPolling::Dock, false, 2.0) && polling.polls(EventPolling::Dock, false, 2.0 + drain - 0.01));
    CHECK(polling.wanted(2.5) && polling.draining(2.5));
    CHECK(!polling.polls(EventPolling::Browser, false, 2.5));  // only that overlay
    CHECK(!polling.polls(EventPolling::Dock, false, 2.0 + drain) && !polling.wanted(2.0 + drain));
    // Setting it off again (already off) starts no new drain
    polling.setInteractive(EventPolling::Dock, false, 10.0);
    CHECK(!polling.polls(EventPolling::Dock, false, 10.0));
    // Each overlay drains on its own
    polling.setInteractive(EventPolling::Browser, true, 20.0);
    polling.setInteractive(EventPolling::Keyboard, true, 20.0);
    polling.setInteractive(EventPolling::Browser, false, 20.5);
    CHECK(polling.polls(EventPolling::Browser, false, 21.0) && polling.polls(EventPolling::Keyboard, false, 21.0));
    polling.setInteractive(EventPolling::Keyboard, false, 21.0);
    CHECK(!polling.polls(EventPolling::Browser, false, 20.5 + drain) && polling.polls(EventPolling::Keyboard, false, 21.6));
    CHECK(!polling.polls(EventPolling::Keyboard, false, 21.0 + drain));
    // The dashboard open (read each wake): every overlay each wake, with no drain wakes of its own; closed: every
    // overlay kEventDrainSec more (a Leave from its laser)
    polling.setDashboardOpen(true, 30.0);
    CHECK(polling.wanted(30.0) && !polling.draining(30.0));
    for (int o = 0; o < EventPolling::kCount; ++o) CHECK(polling.polls(static_cast<EventPolling::Overlay>(o), false, 30.0));
    polling.setDashboardOpen(true, 31.0);  // still open: nothing changes
    polling.setDashboardOpen(false, 40.0);
    CHECK(polling.wanted(40.5) && polling.draining(40.5) && polling.polls(EventPolling::Hud, false, 40.5));
    CHECK(!polling.wanted(40.0 + drain) && !polling.polls(EventPolling::Hud, false, 40.0 + drain));
    polling.setDashboardOpen(false, 50.0);  // closed and still closed: no new drain
    CHECK(!polling.wanted(50.0));
}

int main() {
    testInverseAndDecompose();
    testRayHit();
    testGrabWrist();
    testGrabHead();
    testPointerHands();
    testClickOpenCollapse();
    testLingerAndLeave();
    testTimeoutAndHidden();
    testAdjust();
    testGrabs();
    testDwell();
    testWakeNeverPast();
    testLiveAndGripLost();
    testKeyboardKeepsOpen();
    testBrowserPanel();
    testBrowserLoop();
    testLoopWiring();
    testManifest();
    testReviewHelpers();
    testEventPolling();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("dock: all passed\n");
    return 0;
}
