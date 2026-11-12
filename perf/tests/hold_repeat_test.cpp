// Hold to repeat on the Placement page: the timing (once on the press, again after 0.4 s, then every 0.1 s, faster
// after 2 s), which buttons repeat, and that a repeat stops on release, leaving the button or the tab, a page switch
// and a new press. No SteamVR.
#include "config.h"
#include "draw.h"
#include "hold_repeat.h"
#include "settings_panel.h"

#include <cmath>
#include <cstdio>
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

/** @return the times HoldRepeat fires when polled every step seconds from the press at 0 until end */
std::vector<double> fires(double step, double end) {
    HoldRepeat r;
    r.start(0.0);
    std::vector<double> out;
    for (int i = 1; i * step <= end + 1e-9; ++i) {
        if (r.due(i * step)) out.push_back(i * step);
    }
    return out;
}

void testTiming() {
    HoldRepeat r;
    CHECK(!r.active() && std::isinf(r.nextAt()) && !r.due(100.0));
    r.start(10.0);
    CHECK(r.active());
    CHECK(std::fabs(r.nextAt() - 10.4) < 1e-9);
    CHECK(!r.due(10.0) && !r.due(10.39));
    CHECK(r.due(10.4));
    CHECK(!r.due(10.45));
    CHECK(r.due(10.5));
    r.stop();
    CHECK(!r.active() && !r.due(10.6) && std::isinf(r.nextAt()));

    // Polled every 10 ms for 1 s: repeats at 0.4, 0.5, ..., 1.0 (7), the press itself is the caller's
    const std::vector<double> first = fires(0.01, 1.0);
    CHECK(first.size() == 7);
    if (first.size() == 7) {
        for (size_t i = 0; i < first.size(); ++i) CHECK(std::fabs(first[i] - (0.4 + 0.1 * i)) < 0.011);
    }
    // After 2 s every 50 ms: 0.4 .. 2.0 by 0.1 (17), then 2.05 .. 3.0 by 0.05 (19)
    const std::vector<double> held = fires(0.01, 3.0);
    CHECK(held.size() >= 34 && held.size() <= 37);
    int fast = 0;
    for (size_t i = 1; i < held.size(); ++i) {
        const double gap = held[i] - held[i - 1];
        if (held[i - 1] >= 2.05) {
            CHECK(std::fabs(gap - 0.05) < 0.011);
            ++fast;
        } else if (held[i] < 2.0) {
            CHECK(std::fabs(gap - 0.1) < 0.011);
        }
    }
    CHECK(fast >= 15);
    // A late caller gets one repeat, not a burst, and the cadence starts again from then
    HoldRepeat late;
    late.start(0.0);
    CHECK(late.due(1.0));
    CHECK(!late.due(1.05));
    CHECK(late.due(1.1));

    // Safety stop: no pointer event for kIdleStopSec (a lost release) ends it; a pointer event keeps it going
    HoldRepeat lost;
    lost.start(0.0);
    double lastFire = 0.0;
    for (int i = 1; i <= 1500; ++i) {
        if (lost.due(i * 0.01)) lastFire = i * 0.01;
    }
    CHECK(!lost.active() && std::isinf(lost.nextAt()));
    CHECK(lastFire <= HoldRepeat::kIdleStopSec + 1e-9 && lastFire > HoldRepeat::kIdleStopSec - 0.1);
    HoldRepeat moving;
    moving.start(0.0);
    moving.touch(9.0);
    for (int i = 1; i <= 1500; ++i) moving.due(i * 0.01);
    CHECK(moving.active());  // 15 s, but the last event was at 9 s
    CHECK(!moving.due(19.2) && !moving.active());
    moving.touch(30.0);  // after the stop: nothing starts again
    CHECK(!moving.active() && !moving.due(30.5));
}

