// The 기기 page and the Dock's 소리 · 패스스루 card. See device_page.h.
#include "device_page.h"

#include "app_text.h"
#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

// The page: 소리 and 패스스루 stacked on the left, PC 계정 맞추기 on the right (the 1544 × 468 card area)
constexpr double kLeftW = 840;
constexpr double kCardGap = 24;
constexpr double kStackGap = 14;
constexpr double kPad = 20;        // inside a card
constexpr double kRowH = 68;       // the dashboard's buttons
constexpr double kRowGap = 11;
constexpr double kLabelW = 110;    // row titles (출력, 음량, 화면)
constexpr double kMuteW = 130;
constexpr double kValueW = 90;     // "100%"
constexpr double kStepGap = 12;    // between − / ＋ and the slider's hit area
constexpr double kPassPillW = 480;
constexpr double kRadius = 16;
// PC 계정 맞추기
constexpr double kShutdownW = 120, kShutdownH = 48;
constexpr double kBigH = 92;
constexpr double kAnswerW = 104, kAnswerH = 48;
constexpr double kLineH = 58;      // the question / message line
constexpr double kListRowH = 52, kListRowGap = 6;
constexpr double kChooseW = 104;
constexpr double kChooseNarrowW = 80;  // in two columns

bool inside(double px, double py, double x, double y, double w, double h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

/** Up to maxLines lines of text that fit maxW (wrapText; the last line ellipsized with the rest on it). */
std::vector<std::string> wrapLines(const Pen& pen, const std::string& text, double size, bool bold, double maxW,
                                   size_t maxLines) {
    std::vector<std::string> lines = wrapText(pen, text, size, maxW, bold);
    if (lines.size() > maxLines) {
        std::string rest;
        for (size_t i = maxLines - 1; i < lines.size(); ++i) {
            // wrapText breaks CJK text between characters: join those back without a space
            const unsigned char first = lines[i].empty() ? 0 : static_cast<unsigned char>(lines[i][0]);
            const bool cjk = first >= 0xE3 && first <= 0xE9;
            rest += (rest.empty() || cjk ? "" : " ") + lines[i];
        }
        lines.resize(maxLines);
        lines.back() = rest;
    }
    if (!lines.empty()) lines.back() = ellipsize(pen, lines.back(), size, maxW, bold);
    return lines;
}

/** The first character of a UTF-8 string ("?" when empty). */
std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    std::string first = name.substr(0, std::min(n, name.size()));
    if (n == 1) first[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(first[0])));
    return first;
}

/** A row's title in the room left of its controls (shrunk to fit: "Passthrough" on the Dock's card) */
void rowLabel(const Pen& pen, double labelX, double controlX, double y, double h, const char* label) {
    const double size = fitSize(pen, label, 22, 15, controlX - labelX - 10, false);
    pen.text(labelX, centerBaseline(y, h, size), label, size, kTextMuted);
}

Color toneColor(SyncScreen::Tone tone) {
    switch (tone) {
        case SyncScreen::Tone::On: return kSuccess;
        case SyncScreen::Tone::Waking: return kWarn;
        case SyncScreen::Tone::Error: return kDanger;
        case SyncScreen::Tone::Muted: break;
    }
    return kTextMuted;
}

}  // namespace

int segmentAt(double x, double pillX, double pillW, size_t n) {
    if (n == 0 || x < pillX || x > pillX + pillW) return -1;
    for (size_t i = 0; i < n; ++i) {
        double left = 0, right = 0;
        pillPartLeft(pillX, pillW, n, i, &left, &right);
        if (x >= left && (x < right || (i + 1 == n && x <= right))) return static_cast<int>(i);
    }
    return -1;
}

int draggedVolume(int startVolume, double startX, double x, double trackW) {
    const double moved = trackW > 0 ? (x - startX) / trackW * 100.0 : 0.0;
    return std::clamp(static_cast<int>(std::lround(startVolume + moved)), 0, 100);
}

int steppedVolume(int volume, int dir) {
    // To the grid first (43 -> 40 or 45), then a whole step from a value on it
    const int down = (volume / kVolumeStep) * kVolumeStep;
    const int next = dir < 0 ? (down == volume ? volume - kVolumeStep : down) : down + kVolumeStep;
    return std::clamp(next, 0, 100);
}

DevicePage::~DevicePage() {
    for (auto& [path, surface] : avatars_) {
        if (surface != nullptr) cairo_surface_destroy(surface);
    }
}

