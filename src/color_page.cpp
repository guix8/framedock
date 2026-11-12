#include "color_page.h"

#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace framecolor {

namespace {

// Landscape layout for the 1544 x 468 card area of the tab (FrameColor's portrait panel was 1200 x 920 at the
// same px size in the headset, so the controls keep its sizes: 54-58 px rows, 22 px labels).
constexpr double kRightW = 560;      // presets + auto preset column
constexpr double kGap = 24;          // between cards
constexpr double kPad = 24;          // inside a card
constexpr double kRowH = 54;         // slider / channel rows
constexpr double kLabelW = 300;      // slider titles (the sharpness row also holds the area button here)
constexpr double kValueW = 96;       // value at the right of a slider
constexpr double kAreaW = 160;       // sharpen area button
constexpr double kFitW = 60;         // gaze fit button (with "follow gaze": room from the title and area button)
constexpr double kButtonH = 58;      // preset buttons
constexpr double kButtonGap = 10;
constexpr double kPresetsH = 262;    // presets card height; the auto preset card takes the rest
constexpr double kRuleH = 84;
constexpr double kRadius = 16;

bool inside(double px, double py, double x, double y, double w, double h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

/** Shorten a UTF-8 string with an ellipsis until it fits maxWidth. */
std::string fitText(const Pen& p, std::string text, double size, bool bold, double maxWidth) {
    if (p.measure(text, size, bold) <= maxWidth) return text;
    while (!text.empty() && p.measure(text + "…", size, bold) > maxWidth) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0xC0) text.pop_back();
    }
    return text + "…";
}

/** The channel's colour: all = the accent, then red, green, blue. */
Color tintColor(int tint) {
    switch (tint) {
        case 1: return kDanger;
        case 2: return kSuccess;
        case 3: return kGpu;
        default: return kAccent;
    }
}

std::string sliderText(int index, double value) {
    char text[32];
    if (index == kGammaSlider) std::snprintf(text, sizeof(text), "%.2f", value);
    else std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
    return text;
}

}  // namespace

const ColorPage::Hit* ColorPage::hitTest(double x, double y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (inside(x, y, it->x, it->y, it->w, it->h)) return &*it;
    }
    return nullptr;
}

ColorHit ColorPage::hitAt(double x, double y, int* arg) const {
    const Hit* hit = hitTest(x, y);
    if (arg != nullptr) *arg = hit != nullptr ? hit->arg : -1;
    return hit != nullptr ? hit->kind : ColorHit::None;
}

bool ColorPage::hitCenter(ColorHit kind, int arg, double& x, double& y) const {
    for (const Hit& hit : hits_) {
        if (hit.kind != kind || hit.arg != arg) continue;
        x = hit.x + hit.w / 2;
        y = hit.y + hit.h / 2;
        return true;
    }
    return false;
}

void ColorPage::button(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                       ColorHit kind, int arg, bool selected, bool usable, double size) {
    const bool hovered = usable && inside(px_, py_, x, y, w, h);
    if (!usable) {
        // Disabled: faded, no border (like the Profiles page)
        pen.color(kControl);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
    } else if (selected) {
        pen.color(hovered ? kAccentPressed : kAccent);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
    } else {
        pen.color(hovered ? kControlHover : kControl);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
        strokeRounded(pen, x, y, w, h, kRadius, hovered ? kAccent : kBorder, 2);
    }
    const Color fg = !usable ? kTextDisabled : (selected ? kOnAccent : kText);
    const double labelSize = fitSize(pen, label, size, size * 0.65, w - 20, true);
    textCentered(pen, x + w / 2, centerBaseline(y, h, labelSize), label, labelSize, fg, true);
    if (usable) hits_.push_back({x, y, w, h, kind, arg});
}

