#include "media_strip.h"

#include "app_text.h"
#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr double kTop = 6;        // the gap under the HUD's own picture
constexpr double kCardX = 8, kCardY = kTop + 8;
constexpr double kTitleBaseline = kCardY + 30;
constexpr double kButtonY = kCardY + 44, kButtonH = 58;
constexpr double kButtonGap = 6;
constexpr double kRadius = 12;

constexpr double kReleaseSlop = 16;  // a release this close to the pressed button still counts (a shaky hand)
constexpr Color kStripPressed = blendColor(kAccent, kControl, 0.3);  // a button held down

MediaAction actionAt(int i) { return static_cast<MediaAction>(i + 1); }

/** @return the length of the UTF-8 character at title[i] (0: not a valid one) */
size_t utf8Length(const std::string& title, size_t i) {
    const unsigned char c = static_cast<unsigned char>(title[i]);
    const size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (len == 0 || i + len > title.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(title[i + k]) & 0xc0) != 0x80) return 0;
    }
    return len;
}

/**
 * The title without what would show as boxes: invalid UTF-8, characters the font has no glyph for (emoji, most
 * symbols, U+FFFD), and the joiners and selectors that only make sense with them (U+200D, U+FE0F, U+20E3). Control
 * characters (tabs, newlines) become spaces; spaces left doubled by all that are collapsed.
 * @param font the title's scaled font (null: only the fixed list is removed)
 */