const DevicePage::Hit* DevicePage::hitTest(double x, double y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (inside(x, y, it->x, it->y, it->w, it->h)) return &*it;
    }
    return nullptr;
}

bool DevicePage::hovered(double x, double y, double w, double h) const {
    return inside(px_, py_, x, y, w, h);
}

DeviceHit DevicePage::hitAt(double x, double y, int* arg) const {
    const Hit* hit = hitTest(x, y);
    if (arg != nullptr) *arg = hit != nullptr ? hit->arg : -1;
    return hit != nullptr ? hit->kind : DeviceHit::None;
}

bool DevicePage::hitCenter(DeviceHit kind, int arg, double& x, double& y) const {
    for (const Hit& hit : hits_) {
        if (hit.kind != kind || hit.arg != arg) continue;
        x = hit.x + hit.w / 2;
        y = hit.y + hit.h / 2;
        return true;
    }
    return false;
}

void DevicePage::button(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                        DeviceHit kind, int arg, bool selected, bool usable, double size) {
    const bool active = usable && (hovered(x, y, w, h) || isPressed(kind, arg));
    if (!usable) {
        pen.color(kControl);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
    } else if (selected) {
        pen.color(active ? kAccentPressed : kAccent);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
    } else {
        pen.color(active ? kControlHover : kControl);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
        strokeRounded(pen, x, y, w, h, kRadius, active ? kAccent : kBorder, 2);
    }
    const Color fg = !usable ? kTextDisabled : (selected ? kOnAccent : kText);
    const double labelSize = fitSize(pen, label, size, size * 0.65, w - 20, true);
    textCentered(pen, x + w / 2, centerBaseline(y, h, labelSize), ellipsize(pen, label, labelSize, w - 20, true),
                 labelSize, fg, true);
    if (usable) hits_.push_back({x, y, w, h, kind, arg});
}

void DevicePage::segments(const Pen& pen, double x, double y, double w, double h,
                          const std::vector<std::string>& labels, const std::vector<bool>& usable, int selected,
                          bool allUsable, DeviceHit kind, const std::vector<int>& args, double size) {
    const size_t n = labels.size();
    if (n == 0) return;
    const double part = (w - 2 * kPillInset) / static_cast<double>(n);
    std::vector<PillPart> parts;
    for (size_t i = 0; i < n; ++i) {
        const bool on = allUsable && usable[i];
        double hitLeft = 0, hitRight = 0;
        pillPartLeft(x, w, n, i, &hitLeft, &hitRight);
        const bool isSelected = static_cast<int>(i) == selected;
        // Long names (an output's) are cut to the part; a disabled part's label is drawn below, faded
        const double room = part - 20 - (isSelected ? size * 0.9 : 0);
        PillPart p;
        p.label = on || !allUsable ? ellipsize(pen, labels[i], size * 0.75, room, isSelected) : std::string();
        p.pressed = on && isPressed(kind, args[i]);
        p.hovered = on && hovered(hitLeft, y, hitRight - hitLeft, h);
        parts.push_back(p);
        if (on) hits_.push_back({hitLeft, y, hitRight - hitLeft, h, kind, args[i]});
    }
    drawSegmentedPill(pen, x, y, w, h, parts, selected, allUsable, size);
    if (!allUsable) return;
    for (size_t i = 0; i < n; ++i) {
        if (usable[i]) continue;
        const double left = pillPartLeft(x, w, n, i);
        const double labelSize = fitSize(pen, labels[i], size, size * 0.7, part - 20, false);
        textCentered(pen, left + part / 2, centerBaseline(y + kPillInset, h - 2 * kPillInset, labelSize), labels[i],
                     labelSize, kTextDisabled, false);
    }
}

