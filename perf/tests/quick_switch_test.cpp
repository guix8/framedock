// The footer's quick switch: the first profiles as one-tap buttons on every page, the active one selected, the ids
// they stand for, no stale buttons when profiles go away, long names cut to fit. No SteamVR.
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "settings_panel.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

SettingsAction quick(size_t i) {
    return static_cast<SettingsAction>(static_cast<int>(SettingsAction::QuickProfile0) + static_cast<int>(i));
}

ProfilesUiState profiles(size_t n) {
    ProfilesUiState p;
    for (size_t i = 0; i < n; ++i) {
        const std::string id = "profile-" + std::to_string(i + 1);
        p.items.push_back({id, i == 0 ? "간단 A" : "Profile " + std::to_string(i + 1), i == 0});
    }
    return p;
}

void testQuickSwitch() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    double x = 0, y = 0;

    // No profiles: no buttons
    panel.render(cfg, service, profiles(0));
    for (size_t i = 0; i < SettingsPanel::kQuickProfiles; ++i) CHECK(!panel.buttonCenter(quick(i), x, y));

    // Eight profiles: the first six, on every page, each a tap that comes back as its action
    ProfilesUiState eight = profiles(8);
    eight.activeId = "profile-3";
    for (const SettingsPage page : {SettingsPage::Placement, SettingsPage::Layout, SettingsPage::Profiles,
                                    SettingsPage::Color}) {
        panel.showPage(page);
        panel.render(cfg, service, eight);
        double lastX = 0, autostartX = 0, autostartY = 0, quitX = 0, quitY = 0;
        CHECK(panel.buttonCenter(SettingsAction::AutostartOn, autostartX, autostartY));
        CHECK(panel.buttonCenter(SettingsAction::Quit, quitX, quitY));
        for (size_t i = 0; i < SettingsPanel::kQuickProfiles; ++i) {
            CHECK(panel.buttonCenter(quick(i), x, y));
            CHECK(x > lastX && x < autostartX && std::abs(y - quitY) < 1);  // left to right, left of autostart
            lastX = x;
            const SettingsAction a = panel.pointerDown(x, y, 1.0);
            panel.pointerUp(x, y);
            CHECK(a == quick(i));
            CHECK(panel.quickProfileId(a) == "profile-" + std::to_string(i + 1));
            CHECK(!panel.repeating());  // a tap, not a held button
        }
        CHECK(panel.page() == page);  // a quick switch doesn't change the page
    }
    CHECK(panel.quickProfileId(SettingsAction::ProfileUse).empty());

    // Fewer profiles: the buttons that went away aren't hittable and stand for nothing
    panel.showPage(SettingsPage::Placement);
    panel.render(cfg, service, profiles(2));
    CHECK(panel.buttonCenter(quick(0), x, y) && panel.buttonCenter(quick(1), x, y));
    for (size_t i = 2; i < SettingsPanel::kQuickProfiles; ++i) {
        CHECK(!panel.buttonCenter(quick(i), x, y));
        CHECK(panel.quickProfileId(quick(i)).empty());
    }

    // A long name doesn't push the strip into autostart (cut with an ellipsis; drawing it is enough here)
    ProfilesUiState longNames = profiles(6);
    for (auto& item : longNames.items) item.name = "아주아주아주긴프로필이름입니다XYZ";
    longNames.activeId = "profile-6";
    panel.render(cfg, service, longNames);
    double autostartX = 0, autostartY = 0;
    CHECK(panel.buttonCenter(SettingsAction::AutostartOn, autostartX, autostartY));
    CHECK(panel.buttonCenter(quick(5), x, y) && x < autostartX);
}

