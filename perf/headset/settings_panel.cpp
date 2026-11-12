// 設定パネルの実装。
#include "settings_panel.h"

#include "app_text.h"
#include "color_page.h"
#include "device_page.h"
#include "draw.h"
#include "i18n.h"
#include "placement.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

// 1600×738。1px あたりの大きさは前（1200px を 2.8m）と同じにする（幅は vr_overlay.cpp で px から決める）。
// 見出しの行に新しい版の確認の帯を入れ、パネル・位置・向きのカードを横に 3 つ並べる。
// 3 つのカードは同じ高さの 5 段（ボタン 68px ＋ すき間 11px）にそろえる
constexpr int kWidth = 1600;
constexpr double kPad = 28;          // パネルの外側の余白
constexpr double kButtonH = 68;      // ボタンの高さ（レーザーで押しやすい大きさ。前と同じ）
constexpr double kHeaderY = 20;      // 見出しの行（見出し・新しい版の確認の帯・状態のピル）の上端
constexpr double kHeaderH = kButtonH + 16;  // 帯のボタンも 68px にして、上下に 8px ずつ
constexpr double kCardGap = 24;      // カードどうしの間
constexpr double kCardY = kHeaderY + kHeaderH + 20;  // 3 つのカードの上端
constexpr double kCardPad = 20;      // カードの中の余白
constexpr double kGap = 11;          // ボタンどうしの間（縦も横も）
constexpr int kRows = 5;             // カードの中の段の数
constexpr double kRowY0 = kCardY + 64;         // 1 段目のボタンの上端（上は見出し）
constexpr double kRowStep = kButtonH + kGap;   // 段の間隔
constexpr double kCardH = 64 + kRows * kButtonH + (kRows - 1) * kGap + kCardPad;
// 左のカード「パネル」: 行の見出し（左）とボタン（右）
constexpr double kLeftCardX = kPad;
constexpr double kLeftCardW = 448;
constexpr double kRowControlX = kLeftCardX + 140;  // ボタンの左端（左は行の見出し）
constexpr double kSegmentW = 260;                  // 2 択のセグメント切り替えの幅
constexpr double kStepButtonW = 84;                // − / ＋
constexpr double kValueW = 120;                    // − と ＋ の間の値
constexpr double kControlW = kStepButtonW * 2 + kValueW;  // − 値 ＋ の幅（3 択の時計・既定に戻すも同じ幅）
// 真ん中のカード「位置」
constexpr double kPosCardX = kLeftCardX + kLeftCardW + kCardGap;
constexpr double kPosCardW = 560;
constexpr double kPosInnerX = kPosCardX + kCardPad;
constexpr double kPosInnerW = kPosCardW - kCardPad * 2;
constexpr double kAttachLabelW = 110;              // 「固定先」の見出しの幅
constexpr double kPresetW = (kPosInnerW - 2 * kGap) / 3;  // 位置のボタン 1 つ（3 列）
// 右のカード（配置: 手首で隠す。ほかのページでは各ページのもの）
constexpr double kFacingCardX = kPosCardX + kPosCardW + kCardGap;
constexpr double kFacingCardW = kWidth - kPad - kFacingCardX;
constexpr double kFacingInnerX = kFacingCardX + kCardPad;
constexpr double kFacingInnerW = kFacingCardW - kCardPad * 2;
// 下の段
constexpr double kFooterY = kCardY + kCardH + kCardGap;
constexpr int kHeight = static_cast<int>(kFooterY + kButtonH + 54);  // 下に 1 行の説明
constexpr double kFooterSegmentW = 220;  // autostart (two parts)
constexpr double kQuickPartMaxW = 220;   // one profile in the footer's quick switch (narrower when there are many)
constexpr double kQuickLabelSize = 22;
constexpr double kQuitW = 230;
// The Layout page: the lanes on the left, the palette on the right (same height as the placement cards)
constexpr double kEditorCardW = 1000;
constexpr double kPaletteCardX = kPad + kEditorCardW + kCardGap;
// Seven pages (Placement, Layout, Profiles, Color, Dock, Browser, Device), 112 px each: the Color page's two compare
// buttons still fit between the switch and the status pill
constexpr int kPages = 7;
constexpr double kPageSwitchW = 112 * kPages;
// The Profiles page: the list (left), the selected profile's actions (right)
constexpr double kProfListW = 640;
constexpr double kProfRowH = 44;
constexpr double kProfRowGap = 4;
constexpr double kProfRowY0 = kCardY + 64;
constexpr double kProfCard2X = kPad + kProfListW + kCardGap;
constexpr double kProfCard2W = kWidth - kPad - kProfCard2X;

constexpr double kPresetDistance = 0.5;  // 頭: プリセットの座標はこの距離での値
constexpr double kFadeAngleStepDeg = 5;     // 消える角度の刻み

/**
 * 段の上端。
 * @param row 段（0 から）
 * @return y（px）
 */
constexpr double rowY(int row) {
    return kRowY0 + row * kRowStep;
}

/** 位置のプリセット（距離 0.5m での x, y）。 */
struct Preset {
    SettingsAction action;
    double x, y;
};
constexpr Preset kPresets[] = {
    {SettingsAction::PresetLeftBottom, -0.15, -0.12},  {SettingsAction::PresetCenterBottom, 0.0, -0.16},
    {SettingsAction::PresetRightBottom, 0.15, -0.12},  {SettingsAction::PresetLeftTop, -0.15, 0.12},
    {SettingsAction::PresetRightTop, 0.15, 0.12},
};

/**
 * 1mm 単位に丸める（小数の誤差を設定ファイルに残さない）。
 * @param meters 値（m）
 * @return 丸めた値
 */
double roundMm(double meters) {
    return std::round(meters * 1000.0) / 1000.0;
}

/**
 * パネルまでの距離に合わせて、プリセットの座標を伸び縮みさせる倍率。
 * @param config 設定
 * @return 倍率（z = -0.5m で 1）
 */
double distanceScale(const Config& config) {
    return std::max(0.1, -config.posZ) / kPresetDistance;
}

/**
 * 今の位置がプリセットと同じか。
 * @param preset プリセット
 * @param config 設定
 * @return 同じなら true
 */
bool matchesPreset(const Preset& preset, const Config& config) {
    const double scale = distanceScale(config);
    return std::fabs(config.posX - preset.x * scale) < 0.004 && std::fabs(config.posY - preset.y * scale) < 0.004;
}

/**
 * 手首の位置と向きが標準（WristPose の既定値）のままか。
 * @param pose 手首の位置と向き
 * @return ほぼ同じ（0.5mm・0.05° 未満の差）なら true
 */
bool matchesWristPreset(const WristPose& pose) {
    const WristPose standard;
    return std::fabs(pose.x - standard.x) < 0.0005 && std::fabs(pose.y - standard.y) < 0.0005 &&
           std::fabs(pose.z - standard.z) < 0.0005 && std::fabs(pose.pitch - standard.pitch) < 0.05 &&
           std::fabs(pose.yaw - standard.yaw) < 0.05 && std::fabs(pose.roll - standard.roll) < 0.05;
}

/**
 * 画面で変えられる設定（表示まわり）が同じか。
 * @param a 設定
 * @param b 設定
 * @return 同じなら true
 */
bool sameDisplaySettings(const Config& a, const Config& b) {
    return a.visible == b.visible && a.attachment == b.attachment && a.posX == b.posX &&
           a.posY == b.posY && a.posZ == b.posZ && a.yawDeg == b.yawDeg && a.pitchDeg == b.pitchDeg &&
           a.rollDeg == b.rollDeg && !(a.leftWrist != b.leftWrist) && !(a.rightWrist != b.rightWrist) &&
           a.wristFade == b.wristFade && a.wristFadeEndDeg == b.wristFadeEndDeg && a.clockFormat == b.clockFormat &&
           a.widthM == b.widthM && a.alpha == b.alpha && a.dock.enabled == b.dock.enabled &&
           a.dock.pointerHand == b.dock.pointerHand && a.dock.widthM == b.dock.widthM &&
           a.dock.collapseSec == b.dock.collapseSec && !(a.dock.head != b.dock.head) &&
           !(a.dock.leftWrist != b.dock.leftWrist) && !(a.dock.rightWrist != b.dock.rightWrist) &&
           a.browser == b.browser && a.languageAuto == b.languageAuto && a.language == b.language;
}

/** The next value of a list on a Browser page − / ＋ (a value off the list snaps to its neighbour). */
template <size_t N>
int stepIn(const int (&steps)[N], int value, int direction) {
    if (direction > 0) {
        for (const int s : steps) {
            if (s > value) return s;
        }
        return steps[N - 1];
    }
    for (size_t i = N; i-- > 0;) {
        if (steps[i] < value) return steps[i];
    }
    return steps[0];
}

/** The next idle shutdown on the Browser page (minutes: 5 … 60, then never). */
int stepBrowserIdle(int minutes, int direction) {
    std::vector<double> lengths;
    for (const int step : kBrowserIdleSteps) lengths.push_back(step > 0 ? step : 1e9);
    const double now = minutes > 0 ? minutes : 1e9;
    const size_t n = lengths.size();
    if (direction > 0) {
        for (size_t i = 0; i < n; ++i) {
            if (lengths[i] > now) return kBrowserIdleSteps[i];
        }
        return kBrowserIdleSteps[n - 1];
    }
    for (size_t i = n; i-- > 0;) {
        if (lengths[i] < now) return kBrowserIdleSteps[i];
    }
    return kBrowserIdleSteps[0];
}

/**
 * The next collapse timeout on the Dock page's − / ＋ (kDockCollapseSteps: 10 … 120 s, then never).
 * @param seconds now (0 = never; a value off the list snaps to the next step)
 * @param direction −1 sooner, +1 later
 */
int stepDockTimeout(int seconds, int direction) {
    // The list as "how long": 10 … 120, then never (0) as the longest
    std::vector<double> lengths;
    for (const int step : kDockCollapseSteps) lengths.push_back(step > 0 ? step : 1e9);
    const double now = seconds > 0 ? seconds : 1e9;
    if (direction > 0) {
        for (size_t i = 0; i < lengths.size(); ++i) {
            if (lengths[i] > now) return kDockCollapseSteps[i];
        }
        return kDockCollapseSteps[lengths.size() - 1];
    }
    for (size_t i = lengths.size(); i-- > 0;) {
        if (lengths[i] < now) return kDockCollapseSteps[i];
    }
    return kDockCollapseSteps[0];
}

}  // namespace

framecolor::Language colorLanguage(Language language) {
    switch (language) {
        case Language::Ja: return framecolor::Language::Japanese;
        case Language::Ko: return framecolor::Language::Korean;
        case Language::En: break;
    }
    return framecolor::Language::English;
}