void DevicePage::outputRow(const Pen& pen, double labelX, double controlX, double right, double y, double h,
                           double size) {
    const AudioState& audio = model_->audio;
    rowLabel(pen, labelX, controlX, y, h, tr(Tx::DeviceOutput));
    const double w = right - controlX;
    if (!audio.known || audio.outputs.empty()) {
        // Nothing to pick: a faded pill that says why
        segments(pen, controlX, y, w, h, {tr(audio.known ? Tx::DeviceNoOutputs : Tx::DeviceChecking)}, {false}, -1,
                 false, DeviceHit::Output, {0}, size);
        return;
    }
    std::vector<std::string> labels;
    std::vector<int> args;
    int selected = -1;
    for (size_t i = 0; i < audio.outputs.size(); ++i) {
        labels.push_back(outputLabel(audio.outputs[i]));
        args.push_back(static_cast<int>(i));
        if (audio.outputs[i].id == audio.defaultId) selected = static_cast<int>(i);
    }
    const std::vector<bool> usable(labels.size(), true);
    if (labels.size() == 1) {
        // The only output: half a pill, selected, and a note that there is no other
        const double pillW = std::min(w, std::max(w / 2, 260.0));
        segments(pen, controlX, y, pillW, h, labels, usable, selected, true, DeviceHit::Output, args, size);
        const double room = right - (controlX + pillW + 18);
        if (room > 60) {
            const std::string note = tr(Tx::DeviceOnlyOutput);
            const double noteSize = fitSize(pen, note, 19, 14, room, false);
            pen.text(controlX + pillW + 18, centerBaseline(y, h, noteSize), ellipsize(pen, note, noteSize, room, false),
                     noteSize, kTextMuted);
        }
        return;
    }
    segments(pen, controlX, y, w, h, labels, usable, selected, true, DeviceHit::Output, args, size);
}

void DevicePage::volumeRow(const Pen& pen, double labelX, double controlX, double right, double y, double h,
                           double size, double muteW) {
    const AudioState& audio = model_->audio;
    const bool usable = audio.known && !audio.outputs.empty();
    rowLabel(pen, labelX, controlX, y, h, tr(Tx::DeviceVolume));
    button(pen, controlX, y + 2, muteW, h - 4, tr(Tx::DeviceMute), DeviceHit::Mute, 0, audio.muted, usable, size * 0.9);
    // − slider ＋ value: square steps, the slider's hit area (kSliderReach past its ends) between them
    const double stepW = h - 4;
    const double minusX = controlX + muteW + 16;
    const double plusX = right - kValueW - 12 - stepW;
    button(pen, minusX, y + 2, stepW, h - 4, "−", DeviceHit::VolumeDown, 0, false, usable, size * 1.2);
    button(pen, plusX, y + 2, stepW, h - 4, "＋", DeviceHit::VolumeUp, 0, false, usable, size * 1.2);
    trackX_ = minusX + stepW + kStepGap + kSliderReach;
    trackW_ = std::max(40.0, plusX - kStepGap - kSliderReach - trackX_);
    const bool ring = dragging_ || hovered(trackX_ - kSliderReach, y, trackW_ + 2 * kSliderReach, h);
    // Muted: the track keeps its value, in grey
    drawSlider(pen, trackX_, trackW_, y + h / 2, audio.volume / 100.0, audio.muted ? kTextMuted : kAccent, usable,
               dragging_, ring);
    const std::string value = usable ? std::to_string(audio.volume) + "%" : std::string("-");
    const double valueSize = size * 0.95;
    pen.text(right, centerBaseline(y, h, valueSize), value, valueSize,
             !usable ? kTextDisabled : (audio.muted ? kTextMuted : kText), true, true);
    if (usable) hits_.push_back({trackX_ - kSliderReach, y, trackW_ + 2 * kSliderReach, h, DeviceHit::Volume, 0});
}

void DevicePage::passthroughRow(const Pen& pen, double labelX, double controlX, double pillW, double right, double y,
                                double h, double size, const char* label) {
    const PassthroughState& pass = model_->passthrough;
    rowLabel(pen, labelX, controlX, y, h, label);
    // With Camera Switch: 끔 | 흑백 | 컬러. Without it (no colour cameras' switch): 끔 | 켬, the colour left as it is
    if (pass.cameraSwitchAvailable) {
        const int selected = pass.roomViewKnown ? static_cast<int>(pass.mode) : -1;
        segments(pen, controlX, y, pillW, h, {tr(Tx::DevicePassOff), tr(Tx::DevicePassMono), tr(Tx::DevicePassColor)},
                 {true, true, true}, selected,
                 true, DeviceHit::Passthrough,
                 {static_cast<int>(PassthroughMode::Off), static_cast<int>(PassthroughMode::Mono),
                  static_cast<int>(PassthroughMode::Color)},
                 size);
    } else {
        const int selected = !pass.roomViewKnown ? -1 : pass.mode == PassthroughMode::Off ? 0 : 1;
        segments(pen, controlX, y, pillW, h, {tr(Tx::DevicePassOff), tr(Tx::DevicePassOn)}, {true, true}, selected,
                 true, DeviceHit::Passthrough,
                 {static_cast<int>(PassthroughMode::Off), static_cast<int>(PassthroughMode::Mono)}, size);
    }
    // Beside the pill (the dashboard page; the quick view has no room): what couldn't be read
    const char* note = nullptr;
    if (!pass.roomViewKnown) {
        note = tr(Tx::DevicePassUnknown);
    } else if (pass.mode != PassthroughMode::Off && pass.color != ColorSource::Read) {
        note = tr(pass.color == ColorSource::Remembered ? Tx::DevicePassRemembered : Tx::DevicePassColorUnknown);
    }
    const double room = right - (controlX + pillW + 18);
    if (note != nullptr && room > 60) {
        const double noteSize = fitSize(pen, note, 19, 13, room, false);
        pen.text(controlX + pillW + 18, centerBaseline(y, h, noteSize), ellipsize(pen, note, noteSize, room, false),
                 noteSize, kTextMuted);
    }
}