// A profile action's result (a failed save after a quick switch, say) shows on the footer's bottom line on every page,
// for a while
void testFooterStatus() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    panel.render(cfg, service, profiles(2));
    const std::string usual = panel.footerNote();
    CHECK(!usual.empty());
    const std::string failed = "'간단 A'을(를) 사용하지 못했어요: 설정 파일을 저장하지 못했어요";
    panel.showActionStatus(failed, true, 100.0);
    for (const SettingsPage page : {SettingsPage::Placement, SettingsPage::Layout, SettingsPage::Profiles,
                                    SettingsPage::Color}) {
        panel.showPage(page);
        panel.render(cfg, service, profiles(2));
        CHECK(panel.footerNote() == failed);
    }
    CHECK(!panel.tick(100.0 + SettingsPanel::kActionStatusSec + 1));  // an error stays longer
    CHECK(panel.tick(100.0 + SettingsPanel::kActionErrorSec + 0.5));  // then goes (redraw)
    panel.render(cfg, service, profiles(2));
    CHECK(panel.footerNote() == usual);
    // A success goes sooner
    panel.showActionStatus("사용 중: 간단 A", false, 200.0);
    panel.render(cfg, service, profiles(2));
    CHECK(panel.footerNote() == "사용 중: 간단 A");
    CHECK(panel.tick(200.0 + SettingsPanel::kActionStatusSec + 0.5));
    panel.render(cfg, service, profiles(2));
    CHECK(panel.footerNote() == usual);
    // Autostart's failure still comes first
    panel.showActionStatus(failed, true, 300.0);
    const AutostartStatus broken {AutostartStatus::State::Enabled, false, true};
    panel.render(cfg, service, profiles(2));
    CHECK(panel.footerNote() == failed);
    panel.render(cfg, broken, profiles(2));
    CHECK(panel.footerNote() != failed);
    // "" takes it away at once
    panel.showActionStatus("", false, 301.0);
    panel.render(cfg, service, profiles(2));
    CHECK(panel.footerNote() == usual);
}

// Owner's rule: a quick-switch tap applies at once when nothing would be lost; when it would throw away unsaved
// changes (the target's useLoses), the first tap arms it with a hint on the footer and a second tap on the same
// button within the confirm window applies it. Tapping elsewhere or waiting disarms it
void testQuickSwitchSecondTap() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    const std::string hint = uiText(Language::Ko).quickTapAgain;
    ProfilesUiState ui = profiles(3);  // 간단 A (active, layout-only), 프로필 2, 프로필 3
    ui.activeId = "profile-1";
    ui.items[1].useLoses.placement = true;  // the HUD was moved; 프로필 2 carries a placement
    panel.render(cfg, service, ui);
    const std::string usual = panel.footerNote();
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    CHECK(panel.buttonCenter(quick(0), x0, y0) && panel.buttonCenter(quick(1), x1, y1) &&
          panel.buttonCenter(quick(2), x2, y2));
    const auto tap = [&](double x, double y, double t) {
        panel.setProfilesState(ui);  // as main does before every press
        const SettingsAction a = panel.pointerDown(x, y, t);
        panel.pointerUp(x, y);
        panel.render(cfg, service, ui);
        return a;
    };

    // Nothing to lose: at once
    CHECK(tap(x2, y2, 10) == quick(2));
    CHECK(panel.footerNote() == usual);
    // Something to lose: the first tap arms (hint on the footer), the second applies
    CHECK(tap(x1, y1, 20) == SettingsAction::None);
    CHECK(panel.footerNote() == hint);
    CHECK(tap(x1, y1, 21) == quick(1));
    CHECK(panel.footerNote() != hint);
    // ...the arm is spent
    CHECK(tap(x1, y1, 22) == SettingsAction::None);
    // Tapping elsewhere disarms: another quick button applies (nothing to lose there), the first one arms again
    CHECK(tap(x2, y2, 22.5) == quick(2));
    CHECK(tap(x1, y1, 23) == SettingsAction::None);
    // Timing out disarms (tick takes the hint away)
    CHECK(panel.tick(23 + SettingsPanel::kQuitConfirmSec + 0.5));
    panel.render(cfg, service, ui);
    CHECK(panel.footerNote() == usual);
    CHECK(tap(x1, y1, 30) == SettingsAction::None);  // arms again, not a confirm
    CHECK(tap(x1, y1, 30 + SettingsPanel::kQuitConfirmSec + 0.5) == SettingsAction::None);  // too late: armed again
    CHECK(tap(x1, y1, 34.5) == quick(1));
    // A page switch disarms too
    CHECK(tap(x1, y1, 40) == SettingsAction::None);
    double px = 0, py = 0;
    CHECK(panel.buttonCenter(SettingsAction::PageLayout, px, py));
    tap(px, py, 40.5);
    CHECK(panel.page() == SettingsPage::Layout && panel.footerNote() == usual);
    CHECK(panel.buttonCenter(quick(1), x1, y1));
    CHECK(tap(x1, y1, 41) == SettingsAction::None);
    CHECK(tap(x1, y1, 41.5) == quick(1));  // the same on the Layout page

    // The active profile with unsaved changes (Use on it puts the saved one back): the same rule
    ui.items[0].useLoses.layout = true;
    CHECK(tap(x0, y0, 50) == SettingsAction::None);
    CHECK(tap(x0, y0, 50.5) == quick(0));

    // The button stands for another profile by the second tap (the list changed): that tap arms, it doesn't apply
    ui.items[1].useLoses.layout = true;
    CHECK(tap(x1, y1, 60) == SettingsAction::None);
    ProfilesUiState shifted = ui;
    shifted.items.erase(shifted.items.begin());  // 프로필 2 is gone from the first place: button 1 is 프로필 3 now
    shifted.items[1].useLoses.layout = true;
    ui = shifted;
    panel.render(cfg, service, ui);
    CHECK(panel.quickProfileId(quick(1)) == "profile-3");
    CHECK(tap(x1, y1, 60.5) == SettingsAction::None);
    CHECK(tap(x1, y1, 61) == quick(1));
}