void ColorPage::ruleButton(const Pen& pen, double x, double y, double w, double h, const std::string& name,
                           const std::string& value, int rule, bool selected, bool usable) {
    const bool hovered = usable && inside(px_, py_, x, y, w, h);
    if (selected) {
        pen.color(hovered ? kAccentPressed : kAccent);
    } else {
        pen.color(hovered ? kControlHover : kControl);
    }
    pen.roundedRect(x, y, w, h, kRadius);
    cairo_fill(pen.cr);
    if (usable && !selected) strokeRounded(pen, x, y, w, h, kRadius, hovered ? kAccent : kBorder, 2);
    // Two lines: the rule (small) and the preset it picks (large)
    const Color nameColor = selected ? kOnAccent : (usable ? kTextMuted : kTextDisabled);
    const Color valueColor = selected ? kOnAccent : (usable ? kText : kTextDisabled);
    const double nameSize = fitSize(pen, name, 19, 13, w - 20, false);
    const double valueSize = fitSize(pen, value, 23, 15, w - 20, true);
    textCentered(pen, x + w / 2, y + h * 0.38, name, nameSize, nameColor, false);
    textCentered(pen, x + w / 2, y + h * 0.38 + valueSize + 9, value, valueSize, valueColor, true);
    if (usable) hits_.push_back({x, y, w, h, ColorHit::AutoRule, rule});
}

void ColorPage::slider(const Pen& pen, double y, double h, double labelX, double labelW, double valueRight,
                       const std::string& title, int index, bool usable, int tint) {
    const double titleSize = fitSize(pen, title, 22, 15, labelW, true);
    pen.text(labelX, centerBaseline(y, h, titleSize), title, titleSize, usable ? kText : kTextDisabled, true);
    double lo = 0, hi = 1;
    ColorModel::sliderRange(index, lo, hi);
    const double value = model_.sliderValue(index);
    const bool active = model_.state.activeSlider == index;
    const bool ring = active || inside(px_, py_, trackX_ - kSliderReach, y, trackW_ + 2 * kSliderReach, h);
    drawSlider(pen, trackX_, trackW_, y + h / 2, (value - lo) / (hi - lo), tintColor(tint), usable, active, ring);
    pen.text(valueRight, centerBaseline(y, h, 22), sliderText(index, value), 22, usable ? kText : kTextDisabled, true,
             true);
    // Generous to the sides so the ends are easy to reach (FrameColor's track also took 20 px either side)
    if (usable) hits_.push_back({trackX_ - kSliderReach, y, trackW_ + 2 * kSliderReach, h, ColorHit::Slider, index});
}