/** The panel on the Placement page with a config the actions change. */
struct Rig {
    FontSet fonts;
    SettingsPanel panel {fonts};
    Config cfg;
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};

    Rig() {
        fonts.load(cfg.fontPath, cfg.boldFontPath);
        panel.render(cfg, service, {});
    }
    void center(SettingsAction a, double& x, double& y) { CHECK(panel.buttonCenter(a, x, y)); }
    /** Presses a, holds it until end (polling every 10 ms like a busy main loop), applying every repeat. */
    int hold(SettingsAction a, double start, double end) {
        double x = 0, y = 0;
        center(a, x, y);
        int applied = 0;
        const SettingsAction first = panel.pointerDown(x, y, start);
        CHECK(first == a);
        if (applySettingsAction(first, cfg)) ++applied;
        for (int i = 1; start + i * 0.01 <= end + 0.005; ++i) {
            const double t = start + i * 0.01;
            const SettingsAction again = panel.takeRepeat(t);
            if (again == SettingsAction::None) continue;
            CHECK(again == a);
            if (applySettingsAction(again, cfg)) ++applied;
        }
        return applied;
    }
};

void testPanel() {
    Rig rig;
    // Which buttons repeat
    for (const SettingsAction a :
         {SettingsAction::SizeDown, SettingsAction::SizeUp, SettingsAction::AlphaDown, SettingsAction::AlphaUp,
          SettingsAction::FadeAngleDown, SettingsAction::FadeAngleUp}) {
        CHECK(SettingsPanel::repeatsWhileHeld(a));
    }
    for (const SettingsAction a :
         {SettingsAction::None, SettingsAction::ShowOn, SettingsAction::AttachLeft, SettingsAction::PresetLeftTop,
          SettingsAction::PresetWrist, SettingsAction::FadeOn, SettingsAction::Reset, SettingsAction::Quit, SettingsAction::ClockOff,
          SettingsAction::PageLayout, SettingsAction::ProfileUse}) {
        CHECK(!SettingsPanel::repeatsWhileHeld(a));
    }

    // Size + held for 1 s: once on the press + 7 repeats = 8 steps of 2 cm. Released: no more
    const double w0 = rig.cfg.widthM;
    const int upSteps = rig.hold(SettingsAction::SizeUp, 1.0, 2.0);
    if (upSteps != 8) std::printf("size steps %d, width %.3f -> %.3f\n", upSteps, w0, rig.cfg.widthM);
    CHECK(upSteps == 8);
    CHECK(std::fabs(rig.cfg.widthM - (w0 + 8 * 0.02)) < 1e-6);
    CHECK(rig.panel.repeating() && std::isfinite(rig.panel.nextRepeatAt()));
    double x = 0, y = 0;
    rig.center(SettingsAction::SizeUp, x, y);
    rig.panel.pointerUp(x, y);
    CHECK(!rig.panel.repeating() && std::isinf(rig.panel.nextRepeatAt()));
    CHECK(rig.panel.takeRepeat(5.0) == SettingsAction::None);

    // A tap: just the press
    CHECK(rig.hold(SettingsAction::SizeUp, 10.0, 10.2) == 1);
    rig.panel.pointerUp(x, y);

    // Leaving the tab stops it
    rig.hold(SettingsAction::AlphaDown, 20.0, 20.5);
    CHECK(rig.panel.repeating());
    rig.panel.pointerLeave();
    CHECK(!rig.panel.repeating() && rig.panel.takeRepeat(21.0) == SettingsAction::None);

    // Sliding off the held button stops it; moving inside it doesn't
    rig.hold(SettingsAction::AlphaUp, 30.0, 30.5);
    rig.center(SettingsAction::AlphaUp, x, y);
    rig.panel.pointerMove(x + 3, y + 3);
    CHECK(rig.panel.repeating());
    double ox = 0, oy = 0;
    rig.center(SettingsAction::AlphaDown, ox, oy);
    rig.panel.pointerMove(ox, oy);
    CHECK(!rig.panel.repeating() && rig.panel.takeRepeat(31.0) == SettingsAction::None);
    rig.panel.pointerUp(ox, oy);

    // A page switch (the panel's own, or showPage) stops it
    rig.hold(SettingsAction::SizeDown, 40.0, 40.5);
    rig.center(SettingsAction::PageLayout, x, y);
    CHECK(rig.panel.pointerDown(x, y, 40.6) == SettingsAction::None && rig.panel.page() == SettingsPage::Layout);
    CHECK(!rig.panel.repeating());
    rig.panel.pointerUp(x, y);
    rig.panel.showPage(SettingsPage::Placement);
    rig.panel.render(rig.cfg, rig.service, {});
    rig.hold(SettingsAction::AlphaUp, 50.0, 50.5);
    rig.panel.showPage(SettingsPage::Color);
    CHECK(!rig.panel.repeating());
    rig.panel.showPage(SettingsPage::Placement);
    rig.panel.render(rig.cfg, rig.service, {});

    // A button that doesn't repeat starts nothing, and a new press ends a held one
    rig.center(SettingsAction::PresetLeftTop, x, y);
    CHECK(rig.panel.pointerDown(x, y, 60.0) == SettingsAction::PresetLeftTop);
    CHECK(!rig.panel.repeating() && rig.panel.takeRepeat(61.0) == SettingsAction::None);
    rig.panel.pointerUp(x, y);
    rig.hold(SettingsAction::AlphaDown, 70.0, 70.5);
    rig.center(SettingsAction::Reset, x, y);
    CHECK(rig.panel.pointerDown(x, y, 70.6) == SettingsAction::None && !rig.panel.repeating());  // Reset: two taps
    rig.panel.pointerUp(x, y);
    CHECK(rig.panel.pointerDown(x, y, 70.8) == SettingsAction::Reset && !rig.panel.repeating());
    rig.panel.pointerUp(x, y);

    // Size at its limit keeps repeating but changes nothing (the caller saves nothing new)
    rig.cfg.widthM = 1.0;
    rig.panel.render(rig.cfg, rig.service, {});
    CHECK(rig.hold(SettingsAction::SizeUp, 80.0, 81.0) == 0);
    rig.panel.pointerLeave();

    // Wrist: the fade angle repeats too (5° a step, 35–90°)
    rig.cfg = Config();
    rig.cfg.attachment = Attachment::LeftWrist;
    rig.panel.render(rig.cfg, rig.service, {});
    const double fade0 = rig.cfg.wristFadeEndDeg;
    CHECK(rig.hold(SettingsAction::FadeAngleDown, 90.0, 90.6) == 4);  // press + 0.4, 0.5, 0.6
    CHECK(std::fabs(rig.cfg.wristFadeEndDeg - (fade0 - 4 * 5)) < 1e-9);
    rig.panel.pointerLeave();
}