bool applySettingsAction(SettingsAction action, Config& config) {
    const Config before = config;
    // 位置と向きのボタンは、今の固定先の値を動かす（頭は posX など、手首はその手の WristPose）
    const bool wrist = config.attachment == Attachment::LeftWrist || config.attachment == Attachment::RightWrist;
    WristPose& pose = selectedWrist(config);
    switch (action) {
        case SettingsAction::ShowOn: config.visible = true; break;
        case SettingsAction::ShowOff: config.visible = false; break;
        case SettingsAction::AttachHead: config.attachment = Attachment::Head; break;
        case SettingsAction::AttachLeft: config.attachment = Attachment::LeftWrist; break;
        case SettingsAction::AttachRight: config.attachment = Attachment::RightWrist; break;
        case SettingsAction::PresetLeftBottom:
        case SettingsAction::PresetCenterBottom:
        case SettingsAction::PresetRightBottom:
        case SettingsAction::PresetLeftTop:
        case SettingsAction::PresetRightTop:
            for (const auto& preset : kPresets) {
                if (preset.action != action) continue;
                // 見える方向を保つため、今の距離に合わせて伸び縮みさせる
                const double scale = distanceScale(config);
                config.posX = roundMm(preset.x * scale);
                config.posY = roundMm(preset.y * scale);
                // 隅に置くと斜めから見ることになるので、置いた位置で面を頭に向ける
                faceHead(config);
            }
            break;
        case SettingsAction::PresetWrist:
            if (wrist) pose = WristPose();
            break;
        case SettingsAction::FadeOn: config.wristFade = true; break;
        case SettingsAction::FadeOff: config.wristFade = false; break;
        case SettingsAction::FadeAngleDown:
            config.wristFadeEndDeg = std::clamp(config.wristFadeEndDeg - kFadeAngleStepDeg, 35.0, 90.0);
            break;
        case SettingsAction::FadeAngleUp:
            config.wristFadeEndDeg = std::clamp(config.wristFadeEndDeg + kFadeAngleStepDeg, 35.0, 90.0);
            break;
        case SettingsAction::SizeDown: config.widthM = std::clamp(roundMm(config.widthM - 0.02), 0.06, 1.0); break;
        case SettingsAction::SizeUp: config.widthM = std::clamp(roundMm(config.widthM + 0.02), 0.06, 1.0); break;
        case SettingsAction::AlphaDown:
            config.alpha = std::clamp(std::round((config.alpha - 0.1) * 20.0) / 20.0, 0.2, 1.0);
            break;
        case SettingsAction::AlphaUp:
            config.alpha = std::clamp(std::round((config.alpha + 0.1) * 20.0) / 20.0, 0.2, 1.0);
            break;
        case SettingsAction::ClockOff: config.clockFormat = 0; break;
        case SettingsAction::Clock12: config.clockFormat = 12; break;
        case SettingsAction::Clock24: config.clockFormat = 24; break;
        case SettingsAction::Reset: resetDisplaySettings(config); break;
        case SettingsAction::LanguageCycle: nextLanguageChoice(config.languageAuto, config.language); break;
        case SettingsAction::AutostartOn:   // 自動起動は設定ファイルではなく systemd なので呼び出し側で扱う
        case SettingsAction::AutostartOff:
        case SettingsAction::Quit:  // 終了は呼び出し側で扱う
        case SettingsAction::PageLayout:  // the page is the panel's own state
        case SettingsAction::PagePlacement:
        case SettingsAction::PageColor:
        case SettingsAction::PageProfiles:  // profiles change the layout through the caller (LayoutAuthority)
        case SettingsAction::ProfileRow0:
        case SettingsAction::ProfileRow1:
        case SettingsAction::ProfileRow2:
        case SettingsAction::ProfileRow3:
        case SettingsAction::ProfileRow4:
        case SettingsAction::ProfileRow5:
        case SettingsAction::ProfileRow6:
        case SettingsAction::ProfileRow7:
        case SettingsAction::ProfileNew:
        case SettingsAction::ProfileUse:
        case SettingsAction::ProfileSave:
        case SettingsAction::ProfileRename:
        case SettingsAction::ProfileDelete:
        case SettingsAction::ProfileReset:
        case SettingsAction::QuickProfile0:  // profiles: the caller
        case SettingsAction::QuickProfile1:
        case SettingsAction::QuickProfile2:
        case SettingsAction::QuickProfile3:
        case SettingsAction::QuickProfile4:
        case SettingsAction::QuickProfile5:
        case SettingsAction::PageDock:
        case SettingsAction::None: break;
        case SettingsAction::DockOn: config.dock.enabled = true; break;
        case SettingsAction::DockOff: config.dock.enabled = false; break;
        case SettingsAction::DockHandAuto: config.dock.pointerHand = DockHand::Auto; break;
        case SettingsAction::DockHandLeft: config.dock.pointerHand = DockHand::Left; break;
        case SettingsAction::DockHandRight: config.dock.pointerHand = DockHand::Right; break;
        case SettingsAction::DockSizeDown:
            config.dock.widthM = std::clamp(roundMm(config.dock.widthM - 0.05), 0.15, 0.60);
            break;
        case SettingsAction::DockSizeUp:
            config.dock.widthM = std::clamp(roundMm(config.dock.widthM + 0.05), 0.15, 0.60);
            break;
        case SettingsAction::DockTimeoutDown: config.dock.collapseSec = stepDockTimeout(config.dock.collapseSec, -1); break;
        case SettingsAction::DockTimeoutUp: config.dock.collapseSec = stepDockTimeout(config.dock.collapseSec, 1); break;
        case SettingsAction::DockResetPlace: {
            const DockSettings defaults;
            config.dock.head = defaults.head;
            config.dock.leftWrist = defaults.leftWrist;
            config.dock.rightWrist = defaults.rightWrist;
            break;
        }
        case SettingsAction::BrowserFpsDown: config.browser.fps = stepIn(kBrowserFpsSteps, config.browser.fps, -1); break;
        case SettingsAction::BrowserFpsUp: config.browser.fps = stepIn(kBrowserFpsSteps, config.browser.fps, 1); break;
        case SettingsAction::Browser720p:
        case SettingsAction::Browser1080p: {
            const int(&size)[2] = action == SettingsAction::Browser720p ? kBrowser720p : kBrowser1080p;
            config.browser.viewportWidth = size[0];
            config.browser.viewportHeight = size[1];
            break;
        }
        case SettingsAction::BrowserHeadless: config.browser.headless = true; break;
        case SettingsAction::BrowserWindow: config.browser.headless = false; break;
        case SettingsAction::BrowserKeepAudioOn: config.browser.keepAudio = true; break;
        case SettingsAction::BrowserKeepAudioOff: config.browser.keepAudio = false; break;
        case SettingsAction::BrowserRemovedKeepOn: config.browser.pauseWhenRemoved = false; break;
        case SettingsAction::BrowserRemovedPause: config.browser.pauseWhenRemoved = true; break;
        case SettingsAction::BrowserIdleDown:
            config.browser.idleShutdownMin = stepBrowserIdle(config.browser.idleShutdownMin, -1);
            break;
        case SettingsAction::BrowserIdleUp:
            config.browser.idleShutdownMin = stepBrowserIdle(config.browser.idleShutdownMin, 1);
            break;
        case SettingsAction::BrowserAnchorAuto: config.browser.anchor = BrowserAnchor::Auto; break;
        case SettingsAction::BrowserAnchorLeft: config.browser.anchor = BrowserAnchor::LeftWrist; break;
        case SettingsAction::BrowserAnchorRight: config.browser.anchor = BrowserAnchor::RightWrist; break;
        case SettingsAction::BrowserAnchorHead: config.browser.anchor = BrowserAnchor::Head; break;
        case SettingsAction::BrowserResetPlace: {
            const BrowserSettings defaults;
            config.browser.head = defaults.head;
            config.browser.leftWrist = defaults.leftWrist;
            config.browser.rightWrist = defaults.rightWrist;
            config.browser.widthM = defaults.widthM;
            break;
        }
        case SettingsAction::PageBrowser:        // the page is the panel's own state
        case SettingsAction::PageDevice:
        case SettingsAction::BrowserHomeFromPage:  // the caller (it knows the page's URL)
        case SettingsAction::BrowserStopChrome:    // the caller
        case SettingsAction::BrowserStartChrome:
            break;
    }
    return !sameDisplaySettings(config, before);
}

SettingsPanel::SettingsPanel(const FontSet& fonts)
    : fonts_(fonts),
      editor_({kPad, kCardY, kEditorCardW, kCardH}, {kPaletteCardX, kCardY, kWidth - kPad - kPaletteCardX, kCardH}) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    layoutButtons();
}

SettingsPanel::~SettingsPanel() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

void SettingsPanel::layoutButtons() {
    buttons_.clear();
    /**
     * ボタンを 1 つ足す。
     */
    const auto add = [this](SettingsAction action, double x, double y, double w, double h) {
        buttons_.push_back({action, x, y, w, h});
    };
    /**
     * セグメント切り替えの各部分を、それぞれボタンとして足す（押せる範囲はピル全体の高さを等分した幅）。
     */
    const auto segmented = [&add](double x, double y, double w, std::initializer_list<SettingsAction> actions) {
        const double part = w / static_cast<double>(actions.size());
        double left = x;
        for (const SettingsAction action : actions) {
            add(action, left, y, part, kButtonH);
            left += part;
        }
    };

    // The Layout page's cards are the editor's; only the header and the footer have buttons here
    if (mode_ == ButtonMode::Layout) return;
    if (mode_ == ButtonMode::Profiles) return;  // its rows and buttons are placed as they're drawn
    if (mode_ == ButtonMode::Color) return;     // the Color page keeps its own hit areas
    if (mode_ == ButtonMode::Device) return;    // so does the 기기 page
    if (mode_ == ButtonMode::Dock) {
        // Left card: in-game use on / off, the pointing hand. Middle card: the Dock panel's size, timeout, place
        segmented(kRowControlX, rowY(0), kSegmentW, {SettingsAction::DockOn, SettingsAction::DockOff});
        segmented(kRowControlX, rowY(1), kControlW,
                  {SettingsAction::DockHandAuto, SettingsAction::DockHandLeft, SettingsAction::DockHandRight});
        const double controlX = kPosCardX + kPosCardW - kCardPad - kControlW;
        add(SettingsAction::DockSizeDown, controlX, rowY(0), kStepButtonW, kButtonH);
        add(SettingsAction::DockSizeUp, controlX + kStepButtonW + kValueW, rowY(0), kStepButtonW, kButtonH);
        add(SettingsAction::DockTimeoutDown, controlX, rowY(1), kStepButtonW, kButtonH);
        add(SettingsAction::DockTimeoutUp, controlX + kStepButtonW + kValueW, rowY(1), kStepButtonW, kButtonH);
        add(SettingsAction::DockResetPlace, controlX, rowY(2), kControlW, kButtonH);
        return;
    }
    if (mode_ == ButtonMode::Browser) {
        // Left: the picture (fps, page size, how Chrome runs, sound when closed). Middle: Chrome (idle shutdown,
        // where the panel hangs, its place, stop it now). Right: how to use the panel, and home at the bottom
        add(SettingsAction::BrowserFpsDown, kRowControlX, rowY(0), kStepButtonW, kButtonH);
        add(SettingsAction::BrowserFpsUp, kRowControlX + kStepButtonW + kValueW, rowY(0), kStepButtonW, kButtonH);
        segmented(kRowControlX, rowY(1), kControlW, {SettingsAction::Browser720p, SettingsAction::Browser1080p});
        segmented(kRowControlX, rowY(2), kControlW, {SettingsAction::BrowserHeadless, SettingsAction::BrowserWindow});
        segmented(kRowControlX, rowY(3), kControlW, {SettingsAction::BrowserKeepAudioOn, SettingsAction::BrowserKeepAudioOff});
        segmented(kRowControlX, rowY(4), kControlW,
                  {SettingsAction::BrowserRemovedKeepOn, SettingsAction::BrowserRemovedPause});
        const double controlX = kPosCardX + kPosCardW - kCardPad - kControlW;
        add(SettingsAction::BrowserIdleDown, controlX, rowY(0), kStepButtonW, kButtonH);
        add(SettingsAction::BrowserIdleUp, controlX + kStepButtonW + kValueW, rowY(0), kStepButtonW, kButtonH);
        segmented(kPosInnerX + kAttachLabelW, rowY(1), kPosInnerW - kAttachLabelW,
                  {SettingsAction::BrowserAnchorAuto, SettingsAction::BrowserAnchorLeft, SettingsAction::BrowserAnchorRight,
                   SettingsAction::BrowserAnchorHead});
        add(SettingsAction::BrowserResetPlace, controlX, rowY(2), kControlW, kButtonH);
        add(browserInfo_.running ? SettingsAction::BrowserStopChrome : SettingsAction::BrowserStartChrome, controlX,
            rowY(3), kControlW, kButtonH);
        add(SettingsAction::BrowserHomeFromPage, kFacingInnerX, rowY(4), kFacingInnerW, kButtonH);
        return;
    }
    const bool wristLayout = mode_ == ButtonMode::Wrist;

    // 左のカード「パネル」: 表示・大きさ・透明度・時計・既定に戻す
    segmented(kRowControlX, rowY(0), kSegmentW, {SettingsAction::ShowOn, SettingsAction::ShowOff});
    add(SettingsAction::SizeDown, kRowControlX, rowY(1), kStepButtonW, kButtonH);
    add(SettingsAction::SizeUp, kRowControlX + kStepButtonW + kValueW, rowY(1), kStepButtonW, kButtonH);
    add(SettingsAction::AlphaDown, kRowControlX, rowY(2), kStepButtonW, kButtonH);
    add(SettingsAction::AlphaUp, kRowControlX + kStepButtonW + kValueW, rowY(2), kStepButtonW, kButtonH);
    segmented(kRowControlX, rowY(3), kControlW,
              {SettingsAction::ClockOff, SettingsAction::Clock12, SettingsAction::Clock24});
    add(SettingsAction::Reset, kRowControlX, rowY(4), kControlW, kButtonH);

    // 真ん中のカード「位置」: いちばん上に固定先
    segmented(kPosInnerX + kAttachLabelW, rowY(0), kPosInnerW - kAttachLabelW,
              {SettingsAction::AttachHead, SettingsAction::AttachLeft, SettingsAction::AttachRight});
    const double col[3] = {kPosInnerX, kPosInnerX + kPresetW + kGap, kPosInnerX + 2 * (kPresetW + kGap)};
    if (wristLayout) {
        // 手首: 位置のボタンは「手首の標準の位置」だけ（1 段ぶちぬき）
        add(SettingsAction::PresetWrist, kPosInnerX, rowY(1), kPosInnerW, kButtonH);
    } else {
        // 頭: 見える場所どおりに 3×2（上の段の真ん中は空き）
        add(SettingsAction::PresetLeftTop, col[0], rowY(1), kPresetW, kButtonH);
        add(SettingsAction::PresetRightTop, col[2], rowY(1), kPresetW, kButtonH);
        add(SettingsAction::PresetLeftBottom, col[0], rowY(2), kPresetW, kButtonH);
        add(SettingsAction::PresetCenterBottom, col[1], rowY(2), kPresetW, kButtonH);
        add(SettingsAction::PresetRightBottom, col[2], rowY(2), kPresetW, kButtonH);
    }
    // 右のカード「手首で隠す」: 傾けると消す（オン / オフ）と、消える角度（− 値 ＋）。見出しは左、ボタンは右詰め。
    // 頭に付けているときは使わないので押せない（描くだけ）
    if (wristLayout) {
        const double right = kFacingInnerX + kFacingInnerW;
        segmented(right - kSegmentW, rowY(0), kSegmentW, {SettingsAction::FadeOn, SettingsAction::FadeOff});
        add(SettingsAction::FadeAngleDown, right - kControlW, rowY(1), kStepButtonW, kButtonH);
        add(SettingsAction::FadeAngleUp, right - kStepButtonW, rowY(1), kStepButtonW, kButtonH);
    }
    // 下の段の言語・自動起動・終了と更新の帯は、文言の幅が言語や状態で変わるので render() のたびに置き直す
}