void ColorPage::render(const Pen& pen, const ColorPageArea& a) {
    hits_.clear();
    const ColorState& ui = model_.state;
    const Translations& tr = model_.tr();
    const bool layer = ui.vulkanLayerActive;

    // ---- Header row: auto compare and hold-to-compare, right-aligned before the status pill ----
    {
        const double h = a.headerHeight - 16;
        const double y = a.headerTop + 8;
        const double avail = a.headerRight - a.headerLeft;
        const double w = std::min(260.0, (avail - 12) / 2);
        if (w >= 120) {
            const double compareX = a.headerRight - w;
            const double autoX = compareX - 12 - w;
            // While running, the auto compare button shows which image is on screen
            const char* autoLabel = !ui.autoCompare ? tr.autoCompare
                                    : ui.autoOriginal ? tr.autoCompareShowingOriginal
                                                      : tr.autoCompareShowingCorrected;
            button(pen, autoX, y, w, h, autoLabel, ColorHit::AutoCompare, 0, ui.autoCompare, layer);
            button(pen, compareX, y, w, h, ui.compare ? tr.compareHeld : tr.compare, ColorHit::Compare, 0, ui.compare,
                   layer);
        }
    }

    // ---- Left card: panel brightness, channel, the four channel sliders, sharpness ----
    const double leftW = a.width - kRightW - kGap;
    drawCard(pen, a.left, a.top, leftW, a.height, 20, kCard, kCard, 0);
    const double labelX = a.left + kPad;
    const double valueRight = a.left + leftW - kPad;
    trackX_ = labelX + kLabelW + 24;
    trackW_ = valueRight - kValueW - 20 - trackX_;
    const double step = (a.height - 2 * 18 - kRowH) / 6;
    const auto rowTop = [&](int row) { return a.top + 18 + row * step; };

    slider(pen, rowTop(0), kRowH, labelX, kLabelW - 12, valueRight, tr.panelBrightness, kPanelSlider, true, 0);

    // The channel the next four sliders adjust; a bar in its colour ties the five rows together
    const double channelSize = fitSize(pen, tr.adjustmentChannel, 22, 15, kLabelW - 12, true);
    pen.text(labelX, centerBaseline(rowTop(1), kRowH, channelSize), tr.adjustmentChannel, channelSize, kText, true);
    {
        const double x = trackX_ - 16;
        const double w = valueRight - x;
        const double part = w / 4;
        std::vector<PillPart> parts;
        for (int i = 0; i < 4; ++i) {
            const bool hovered = inside(px_, py_, x + part * i, rowTop(1), part, kRowH);
            parts.push_back({tr.channelNames[i], false, hovered});
            hits_.push_back({x + part * i, rowTop(1), part, kRowH, ColorHit::Channel, i});
        }
        drawSegmentedPill(pen, x, rowTop(1), w, kRowH, parts, ui.channel, true, 22);
    }
    pen.color(tintColor(ui.channel), layer ? 0.9 : 0.35);
    pen.roundedRect(a.left + 9, rowTop(1) + 6, 5, rowTop(5) + kRowH - rowTop(1) - 12, 2.5);
    cairo_fill(pen.cr);

    const struct {
        const char* title;
        int index;
    } channelSliders[] = {{tr.brightness, kGainSlider},
                          {tr.saturation, kSaturationSlider},
                          {tr.contrast, kContrastSlider},
                          {tr.gamma, kGammaSlider}};
    for (int i = 0; i < 4; ++i) {
        slider(pen, rowTop(2 + i), kRowH, labelX, kLabelW - 12, valueRight, channelSliders[i].title,
               channelSliders[i].index, layer, ui.channel);
    }
    // Sharpness, with the sharpen area button between the title and the track
    const bool fitShown = ui.sharpenGaze && !ui.sharpenWhole;
    const double areaW = fitShown ? kAreaW - 48 : kAreaW;
    const double areaX = labelX + kLabelW - areaW;
    const double fitX = areaX - 6 - kFitW;
    const double titleRight = fitShown ? fitX : areaX;
    // Two lines in place of the track: while a fit runs and a few seconds after one, what the tint shows and when it
    // goes (only when the tint can show: sharpening on and the layer loaded; at 0 the slider stays to turn it up); for
    // a while after a fit failed, why (the button itself only ever says what it does)
    const bool tintShows = fitShown && model_.gazeFollowActive();
    char reviewFirst[160];
    std::snprintf(reviewFirst, sizeof(reviewFirst), tr.gazeFitReviewHint[0], ui.gazeFitReviewLeft);
    const char* hint[2] = {nullptr, nullptr};
    if (tintShows && ui.gazeFitStep >= 0) {
        hint[0] = tr.gazeFitHint[0];
        hint[1] = tr.gazeFitHint[1];
    } else if (tintShows && ui.gazeFitReviewing) {
        hint[0] = reviewFirst;
        hint[1] = tr.gazeFitReviewHint[1];
    } else if (fitShown && ui.gazeFitStep < 0 && ui.gazeFitOutcome == 2 && ui.gazeFitFailure >= 1 && ui.gazeFitFailure <= 4) {
        hint[0] = tr.gazeFitFailed;
        hint[1] = tr.gazeFitFailReason[ui.gazeFitFailure - 1];
    }
    if (hint[0]) {
        const double titleW = titleRight - labelX - 12;
        const double titleSize = fitSize(pen, tr.sharpness, 22, 15, titleW, true);
        pen.text(labelX, centerBaseline(rowTop(6), kRowH, titleSize), tr.sharpness, titleSize, layer ? kText : kTextDisabled,
                 true);
        const double hintX = trackX_ - 16, hintW = valueRight - hintX;
        const double size = std::min(fitSize(pen, hint[0], 19, 13, hintW, false), fitSize(pen, hint[1], 19, 13, hintW, false));
        const double middle = rowTop(6) + kRowH / 2;
        pen.text(hintX, middle - 4, fitText(pen, hint[0], size, false, hintW), size,
                 ui.gazeFitOutcome == 2 && ui.gazeFitStep < 0 && !ui.gazeFitReviewing ? kDanger : kText, false);
        pen.text(hintX, middle + size + 2, fitText(pen, hint[1], size, false, hintW), size, kTextMuted, false);
    } else {
        slider(pen, rowTop(6), kRowH, labelX, titleRight - labelX - 12, valueRight, tr.sharpness, kSharpnessSlider, layer,
               0);
    }
    if (fitShown) {
        char running[48];
        std::snprintf(running, sizeof(running), tr.gazeFitRunning, ui.gazeFitStep + 1, ui.gazeFitSteps);
        // The running step (a tap stops it), else what a tap does: the outcome is told next to it, not on it (2026-11-10:
        // "Done" on the button looked like something to confirm, and a tap started another fit)
        const char* label = ui.gazeFitStep >= 0 ? running : tr.gazeFit;
        button(pen, fitX, rowTop(6) + 4, kFitW, kRowH - 8, label, ColorHit::GazeFit, 0, ui.gazeFitStep >= 0, layer, 20);
    }
    button(pen, areaX, rowTop(6) + 4, areaW, kRowH - 8, ui.sharpenWhole ? tr.sharpenWhole : ui.sharpenGaze ? tr.sharpenGaze : tr.sharpenCenter,
           ColorHit::SharpenArea, 0, false, layer, 20);

    // ---- Right column: presets, then the auto preset rules ----
    const double rx = a.left + leftW + kGap;
    const double innerX = rx + kPad;
    const double innerW = kRightW - 2 * kPad;
    drawCard(pen, rx, a.top, kRightW, kPresetsH, 20, kCard, kCard, 0);
    pen.text(innerX, a.top + 42, tr.presets, 24, kText, true);
    // A newer layer waits for a SteamVR restart: said on the title's row
    if (model_.checkLayerReload(Clock::now())) {
        const double noteW = innerW - pen.measure(tr.presets, 24, true) - 20;
        const double size = fitSize(pen, tr.layerReload, 19, 13, noteW, false);
        pen.text(innerX + innerW, a.top + 42, fitText(pen, tr.layerReload, size, false, noteW), size, kAccentText,
                 false, true);
    }
    const double bw = (innerW - 2 * kButtonGap) / 3;
    const double row0 = a.top + 60;
    const double row1 = row0 + kButtonH + 8;
    const double row2 = row1 + kButtonH + 8;
    // The preset whose values are the ones in use shows selected (not while picking a slot to save to)
    for (int i = 0; i < 3; ++i) {
        // Built-in presets can't be a save target
        button(pen, innerX + i * (bw + kButtonGap), row0, bw, kButtonH, tr.builtinPresets[i], ColorHit::Preset, i,
               !ui.saveMode && model_.presetInUse(i), !ui.saveMode);
    }
    for (int i = 0; i < 3; ++i) {
        std::string label = "P" + std::to_string(i + 1);
        if (!ui.custom[i].valid) label += tr.emptySlotSuffix;
        button(pen, innerX + i * (bw + kButtonGap), row1, bw, kButtonH, label, ColorHit::Preset, i + 3,
               !ui.saveMode && model_.presetInUse(i + 3), true);
    }
    const double halfW = (innerW - kButtonGap) / 2;
    button(pen, innerX, row2, halfW, kButtonH, ui.saveMode ? tr.selectSlot : tr.save, ColorHit::SaveMode, 0,
           ui.saveMode, true);
    button(pen, innerX + halfW + kButtonGap, row2, halfW, kButtonH, ui.resetConfirm ? tr.resetConfirm : tr.reset,
           ColorHit::Reset, 0, ui.resetConfirm, true);

    const double autoY = a.top + kPresetsH + 16;
    const double autoH = a.height - kPresetsH - 16;
    drawCard(pen, rx, autoY, kRightW, autoH, 20, kCard, kCard, 0);
    pen.text(innerX, autoY + 42, tr.autoPreset, 24, kText, true);
    const char* ruleNames[3] = {tr.standalone, tr.streaming, tr.thisApp};
    const double rw = (innerW - 2 * kButtonGap) / 3;
    for (int rule = 0; rule < 3; ++rule) {
        const bool usable = rule < 2 || !ui.context.appKey.empty();
        const int choice = model_.ruleValue(rule);
        std::string value = usable ? model_.presetName(choice) : std::string("-");
        if (choice >= 3 && !ui.custom[choice - 3].valid) value += tr.emptySlotSuffix;
        const bool selected = ui.contextKnown && model_.governingRule() == rule && choice >= 0;
        ruleButton(pen, innerX + rule * (rw + kButtonGap), autoY + 60, rw, kRuleH, ruleNames[rule], value, rule,
                   selected, usable);
    }
    std::string now = std::string(tr.nowPrefix) + (ui.context.streaming ? tr.streaming : tr.standalone) + " · " +
                      (ui.appName.empty() ? tr.noApp : ui.appName);
    if (model_.sharpenPausedForStreaming()) now += std::string(" · ") + tr.sharpenPausedStreaming;
    pen.text(innerX, autoY + 60 + kRuleH + 30, fitText(pen, now, 19, false, innerW), 19, kTextMuted);
}