// The main loop's safety checks: a due repeat first asks whether the tab is still visible (a hidden tab stops it at
// once, not on the next 2 Hz tick), and a hold with no pointer event for 10 s stops by itself
void testSafetyStops() {
    Rig rig;
    double x = 0, y = 0;
    rig.center(SettingsAction::SizeUp, x, y);
    CHECK(rig.panel.pointerDown(x, y, 1.0) == SettingsAction::SizeUp);
    int asked = 0;
    bool visible = true;
    const auto check = [&] {
        ++asked;
        return visible;
    };
    CHECK(rig.panel.takeRepeat(1.2, check) == SettingsAction::None && asked == 0);  // not due: not even asked
    CHECK(rig.panel.takeRepeat(1.4, check) == SettingsAction::SizeUp && asked == 1);
    visible = false;  // the dashboard closed between two 2 Hz ticks
    CHECK(rig.panel.takeRepeat(1.5, check) == SettingsAction::None && asked == 2);
    CHECK(!rig.panel.repeating() && std::isinf(rig.panel.nextRepeatAt()));
    CHECK(rig.panel.takeRepeat(1.6, check) == SettingsAction::None && asked == 2);
    rig.panel.pointerUp(x, y);

    // Held with no pointer event at all: repeats until 10 s after the press, then stops on its own
    visible = true;
    CHECK(rig.panel.pointerDown(x, y, 100.0) == SettingsAction::SizeUp);
    int repeats = 0;
    for (int i = 1; i <= 1500; ++i) repeats += rig.panel.takeRepeat(100.0 + i * 0.01, check) != SettingsAction::None;
    CHECK(repeats > 100 && !rig.panel.repeating());
    rig.panel.pointerUp(x, y);
    // With pointer events coming in (SteamVR's moves while held), it keeps going
    CHECK(rig.panel.pointerDown(x, y, 200.0) == SettingsAction::SizeUp);
    for (int i = 1; i <= 1500; ++i) {
        const double t = 200.0 + i * 0.01;
        if (i % 50 == 0) rig.panel.pointerSeen(t);
        rig.panel.takeRepeat(t, check);
    }
    CHECK(rig.panel.repeating());
    rig.panel.pointerUp(x, y);
}

}  // namespace

int main() {
    testTiming();
    testPanel();
    testSafetyStops();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("hold repeat OK\n");
    return 0;
}