std::string SettingsPanel::labelOf(SettingsAction action, const UiText& text) const {
    switch (action) {
        case SettingsAction::ShowOn: return text.on;
        case SettingsAction::ShowOff: return profileArmed_ == action ? text.offConfirm : text.off;
        case SettingsAction::AttachHead: return text.attachHead;
        case SettingsAction::AttachLeft: return text.attachLeft;
        case SettingsAction::AttachRight: return text.attachRight;
        case SettingsAction::PresetLeftBottom: return text.bottomLeft;
        case SettingsAction::PresetCenterBottom: return text.bottomCenter;
        case SettingsAction::PresetRightBottom: return text.bottomRight;
        case SettingsAction::PresetLeftTop: return text.topLeft;
        case SettingsAction::PresetRightTop: return text.topRight;
        case SettingsAction::PresetWrist: return text.wristPreset;
        case SettingsAction::FadeOn: return text.on;
        case SettingsAction::FadeOff: return text.off;
        case SettingsAction::SizeDown:
        case SettingsAction::AlphaDown:
        case SettingsAction::FadeAngleDown: return "−";
        case SettingsAction::SizeUp:
        case SettingsAction::AlphaUp:
        case SettingsAction::FadeAngleUp: return "＋";
        case SettingsAction::ClockOff: return text.off;
        case SettingsAction::Clock12: return text.clock12;
        case SettingsAction::Clock24: return text.clock24;
        case SettingsAction::Reset: return profileArmed_ == action ? text.resetConfirm : text.reset;
        case SettingsAction::Quit: return quitArmed_ ? text.quitConfirm : text.quit;
        case SettingsAction::AutostartOn: return text.on;
        case SettingsAction::AutostartOff: return text.off;
        case SettingsAction::PageLayout: return text.pageLayout;
        case SettingsAction::PagePlacement: return text.pagePlacement;
        case SettingsAction::PageProfiles: return text.pageProfiles;
        case SettingsAction::PageColor: return text.pageColor;
        case SettingsAction::PageDock: return text.pageDock;
        case SettingsAction::DockOn: return text.on;
        case SettingsAction::DockOff: return profileArmed_ == action ? text.offConfirm : text.off;
        case SettingsAction::DockHandAuto: return text.dockHandAuto;
        case SettingsAction::DockHandLeft: return text.attachLeft;
        case SettingsAction::DockHandRight: return text.attachRight;
        case SettingsAction::DockSizeDown:
        case SettingsAction::DockTimeoutDown: return "−";
        case SettingsAction::DockSizeUp:
        case SettingsAction::DockTimeoutUp: return "＋";
        case SettingsAction::DockResetPlace: return text.dockResetPlace;
        // The Browser and 기기 pages: app_text's words
        case SettingsAction::PageBrowser: return tr(Tx::PageBrowser);
        case SettingsAction::PageDevice: return tr(Tx::PageDevice);
        case SettingsAction::LanguageCycle: return "";  // drawLanguageButton draws its own
        case SettingsAction::BrowserFpsDown:
        case SettingsAction::BrowserIdleDown: return "−";
        case SettingsAction::BrowserFpsUp:
        case SettingsAction::BrowserIdleUp: return "＋";
        case SettingsAction::Browser720p: return "720p";
        case SettingsAction::Browser1080p: return "1080p";
        case SettingsAction::BrowserHeadless: return tr(Tx::BrowserHeadless);
        case SettingsAction::BrowserWindow: return tr(Tx::BrowserWindow);
        case SettingsAction::BrowserKeepAudioOn:
        case SettingsAction::BrowserRemovedKeepOn: return tr(Tx::BrowserKeep);
        case SettingsAction::BrowserKeepAudioOff:
        case SettingsAction::BrowserRemovedPause: return tr(Tx::BrowserPause);
        case SettingsAction::BrowserAnchorAuto: return text.dockHandAuto;
        case SettingsAction::BrowserAnchorLeft: return text.attachLeft;
        case SettingsAction::BrowserAnchorRight: return text.attachRight;
        case SettingsAction::BrowserAnchorHead: return text.attachHead;
        case SettingsAction::BrowserResetPlace: return text.dockResetPlace;
        case SettingsAction::BrowserHomeFromPage: return tr(Tx::BrowserHomeFromPage);
        case SettingsAction::BrowserStopChrome:
            return profileArmed_ == action ? tr(Tx::BrowserStopChromeAgain) : tr(Tx::BrowserStopChrome);
        case SettingsAction::BrowserStartChrome: return tr(Tx::BrowserStartChrome);
        case SettingsAction::ProfileNew: return text.profileNew;
        case SettingsAction::ProfileSave: return profileArmed_ == action ? text.profileSaveAgain : text.profileSave;
        case SettingsAction::ProfileRename: return text.profileRename;
        case SettingsAction::ProfileUse:
        case SettingsAction::ProfileReset: {
            if (profileArmed_ != action) return action == SettingsAction::ProfileUse ? text.profileUse : text.profileReset;
            const std::string again = text.profileTapAgain;  // "Tap again: ..." -> "Tap again" on the button
            return again.substr(0, again.find(':'));
        }
        case SettingsAction::ProfileDelete:
            return profileArmed_ == action ? text.profileDeleteAgain : text.profileDelete;
        case SettingsAction::ProfileRow0:  // the rows draw their own names
        case SettingsAction::ProfileRow1:
        case SettingsAction::ProfileRow2:
        case SettingsAction::ProfileRow3:
        case SettingsAction::ProfileRow4:
        case SettingsAction::ProfileRow5:
        case SettingsAction::ProfileRow6:
        case SettingsAction::ProfileRow7:
        case SettingsAction::QuickProfile0:  // the strip draws the names
        case SettingsAction::QuickProfile1:
        case SettingsAction::QuickProfile2:
        case SettingsAction::QuickProfile3:
        case SettingsAction::QuickProfile4:
        case SettingsAction::QuickProfile5: return "";
        case SettingsAction::None: break;
    }
    return "";
}

SettingsAction SettingsPanel::hitTest(double x, double y) const {
    for (const auto& b : buttons_) {
        const bool isAutostart = b.action == SettingsAction::AutostartOn || b.action == SettingsAction::AutostartOff;
        if (isAutostart && !autostartInstalled_) continue;  // ユニットが無いときは押せない
        // A Profiles page button drawn disabled (nothing selected, all 8 used, no active profile)
        if (std::find(profileDisabled_.begin(), profileDisabled_.end(), b.action) != profileDisabled_.end()) continue;
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return b.action;
    }
    return SettingsAction::None;
}

bool SettingsPanel::pointerMove(double x, double y) {
    // The editor gets every move on its page (a drag may run over the header and the footer)
    const bool editor = page_ == SettingsPage::Layout && editor_.pointerMove(x, y);
    const bool color = page_ == SettingsPage::Color && colorPage_ != nullptr && colorPage_->pointerMove(x, y);
    const bool device = page_ == SettingsPage::Device && devicePage_ != nullptr && devicePage_->pointerMove(x, y);
    const SettingsAction now = editor_.busy() ? SettingsAction::None : hitTest(x, y);
    if (repeat_.active() && now != repeatAction_) stopRepeat();  // the laser slid off the held button
    if (now == hover_) return editor || color || device;
    hover_ = now;
    return true;
}

bool SettingsPanel::scroll(double dy) {
    return page_ == SettingsPage::Layout && editor_.scroll(dy);
}

bool SettingsPanel::repeatsWhileHeld(SettingsAction action) {
    switch (action) {
        case SettingsAction::SizeDown:
        case SettingsAction::SizeUp:
        case SettingsAction::AlphaDown:
        case SettingsAction::AlphaUp:
        case SettingsAction::FadeAngleDown:
        case SettingsAction::FadeAngleUp: return true;
        default: return false;
    }
}