void DevicePage::drawAvatar(const Pen& pen, const std::string& steamId, const std::string& name, double cx, double cy,
                            double r) {
    cairo_t* cr = pen.cr;
    cairo_surface_t* image = nullptr;
    const auto path = model_->sync.avatars.find(steamId);
    if (path != model_->sync.avatars.end() && !path->second.empty()) {
        auto cached = avatars_.find(path->second);
        if (cached == avatars_.end()) {
            cairo_surface_t* loaded = cairo_image_surface_create_from_png(path->second.c_str());
            if (cairo_surface_status(loaded) != CAIRO_STATUS_SUCCESS || cairo_image_surface_get_width(loaded) <= 0) {
                cairo_surface_destroy(loaded);  // a broken image (Steam writing it) never stops the page
                loaded = nullptr;
            }
            cached = avatars_.emplace(path->second, loaded).first;
        }
        image = cached->second;
    }
    if (image != nullptr) {
        const double iw = cairo_image_surface_get_width(image);
        const double ih = cairo_image_surface_get_height(image);
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_clip(cr);
        cairo_translate(cr, cx - r, cy - r);
        cairo_scale(cr, 2 * r / iw, 2 * r / ih);
        cairo_set_source_surface(cr, image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    // No picture: the initial on a colour of its own
    static const Color kAvatarColors[] = {kAccent, kGpu, kSuccess, kWarn};
    unsigned hash = 2166136261u;
    for (const char c : steamId) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    drawDisc(cr, cx, cy, r, kAvatarColors[hash % 4]);
    const std::string letter = initial(name);
    const double size = r * 1.05;
    textCentered(pen, cx, centerBaseline(cy - r, 2 * r, size), letter, size, kOnAccent, true);
}

void DevicePage::syncCard(const Pen& pen, double x, double y, double w, double h) {
    const SyncState& state = model_->sync;
    const SyncScreen screen = syncScreen(state);
    const auto names = state.names();
    const double innerX = x + kPad;
    const double innerW = w - 2 * kPad;
    const double right = x + w - kPad;
    drawCard(pen, x, y, w, h, 20, kCard, kCard, 0);
    pen.text(innerX, y + 42, tr(Tx::SyncTitle), 24, kText, true);
    button(pen, right - kShutdownW, y + 14, kShutdownW, kShutdownH, tr(Tx::SyncShutdown), DeviceHit::Shutdown, 0, false,
           screen.shutdownEnabled, 20);

    // The PC's line: ● 집 PC 켜짐 · (avatar) the account it runs as · 스트리밍 중 · 게임 중
    const double stripY = y + 60, stripH = 40;
    double sx = innerX;
    if (!screen.strip.empty()) {
        const Color tone = toneColor(screen.tone);
        std::string strip = screen.strip;
        const std::string dot = "● ";
        if (strip.compare(0, dot.size(), dot) == 0) {
            strip = strip.substr(dot.size());
            drawDisc(pen.cr, sx + 7, stripY + stripH / 2, 7, tone);
            sx += 22;
        }
        sx += pen.text(sx, centerBaseline(stripY, stripH, 21), strip, 21,
                       screen.tone == SyncScreen::Tone::Muted ? kTextMuted : kText, true);
    }
    if (!screen.pcAccountId.empty()) {
        sx += pen.text(sx, centerBaseline(stripY, stripH, 21), "  ·  ", 21, kTextMuted);
        drawAvatar(pen, screen.pcAccountId, screen.pcAccountName, sx + 15, stripY + stripH / 2, 15);
        sx += 38;
        const std::string name = ellipsize(pen, screen.pcAccountName, 21, std::max(40.0, right - sx - 150), false);
        sx += pen.text(sx, centerBaseline(stripY, stripH, 21), name, 21, kText);
    }
    if (!screen.extra.empty()) {
        const std::string extra = "  ·  " + screen.extra;
        pen.text(sx, centerBaseline(stripY, stripH, 19), ellipsize(pen, extra, 19, right - sx, false), 19, kTextMuted);
    }

    // The big button: the Frame's account and what pressing it does
    const double bigY = y + 106;
    const bool bigOn = screen.bigEnabled;
    const bool bigActive = bigOn && (hovered(innerX, bigY, innerW, kBigH) || isPressed(DeviceHit::SyncBig, 0));
    pen.color(!bigOn ? kControl : (bigActive ? kAccentPressed : kAccent), bigOn || !state.busy ? 1.0 : 0.6);
    pen.roundedRect(innerX, bigY, innerW, kBigH, 22);
    cairo_fill(pen.cr);
    if (state.busy) {
        // A run: the accent outline says something is happening although the button can't be pressed
        strokeRounded(pen, innerX, bigY, innerW, kBigH, 22, kAccent, 3);
    }
    const double avatarR = 30;
    const auto frameName = names.find(screen.bigAccountId);
    drawAvatar(pen, screen.bigAccountId, frameName != names.end() ? frameName->second : std::string(),
               innerX + 22 + avatarR, bigY + kBigH / 2, avatarR);
    const double textX = innerX + 22 + 2 * avatarR + 20;
    const double textW = innerX + innerW - 20 - textX;
    const double bigSize = fitSize(pen, screen.bigText, 28, 18, textW, true);
    pen.text(textX, centerBaseline(bigY, kBigH, bigSize), ellipsize(pen, screen.bigText, bigSize, textW, true),
             bigSize, bigOn ? kOnAccent : (state.busy ? kText : kTextDisabled), true);
    if (bigOn) hits_.push_back({innerX, bigY, innerW, kBigH, DeviceHit::SyncBig, 0});
    bigId_ = screen.bigAccountId;

    // The question line (계속 / 취소), else the message (two lines at most)
    const double lineY = bigY + kBigH + 8;
    const double lineH = kLineH;
    if (!screen.question.empty()) {
        pen.color(blendColor(kWarn, kCard, 0.14));
        pen.roundedRect(innerX, lineY, innerW, lineH, 14);
        cairo_fill(pen.cr);
        const double buttonsX = right - 2 * kAnswerW - 10 - 8;
        const auto lines = wrapLines(pen, screen.question, 18, true, buttonsX - innerX - 24, 2);
        const double first = lineY + (lines.size() > 1 ? 25 : 36);
        for (size_t i = 0; i < lines.size(); ++i) pen.text(innerX + 14, first + 23 * i, lines[i], 18, kText, true);
        const double by = lineY + (lineH - kAnswerH) / 2;
        button(pen, buttonsX, by, kAnswerW, kAnswerH, screen.yesText, DeviceHit::AnswerYes, 0, true, true, 21);
        button(pen, buttonsX + kAnswerW + 10, by, kAnswerW, kAnswerH, tr(Tx::SyncCancel), DeviceHit::AnswerNo, 0, false,
               true, 21);
    } else if (!screen.message.empty()) {
        const auto lines = wrapLines(pen, screen.message, 19, screen.messageError, innerW, 2);
        const double first = lineY + (lines.size() > 1 ? 23 : 36);
        for (size_t i = 0; i < lines.size(); ++i) {
            pen.text(innerX, first + 25 * i, lines[i], 19, screen.messageError ? kDanger : kSuccess,
                     screen.messageError);
        }
    }

    // The other accounts: avatar, name, 선택
    const double listY = lineY + lineH + 8;
    pen.text(innerX, listY + 20, tr(Tx::SyncOthers), 19, kTextMuted);
    rowIds_.clear();
    // More than two accounts go into two columns, so four fit where two rows would
    const int columns = screen.rows.size() > 2 ? 2 : 1;
    const double tileW = (innerW - (columns - 1) * kListRowGap) / columns;
    const double chooseW = columns > 1 ? kChooseNarrowW : kChooseW;
    const double bottom = y + h - 14;
    size_t shown = 0;
    for (const framesync::Row& row : screen.rows) {
        const double tileX = innerX + static_cast<double>(shown % columns) * (tileW + kListRowGap);
        const double rowY = listY + 30 + static_cast<double>(shown / columns) * (kListRowH + kListRowGap);
        if (rowY + kListRowH > bottom) break;
        pen.color(kControl, 0.55);
        pen.roundedRect(tileX, rowY, tileW, kListRowH, 14);
        cairo_fill(pen.cr);
        drawAvatar(pen, row.steamId, row.name, tileX + 12 + 17, rowY + kListRowH / 2, 17);
        const double nameX = tileX + 12 + 36 + 14;
        const double nameW = tileX + tileW - chooseW - 16 - nameX;
        pen.text(nameX, centerBaseline(rowY, kListRowH, 21), ellipsize(pen, row.name, 21, nameW, false), 21, kText);
        button(pen, tileX + tileW - chooseW - 6, rowY + 5, chooseW, kListRowH - 10, tr(Tx::SyncChoose),
               DeviceHit::SyncRow,
               static_cast<int>(rowIds_.size()), false, screen.rowsEnabled, 20);
        rowIds_.push_back(row.steamId);
        ++shown;
    }
    if (screen.rows.empty()) {
        pen.text(innerX, listY + 30 + 30, tr(Tx::SyncNoOthers), 19, kTextDisabled);
    } else if (shown < screen.rows.size()) {
        const std::string more = trf(Tx::SyncMore, {std::to_string(screen.rows.size() - shown)});
        pen.text(right, listY + 20, more, 19, kTextMuted, false, true);
    }
}

void DevicePage::render(const Pen& pen, const DevicePageArea& a) {
    hits_.clear();
    rowIds_.clear();
    const PassthroughState& pass = model_->passthrough;
    // PC 계정 맞추기 only where Frame Sync is set up (its config.json and key): else 소리 and 패스스루 take the width
    const bool sync = model_->sync.present;
    const double leftW = sync ? kLeftW : a.width;

    // ---- 소리 ----
    const double soundH = 60 + 2 * kRowH + kRowGap + 20;
    drawCard(pen, a.left, a.top, leftW, soundH, 20, kCard, kCard, 0);
    const double labelX = a.left + kPad;
    const double controlX = labelX + kLabelW;
    const double right = a.left + leftW - kPad;
    pen.text(labelX, a.top + 42, tr(Tx::DeviceSound), 24, kText, true);
    outputRow(pen, labelX, controlX, right, a.top + 60, kRowH, 22);
    volumeRow(pen, labelX, controlX, right, a.top + 60 + kRowH + kRowGap, kRowH, 22, kMuteW);

    // ---- 패스스루 ----
    const double passY = a.top + soundH + kStackGap;
    const double passH = a.height - soundH - kStackGap;
    drawCard(pen, a.left, passY, leftW, passH, 20, kCard, kCard, 0);
    pen.text(labelX, passY + 42, tr(Tx::DevicePassthrough), 24, kText, true);
    passthroughRow(pen, labelX, controlX, kPassPillW, right, passY + 60, kRowH, 22, tr(Tx::DevicePassView));
    const double noteY = passY + 60 + kRowH + 30;
    const double noteW = right - labelX;
    const auto note = [&](double y, Tx key, Color color, bool bold) {
        const std::string text = tr(key);
        const double size = fitSize(pen, text, 19, 14, noteW, bold);
        pen.text(labelX, y, ellipsize(pen, text, size, noteW, bold), size, color, bold);
    };
    if (!pass.cameraSwitchAvailable) {
        note(noteY, Tx::DevicePassNoCameraSwitch, kWarn, true);
        note(noteY + 28, Tx::DevicePassNote1, kTextMuted, false);
    } else {
        note(noteY, Tx::DevicePassNote1, kTextMuted, false);
        note(noteY + 28, Tx::DevicePassNote2, kTextMuted, false);
    }

    // ---- PC 계정 맞추기 ----
    if (sync) {
        const double syncX = a.left + kLeftW + kCardGap;
        syncCard(pen, syncX, a.top, a.left + a.width - syncX, a.height);
    }
}

void DevicePage::renderQuick(const Pen& pen, double x, double y, double w) {
    hits_.clear();
    rowIds_.clear();
    const double labelW = 128;
    const double controlX = x + labelW;
    const double right = x + w;
    outputRow(pen, x, controlX, right, y, kQuickRowH, 24);
    volumeRow(pen, x, controlX, right, y + kQuickRowH + kQuickRowGap, kQuickRowH, 24, 140);
    passthroughRow(pen, x, controlX, right - controlX, right, y + 2 * (kQuickRowH + kQuickRowGap), kQuickRowH, 24,
                   tr(Tx::DevicePassthrough));
}

bool DevicePage::pointerMove(double x, double y) {
    const Hit* before = hitTest(px_, py_);
    px_ = x;
    py_ = y;
    if (dragging_) {
        // Nothing until it has moved a little (the press's own jitter). The drag starts there, from the volume then:
        // the backend may have changed it since the press
        if (!dragMoved_) {
            if (std::fabs(x - dragStartX_) < kVolumeDragStartPx) return false;
            dragMoved_ = true;
            dragStartX_ = x;
            dragStartVolume_ = model_->audio.volume;
        }
        model_->dragVolume(draggedVolume(dragStartVolume_, dragStartX_, x, trackW_));
        return true;
    }
    const Hit* hit = hitTest(x, y);
    // The laser slid off the held − / ＋: no more steps (the volume is let go on the release)
    if (repeat_.active() && (hit == nullptr || hit->kind != pressedKind_)) repeat_.stop();
    return hit != before;
}

bool DevicePage::pointerDown(double x, double y, double now) {
    px_ = x;
    py_ = y;
    repeat_.stop();
    const Hit* hit = hitTest(x, y);
    if (hit == nullptr) return false;
    pressing_ = true;
    pressedKind_ = hit->kind;
    pressedArg_ = hit->arg;
    DeviceModel& m = *model_;
    switch (hit->kind) {
        case DeviceHit::Output:
            if (hit->arg >= 0 && static_cast<size_t>(hit->arg) < m.audio.outputs.size()) {
                m.selectOutput(m.audio.outputs[static_cast<size_t>(hit->arg)].id);
            }
            break;
        case DeviceHit::Volume:
            // Nothing changes or is queued until a drag (pointerMove); a press and release alone leave it as it was
            dragging_ = true;
            dragMoved_ = false;
            dragStartX_ = x;
            break;
        case DeviceHit::VolumeDown:
        case DeviceHit::VolumeUp:
            // One step now, more while held (takeRepeat); the release queues the final value like a drag's
            m.dragVolume(steppedVolume(m.audio.volume, hit->kind == DeviceHit::VolumeDown ? -1 : 1));
            repeat_.start(now);
            break;
        case DeviceHit::Mute: m.setMuted(!m.audio.muted); break;
        case DeviceHit::Passthrough: m.setPassthrough(static_cast<PassthroughMode>(hit->arg)); break;
        case DeviceHit::SyncBig: m.startSync(bigId_, false); break;
        case DeviceHit::SyncRow:
            if (hit->arg >= 0 && static_cast<size_t>(hit->arg) < rowIds_.size()) {
                m.startSync(rowIds_[static_cast<size_t>(hit->arg)], true);
            }
            break;
        case DeviceHit::Shutdown: m.askShutdown(); break;
        case DeviceHit::AnswerYes: m.answer(true); break;
        case DeviceHit::AnswerNo: m.answer(false); break;
        case DeviceHit::None: break;
    }
    return true;
}

bool DevicePage::pointerUp() {
    const bool was = pressing_ || dragging_;
    if (holdsVolume()) model_->releaseVolume();
    dragging_ = false;
    pressing_ = false;
    repeat_.stop();
    return was;
}

bool DevicePage::pointerLeave() {
    const bool changed = pressing_ || dragging_ || px_ >= 0;
    if (holdsVolume()) model_->releaseVolume();  // lets go where it is, like the Color page's sliders
    dragging_ = false;
    pressing_ = false;
    repeat_.stop();
    px_ = py_ = -1;
    return changed;
}

bool DevicePage::takeRepeat(double now) {
    if (!repeat_.due(now)) return false;
    model_->dragVolume(steppedVolume(model_->audio.volume, pressedKind_ == DeviceHit::VolumeDown ? -1 : 1));
    return true;
}
