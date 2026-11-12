// The Dock panel's views. See dock_view.h. Its words are app_text's and the dashboard's (i18n), in uiLanguage().
#include "dock_view.h"

#include "app_text.h"
#include "color_model.h"
#include "draw.h"
#include "i18n.h"
#include "settings_panel.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr double kPad = 28;
constexpr double kInner = 20;     // inside a card
constexpr double kButtonH = 76;   // a little taller than the tab's 68: the panel is small and close
constexpr double kGap = 10;
constexpr double kRadius = 18;
// Quick view: the battery row, the profiles card, the colour card, the bottom row, the note
constexpr double kBatteryY = 98, kBatteryH = 70;
constexpr double kProfilesY = 184, kProfilesH = 176;
constexpr double kColorY = 374, kColorH = 316;
constexpr double kDeviceY = 704, kDeviceH = 52 + DevicePage::kQuickRowsH + 20;  // output, volume, passthrough
constexpr double kBottomY = kDeviceY + kDeviceH + 14;
static_assert(kBottomY + kButtonH + 50 <= DockPanel::kQuickHeight, "the quick view's rows fit");
// The full view's bar buttons
constexpr double kBarButtonW = 170, kBarButtonH = 64;

const char* kTitle = "Frame Dock";

/** A battery %'s colour: red at or below crit, yellow at or below warn, else the text colour */
Color batteryColor(double pct, double warn, double crit) {
    if (std::isnan(pct)) return kTextMuted;
    return pct <= crit ? kDanger : pct <= warn ? kWarn : kText;
}

bool inside(double px, double py, double x, double y, double w, double h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

/** Six grip dots: "hold here". */
void drawGripDots(cairo_t* cr, double cx, double cy, Color c) {
    for (int row = -1; row <= 1; ++row) {
        for (int col = 0; col < 2; ++col) drawDisc(cr, cx - 6 + col * 12, cy + row * 12, 3.6, c);
    }
}

}  // namespace

DockPanel::DockPanel(const FontSet& fonts, framecolor::ColorModel* color) : fonts_(fonts), color_(color) {
    ensureSurface(kQuickWidth, kQuickHeight);
}