SettingsAction SettingsPanel::pointerDown(double x, double y, double now) {
    stopRepeat();  // a second press (another controller) lets go of the first
    hover_ = hitTest(x, y);
    // Any press but the armed profile button cancels the pending second tap (before the early returns below)
    if (hover_ != profileArmed_) profileArmed_ = SettingsAction::None;
    // Off the header / footer buttons, the Layout page's press goes to the editor
    if (hover_ == SettingsAction::None && page_ == SettingsPage::Layout) {
        quitArmed_ = false;
        editor_.pointerDown(x, y);
        return SettingsAction::None;
    }
    // Off the header / footer buttons, the Color page's press goes to the page
    if (page_ == SettingsPage::Color && colorPage_ != nullptr) {
        if (hover_ == SettingsAction::None) {
            quitArmed_ = false;
            colorPage_->pointerDown(x, y);
            return SettingsAction::None;
        }
        colorPage_->dismiss();
    }
    // ...and the 기기 page's to that page
    if (page_ == SettingsPage::Device && devicePage_ != nullptr && hover_ == SettingsAction::None) {
        quitArmed_ = false;
        devicePage_->pointerDown(x, y, now);
        return SettingsAction::None;
    }
    pressed_ = hover_;
    if (pressed_ == SettingsAction::PageLayout || pressed_ == SettingsAction::PagePlacement ||
        pressed_ == SettingsAction::PageProfiles || pressed_ == SettingsAction::PageColor ||
        pressed_ == SettingsAction::PageDock || pressed_ == SettingsAction::PageBrowser ||
        pressed_ == SettingsAction::PageDevice) {
        page_ = pressed_ == SettingsAction::PageLayout      ? SettingsPage::Layout
                : pressed_ == SettingsAction::PagePlacement ? SettingsPage::Placement
                : pressed_ == SettingsAction::PageColor     ? SettingsPage::Color
                : pressed_ == SettingsAction::PageDock      ? SettingsPage::Dock
                : pressed_ == SettingsAction::PageBrowser   ? SettingsPage::Browser
                : pressed_ == SettingsAction::PageDevice    ? SettingsPage::Device
                                                            : SettingsPage::Profiles;
        editor_.pointerLeave();
        if (colorPage_ != nullptr && page_ != SettingsPage::Color) colorPage_->pointerLeave();
        if (devicePage_ != nullptr && page_ != SettingsPage::Device) devicePage_->pointerLeave();
        quitArmed_ = false;
        profileArmed_ = SettingsAction::None;
        return SettingsAction::None;
    }
    if (pressed_ == SettingsAction::Quit) {
        // 誤って押しても終わらないよう、1 回目は確認の表示にするだけ
        if (quitArmed_ && now <= quitArmedUntil_) return SettingsAction::Quit;
        quitArmed_ = true;
        quitArmedUntil_ = now + kQuitConfirmSec;
        return SettingsAction::None;
    }
    quitArmed_ = false;  // 別のボタンを押したら確認は取り消す
    // Profiles page. A row selects; Delete always, and Use / Reset when the live layout would be lost, need a
    // second tap within kQuitConfirmSec. Any other press cancels a pending one
    if (pressed_ >= SettingsAction::ProfileRow0 && pressed_ <= SettingsAction::ProfileRow7) {
        const size_t row = static_cast<size_t>(pressed_) - static_cast<size_t>(SettingsAction::ProfileRow0);
        if (row < profileRowIds_.size()) selectedProfile_ = profileRowIds_[row];
        profileArmed_ = SettingsAction::None;
        return SettingsAction::None;
    }
    if (pressed_ == SettingsAction::ProfileUse || pressed_ == SettingsAction::ProfileReset ||
        pressed_ == SettingsAction::ProfileDelete || pressed_ == SettingsAction::ProfileSave) {
        // Use: per target (a profile with a placement would also throw away a moved HUD); Reset: the layout only;
        // Save (overwrite): when the live layout differs from the profile (else it writes what is there already)
        const bool twoTaps = pressed_ == SettingsAction::ProfileDelete ||
                             (pressed_ == SettingsAction::ProfileSave && lastProfiles_.unsaved) ||
                             armedLoss(pressed_).any();
        if (twoTaps && !(profileArmed_ == pressed_ && now <= profileArmedUntil_)) {
            profileArmed_ = pressed_;
            profileArmedUntil_ = now + kQuitConfirmSec;
            return SettingsAction::None;
        }
    }
    // The quick switch: at once, unless it would throw away unsaved changes; then the first tap arms it (a hint on
    // the footer) and a second tap on the same button, still standing for the same profile, within kQuitConfirmSec
    // uses it. Tapping elsewhere or waiting disarms it (above, tick())
    if (pressed_ >= SettingsAction::QuickProfile0 && pressed_ <= SettingsAction::QuickProfile5 &&
        armedLoss(pressed_).any()) {
        const std::string id = quickProfileId(pressed_);
        if (!(profileArmed_ == pressed_ && quickArmedId_ == id && now <= profileArmedUntil_)) {
            profileArmed_ = pressed_;
            quickArmedId_ = id;
            profileArmedUntil_ = now + kQuitConfirmSec;
            return SettingsAction::None;
        }
    }
    // The Placement page's reset (all of it at once), and on the Dock's full view 표시 끔 and 게임 중 조작 끔 (the Dock
    // goes away with them and only the dashboard tab brings it back): a second tap within kQuitConfirmSec; the first
    // one says on the footer what will happen
    const bool confirm = pressed_ == SettingsAction::Reset || pressed_ == SettingsAction::BrowserStopChrome ||
                         (inDock_ && (pressed_ == SettingsAction::ShowOff || pressed_ == SettingsAction::DockOff));
    if (confirm && !(profileArmed_ == pressed_ && now <= profileArmedUntil_)) {
        profileArmed_ = pressed_;
        profileArmedUntil_ = now + kQuitConfirmSec;
        return SettingsAction::None;
    }
    profileArmed_ = SettingsAction::None;
    if (page_ == SettingsPage::Placement && repeatsWhileHeld(pressed_)) {
        repeatAction_ = pressed_;  // acts once now (the caller), then repeats while held (takeRepeat)
        repeat_.start(now);
    }
    return pressed_;
}

LiveLoss SettingsPanel::armedLoss(SettingsAction action) const {
    if (action == SettingsAction::ProfileReset) return lastProfiles_.resetLoses;
    const bool quick = action >= SettingsAction::QuickProfile0 && action <= SettingsAction::QuickProfile5;
    if (action != SettingsAction::ProfileUse && !quick) return LiveLoss();
    const std::string id = quick ? quickProfileId(action) : selectedProfile_;
    for (const ProfilesUiState::Item& item : lastProfiles_.items) {
        if (item.id == id) return item.useLoses;
    }
    return LiveLoss();  // gone: the caller says so
}

void SettingsPanel::takeDeviceRepeat(double now) {
    if (page_ == SettingsPage::Device && devicePage_ != nullptr) devicePage_->takeRepeat(now);
}

double SettingsPanel::deviceRepeatAt() const {
    if (page_ != SettingsPage::Device || devicePage_ == nullptr) return std::numeric_limits<double>::infinity();
    return devicePage_->nextRepeatAt();
}

void SettingsPanel::deviceSeen(double now) {
    if (devicePage_ != nullptr) devicePage_->pointerSeen(now);
}

void SettingsPanel::stopDeviceRepeat() {
    if (devicePage_ != nullptr) devicePage_->pointerLeave();  // lets go of the volume too
}

bool SettingsPanel::pointerUp(double x, double y) {
    stopRepeat();
    const bool editor = editor_.pointerUp(x, y);  // A drop or a tap in the editor
    const bool color = colorPage_ != nullptr && colorPage_->pointerUp();  // ends a slider drag or compare
    const bool device = devicePage_ != nullptr && devicePage_->pointerUp();  // ends the volume drag
    if (pressed_ == SettingsAction::None) return editor || color || device;
    pressed_ = SettingsAction::None;
    return true;
}

bool SettingsPanel::pointerLeave() {
    // The laser left the tab (or the dashboard closed) mid-drag: cancel it, commit nothing. A held button stops
    stopRepeat();
    bool changed = editor_.pointerLeave() || hover_ != SettingsAction::None || pressed_ != SettingsAction::None;
    if (colorPage_ != nullptr) changed = colorPage_->pointerLeave() || changed;  // releases compare
    if (devicePage_ != nullptr) changed = devicePage_->pointerLeave() || changed;  // lets go of the volume
    hover_ = SettingsAction::None;
    pressed_ = SettingsAction::None;
    return changed;
}

bool SettingsPanel::tick(double now) {
    bool changed = false;
    if (quitArmed_ && now > quitArmedUntil_) {
        quitArmed_ = false;
        changed = true;
    }
    if (profileArmed_ != SettingsAction::None && now > profileArmedUntil_) {
        profileArmed_ = SettingsAction::None;
        changed = true;
    }
    if (!actionStatus_.empty() && now > actionStatusUntil_) {
        actionStatus_.clear();
        changed = true;
    }
    return changed;
}

void SettingsPanel::showActionStatus(const std::string& text, bool error, double now) {
    actionStatus_ = text;
    actionStatusError_ = error;
    actionStatusUntil_ = now + (error ? kActionErrorSec : kActionStatusSec);
}

void SettingsPanel::armQuitForPreview() {
    quitArmed_ = true;
    quitArmedUntil_ = 1e300;
}

const SettingsPanel::Button* SettingsPanel::findButton(SettingsAction action) const {
    for (const auto& b : buttons_) {
        if (b.action == action) return &b;
    }
    return nullptr;
}

void SettingsPanel::placeFooterButton(SettingsAction action, double x, double y, double w, double h) {
    for (auto& b : buttons_) {
        if (b.action == action) {
            b = {action, x, y, w, h};
            return;
        }
    }
    buttons_.push_back({action, x, y, w, h});
}

void SettingsPanel::drawSegmented(const Pen& pen, const UiText& text, std::initializer_list<SettingsAction> actions,
                                  int selected, bool usable) const {
    std::vector<const Button*> parts;
    for (const SettingsAction action : actions) {
        const Button* b = findButton(action);
        if (b == nullptr) return;
        parts.push_back(b);
    }
    if (parts.empty()) return;
    // Drawn by the shared pill (core/render/draw.h), the one the layout editors use too
    std::vector<PillPart> pill;
    for (const Button* b : parts) pill.push_back({labelOf(b->action, text), pressed_ == b->action, hover_ == b->action});
    const double x = parts.front()->x;
    drawSegmentedPill(pen, x, parts.front()->y, parts.back()->x + parts.back()->w - x, parts.front()->h, pill, selected,
                      usable);
}

void SettingsPanel::drawButton(const Pen& pen, const UiText& text, SettingsAction action, bool selected,
                               double size, bool check) const {
    const Button* b = findButton(action);
    if (b == nullptr) return;
    const bool pressed = pressed_ == action;
    const bool hovered = hover_ == action;
    const bool armed = profileArmed_ == action;  // waiting for the second tap: red like Quit's confirm
    const double r = 18;
    if (armed) {
        pen.color(kDanger);
        pen.roundedRect(b->x, b->y, b->w, b->h, r);
        cairo_fill(pen.cr);
    } else if (selected) {
        // 選ばれている位置: アクセントの塗り ＋ ✓ ＋ 太字
        pen.color(pressed ? kAccentPressed : kAccent);
        pen.roundedRect(b->x, b->y, b->w, b->h, r);
        cairo_fill(pen.cr);
    } else {
        pen.color(pressed || hovered ? kControlHover : kControl);
        pen.roundedRect(b->x, b->y, b->w, b->h, r);
        cairo_fill(pen.cr);
        // 押している間は枠をアクセントにして、押したことが分かるようにする
        strokeRounded(pen, b->x, b->y, b->w, b->h, r, pressed ? kAccent : kBorder, 2);
    }
    const std::string label = labelOf(action, text);
    const double checkW = selected && check ? size * 0.9 : 0;
    const double labelSize = fitSize(pen, label, size, size * 0.65, b->w - 20 - checkW, true);
    const double labelW = pen.measure(label, labelSize, true);
    const double tx = b->x + (b->w - labelW - checkW) / 2;
    const Color fg = selected || armed ? kOnAccent : kText;
    if (selected && check) drawCheck(pen.cr, tx + checkW * 0.4, b->y + b->h / 2, size * 0.72, kOnAccent);
    const double labelX = tx + checkW;
    pen.text(labelX, centerBaseline(b->y, b->h, labelSize), label, labelSize, fg, true);
}