bool ColorPage::statusLine(std::string& text, bool& error) const {
    const ColorState& ui = model_.state;
    if (!ui.message.empty()) {
        text = ui.message;
        error = ui.messageIsError;
        return true;
    }
    if (!ui.vulkanLayerActive) {
        text = model_.tr().layerOff;
        error = false;
        return true;
    }
    return false;
}

bool ColorPage::pointerMove(double x, double y) {
    const Hit* before = hitTest(px_, py_);
    px_ = x;
    py_ = y;
    if (model_.state.activeSlider >= 0) {
        model_.updateSlider(model_.state.activeSlider, fractionAt(x));
        return true;
    }
    return hitTest(x, y) != before;
}

bool ColorPage::pointerDown(double x, double y, Clock::time_point now) {
    px_ = x;
    py_ = y;
    pressed_ = true;
    const Hit* hit = hitTest(x, y);
    if (hit == nullptr) {
        model_.dismiss();
        return true;
    }
    switch (hit->kind) {
        case ColorHit::Slider:
            model_.state.activeSlider = hit->arg;
            model_.state.resetConfirm = false;
            model_.updateSlider(hit->arg, fractionAt(x));
            break;
        case ColorHit::Channel: model_.selectChannel(hit->arg); break;
        case ColorHit::Preset: model_.pressPreset(hit->arg); break;
        case ColorHit::SaveMode: model_.toggleSaveMode(); break;
        case ColorHit::Reset: model_.pressReset(); break;
        case ColorHit::Compare: model_.pressCompare(); break;
        case ColorHit::AutoCompare: model_.toggleAutoCompare(now); break;
        case ColorHit::SharpenArea: model_.toggleSharpenArea(); break;
        case ColorHit::AutoRule: model_.pressRule(hit->arg); break;
        case ColorHit::GazeFit: model_.state.gazeFitPressed = true; break;
        case ColorHit::None: break;
    }
    return true;
}

bool ColorPage::pointerUp() {
    if (!pressed_) return false;
    pressed_ = false;
    model_.release();
    return true;
}

bool ColorPage::pointerLeave() {
    px_ = py_ = -1;
    pressed_ = false;
    model_.leave();
    return true;
}

}  // namespace framecolor