DockPanel::~DockPanel() {
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

void DockPanel::ensureSurface(int w, int h) {
    if (surface_ != nullptr && w == surfaceW_ && h == surfaceH_) return;
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    surfaceW_ = w;
    surfaceH_ = h;
}

void DockPanel::clear() {
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    hits_.clear();
}

const DockPanel::Hit* DockPanel::hitAt(double x, double y) const {
    for (const Hit& hit : hits_) {
        if (hit.usable && inside(x, y, hit.x, hit.y, hit.w, hit.h)) return &hit;
    }
    return nullptr;
}

bool DockPanel::hovered(const Hit& hit) const {
    return hit.usable && inside(px_, py_, hit.x, hit.y, hit.w, hit.h);
}

bool DockPanel::hitCenter(Hit::Kind kind, int arg, double& x, double& y) const {
    for (const Hit& hit : hits_) {
        if (hit.kind != kind || hit.arg != arg || !hit.usable) continue;
        x = hit.x + hit.w / 2;
        y = hit.y + hit.h / 2;
        return true;
    }
    return false;
}

void DockPanel::drawButton(const Pen& pen, Hit hit, const std::string& label, bool selected, double size) {
    const bool pressed = isPressed(hit.kind, hit.arg);
    const bool over = hovered(hit);
    if (!hit.usable) {
        pen.color(kControl);
        pen.roundedRect(hit.x, hit.y, hit.w, hit.h, kRadius);
        cairo_fill(pen.cr);
    } else if (selected) {
        pen.color(pressed || over ? kAccentPressed : kAccent);
        pen.roundedRect(hit.x, hit.y, hit.w, hit.h, kRadius);
        cairo_fill(pen.cr);
    } else {
        pen.color(pressed || over ? kControlHover : kControl);
        pen.roundedRect(hit.x, hit.y, hit.w, hit.h, kRadius);
        cairo_fill(pen.cr);
        strokeRounded(pen, hit.x, hit.y, hit.w, hit.h, kRadius, pressed || over ? kAccent : kBorder, 2);
    }
    const Color fg = !hit.usable ? kTextDisabled : (selected ? kOnAccent : kText);
    const double labelSize = fitSize(pen, label, size, size * 0.65, hit.w - 20, true);
    textCentered(pen, hit.x + hit.w / 2, centerBaseline(hit.y, hit.h, labelSize), label, labelSize, fg, true);
    hits_.push_back(hit);
}

void DockPanel::drawHandle(const Pen& pen, double width, double hitWidth, const std::string& title,
                           const std::string& hint) {
    // The handle bar: the card colour, grip dots, the title; holding the trigger on it carries the panel
    const Hit handle {Hit::Kind::Handle, 0, 0, 0, hitWidth, static_cast<double>(kBarHeight), true};
    const bool active = isPressed(Hit::Kind::Handle, 0) || hovered(handle);
    pen.color(active ? kControlHover : kCard);
    pen.roundedRect(0, 0, width, kBarHeight, 24);
    cairo_fill(pen.cr);
    pen.color(active ? kControlHover : kCard);
    cairo_rectangle(pen.cr, 0, kBarHeight / 2.0, width, kBarHeight / 2.0);  // square bottom corners
    cairo_fill(pen.cr);
    drawGripDots(pen.cr, kPad + 10, kBarHeight / 2.0, active ? kAccent : kTextMuted);
    const double titleX = kPad + 36;
    const double titleW = pen.text(titleX, centerBaseline(0, kBarHeight, 30), title, 30, kText, true);
    if (!hint.empty()) {
        const double left = titleX + titleW + 24;
        const double room = width - kPad - left;
        if (room > 80) {
            const double size = fitSize(pen, hint, 20, 15, room, false);
            pen.text(width - kPad, centerBaseline(0, kBarHeight, size), hint, size, kTextMuted, false, true);
        }
    }
    hits_.push_back(handle);
}

void DockPanel::renderQuick(const ProfilesUiState& profiles, double now) {
    ensureSurface(kQuickWidth, kQuickHeight);
    clear();
    profiles_ = profiles;
    tick(now);
    const Pen pen {cr_, &fonts_};
    const UiText& text = uiText(uiLanguage());
    const double W = kQuickWidth;
    pen.color(kBg);
    pen.roundedRect(0, 0, W, kQuickHeight, 24);
    cairo_fill(cr_);
    drawHandle(pen, W, W, kTitle, tr(Tx::DockHandleHint));

    // ---- Battery: the headset (% and the time left, or 충전 중), then each controller ----
    drawCard(pen, kPad, kBatteryY, W - 2 * kPad, kBatteryH, 20, kCard, kCard, 0);
    {
        const double baseline = centerBaseline(kBatteryY, kBatteryH, 24);
        const double x0 = kPad + kInner, inner = W - 2 * (kPad + kInner);
        const double handX[2] = {x0 + inner * 0.52, x0 + inner * 0.76};
        const auto pctText = [](double pct) { return std::to_string(static_cast<int>(std::lround(pct))) + "%"; };
        double x = x0 + pen.text(x0, baseline, std::string(tr(Tx::DockHeadset)) + " ", 22, kTextMuted);
        if (std::isnan(battery_.headsetPct)) {
            pen.text(x, baseline, "--", 24, kTextMuted, true);
        } else {
            x += pen.text(x, baseline, pctText(battery_.headsetPct), 26,
                          batteryColor(battery_.headsetPct, battery_.warnPct, battery_.critPct), true);
            const std::string state = !battery_.headsetState.empty() ? battery_.headsetState
                                      : !battery_.minutesText.empty() ? "· " + battery_.minutesText
                                                                       : "";
            if (!state.empty()) {
                const double size = fitSize(pen, state, 22, 16, handX[0] - 16 - (x + 10), false);
                pen.text(x + 10, baseline, state, size, battery_.headsetCharging ? kSuccess : kTextMuted);
            }
        }
        const char* names[2] = {tr(Tx::DockLeftHand), tr(Tx::DockRightHand)};
        const DockBattery::Hand* hands[2] = {&battery_.left, &battery_.right};
        for (int i = 0; i < 2; ++i) {
            const DockBattery::Hand& hand = *hands[i];
            double hx = handX[i] + pen.text(handX[i], baseline, std::string(names[i]) + " ", 22, kTextMuted);
            if (!hand.present || std::isnan(hand.pct)) {
                pen.text(hx, baseline, tr(Tx::DockOff), 22, kTextMuted);
                continue;
            }
            hx += pen.text(hx, baseline, pctText(hand.pct), 26,
                           batteryColor(hand.pct, battery_.controllerWarnPct, battery_.controllerCritPct), true);
            if (hand.charging) {
                const frame_perf_overlay::WidgetText& wt = frame_perf_overlay::widgetText(widgetLanguage(uiLanguage()));
                pen.text(hx + 8, baseline, wt.statusCharging, 18, kSuccess);
            }
        }
    }

    // ---- Profiles: the quick switch (the first six), the active one selected ----
    drawCard(pen, kPad, kProfilesY, W - 2 * kPad, kProfilesH, 20, kCard, kCard, 0);
    pen.text(kPad + kInner, kProfilesY + 36, text.cardProfiles, 22, kText, true);
    const double pillX = kPad + kInner, pillW = W - 2 * (kPad + kInner), pillY = kProfilesY + 52;
    quickIds_.clear();
    const size_t n = std::min(kQuickProfiles, profiles.items.size());
    std::string line;
    Color lineColor = kTextMuted;
    bool lineBold = false;
    if (n > 0) {
        std::vector<PillPart> parts;
        int selected = -1;
        for (size_t i = 0; i < n; ++i) {
            const ProfilesUiState::Item& item = profiles.items[i];
            double hitLeft = 0, hitRight = 0;
            pillPartLeft(pillX, pillW, n, i, &hitLeft, &hitRight);
            const Hit hit {Hit::Kind::Profile, static_cast<int>(i), hitLeft, pillY, hitRight - hitLeft, kButtonH, true};
            hits_.push_back(hit);
            quickIds_.push_back(item.id);
            const bool isSelected = item.id == profiles.activeId;
            if (isSelected) selected = static_cast<int>(i);
            const double room = (pillW - 2 * kPillInset) / static_cast<double>(n) - 20 - (isSelected ? 22 : 0);
            // An armed button (waiting for its second tap) shows pressed
            parts.push_back({ellipsize(pen, item.name, 24 * 0.7, room, isSelected),
                             isPressed(Hit::Kind::Profile, static_cast<int>(i)) || armedIndex_ == static_cast<int>(i),
                             hovered(hit)});
        }
        drawSegmentedPill(pen, pillX, pillY, pillW, kButtonH, parts, selected, true, 24);
        if (armedIndex_ >= 0 && static_cast<size_t>(armedIndex_) < n) {
            // The button waiting for its second tap: a red ring, next to the red line below
            const double partW = (pillW - 2 * kPillInset) / static_cast<double>(n);
            const double left = pillPartLeft(pillX, pillW, n, static_cast<size_t>(armedIndex_));
            const double inner = kButtonH - 2 * kPillInset;
            strokeRounded(pen, left, pillY + kPillInset, partW, inner, inner / 2, kDanger, 3);
        }
        std::string activeName;
        for (const auto& item : profiles.items) {
            if (item.id == profiles.activeId) activeName = item.name;
        }
        line = activeName.empty() ? std::string(tr(Tx::DockNoActive)) : trf(Tx::DockUsing, {activeName});
        if (profiles.unsaved) line += tr(Tx::DockUnsaved);
    } else {
        line = text.quickNone;
    }
    if (armedIndex_ >= 0) {
        line = text.quickTapAgain;  // the answer to the tap just made
        lineColor = kDanger;
        lineBold = true;
    } else if (!status_.empty()) {
        line = status_;
        lineColor = statusError_ ? kDanger : kText;
        lineBold = statusError_;
    }
    const double lineSize = fitSize(pen, line, 20, 14, pillW, lineBold);
    pen.text(pillX, kProfilesY + kProfilesH - 22, line, lineSize, lineColor, lineBold);

    // ---- Colour: the six presets, hold to compare, auto compare ----
    drawCard(pen, kPad, kColorY, W - 2 * kPad, kColorH, 20, kCard, kCard, 0);
    pen.text(kPad + kInner, kColorY + 36, text.pageColor, 22, kText, true);
    const double innerX = kPad + kInner, innerW = W - 2 * (kPad + kInner);
    const double bw = (innerW - 2 * kGap) / 3;
    const double row0 = kColorY + 52;
    const bool haveColor = color_ != nullptr;
    const bool layer = haveColor && color_->state.vulkanLayerActive;
    // A newer layer waits for a SteamVR restart: said on the title's row
    if (haveColor && color_->checkLayerReload(std::chrono::steady_clock::now())) {
        const double titleRight = innerX + pen.measure(text.pageColor, 22, true) + 24;
        const char* reload = color_->tr().layerReload;
        const double size = fitSize(pen, reload, 19, 13, innerX + innerW - titleRight, false);
        pen.text(innerX + innerW, kColorY + 36, reload, size, kAccentText, false, true);
    }
    for (int i = 0; i < 6; ++i) {
        const double x = innerX + (i % 3) * (bw + kGap);
        const double y = row0 + (i / 3) * (kButtonH + kGap);
        std::string label;
        bool usable = haveColor;
        bool inUse = false;  // its values are the ones in use (not while the tab picks a slot to save to)
        if (haveColor) {
            const framecolor::ColorState& ui = color_->state;
            label = color_->presetName(i);
            inUse = !ui.saveMode && color_->presetInUse(i);
            if (i >= 3 && !ui.custom[i - 3].valid) {
                label += color_->tr().emptySlotSuffix;
                usable = false;  // nothing to apply
            }
        } else {
            label = i < 3 ? "-" : "P" + std::to_string(i - 2);
        }
        drawButton(pen, {Hit::Kind::Preset, i, x, y, bw, kButtonH, usable}, label, inUse, 26);
    }
    const double row2 = row0 + 2 * (kButtonH + kGap);
    const double halfW = (innerW - kGap) / 2;
    if (haveColor) {
        const framecolor::ColorState& ui = color_->state;
        const framecolor::Translations& tr = color_->tr();
        drawButton(pen, {Hit::Kind::Compare, 0, innerX, row2, halfW, kButtonH, layer},
                   ui.compare ? tr.compareHeld : tr.compare, ui.compare, 26);
        const char* autoLabel = !ui.autoCompare ? tr.autoCompare
                                : ui.autoOriginal ? tr.autoCompareShowingOriginal
                                                  : tr.autoCompareShowingCorrected;
        drawButton(pen, {Hit::Kind::AutoCompare, 0, innerX + halfW + kGap, row2, halfW, kButtonH, layer}, autoLabel,
                   ui.autoCompare, 26);
    } else {
        drawButton(pen, {Hit::Kind::Compare, 0, innerX, row2, halfW, kButtonH, false}, "-", false);
        drawButton(pen, {Hit::Kind::AutoCompare, 0, innerX + halfW + kGap, row2, halfW, kButtonH, false}, "-", false);
    }

    // ---- Sound and passthrough: the output, the volume (drag), 끔 | 흑백 | 컬러 ----
    drawCard(pen, kPad, kDeviceY, W - 2 * kPad, kDeviceH, 20, kCard, kCard, 0);
    pen.text(kPad + kInner, kDeviceY + 36, tr(Tx::DockDeviceTitle), 22, kText, true);
    devicePage_.renderQuick(pen, kPad + kInner, kDeviceY + 52, W - 2 * (kPad + kInner));
    quickShown_ = true;

    // ---- The bottom row: the browser panel, 녹화 (framecorder installed), the full view, collapse ----
    const double third = (W - 2 * kPad - 2 * kGap) / 3;
    // With 녹화: 브라우저 keeps room for 끄기, the other three share the rest
    const double browserPart = record_.available ? (W - 2 * kPad - 3 * kGap) * 0.31 : third;
    const double restPart = record_.available ? (W - 2 * kPad - 3 * kGap - browserPart) / 3 : third;
    if (chromeRunning_) {
        // 브라우저 and, while Chrome runs, 끄기 in its part (a red ring while it waits for the second tap)
        const double quitW = 110, browserW = browserPart - kGap - quitW;
        drawButton(pen, {Hit::Kind::Browser, 0, kPad, kBottomY, browserW, kButtonH, true}, tr(Tx::PageBrowser), false,
                   28);
        const double qx = kPad + browserW + kGap;
        drawButton(pen, {Hit::Kind::QuitChrome, 0, qx, kBottomY, quitW, kButtonH, true}, tr(Tx::DockQuitChrome), false,
                   26);
        if (quitArmedUntil_ >= 0) strokeRounded(pen, qx, kBottomY, quitW, kButtonH, kRadius, kDanger, 3);
    } else {
        drawButton(pen, {Hit::Kind::Browser, 0, kPad, kBottomY, browserPart, kButtonH, true}, tr(Tx::PageBrowser),
                   false, 28);
    }
    double x = kPad + browserPart + kGap;
    if (record_.available) {
        const bool recording = record_.state == 1;
        const std::string label = recording ? frame_perf_overlay::recordElapsedText(record_.seconds)
                                            : std::string(tr(Tx::DockRecord));
        drawButton(pen, {Hit::Kind::Record, 0, x, kBottomY, restPart, kButtonH, record_.state != 2}, label, recording,
                   28);
        x += restPart + kGap;
    }
    drawButton(pen, {Hit::Kind::Full, 0, x, kBottomY, restPart, kButtonH, true}, tr(Tx::DockFull), false, 28);
    drawButton(pen, {Hit::Kind::Collapse, 0, x + restPart + kGap, kBottomY, restPart, kButtonH, true},
               tr(Tx::DockCollapse), false, 28);

    // ---- The note: the colour model's message, the layer being off, or where the controller goes ----
    note_ = tr(Tx::DockAwayHint);
    Color noteColor = kTextMuted;
    bool noteBold = false;
    if (haveColor && !color_->state.message.empty()) {
        note_ = color_->state.message;
        noteColor = color_->state.messageIsError ? kDanger : kSuccess;
        noteBold = color_->state.messageIsError;
    } else if (haveColor && !layer) {
        note_ = color_->tr().layerOff;
    }
    if (record_.available && record_.state >= 2) {  // the recording's news: saving, ended by itself, refused
        const frame_perf_overlay::WidgetText& wt = frame_perf_overlay::widgetText(widgetLanguage(uiLanguage()));
        note_ = record_.state == 2 ? wt.ctlRecordSaving : record_.state == 3 ? wt.ctlRecordFailed : wt.ctlRecordBusy;
        noteColor = record_.state == 2 ? kTextMuted : record_.state == 3 ? kDanger : kWarn;
        noteBold = record_.state != 2;
    }
    if (quitArmedUntil_ >= 0) {  // the answer to the tap just made
        note_ = tr(Tx::DockQuitAgain);
        noteColor = kDanger;
        noteBold = true;
    }
    const double noteSize = fitSize(pen, note_, 19, 13, W - 2 * kPad, noteBold);
    pen.text(kPad, kQuickHeight - 22, note_, noteSize, noteColor, noteBold);
    cairo_surface_flush(surface_);
}

bool DockPanel::pointerMove(double x, double y) {
    const Hit* before = hitAt(px_, py_);
    px_ = x;
    py_ = y;
    const bool device = quickShown_ && devicePage_.pointerMove(x, y);  // hover, the volume drag
    return hitAt(x, y) != before || device;
}

DockPress DockPanel::pointerDown(double x, double y, double now) {
    px_ = x;
    py_ = y;
    DockPress press;
    if (quickShown_ && devicePage_.hitAt(x, y) != DeviceHit::None) {
        armedIndex_ = -1;  // like any other press
        quitArmedUntil_ = -1;
        devicePage_.pointerDown(x, y, now);
        press.kind = DockPress::Kind::DeviceChanged;
        return press;
    }
    const Hit* hit = hitAt(x, y);
    // Any press but the armed button's cancels the pending second tap
    if (hit == nullptr || hit->kind != Hit::Kind::Profile || hit->arg != armedIndex_) armedIndex_ = -1;
    const bool quitWasArmed = quitArmedUntil_ >= 0 && now <= quitArmedUntil_;
    if (hit == nullptr || hit->kind != Hit::Kind::QuitChrome) quitArmedUntil_ = -1;
    if (hit == nullptr) return press;
    pressing_ = true;
    pressedKind_ = hit->kind;
    pressedArg_ = hit->arg;
    switch (hit->kind) {
        case Hit::Kind::Handle: press.kind = DockPress::Kind::GrabHandle; break;
        case Hit::Kind::Profile: {
            const size_t i = static_cast<size_t>(hit->arg);
            if (i >= quickIds_.size()) break;
            const std::string id = quickIds_[i];
            LiveLoss loss;
            for (const auto& item : profiles_.items) {
                if (item.id == id) loss = item.useLoses;
            }
            if (loss.any() && !(armedIndex_ == hit->arg && armedId_ == id && now <= armedUntil_)) {
                armedIndex_ = hit->arg;
                armedId_ = id;
                armedUntil_ = now + SettingsPanel::kQuitConfirmSec;
                break;
            }
            armedIndex_ = -1;
            press.kind = DockPress::Kind::UseProfile;
            press.profileId = id;
            break;
        }
        case Hit::Kind::Preset:
            if (color_ == nullptr) break;
            color_->state.saveMode = false;  // a tap here applies, never saves into the slot
            color_->pressPreset(hit->arg);
            colorPressed_ = true;
            press.kind = DockPress::Kind::ColorChanged;
            break;
        case Hit::Kind::Compare:
            if (color_ == nullptr) break;
            color_->pressCompare();  // the original image while held
            colorPressed_ = true;
            press.kind = DockPress::Kind::ColorChanged;
            break;
        case Hit::Kind::AutoCompare:
            if (color_ == nullptr) break;
            color_->toggleAutoCompare(std::chrono::steady_clock::now());
            colorPressed_ = true;
            press.kind = DockPress::Kind::ColorChanged;
            break;
        case Hit::Kind::Browser: press.kind = DockPress::Kind::OpenBrowser; break;
        case Hit::Kind::QuitChrome:
            if (quitWasArmed) {
                quitArmedUntil_ = -1;
                press.kind = DockPress::Kind::QuitChrome;
            } else {
                quitArmedUntil_ = now + SettingsPanel::kQuitConfirmSec;  // the first tap: asks for a second
            }
            break;
        case Hit::Kind::Record: press.kind = DockPress::Kind::ToggleRecord; break;
        case Hit::Kind::Full: press.kind = DockPress::Kind::ShowFull; break;
        case Hit::Kind::Quick: press.kind = DockPress::Kind::ShowQuick; break;
        case Hit::Kind::Collapse: press.kind = DockPress::Kind::Collapse; break;
        case Hit::Kind::Done: press.kind = DockPress::Kind::Done; break;
    }
    return press;
}

bool DockPanel::pointerUp(double x, double y) {
    px_ = x;
    py_ = y;
    const bool device = devicePage_.pointerUp();  // the volume's final value
    const bool was = pressing_ || device;
    pressing_ = false;
    if (colorPressed_ && color_ != nullptr) color_->release();  // ends compare, like the Color page's release
    colorPressed_ = false;
    return was;
}

bool DockPanel::pointerLeave() {
    const bool device = devicePage_.pointerLeave();  // a volume drag lets go where it is
    const bool changed = pressing_ || px_ >= 0 || colorPressed_ || device;
    px_ = py_ = -1;
    pressing_ = false;
    if (colorPressed_ && color_ != nullptr) color_->leave();  // compare lets go
    colorPressed_ = false;
    return changed;
}

void DockPanel::showStatus(const std::string& text, bool error, double now) {
    status_ = text;
    statusError_ = error;
    statusUntil_ = now + (error ? SettingsPanel::kActionErrorSec : SettingsPanel::kActionStatusSec);
}

bool DockPanel::tick(double now) {
    bool changed = quickShown_ && devicePage_.takeRepeat(now);  // a held − / ＋ of the volume
    if (armedIndex_ >= 0 && now > armedUntil_) {
        armedIndex_ = -1;
        changed = true;
    }
    if (quitArmedUntil_ >= 0 && now > quitArmedUntil_) {
        quitArmedUntil_ = -1;
        changed = true;
    }
    if (!status_.empty() && now > statusUntil_) {
        status_.clear();
        changed = true;
    }
    return changed;
}

double DockPanel::nextWake() const {
    double next = devicePage_.nextRepeatAt();
    if (armedIndex_ >= 0) next = std::min(next, armedUntil_ + 0.01);
    if (quitArmedUntil_ >= 0) next = std::min(next, quitArmedUntil_ + 0.01);
    if (!status_.empty()) next = std::min(next, statusUntil_ + 0.01);
    return next;
}

void DockPanel::renderFull(const SettingsPanel& panel, bool adjusting) {
    const int w = panel.width();
    const int h = panel.height() + kBarHeight;
    ensureSurface(w, h);
    clear();
    if (quickShown_) devicePage_.pointerLeave();
    quickShown_ = false;
    const Pen pen {cr_, &fonts_};
    pen.color(kBg);
    pen.roundedRect(0, 0, w, h, 24);
    cairo_fill(cr_);
    // The buttons first (right to left: 접기, ← 빠른, 완료 while adjusting), then the handle takes the rest
    const double y = (kBarHeight - kBarButtonH) / 2;
    double right = w - kPad;
    std::vector<Hit> buttons;
    buttons.push_back({Hit::Kind::Collapse, 0, right - kBarButtonW, y, kBarButtonW, kBarButtonH, true});
    right -= kBarButtonW + kGap;
    buttons.push_back({Hit::Kind::Quick, 0, right - kBarButtonW, y, kBarButtonW, kBarButtonH, true});
    right -= kBarButtonW + kGap;
    if (adjusting) {
        buttons.push_back({Hit::Kind::Done, 0, right - kBarButtonW, y, kBarButtonW, kBarButtonH, true});
        right -= kBarButtonW + kGap;
    }
    drawHandle(pen, w, right, tr(Tx::DockFullTitle), "");
    if (adjusting) {
        const std::string hint = tr(Tx::DockAdjustHint);
        const double left = kPad + 36 + pen.measure(tr(Tx::DockFullTitle), 30, true) + 24;
        const double size = fitSize(pen, hint, 21, 14, right - 16 - left, true);
        pen.text(left, centerBaseline(0, kBarHeight, size), ellipsize(pen, hint, size, right - 16 - left, true), size,
                 kAccent, true);
    }
    // The handle's hit area stops where the buttons begin; the buttons go first in hits_
    const Hit handle = hits_.back();
    hits_.pop_back();
    for (const Hit& b : buttons) {
        const std::string label = tr(b.kind == Hit::Kind::Collapse ? Tx::DockCollapse
                                     : b.kind == Hit::Kind::Quick  ? Tx::DockQuick
                                                                   : Tx::DockDone);
        drawButton(pen, b, label, b.kind == Hit::Kind::Done, 26);
    }
    hits_.push_back(handle);
    // The pages under the bar
    cairo_save(cr_);
    cairo_set_source_surface(cr_, panel.surface(), 0, kBarHeight);
    cairo_paint(cr_);
    cairo_restore(cr_);
    cairo_surface_flush(surface_);
}

bool DockPanel::barMove(double x, double y) {
    return pointerMove(x, y);
}

DockPress DockPanel::barDown(double x, double y) {
    return pointerDown(x, y, 0.0);
}

bool DockPanel::barUp() {
    const bool was = pressing_;
    pressing_ = false;
    return was;
}

const uint8_t* DockPanel::pixels() const {
    return surface_ != nullptr ? cairo_image_surface_get_data(surface_) : nullptr;
}

int DockPanel::width() const {
    return surfaceW_;
}

int DockPanel::height() const {
    return surfaceH_;
}

const std::vector<uint8_t>& DockPanel::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool DockPanel::writePng(const std::string& path) const {
    return surface_ != nullptr && cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}