double SettingsPanel::drawHeader(const Pen& pen, const UiText& text, const Config& config) const {
    pen.text(kPad, centerBaseline(kHeaderY, kHeaderH, 34), text.settingsTitle, 34, kText, true);
    // 右上の状態のピル: 表示中は緑の塗り＋●＋文字、非表示は灰色の塗り＋○＋文字（色だけで伝えない）
    const std::string label = config.visible ? text.panelShown : text.panelHidden;
    const double size = 20;
    const double h = 42;
    const double y = kHeaderY + (kHeaderH - h) / 2;
    const double w = pen.measure(label, size, true) + 58;
    const double x = kWidth - kPad - w;
    pen.color(config.visible ? kSuccessTint : kControl);
    pen.roundedRect(x, y, w, h, h / 2);
    cairo_fill(pen.cr);
    const Color c = config.visible ? kSuccess : kTextMuted;
    if (config.visible) {
        drawDisc(pen.cr, x + 24, y + h / 2, 6.5, c);
    } else {
        pen.color(c);
        cairo_set_line_width(pen.cr, 2.5);
        cairo_new_sub_path(pen.cr);
        cairo_arc(pen.cr, x + 24, y + h / 2, 5.5, 0, 2 * M_PI);
        cairo_stroke(pen.cr);
    }
    pen.text(x + 40, centerBaseline(y, h, size), label, size, c, true);
    return x;
}

void SettingsPanel::drawLanguageButton(const Pen& pen, const Config& config, double left, double right) {
    // The approved mockup: a control-style pill as tall as the page switch, a drawn globe, the words
    buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(),
                                  [](const Button& b) { return b.action == SettingsAction::LanguageCycle; }),
                   buttons_.end());
    const double h = kButtonH, y = kHeaderY + (kHeaderH - h) / 2, w = right - left;
    if (w < 120) return;
    placeFooterButton(SettingsAction::LanguageCycle, left, y, w, h);
    const bool active = hover_ == SettingsAction::LanguageCycle || pressed_ == SettingsAction::LanguageCycle;
    pen.color(active ? kControlHover : kControl);
    pen.roundedRect(left, y, w, h, h / 2);
    cairo_fill(pen.cr);
    strokeRounded(pen, left + 1, y + 1, w - 2, h - 2, h / 2 - 1,
                  pressed_ == SettingsAction::LanguageCycle ? kAccent : kBorder, 2);
    // The globe: a circle, a meridian squeezed to 45 %, the equator
    const double cx = left + 38, cy = y + h / 2, r = 13;
    cairo_t* cr = pen.cr;
    pen.color(kText);
    cairo_set_line_width(cr, 2.4);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, 0.45, 1);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - r, cy);
    cairo_line_to(cr, cx + r, cy);
    cairo_stroke(cr);
    // "자동" bold and "· 한국어" muted, or the fixed language's name alone; smaller if it doesn't fit
    const std::string label = languageButtonText(config.languageAuto, config.language);
    const size_t dot = label.find(" · ");
    const std::string first = label.substr(0, dot);
    const std::string second = dot == std::string::npos ? std::string() : label.substr(dot + 1);
    const double textX = left + 64, room = right - 24 - textX;
    double scale = 1.0;
    const double full = pen.measure(first, 26, true) + (second.empty() ? 0 : 10 + pen.measure(second, 22, false));
    if (full > room) scale = std::max(0.6, room / full);
    const double x = textX + pen.text(textX, centerBaseline(y, h, 26 * scale), first, 26 * scale, kText, true);
    if (!second.empty()) pen.text(x + 10 * scale, centerBaseline(y, h, 26 * scale), second, 22 * scale, kTextMuted);
}

void SettingsPanel::drawPanelCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kLeftCardX, kCardY, kLeftCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kLeftCardX + kCardPad, kCardY + 42, text.cardPanel, 24, kText, true);
    // 行の見出し（左）とボタン（右）
    const char* labels[4] = {text.rowShow, text.rowSize, text.rowOpacity, text.rowClock};
    for (int i = 0; i < 4; ++i) {
        const double size = fitSize(pen, labels[i], 24, 16, kRowControlX - kLeftCardX - kCardPad - 12, false);
        pen.text(kLeftCardX + kCardPad, centerBaseline(rowY(i), kButtonH, size), labels[i], size, kTextMuted);
    }
    drawSegmented(pen, text, {SettingsAction::ShowOn, SettingsAction::ShowOff}, config.visible ? 0 : 1, true);

    // 大きさ・透明度: − 値 ＋
    char value[32];
    const double valueCx = kRowControlX + kStepButtonW + kValueW / 2;
    std::snprintf(value, sizeof(value), "%.0f cm", config.widthM * 100.0);
    textCentered(pen, valueCx, centerBaseline(rowY(1), kButtonH, 28), value, 28, kText, true);
    std::snprintf(value, sizeof(value), "%.0f%%", config.alpha * 100.0);
    textCentered(pen, valueCx, centerBaseline(rowY(2), kButtonH, 28), value, 28, kText, true);
    for (const SettingsAction action :
         {SettingsAction::SizeDown, SettingsAction::SizeUp, SettingsAction::AlphaDown, SettingsAction::AlphaUp}) {
        drawButton(pen, text, action, false, 30);
    }
    // 時計: オフ / 12h / 24h
    const int clock = config.clockFormat == 12 ? 1 : (config.clockFormat == 24 ? 2 : 0);
    drawSegmented(pen, text, {SettingsAction::ClockOff, SettingsAction::Clock12, SettingsAction::Clock24}, clock, true);
    drawButton(pen, text, SettingsAction::Reset, false, 24);
}

void SettingsPanel::drawBrowserPage(const Pen& pen, const UiText& text, const Config& config) const {
    const BrowserSettings& b = config.browser;
    const double hintSize = 19;
    char value[48];
    // Left: the picture
    drawCard(pen, kLeftCardX, kCardY, kLeftCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kLeftCardX + kCardPad, kCardY + 42, tr(Tx::BrowserCardPicture), 24, kText, true);
    const char* leftLabels[5] = {tr(Tx::BrowserRowFps), tr(Tx::BrowserRowResolution), tr(Tx::BrowserRowMode),
                                 tr(Tx::BrowserRowClosedSound), tr(Tx::BrowserRowRemovedVideo)};
    for (int i = 0; i < 5; ++i) {
        const double size = fitSize(pen, leftLabels[i], 24, 16, kRowControlX - kLeftCardX - kCardPad - 12, false);
        pen.text(kLeftCardX + kCardPad, centerBaseline(rowY(i), kButtonH, size), leftLabels[i], size, kTextMuted);
    }
    const double valueCx = kRowControlX + kStepButtonW + kValueW / 2;
    std::snprintf(value, sizeof(value), "%d", b.fps);
    textCentered(pen, valueCx, centerBaseline(rowY(0), kButtonH, 28), value, 28, kText, true);
    for (const SettingsAction action : {SettingsAction::BrowserFpsDown, SettingsAction::BrowserFpsUp}) {
        drawButton(pen, text, action, false, 30);
    }
    // Neither when config.json names another size
    const int resolution = b.viewportWidth == kBrowser720p[0] && b.viewportHeight == kBrowser720p[1]     ? 0
                           : b.viewportWidth == kBrowser1080p[0] && b.viewportHeight == kBrowser1080p[1] ? 1
                                                                                                          : -1;
    drawSegmented(pen, text, {SettingsAction::Browser720p, SettingsAction::Browser1080p}, resolution, true);
    drawSegmented(pen, text, {SettingsAction::BrowserHeadless, SettingsAction::BrowserWindow}, b.headless ? 0 : 1, true);
    drawSegmented(pen, text, {SettingsAction::BrowserKeepAudioOn, SettingsAction::BrowserKeepAudioOff},
                  b.keepAudio ? 0 : 1, true);
    drawSegmented(pen, text, {SettingsAction::BrowserRemovedKeepOn, SettingsAction::BrowserRemovedPause},
                  b.pauseWhenRemoved ? 1 : 0, true);
    // (The 1080p and 실행 방식 note is on the bottom line: this card's five rows are full)

    // Middle: Chrome
    drawCard(pen, kPosCardX, kCardY, kPosCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kPosInnerX, kCardY + 42, "Chrome", 24, kText, true);
    const double controlX = kPosCardX + kPosCardW - kCardPad - kControlW;
    const char* midLabels[4] = {tr(Tx::BrowserRowIdle), text.rowAttachment, tr(Tx::BrowserRowPlace),
                                tr(Tx::BrowserRowNow)};
    for (int i = 0; i < 4; ++i) {
        const double room = (i == 1 ? kAttachLabelW : controlX - kPosInnerX) - 12;
        const double size = fitSize(pen, midLabels[i], 24, 16, room, false);
        pen.text(kPosInnerX, centerBaseline(rowY(i), kButtonH, size), midLabels[i], size, kTextMuted);
    }
    const std::string idle = b.idleShutdownMin > 0 ? trf(Tx::BrowserIdleMinutes, {std::to_string(b.idleShutdownMin)})
                                                   : std::string(text.dockTimeoutNever);
    const double idleSize = fitSize(pen, idle, 28, 18, kValueW - 8, true);
    textCentered(pen, controlX + kStepButtonW + kValueW / 2, centerBaseline(rowY(0), kButtonH, idleSize), idle,
                 idleSize, kText, true);
    drawButton(pen, text, SettingsAction::BrowserIdleDown, false, 30);
    drawButton(pen, text, SettingsAction::BrowserIdleUp, false, 30);
    const int anchor = b.anchor == BrowserAnchor::LeftWrist    ? 1
                       : b.anchor == BrowserAnchor::RightWrist ? 2
                       : b.anchor == BrowserAnchor::Head       ? 3
                                                               : 0;
    drawSegmented(pen, text,
                  {SettingsAction::BrowserAnchorAuto, SettingsAction::BrowserAnchorLeft, SettingsAction::BrowserAnchorRight,
                   SettingsAction::BrowserAnchorHead},
                  anchor, true);
    drawButton(pen, text, SettingsAction::BrowserResetPlace, false, 24);
    drawButton(pen, text, browserInfo_.running ? SettingsAction::BrowserStopChrome : SettingsAction::BrowserStartChrome,
               false, 24);
    double y = rowY(4) + 26;
    const std::string chromeLine =
        "Chrome: " + (browserInfo_.status.empty() ? std::string(tr(Tx::ChromeOff)) : browserInfo_.status);
    pen.text(kPosInnerX, y, ellipsize(pen, chromeLine, hintSize, kPosInnerW, true), hintSize,
             browserInfo_.running ? kSuccess : kTextMuted, true);
    y += hintSize * 1.6;
    for (const std::string& line : wrapText(pen, tr(Tx::BrowserIdleHint), hintSize, kPosInnerW)) {
        pen.text(kPosInnerX, y, line, hintSize, kTextMuted);
        y += hintSize * 1.6;
    }

    // Right: how to use the panel (what isn't on its buttons), and home at the bottom
    drawCard(pen, kFacingCardX, kCardY, kFacingCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kFacingInnerX, kCardY + 42, tr(Tx::BrowserGuideTitle), 24, kText, true);
    const char* guide[] = {
        tr(Tx::BrowserGuideTap),        tr(Tx::BrowserGuideSelect), tr(Tx::BrowserGuideMenu),
        tr(Tx::BrowserGuideThumbstick), tr(Tx::BrowserGuideMove),   tr(Tx::BrowserGuidePinned),
    };
    // As large as fits above the home line (longer words in some languages wrap to more lines)
    const double guideBottom = rowY(4) - 16 - 22;
    double guideSize = 20;
    for (; guideSize > 15; guideSize -= 1) {
        double h = 0;
        for (const char* item : guide) {
            h += wrapText(pen, item, guideSize, kFacingInnerW - 18).size() * guideSize * 1.5 + 8;
        }
        if (rowY(0) + 20 + h - guideSize * 1.5 <= guideBottom) break;
    }
    y = rowY(0) + 20;
    for (const char* item : guide) {
        drawDisc(pen.cr, kFacingInnerX + 5, y - guideSize * 0.35, 3, kAccent);
        for (const std::string& line : wrapText(pen, item, guideSize, kFacingInnerW - 18)) {
            pen.text(kFacingInnerX + 18, y, line, guideSize, kText);
            y += guideSize * 1.5;
        }
        y += 8;
    }
    const std::string home = trf(Tx::BrowserHome, {b.home});
    pen.text(kFacingInnerX, rowY(4) - 16, home, fitSize(pen, home, 20, 13, kFacingInnerW, false), kTextMuted);
    drawButton(pen, text, SettingsAction::BrowserHomeFromPage, false, 22);
}