std::string drawableTitle(const std::string& title, cairo_scaled_font_t* font) {
    std::string out;
    for (size_t i = 0; i < title.size();) {
        const size_t len = utf8Length(title, i);
        if (len == 0) {
            ++i;  // a stray byte
            continue;
        }
        std::string ch = title.substr(i, len);
        i += len;
        if (len == 1 && (static_cast<unsigned char>(ch[0]) < 0x20 || ch[0] == 0x7f)) ch = " ";  // tabs, newlines
        if (ch == "\xe2\x80\x8d" || ch == "\xef\xb8\x8f" || ch == "\xe2\x83\xa3") continue;
        if (len > 1 && font != nullptr) {
            cairo_glyph_t* glyphs = nullptr;
            int count = 0;
            const bool known = cairo_scaled_font_text_to_glyphs(font, 0, 0, ch.data(), static_cast<int>(len), &glyphs,
                                                                &count, nullptr, nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                               count == 1 && glyphs[0].index != 0;
            cairo_glyph_free(glyphs);
            if (!known) continue;
        }
        if (ch == " " && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

/** The title without the parts that only take room: YouTube's "(3) " (unread notifications) and " - YouTube" */
std::string shortTitle(std::string title) {
    // Only YouTube's own titles: "(2019) 기생충 예고편" elsewhere keeps its brackets
    bool youtube = false;
    for (const char* suffix : {" - YouTube Music", " - YouTube"}) {
        const size_t n = std::strlen(suffix);
        if (title.size() > n && title.compare(title.size() - n, n, suffix) == 0) {
            title.erase(title.size() - n);
            youtube = true;
            break;
        }
    }
    if (youtube && title.size() > 3 && title[0] == '(') {
        size_t i = 1;
        while (i < title.size() && title[i] >= '0' && title[i] <= '9') ++i;
        if (i > 1 && i + 1 < title.size() && title[i] == ')' && title[i + 1] == ' ') title.erase(0, i + 2);
    }
    return title;
}

}  // namespace

std::string mediaPositionText(double position, double duration) {
    if (!std::isfinite(position) || position < 0) return "";
    const auto clock = [](double sec) {
        const long total = static_cast<long>(sec);
        const long h = total / 3600, m = total / 60 % 60, s = total % 60;
        char buf[32];
        if (h > 0) {
            std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", h, m, s);
        } else {
            std::snprintf(buf, sizeof buf, "%ld:%02ld", m, s);
        }
        return std::string(buf);
    };
    if (!(duration > 0) || !std::isfinite(duration)) return clock(position);
    return clock(position) + " / " + clock(duration);
}

MediaStrip::MediaStrip(const FontSet& fonts) : fonts_(fonts) {}

MediaStrip::~MediaStrip() {
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

void MediaStrip::buttonRect(int i, double& x, double& y, double& w, double& h) const {
    const double left = kCardX + 8, inner = width_ - 2 * left;
    w = (inner - (kButtons - 1) * kButtonGap) / kButtons;
    x = left + i * (w + kButtonGap);
    y = kButtonY;
    h = kButtonH;
}

void MediaStrip::drawIcon(const Pen& pen, MediaAction action, double cx, double cy, double s,
                          const MediaStripState& state, bool onAccent) const {
    cairo_t* cr = pen.cr;
    const Color fg = onAccent ? kOnAccent : kText;
    pen.color(fg);
    const auto triangle = [&](double tipX, double baseX, double half) {
        cairo_move_to(cr, tipX, cy);
        cairo_line_to(cr, baseX, cy - half);
        cairo_line_to(cr, baseX, cy + half);
        cairo_close_path(cr);
        cairo_fill(cr);
    };
    switch (action) {
        case MediaAction::Back:
            triangle(cx - s * 0.5, cx, s * 0.32);
            triangle(cx, cx + s * 0.5, s * 0.32);
            break;
        case MediaAction::Forward:
            triangle(cx + s * 0.5, cx, s * 0.32);
            triangle(cx, cx - s * 0.5, s * 0.32);
            break;
        case MediaAction::PlayPause:
            if (state.playing) {
                cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.34, s * 0.2, s * 0.68);
                cairo_rectangle(cr, cx + s * 0.1, cy - s * 0.34, s * 0.2, s * 0.68);
                cairo_fill(cr);
            } else {
                triangle(cx + s * 0.38, cx - s * 0.26, s * 0.38);
            }
            break;
        case MediaAction::Mute: {
            // A speaker; muted: a cross beside it, else two waves
            const double x0 = cx - s * 0.42;
            cairo_rectangle(cr, x0, cy - s * 0.13, s * 0.16, s * 0.26);
            cairo_fill(cr);
            cairo_move_to(cr, x0 + s * 0.14, cy - s * 0.13);
            cairo_line_to(cr, x0 + s * 0.38, cy - s * 0.34);
            cairo_line_to(cr, x0 + s * 0.38, cy + s * 0.34);
            cairo_line_to(cr, x0 + s * 0.14, cy + s * 0.13);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_set_line_width(cr, std::max(2.0, s * 0.08));
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            const double wx = x0 + s * 0.52;
            if (state.muted) {
                cairo_move_to(cr, wx, cy - s * 0.16);
                cairo_line_to(cr, wx + s * 0.3, cy + s * 0.16);
                cairo_move_to(cr, wx + s * 0.3, cy - s * 0.16);
                cairo_line_to(cr, wx, cy + s * 0.16);
                cairo_stroke(cr);
            } else {
                cairo_arc(cr, x0 + s * 0.38, cy, s * 0.2, -0.9, 0.9);
                cairo_stroke(cr);
                cairo_arc(cr, x0 + s * 0.38, cy, s * 0.36, -0.9, 0.9);
                cairo_stroke(cr);
            }
            break;
        }
        case MediaAction::OpenPanel:
            // A panel with its handle bar
            cairo_set_line_width(cr, std::max(2.0, s * 0.08));
            pen.roundedRect(cx - s * 0.42, cy - s * 0.3, s * 0.84, s * 0.6, s * 0.08);
            cairo_stroke(cr);
            cairo_rectangle(cr, cx - s * 0.42, cy - s * 0.3, s * 0.84, s * 0.16);
            cairo_fill(cr);
            break;
        case MediaAction::None: break;
    }
}

void MediaStrip::render(int width, const MediaStripState& state, bool highlighted) {
    // A new width, or a surface cairo couldn't make last time (tried again)
    if (surface_ == nullptr || width != width_ || cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        if (cr_ != nullptr) cairo_destroy(cr_);
        if (surface_ != nullptr) cairo_surface_destroy(surface_);
        width_ = width;
        surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_, kHeight);
        cr_ = cairo_create(surface_);
    }
    const Pen pen {cr_, &fonts_};
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    // The panel's background and border, like the HUD's, then one card
    pen.color(kBg);
    pen.roundedRect(0, kTop, width_, kHeight - kTop, 16);
    cairo_fill(cr_);
    if (highlighted) strokeRounded(pen, 1.5, kTop + 1.5, width_ - 3, kHeight - kTop - 3, 15, kAccent, 3);
    drawCard(pen, kCardX, kCardY, width_ - 2 * kCardX, kHeight - kCardY - 8, kRadius, kCard, kCard, 0);

    // Title line: the state on the right, the page's title in what's left
    const char* stateText = !state.position.empty() ? state.position.c_str()
                            : state.playing         ? tr(Tx::MediaPlaying)
                            : state.pausedByRemoval ? tr(Tx::MediaPausedByRemoval)
                                                    : tr(Tx::MediaPaused);
    const Color stateColor = !state.position.empty() ? kText
                             : state.playing         ? kSuccess
                             : state.pausedByRemoval ? kAccentText  // not a warning: it was paused for them
                                                     : kTextMuted;
    const double right = width_ - kCardX - 12;
    const double stateSize = fitSize(pen, stateText, 18, 13, (width_ - 2 * kCardX) * 0.5, true);
    const double stateW = pen.text(right, kTitleBaseline, stateText, stateSize, stateColor, true, true);
    const double titleX = kCardX + 12, titleRoom = right - stateW - 12 - titleX;
    cairo_set_font_face(cr_, fonts_.regular());
    cairo_set_font_size(cr_, 18);
    const std::string title =
        drawableTitle(shortTitle(state.title), fonts_.regular() != nullptr ? cairo_get_scaled_font(cr_) : nullptr);
    if (titleRoom > 30 && !title.empty()) pen.text(titleX, kTitleBaseline, ellipsize(pen, title, 18, titleRoom, false), 18, kText);

    // The buttons: icon on top, a word under it (what a press does; 음소거 is filled pink while muted, like the app's
    // other on-buttons, and a press then turns the sound back on); a held button is tinted pink. The words share one
    // size: the largest at which each fits
    const auto wordOf = [&](MediaAction action) {
        return tr(action == MediaAction::Back        ? Tx::MediaBack
                  : action == MediaAction::Forward   ? Tx::MediaForward
                  : action == MediaAction::PlayPause ? (state.playing ? Tx::MediaPause : Tx::MediaPlay)
                  : action == MediaAction::Mute      ? Tx::MediaMute
                                                     : Tx::MediaBrowser);
    };
    double wordSize = 16;
    for (int i = 0; i < kButtons; ++i) {
        double x, y, w, h;
        buttonRect(i, x, y, w, h);
        wordSize = std::min(wordSize, fitSize(pen, wordOf(actionAt(i)), 16, 12, w - 6, false));
    }
    for (int i = 0; i < kButtons; ++i) {
        double x, y, w, h;
        buttonRect(i, x, y, w, h);
        const MediaAction action = actionAt(i);
        const bool held = pressed_ == action;
        const bool active = hover_ == action || held;
        const bool on = action == MediaAction::Mute && state.muted;
        pen.color(on ? (active ? kAccentPressed : kAccent) : held ? kStripPressed : active ? kControlHover : kControl);
        pen.roundedRect(x, y, w, h, 10);
        cairo_fill(cr_);
        if (!on) strokeRounded(pen, x, y, w, h, 10, active ? kAccent : kBorder, 2);
        drawIcon(pen, action, x + w / 2, y + 21, std::min(26.0, w * 0.6), state, on);
        textCentered(pen, x + w / 2, y + h - 8, wordOf(action), wordSize, on ? kOnAccent : kText, false);
    }
    cairo_surface_flush(surface_);
}

const uint8_t* MediaStrip::pixels() const {
    return surface_ != nullptr ? cairo_image_surface_get_data(surface_) : nullptr;
}

bool MediaStrip::writePng(const std::string& path) const {
    return surface_ != nullptr && cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

MediaAction MediaStrip::hit(double px, double py) const {
    if (width_ <= 0) return MediaAction::None;
    for (int i = 0; i < kButtons; ++i) {
        double x, y, w, h;
        buttonRect(i, x, y, w, h);
        // Half a gap round each button counts: no dead line between them
        if (px >= x - kButtonGap / 2 && px < x + w + kButtonGap / 2 && py >= y - 4 && py < y + h + 4) return actionAt(i);
    }
    return MediaAction::None;
}

bool MediaStrip::pointerMove(double x, double y) {
    const MediaAction over = hit(x, y);
    if (over == hover_) return false;
    hover_ = over;
    return true;
}

bool MediaStrip::pointerDown(double x, double y) {
    pressed_ = hit(x, y);
    hover_ = pressed_;
    return pressed_ != MediaAction::None;
}

MediaAction MediaStrip::pointerUp(double x, double y) {
    const MediaAction held = pressed_;
    pressed_ = MediaAction::None;
    hover_ = hit(x, y);
    if (held == MediaAction::None) return MediaAction::None;
    if (hover_ == held) return held;
    // Let go just off it (not on another button): still that button
    double bx, by, bw, bh;
    buttonRect(static_cast<int>(held) - 1, bx, by, bw, bh);
    const bool near = x >= bx - kReleaseSlop && x < bx + bw + kReleaseSlop && y >= by - kReleaseSlop &&
                      y < by + bh + kReleaseSlop;
    return near && hover_ == MediaAction::None ? held : MediaAction::None;
}

bool MediaStrip::pointerLeave() {
    const bool any = hover_ != MediaAction::None || pressed_ != MediaAction::None;
    hover_ = pressed_ = MediaAction::None;
    return any;
}