// The quick switch's hit areas are the parts as drawn (inside the pill's 5 px rim), not even splits of the whole:
// just inside the selected part's drawn left edge is that part, just outside it the one before
void testHitAreasMatchPill() {
    Config cfg;
    FontSet fonts;
    fonts.load(cfg.fontPath, cfg.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    ProfilesUiState six = profiles(6);
    for (size_t sel = 1; sel < 6; ++sel) {
        six.activeId = "profile-" + std::to_string(sel + 1);
        panel.render(cfg, service, six);
        const std::vector<uint8_t>& px = panel.toRgba();
        const auto rgb = [&](int x, int y) {
            const size_t i = (static_cast<size_t>(y) * panel.width() + x) * 4;
            return (unsigned(px[i]) << 16) | (unsigned(px[i + 1]) << 8) | px[i + 2];
        };
        double px0 = 0, py0 = 0, cx = 0, cy = 0;
        CHECK(panel.buttonCenter(quick(sel - 1), px0, py0) && panel.buttonCenter(quick(sel), cx, cy));
        // The selected part's accent fill, found from the middle of the part before it
        int edge = static_cast<int>(px0);
        while (edge < static_cast<int>(cx) && rgb(edge, static_cast<int>(cy)) != 0xe27dfd) ++edge;
        CHECK(edge < static_cast<int>(cx));
        CHECK(panel.pointerDown(edge + 2, cy, 1.0) == quick(sel));
        panel.pointerUp(edge + 2, cy);
        CHECK(panel.pointerDown(edge - 2, cy, 2.0) == quick(sel - 1));
        panel.pointerUp(edge - 2, cy);
    }
}

// The profile actions' messages (Profiles page, footer) in Korean: a short Korean reason, none when there is none
void testStatusTexts() {
    CHECK(profileStatusText(Language::Ko, "use-failed", "간단 A", "설정 파일을 저장하지 못했어요") ==
          "'간단 A'을(를) 사용하지 못했어요: 설정 파일을 저장하지 못했어요");
    CHECK(profileStatusText(Language::Ko, "keyboard-failed", "", "") == "키보드를 열지 못했어요");
    CHECK(profileStatusText(Language::Ko, "new-failed", "", "프로필은 8개까지 만들 수 있어요") ==
          "새 프로필을 저장하지 못했어요: 프로필은 8개까지 만들 수 있어요");
    CHECK(profileStatusText(Language::Ko, "reset-failed", "", "") == "기본값으로 되돌리지 못했어요");
}

}  // namespace

int main() {
    testQuickSwitch();
    testStatusTexts();
    testFooterStatus();
    testQuickSwitchSecondTap();
    testHitAreasMatchPill();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("quick switch OK\n");
    return 0;
}