void SettingsPanel::drawDockPage(const Pen& pen, const UiText& text, const Config& config) const {
    // Left: in-game use and the pointing hand, with what auto means
    drawCard(pen, kLeftCardX, kCardY, kLeftCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kLeftCardX + kCardPad, kCardY + 42, text.cardDockInGame, 24, kText, true);
    const char* leftLabels[2] = {text.rowDockUse, text.rowDockHand};
    for (int i = 0; i < 2; ++i) {
        const double size = fitSize(pen, leftLabels[i], 24, 16, kRowControlX - kLeftCardX - kCardPad - 12, false);
        pen.text(kLeftCardX + kCardPad, centerBaseline(rowY(i), kButtonH, size), leftLabels[i], size, kTextMuted);
    }
    drawSegmented(pen, text, {SettingsAction::DockOn, SettingsAction::DockOff}, config.dock.enabled ? 0 : 1, true);
    const int hand = config.dock.pointerHand == DockHand::Left ? 1 : config.dock.pointerHand == DockHand::Right ? 2 : 0;
    drawSegmented(pen, text, {SettingsAction::DockHandAuto, SettingsAction::DockHandLeft, SettingsAction::DockHandRight},
                  hand, config.dock.enabled);
    const double hintSize = 19;
    double y = rowY(2) + 30;
    for (const std::string& line : wrapText(pen, text.dockHandHint, hintSize, kLeftCardW - 2 * kCardPad)) {
        pen.text(kLeftCardX + kCardPad, y, line, hintSize, kTextMuted);
        y += hintSize * 1.6;
    }

    // Middle: the Dock panel (size, collapse timeout, its place back to the default)
    drawCard(pen, kPosCardX, kCardY, kPosCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kPosInnerX, kCardY + 42, text.cardDockPanel, 24, kText, true);
    const double controlX = kPosCardX + kPosCardW - kCardPad - kControlW;
    const char* midLabels[3] = {text.rowDockSize, text.rowDockTimeout, text.rowDockPlace};
    for (int i = 0; i < 3; ++i) {
        const double size = fitSize(pen, midLabels[i], 24, 16, controlX - kPosInnerX - 12, false);
        pen.text(kPosInnerX, centerBaseline(rowY(i), kButtonH, size), midLabels[i], size, kTextMuted);
    }
    char value[32];
    const double valueCx = controlX + kStepButtonW + kValueW / 2;
    std::snprintf(value, sizeof(value), "%.0f cm", config.dock.widthM * 100.0);
    textCentered(pen, valueCx, centerBaseline(rowY(0), kButtonH, 28), value, 28, kText, true);
    if (config.dock.collapseSec > 0) {
        std::snprintf(value, sizeof(value), text.dockTimeoutFormat, config.dock.collapseSec);
    } else {
        std::snprintf(value, sizeof(value), "%s", text.dockTimeoutNever);
    }
    const double timeoutSize = fitSize(pen, value, 28, 18, kValueW - 8, true);
    textCentered(pen, valueCx, centerBaseline(rowY(1), kButtonH, timeoutSize), value, timeoutSize, kText, true);
    for (const SettingsAction action : {SettingsAction::DockSizeDown, SettingsAction::DockSizeUp,
                                        SettingsAction::DockTimeoutDown, SettingsAction::DockTimeoutUp}) {
        drawButton(pen, text, action, false, 30);
    }
    drawButton(pen, text, SettingsAction::DockResetPlace, false, 24);
    y = rowY(3) + 30;
    for (const std::string& line : wrapText(pen, text.dockPanelHint, hintSize, kPosInnerW)) {
        pen.text(kPosInnerX, y, line, hintSize, kTextMuted);
        y += hintSize * 1.6;
    }

    // Right: how to use it in a game, one paragraph per gesture ("가리키기:" in bold)
    drawCard(pen, kFacingCardX, kCardY, kFacingCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kFacingInnerX, kCardY + 42, text.cardDockHelp, 24, kText, true);
    y = rowY(0) + 26;
    const std::string help = text.dockHelp;
    size_t start = 0;
    while (start < help.size()) {
        const size_t end = std::min(help.find('\n', start), help.size());
        const std::string paragraph = help.substr(start, end - start);
        const size_t colon = paragraph.find(':');
        bool first = true;
        for (const std::string& line : wrapText(pen, paragraph, hintSize, kFacingInnerW)) {
            double x = kFacingInnerX;
            if (first && colon != std::string::npos && colon < line.size()) {
                x += pen.text(x, y, line.substr(0, colon + 1), hintSize, kText, true);
                pen.text(x, y, line.substr(colon + 1), hintSize, kText);
            } else {
                pen.text(x, y, line, hintSize, kText);
            }
            first = false;
            y += hintSize * 1.6;
        }
        y += hintSize * 0.7;
        start = end + 1;
    }
}

void SettingsPanel::drawPositionCard(const Pen& pen, const UiText& text, const Config& config) const {
    drawCard(pen, kPosCardX, kCardY, kPosCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kPosInnerX, kCardY + 42, text.rowPosition, 24, kText, true);

    // 固定先: 見出し（左）と 3 択（右）
    const double attachSize = fitSize(pen, text.rowAttachment, 24, 16, kAttachLabelW - 12, false);
    pen.text(kPosInnerX, centerBaseline(rowY(0), kButtonH, attachSize), text.rowAttachment, attachSize, kTextMuted);
    const int attach = config.attachment == Attachment::LeftWrist ? 1 : (config.attachment == Attachment::RightWrist ? 2 : 0);
    drawSegmented(pen, text, {SettingsAction::AttachHead, SettingsAction::AttachLeft, SettingsAction::AttachRight},
                  attach, true);

    // 位置のボタン（今その位置なら ✓）
    if (mode_ == ButtonMode::Wrist) {
        drawButton(pen, text, SettingsAction::PresetWrist, matchesWristPreset(selectedWrist(config)), 24);
    } else {
        for (const auto& preset : kPresets) drawButton(pen, text, preset.action, matchesPreset(preset, config), 24);
    }
    // The fine position and facing are the laser's (hold the trigger on the panel): how, under the buttons
    double y = rowY(mode_ == ButtonMode::Wrist ? 2 : 3) + 30;
    for (const std::string& line : wrapText(pen, text.moveHint, 19, kPosInnerW)) {
        pen.text(kPosInnerX, y, line, 19, kTextMuted);
        y += 19 * 1.6;
    }
}

void SettingsPanel::drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const {
    // 手首で隠す: 傾けると消す（オン / オフ）と、消える角度（薄くなり始める角度〜消えきる角度）。頭のときは使わない
    // ので薄く描くだけ（押せない）
    drawCard(pen, kFacingCardX, kCardY, kFacingCardW, kCardH, 20, kCard, kCard, 0);
    pen.text(kFacingInnerX, kCardY + 42, text.cardWristHide, 24, kText, true);
    const bool wrist = mode_ == ButtonMode::Wrist;
    const Color label = wrist ? kTextMuted : kTextDisabled;
    const double right = kFacingInnerX + kFacingInnerW;
    const char* labels[2] = {text.rowWristFade, text.rowFadeAngle};
    const double labelW[2] = {kFacingInnerW - kSegmentW - 12, kFacingInnerW - kControlW - 12};
    for (int i = 0; i < 2; ++i) {
        const double size = fitSize(pen, labels[i], 24, 16, labelW[i], false);
        pen.text(kFacingInnerX, centerBaseline(rowY(i), kButtonH, size), labels[i], size, label);
    }
    char value[48];
    std::snprintf(value, sizeof(value), text.fadeRangeFormat, config.wristFadeEndDeg - 30.0, config.wristFadeEndDeg);
    const double valueSize = fitSize(pen, value, 28, 18, kValueW - 20, true);
    textCentered(pen, right - kStepButtonW - kValueW / 2, centerBaseline(rowY(1), kButtonH, valueSize), value,
                 valueSize, wrist ? kText : kTextDisabled, true);
    if (wrist) {
        drawSegmented(pen, text, {SettingsAction::FadeOn, SettingsAction::FadeOff}, config.wristFade ? 0 : 1, true);
        drawButton(pen, text, SettingsAction::FadeAngleDown, false, 30);
        drawButton(pen, text, SettingsAction::FadeAngleUp, false, 30);
    }
    char note[256];
    if (wrist) {
        std::snprintf(note, sizeof(note), text.wristHideNote, config.wristFadeEndDeg - 30.0, config.wristFadeEndDeg);
    } else {
        std::snprintf(note, sizeof(note), "%s", text.wristHideHeadNote);
    }
    double y = rowY(2) + 30;
    for (const std::string& line : wrapText(pen, note, 19, kFacingInnerW)) {
        pen.text(kFacingInnerX, y, line, 19, kTextMuted);
        y += 19 * 1.6;
    }
}

std::string SettingsPanel::quickProfileId(SettingsAction action) const {
    if (action < SettingsAction::QuickProfile0 || action > SettingsAction::QuickProfile5) return "";
    const size_t i = static_cast<size_t>(action) - static_cast<size_t>(SettingsAction::QuickProfile0);
    return i < quickIds_.size() ? quickIds_[i] : std::string();
}

void SettingsPanel::drawFooter(const Pen& pen, const UiText& text, const AutostartStatus& autostart) {
    const double y = kFooterY;
    const double labelSize = 20;
    // Right to left: Quit, autostart, then the quick switch takes the rest
    const double quitX = kWidth - kPad - kQuitW;
    const double autostartX = quitX - 40 - kFooterSegmentW;
    const double autostartLabelX = autostartX - 14 - pen.measure(text.rowAutostart, labelSize, false);

    // The quick switch: the first kQuickProfiles profiles as one pill, the active one selected. One tap uses it (two
    // when it would throw away unsaved changes: pointerDown)
    buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(),
                                  [](const Button& b) {
                                      return b.action >= SettingsAction::QuickProfile0 &&
                                             b.action <= SettingsAction::QuickProfile5;
                                  }),
                   buttons_.end());
    quickIds_.clear();
    double x = kPad;
    x += pen.text(x, centerBaseline(y, kButtonH, labelSize), text.rowQuickProfiles, labelSize, kTextMuted) + 14;
    const double stripRight = autostartLabelX - 40;
    const size_t n = std::min(kQuickProfiles, lastProfiles_.items.size());
    if (n == 0) {
        const double size = fitSize(pen, text.quickNone, labelSize, 13, stripRight - x, false);
        pen.text(x, centerBaseline(y, kButtonH, size), text.quickNone, size, kTextDisabled);
    } else {
        const double partW = std::min(kQuickPartMaxW, (stripRight - x) / static_cast<double>(n));
        const double pillW = partW * static_cast<double>(n);
        std::vector<PillPart> parts;
        int selected = -1;
        for (size_t i = 0; i < n; ++i) {
            const ProfilesUiState::Item& item = lastProfiles_.items[i];
            const SettingsAction action =
                static_cast<SettingsAction>(static_cast<int>(SettingsAction::QuickProfile0) + static_cast<int>(i));
            // The hit areas are the parts as the pill draws them (inside its 5 px rim; the end ones take the rim too)
            double hitLeft = 0, hitRight = 0;
            pillPartLeft(x, pillW, n, i, &hitLeft, &hitRight);
            placeFooterButton(action, hitLeft, y, hitRight - hitLeft, kButtonH);
            quickIds_.push_back(item.id);
            if (item.id == lastProfiles_.activeId) selected = static_cast<int>(i);
            // A long name gets an ellipsis: the pill shrinks it to 70 % at most, then it would run over
            const bool isSelected = item.id == lastProfiles_.activeId;
            const double room = (pillW - 2 * kPillInset) / static_cast<double>(n) - 20 -
                                (isSelected ? kQuickLabelSize * 0.9 : 0);  // the drawn part's label room
            parts.push_back({ellipsize(pen, item.name, kQuickLabelSize * 0.7, room, isSelected), pressed_ == action,
                             hover_ == action});
        }
        drawSegmentedPill(pen, x, y, pillW, kButtonH, parts, selected, true, kQuickLabelSize);
    }

    // 自動起動（ユニットが無いときはグレーで押せない。切り替え中も薄く）
    const bool installed = autostart.state != AutostartStatus::State::NotInstalled;
    autostartInstalled_ = installed;
    pen.text(autostartLabelX, centerBaseline(y, kButtonH, labelSize), text.rowAutostart, labelSize, kTextMuted);
    x = autostartX;
    placeFooterButton(SettingsAction::AutostartOn, x, y, kFooterSegmentW / 2, kButtonH);
    placeFooterButton(SettingsAction::AutostartOff, x + kFooterSegmentW / 2, y, kFooterSegmentW / 2, kButtonH);
    const int selected = autostart.state == AutostartStatus::State::Enabled
                             ? 0
                             : (autostart.state == AutostartStatus::State::Disabled ? 1 : -1);
    drawSegmented(pen, text, {SettingsAction::AutostartOn, SettingsAction::AutostartOff}, selected,
                  installed && !autostart.busy);

    // 終了（赤い枠の控えめなボタン。確認中は赤い塗り）
    placeFooterButton(SettingsAction::Quit, quitX, y, kQuitW, kButtonH);
    const bool quitHover = hover_ == SettingsAction::Quit || pressed_ == SettingsAction::Quit;
    pen.color(quitArmed_ ? kDanger : (quitHover ? kControlHover : kQuitFill));
    pen.roundedRect(quitX, y, kQuitW, kButtonH, kButtonH / 2);
    cairo_fill(pen.cr);
    strokeRounded(pen, quitX, y, kQuitW, kButtonH, kButtonH / 2, kDanger, 2);
    const std::string quitLabel = labelOf(SettingsAction::Quit, text);
    const double quitSize = fitSize(pen, quitLabel, 24, 14, kQuitW - 28, true);
    textCentered(pen, quitX + kQuitW / 2, centerBaseline(y, kButtonH, quitSize), quitLabel, quitSize,
                 quitArmed_ ? kOnAccent : kText, true);

    // The bottom line (one line): autostart failure (red) > switching > an armed quick switch's "tap again" > an armed
    // reset's / 끔's what happens > a Color page error > a profile action's result (a while, on every page) > the
    // Color page's other messages > no unit > the usual note
    const double lineY = kHeight - 24;
    const double maxW = kWidth - kPad * 2;
    std::string note;
    Color noteColor = kTextMuted;
    bool bold = false;
    std::string colorNote;
    bool colorError = false;
    const bool hasColorNote =
        page_ == SettingsPage::Color && colorPage_ != nullptr && colorPage_->statusLine(colorNote, colorError);
    const bool quickArmed =
        profileArmed_ >= SettingsAction::QuickProfile0 && profileArmed_ <= SettingsAction::QuickProfile5;
    const char* armedNote = profileArmed_ == SettingsAction::Reset     ? text.resetConfirmNote
                            : profileArmed_ == SettingsAction::ShowOff ? text.showOffConfirmNote
                            : profileArmed_ == SettingsAction::DockOff ? text.dockOffConfirmNote
                            : profileArmed_ == SettingsAction::BrowserStopChrome ? tr(Tx::DockQuitAgain)
                                                                       : nullptr;
    if (autostart.failed) {
        note = text.autostartFailed;
        noteColor = kDanger;
        bold = true;
    } else if (autostart.busy) {
        note = text.autostartBusy;
        noteColor = kText;
    } else if (quickArmed) {
        // The answer to the tap just made (3 s at most): the quick switch waits for a second tap
        note = text.quickTapAgain;
        noteColor = kDanger;
        bold = true;
    } else if (armedNote != nullptr) {
        note = armedNote;
        noteColor = kDanger;
        bold = true;
    } else if (hasColorNote && colorError) {
        note = colorNote;  // the Color page's error keeps its place
        noteColor = kDanger;
        bold = true;
    } else if (!actionStatus_.empty()) {
        // A profile action (the quick switch works on every page; a failure would otherwise go unseen)
        note = actionStatus_;
        noteColor = actionStatusError_ ? kDanger : kText;
        bold = actionStatusError_;
    } else if (hasColorNote) {
        // The Color page's message (FrameColor showed it at the bottom of its panel): green, the "layer is off" hint
        // muted
        note = colorNote;
        noteColor = colorPage_->model().state.message.empty() ? kTextMuted : kSuccess;
    } else if (page_ == SettingsPage::Browser) {
        // Its left card has no room for this under its rows
        note = tr(Tx::BrowserPageNote);
    } else if (!installed) {
        note = text.autostartNotInstalled;
    } else {
        note = std::string(text.footer) + text.sentenceBreak + text.autostartNextLaunch;
    }
    pen.text(kPad, lineY, note, fitSize(pen, note, 17, 12, maxW, bold), noteColor, bold);
    footerNote_ = note;
}

double SettingsPanel::drawPageSwitch(const Pen& pen, const UiText& text, double left) {
    // Placement | Layout | Profiles | Color | Dock | Browser | Device
    const double y = kHeaderY + (kHeaderH - kButtonH) / 2;
    const double part = kPageSwitchW / kPages;
    placeFooterButton(SettingsAction::PagePlacement, left, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageLayout, left + part, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageProfiles, left + 2 * part, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageColor, left + 3 * part, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageDock, left + 4 * part, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageBrowser, left + 5 * part, y, part, kButtonH);
    placeFooterButton(SettingsAction::PageDevice, left + 6 * part, y, part, kButtonH);
    const int selected = page_ == SettingsPage::Placement ? 0
                         : page_ == SettingsPage::Layout  ? 1
                         : page_ == SettingsPage::Profiles ? 2
                         : page_ == SettingsPage::Color    ? 3
                         : page_ == SettingsPage::Dock     ? 4
                         : page_ == SettingsPage::Browser  ? 5
                                                           : 6;
    drawSegmented(pen, text,
                  {SettingsAction::PagePlacement, SettingsAction::PageLayout, SettingsAction::PageProfiles,
                   SettingsAction::PageColor, SettingsAction::PageDock, SettingsAction::PageBrowser,
                   SettingsAction::PageDevice},
                  selected, true);
    return left + kPageSwitchW;
}

void SettingsPanel::drawProfileButton(const Pen& pen, const UiText& text, SettingsAction action, double x, double y,
                                      double w, bool enabled, bool primary) {
    placeFooterButton(action, x, y, w, kButtonH);
    if (enabled && profileArmed_ != action) {
        drawButton(pen, text, action, primary, 22, false);
        return;
    }
    if (!enabled) profileDisabled_.push_back(action);
    // disabled: faded, no border; armed (waiting for the second tap): red like Quit's confirm
    const bool armed = enabled;
    pen.color(armed ? kDanger : kControl);
    pen.roundedRect(x, y, w, kButtonH, 18);
    cairo_fill(pen.cr);
    const std::string label = labelOf(action, text);
    const double size = fitSize(pen, label, 22, 14, w - 20, true);
    textCentered(pen, x + w / 2, centerBaseline(y, kButtonH, size), label, size, armed ? kOnAccent : kTextDisabled, true);
}

void SettingsPanel::drawProfilesPage(const Pen& pen, const UiText& text, const ProfilesUiState& profiles) {
    profileDisabled_.clear();
    profileRowIds_.clear();
    for (const auto& item : profiles.items) profileRowIds_.push_back(item.id);
    const auto has = [&](const std::string& id) {
        return std::find(profileRowIds_.begin(), profileRowIds_.end(), id) != profileRowIds_.end();
    };
    if (!has(selectedProfile_)) {
        selectedProfile_ = has(profiles.activeId) ? profiles.activeId
                           : profileRowIds_.empty() ? std::string()
                                                    : profileRowIds_.front();
    }
    // Rows that no longer exist mustn't stay hittable
    buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(),
                                  [](const Button& b) {
                                      return b.action >= SettingsAction::ProfileRow0 &&
                                             b.action <= SettingsAction::ProfileRow7;
                                  }),
                   buttons_.end());

    // The list
    const double lx = kPad + kCardPad;
    const double lw = kProfListW - 2 * kCardPad;
    drawCard(pen, kPad, kCardY, kProfListW, kCardH, 20, kCard, kCard, 0);
    pen.text(lx, kCardY + 42, text.cardProfiles, 24, kText, true);
    char count[32];
    std::snprintf(count, sizeof(count), text.profileCountFormat, profiles.items.size(), profiles.maxProfiles);
    pen.text(kPad + kProfListW - kCardPad - pen.measure(count, 18, false), kCardY + 42, count, 18, kTextMuted);
    if (profiles.items.empty()) pen.text(lx, kProfRowY0 + 30, text.profileNone, fitSize(pen, text.profileNone, 18, 12, lw, false), kTextMuted);
    for (size_t i = 0; i < profiles.items.size() && i < 8; ++i) {
        const auto& item = profiles.items[i];
        const SettingsAction action = static_cast<SettingsAction>(static_cast<int>(SettingsAction::ProfileRow0) + static_cast<int>(i));
        const double y = kProfRowY0 + static_cast<double>(i) * (kProfRowH + kProfRowGap);
        placeFooterButton(action, lx, y, lw, kProfRowH);
        const bool selected = item.id == selectedProfile_;
        const bool active = item.id == profiles.activeId;
        pen.color(selected || hover_ == action || pressed_ == action ? kControlHover : kControl);
        pen.roundedRect(lx, y, lw, kProfRowH, 12);
        cairo_fill(pen.cr);
        if (selected) strokeRounded(pen, lx, y, lw, kProfRowH, 12, kAccent, 2);
        // right: "Active" (+ "unsaved changes"), left: the name in what's left
        double right = lx + lw - 14;
        if (active) {
            if (profiles.unsaved) {
                right -= pen.measure(text.profileUnsaved, 16, true);
                pen.text(right, centerBaseline(y, kProfRowH, 16), text.profileUnsaved, 16, kWarn, true);
                right -= 14;
            }
            right -= pen.measure(text.profileActive, 16, true);
            pen.text(right, centerBaseline(y, kProfRowH, 16), text.profileActive, 16, kAccentText, true);
            right -= 16;
        }
        if (item.layoutOnly) {  // Use leaves the placement as it is
            right -= pen.measure(text.profileLayoutOnly, 16, false);
            pen.text(right, centerBaseline(y, kProfRowH, 16), text.profileLayoutOnly, 16, kTextMuted);
            right -= 16;
        }
        const double nameW = right - (lx + 16);
        pen.text(lx + 16, centerBaseline(y, kProfRowH, 20), item.name, fitSize(pen, item.name, 20, 13, nameW, active),
                 kText, active);
    }
    // Files that couldn't be read hold their numbers (never written over): say so under the rows. There are at most
    // 7 rows then, so the bottom of the card is free
    if (!profiles.unreadable.empty()) {
        std::string note = text.profileUnreadable;
        for (size_t i = 0; i < profiles.unreadable.size(); ++i) note += (i == 0 ? "" : ", ") + profiles.unreadable[i];
        pen.text(lx, kCardY + kCardH - kCardPad, note, fitSize(pen, note, 17, 11, lw, false), kWarn);
    }

    // The selected profile: its actions, then the live layout's state
    const double x = kProfCard2X + kCardPad;
    const double w = kProfCard2W - 2 * kCardPad;
    drawCard(pen, kProfCard2X, kCardY, kProfCard2W, kCardH, 20, kCard, kCard, 0);
    std::string title = "-";
    for (const auto& item : profiles.items) {
        if (item.id == selectedProfile_) title = item.name;
    }
    pen.text(x, kCardY + 42, title, fitSize(pen, title, 24, 14, w, true), kText, true);
    const bool haveSelected = !selectedProfile_.empty();
    const bool haveActive = has(profiles.activeId);
    const double bw = (w - 2 * kGap) / 3;
    const double row0 = kCardY + 70;
    const double row1 = row0 + kButtonH + kGap;
    drawProfileButton(pen, text, SettingsAction::ProfileUse, x, row0, bw, haveSelected, true);
    drawProfileButton(pen, text, SettingsAction::ProfileRename, x + bw + kGap, row0, bw, haveSelected, false);
    drawProfileButton(pen, text, SettingsAction::ProfileDelete, x + 2 * (bw + kGap), row0, bw, haveSelected, false);
    drawProfileButton(pen, text, SettingsAction::ProfileNew, x, row1, bw,
                      profiles.items.size() + profiles.unreadable.size() < profiles.maxProfiles, false);
    // Save writes the active profile: only offered on its own card, not under another profile's name
    drawProfileButton(pen, text, SettingsAction::ProfileSave, x + bw + kGap, row1, bw,
                      haveActive && (!haveSelected || selectedProfile_ == profiles.activeId), false);
    drawProfileButton(pen, text, SettingsAction::ProfileReset, x + 2 * (bw + kGap), row1, bw, true, false);

    double y = row1 + kButtonH + 40;
    std::string state;
    Color stateColor = kTextMuted;
    if (haveActive) {
        std::string activeName;
        for (const auto& item : profiles.items) {
            if (item.id == profiles.activeId) activeName = item.name;
        }
        state = std::string(text.profileActive) + ": " + activeName +
                (profiles.unsaved ? std::string(" · ") + text.profileUnsaved : std::string());
        if (profiles.unsaved) stateColor = kWarn;
    } else if (profiles.unsaved) {
        state = text.profileNotSaved;
        stateColor = kWarn;
    }
    if (!state.empty()) {
        pen.text(x, y, state, fitSize(pen, state, 19, 13, w, true), stateColor, true);
        y += 30;
    }
    if (profileArmed_ == SettingsAction::ProfileUse || profileArmed_ == SettingsAction::ProfileReset) {
        // Says what would be lost: Reset never touches the placement
        const LiveLoss loss = armedLoss(profileArmed_);
        const char* again = loss.layout && loss.placement ? text.profileTapAgain
                            : loss.placement               ? text.profileTapAgainPlacement
                                                           : text.profileTapAgainLayout;
        pen.text(x, y, again, fitSize(pen, again, 18, 12, w, true), kDanger, true);
        y += 28;
    }
    if (!profiles.status.empty()) pen.text(x, y, profiles.status, fitSize(pen, profiles.status, 18, 12, w, false), kText);
    // The hint: two lines at the bottom of the card
    const std::string hint = text.profileHint;
    const size_t br = hint.find('\n');
    const std::string lines[2] = {hint.substr(0, br), br == std::string::npos ? std::string() : hint.substr(br + 1)};
    const double hintSize = std::min(fitSize(pen, lines[0], 17, 11, w, false), fitSize(pen, lines[1], 17, 11, w, false));
    pen.text(x, kCardY + kCardH - kCardPad - 26, lines[0], hintSize, kTextMuted);
    pen.text(x, kCardY + kCardH - kCardPad, lines[1], hintSize, kTextMuted);
}

void SettingsPanel::render(const Config& config, const AutostartStatus& autostart, const ProfilesUiState& profiles) {
    const Pen pen {cr_, &fonts_};
    const UiText& t = uiText(config.language);
    // 固定先・ページが変わったら、カードのボタンを置き直す（下の段と更新の帯はこのあと置き直す）
    const ButtonMode mode = page_ == SettingsPage::Layout            ? ButtonMode::Layout
                            : page_ == SettingsPage::Profiles       ? ButtonMode::Profiles
                            : page_ == SettingsPage::Color          ? ButtonMode::Color
                            : page_ == SettingsPage::Dock           ? ButtonMode::Dock
                            : page_ == SettingsPage::Browser        ? ButtonMode::Browser
                            : page_ == SettingsPage::Device         ? ButtonMode::Device
                            : config.attachment == Attachment::Head ? ButtonMode::Head
                                                                    : ButtonMode::Wrist;
    if (mode != mode_) {
        mode_ = mode;
        layoutButtons();
        stopRepeat();  // its button may be gone
    }
    editor_.sync(config.layout);
    editor_.setLanguage(widgetLanguage(config.language));
    lastProfiles_ = profiles;  // the Profiles page and the footer's quick switch

    // 地（不透明。コントラスト比は不透明な地で計算している）
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    pen.color(kBg);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(cr_);

    // 上の行 = 見出し・新しい版の確認の帯・状態のピル、その下に左から パネル・位置・向き のカード、
    // 下 = 言語・自動起動・終了と説明
    const double pillX = drawHeader(pen, t, config);
    const double titleRight = kPad + pen.measure(t.settingsTitle, 34, true);
    // The page switch after the title
    const double switchRight = drawPageSwitch(pen, t, titleRight + 24);
    // The language button between the switch and the pill (the Color page keeps that place for its compare buttons)
    if (page_ == SettingsPage::Color) {
        drawLanguageButton(pen, config, 0, 0);  // takes its button away
    } else {
        drawLanguageButton(pen, config, switchRight + 24, pillX - 24);
    }
    if (page_ == SettingsPage::Color && colorPage_ != nullptr) {
        framecolor::ColorPageArea area;
        area.headerLeft = switchRight + 24;
        area.headerRight = pillX - 20;
        area.headerTop = kHeaderY;
        area.headerHeight = kHeaderH;
        area.left = kPad;
        area.top = kCardY;
        area.width = kWidth - 2 * kPad;
        area.height = kCardH;
        colorPage_->model().state.language = colorLanguage(config.language);
        colorPage_->render(pen, area);
    }
    if (page_ == SettingsPage::Placement) {
        drawPanelCard(pen, t, config);
        drawPositionCard(pen, t, config);
        drawFacingCard(pen, t, config);
    }
    if (page_ == SettingsPage::Device && devicePage_ != nullptr) {
        devicePage_->render(pen, {kPad, kCardY, kWidth - 2 * kPad, kCardH});
    }
    if (page_ == SettingsPage::Dock) drawDockPage(pen, t, config);
    if (page_ == SettingsPage::Browser) drawBrowserPage(pen, t, config);
    if (page_ == SettingsPage::Profiles) {
        drawProfilesPage(pen, t, profiles);
    } else {
        profileDisabled_.clear();  // its buttons are gone with the page (layoutButtons), so nothing to skip
    }
    drawFooter(pen, t, autostart);
    if (page_ == SettingsPage::Layout) editor_.render(pen, t);  // last: a dragged card is drawn over everything

    cairo_surface_flush(surface_);
}

const uint8_t* SettingsPanel::pixels() const { return cairo_image_surface_get_data(surface_); }

const std::vector<uint8_t>& SettingsPanel::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool SettingsPanel::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

int SettingsPanel::width() const {
    return kWidth;
}

int SettingsPanel::height() const {
    return kHeight;
}

void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    const double s = size / 256.0;

    // 地と内側の縁
    pen.color(kBg);
    pen.roundedRect(8 * s, 8 * s, 240 * s, 240 * s, 48 * s);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 2 * s);
    pen.roundedRect(9 * s, 9 * s, 238 * s, 238 * s, 47 * s);
    cairo_stroke(cr);

    // グラフの台（カードの色）と、目標の点線
    pen.color(kCard);
    pen.roundedRect(34 * s, 40 * s, 188 * s, 118 * s, 18 * s);
    cairo_fill(cr);
    pen.color(kTextMuted);
    cairo_set_line_width(cr, 3 * s);
    const double dashes[] = {8 * s, 7 * s};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_move_to(cr, 48 * s, 70 * s);
    cairo_line_to(cr, 208 * s, 70 * s);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);

    // 折れ線（イメージカラー）と、その光彩
    const double ys[] = {128, 104, 116, 82, 94, 64, 76};
    /**
     * 折れ線の道筋を作る。
     */
    const auto path = [&]() {
        for (int i = 0; i < 7; ++i) {
            const double x = (48 + i * 26.7) * s;
            if (i == 0) cairo_move_to(cr, x, ys[i] * s); else cairo_line_to(cr, x, ys[i] * s);
        }
    };
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 3; i >= 1; --i) {
        pen.color(kAccent, 0.10);
        cairo_set_line_width(cr, (10 + i * 7) * s);
        path();
        cairo_stroke(cr);
    }
    pen.color(kAccent);
    cairo_set_line_width(cr, 10 * s);
    path();
    cairo_stroke(cr);
    drawDisc(cr, (48 + 6 * 26.7) * s, 76 * s, 11 * s, kAccent);

    // FrameColor's part: red, green and blue discs overlapping at the graph's lower right (added light)
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    const Color rgb[3] = {hexColor(0xd23a3a), hexColor(0x2fa84a), hexColor(0x3b6fe0)};
    const double discX[3] = {176, 198, 187}, discY[3] = {120, 120, 139};
    for (int i = 0; i < 3; ++i) drawDisc(cr, discX[i] * s, discY[i] * s, 21 * s, rgb[i]);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const double w = pen.measure("Dock", 54 * s, true);
    pen.text((size - w) / 2, 222 * s, "Dock", 54 * s, kText, true);

    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
