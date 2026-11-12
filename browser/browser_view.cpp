// The browser panel's picture. See browser_view.h. Its words are app_text's, in uiLanguage().
#include "browser_view.h"

#include "app_text.h"
#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kPad = 16;
constexpr double kGap = 8;
constexpr double kRadius = 14;
constexpr double kRow1Y = 8, kRow1H = 62;
constexpr double kRow2Y = 82, kRow2H = 66;

bool inside(double px, double py, double x, double y, double w, double h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

void drawGripDots(cairo_t* cr, double cx, double cy, Color c) {
    for (int row = -1; row <= 1; ++row) {
        for (int col = 0; col < 2; ++col) drawDisc(cr, cx - 6 + col * 12, cy + row * 11, 3.4, c);
    }
}

void triangle(cairo_t* cr, double cx, double cy, bool left, Color c) {
    const double d = left ? -1 : 1;
    cairo_new_path(cr);
    cairo_move_to(cr, cx + d * 12, cy);
    cairo_line_to(cr, cx - d * 9, cy - 13);
    cairo_line_to(cr, cx - d * 9, cy + 13);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_fill(cr);
}

/** ⟳: an open circle with an arrow head */
void reloadIcon(cairo_t* cr, double cx, double cy, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 3.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, 13, -M_PI * 0.35, M_PI * 1.45);
    cairo_stroke(cr);
    const double ax = cx + 13 * std::cos(-M_PI * 0.35), ay = cy + 13 * std::sin(-M_PI * 0.35);
    cairo_new_path(cr);
    cairo_move_to(cr, ax + 7, ay - 6);
    cairo_line_to(cr, ax + 2, ay + 6);
    cairo_line_to(cr, ax - 7, ay - 3);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/** ☆ / ★: a five-pointed star, outlined or filled */
void starIcon(cairo_t* cr, double cx, double cy, bool filled, Color c) {
    cairo_new_path(cr);
    for (int i = 0; i < 10; ++i) {
        const double r = i % 2 == 0 ? 16 : 6.8;
        const double a = -M_PI / 2 + i * M_PI / 5;
        cairo_line_to(cr, cx + r * std::cos(a), cy + 1.5 + r * std::sin(a));
    }
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    if (filled) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 2.6);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }
}

/** ✕ */
void crossIcon(cairo_t* cr, double cx, double cy, double r, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - r, cy - r);
    cairo_line_to(cr, cx + r, cy + r);
    cairo_move_to(cr, cx + r, cy - r);
    cairo_line_to(cr, cx - r, cy + r);
    cairo_stroke(cr);
}

// The overlays over the page (layout units)
constexpr double kListW = 600, kListRowH = 58, kListPad = 10, kListNoteH = 50;
constexpr double kMenuW = 260, kMenuRowH = 58, kMenuPad = 8;
constexpr double kViewW = 600, kViewRowH = 66, kViewPad = 22, kViewNoteH = 44;  // 보기's list: three rows and a note
const char* kAspectNames[4] = {"16:9", "4:3", "1:1", "9:16"};
constexpr double kRingR = 26;

}  // namespace

const char* menuItemLabel(MenuItem item) {
    switch (item) {
        case MenuItem::Copy: return tr(Tx::BrowserMenuCopy);
        case MenuItem::Paste: return tr(Tx::BrowserMenuPaste);
        case MenuItem::SelectAll: return tr(Tx::BrowserMenuSelectAll);
        case MenuItem::OpenLink: return tr(Tx::BrowserMenuOpenLink);
    }
    return "";
}

std::string browserDialogText(const BrowserSession::Status& status) {
    if (status.dialogType.empty()) return {};
    if (status.dialogType == "beforeunload") return tr(Tx::BrowserDialogLeave);
    std::string message = status.dialogMessage;
    for (char& c : message) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (status.dialogType == "prompt") return trf(Tx::BrowserDialogPrompt, {message});
    return trf(Tx::BrowserDialogAsk, {message});
}

std::string browserStatusText(const BrowserSession::Status& status) {
    using Phase = BrowserSession::Phase;
    switch (status.phase) {
        case Phase::Off: return tr(Tx::BrowserStatusOff);
        case Phase::Starting: return tr(Tx::BrowserStatusStarting);
        case Phase::Connecting:
        case Phase::Attaching: return tr(Tx::BrowserStatusConnecting);
        case Phase::Ready: return {};
        case Phase::Stopping: return tr(Tx::BrowserStatusClosing);
        case Phase::Restarting:
            return trf(Tx::BrowserStatusRestarting, {std::to_string(status.restarts)}) +
                   (status.error.empty() ? std::string() : "\n" + status.error);
        case Phase::Failed:
            return tr(Tx::BrowserStatusFailed) + (status.error.empty() ? std::string() : ": " + status.error) + "\n" +
                   tr(Tx::BrowserStatusRetry);
    }
    return {};
}

BrowserPanel::BrowserPanel(const FontSet& fonts, int viewportWidth, int viewportHeight)
    : fonts_(fonts), viewportW_(viewportWidth), viewportH_(viewportHeight) {
    makeSurface();
}

BrowserPanel::~BrowserPanel() {
    if (overlay_ != nullptr) cairo_surface_destroy(overlay_);
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

void BrowserPanel::makeSurface() {
    scale_ = static_cast<double>(viewportW_) / kDesignWidth;
    barH_ = static_cast<int>(std::lround(kBarHeight * scale_));
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width(), height());
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    hasPage_ = false;
    resetPageRect();
    spots_.clear();
    under_.clear();
    underW_ = underH_ = 0;
    if (overlay_ != nullptr) cairo_surface_destroy(overlay_);
    overlay_ = nullptr;
}

void BrowserPanel::setViewport(int width, int height) {
    if (width == viewportW_ && height == viewportH_) return;
    viewportW_ = width;
    viewportH_ = height;
    makeSurface();
}

const uint8_t* BrowserPanel::pixels() const {
    cairo_surface_flush(surface_);
    return cairo_image_surface_get_data(surface_);
}

bool BrowserPanel::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

bool BrowserPanel::over(const Spot& s) const { return s.usable && inside(px_, py_, s.x, s.y, s.w, s.h); }

BrowserPanel::Hit BrowserPanel::hitAt(double x, double y, int* arg) const {
    for (const Spot& s : spots_) {
        if (s.usable && inside(x / scale_, y / scale_, s.x, s.y, s.w, s.h)) {
            if (arg != nullptr) *arg = s.arg;
            return s.hit;
        }
    }
    // The page where it shows (beside a frame of another shape, the black is nothing)
    if (x >= pageX_ && x <= pageX_ + pageW_ && y >= pageY_ && y <= pageY_ + pageH_) return Hit::Page;
    return Hit::None;
}

size_t BrowserPanel::addressOffsetAt(double x) const {
    if (!editing_) return std::string::npos;
    const double lx = x / scale_;
    const Pen pen {cr_, &fonts_};
    const auto isLead = [&](size_t i) {
        return i >= editText_.size() || (static_cast<unsigned char>(editText_[i]) & 0xC0) != 0x80;
    };
    // Each boundary's x as drawn: those before the caret from where its drawn part starts, those after it from after
    // the composing syllable, up to the room's end
    size_t best = editCaret_;
    double bestDist = std::fabs(lx - (editAfterX_ - 4));
    const auto consider = [&](size_t offset, double at) {
        const double d = std::fabs(lx - at);
        if (d < bestDist) {
            bestDist = d;
            best = offset;
        }
    };
    for (size_t i = editShownFrom_; i < editCaret_; ++i) {
        if (isLead(i)) consider(i, editX_ + pen.measure(editText_.substr(editShownFrom_, i - editShownFrom_), 22, false));
    }
    for (size_t i = editCaret_ + 1; i <= editText_.size(); ++i) {
        if (!isLead(i)) continue;
        const double at = editAfterX_ + pen.measure(editText_.substr(editCaret_, i - editCaret_), 22, false);
        if (at > editRight_) break;
        consider(i, at);
    }
    return best;
}

bool BrowserPanel::hitCenter(Hit hit, int arg, double& x, double& y) const {
    if (hit == Hit::Page) {
        x = pageX_ + pageW_ / 2;
        y = pageY_ + pageH_ / 2;
        return true;
    }
    for (const Spot& s : spots_) {
        if (s.hit != hit || s.arg != arg || !s.usable) continue;
        x = (s.x + s.w / 2) * scale_;
        y = (s.y + s.h / 2) * scale_;
        return true;
    }
    return false;
}

bool BrowserPanel::setPointer(double x, double y, bool pressed) {
    const auto spotAt = [&](double px, double py) -> const Spot* {
        for (const Spot& s : spots_) {
            if (s.usable && inside(px, py, s.x, s.y, s.w, s.h)) return &s;
        }
        return nullptr;
    };
    const Spot* before = spotAt(px_, py_);
    const bool wasPressed = pressed_;
    px_ = x < 0 ? -1 : x / scale_;
    py_ = y < 0 ? -1 : y / scale_;
    pressed_ = pressed;
    const Spot* after = spotAt(px_, py_);
    return before != after || (after != nullptr && wasPressed != pressed);
}

cdp::ViewMap BrowserPanel::viewMap() const {
    cdp::ViewMap m;
    m.areaX = pageX_;
    m.areaY = pageY_;
    m.areaW = pageW_;
    m.areaH = pageH_;
    m.viewportW = pageCssW_;
    m.viewportH = pageCssH_;
    return m;
}

void BrowserPanel::resetPageRect() {
    pageX_ = 0;
    pageY_ = barH_;
    pageW_ = viewportW_;
    pageH_ = viewportH_;
    pageCssW_ = viewportW_;
    pageCssH_ = viewportH_;
}

void BrowserPanel::button(const Pen& pen, Hit hit, int arg, double x, double y, double w, double h,
                          const std::string& label, bool selected, bool usable, double size) {
    const Spot spot {hit, arg, x, y, w, h, usable};
    const bool active = over(spot);
    const bool down = active && pressed_;
    if (!usable) {
        pen.color(kControl);
    } else if (selected) {
        pen.color(down || active ? kAccentPressed : kAccent);
    } else {
        pen.color(down ? kControlHover : (active ? kControlHover : kControl));
    }
    pen.roundedRect(x, y, w, h, kRadius);
    cairo_fill(pen.cr);
    if (usable && !selected) strokeRounded(pen, x, y, w, h, kRadius, active ? kAccent : kBorder, active ? 2.5 : 1.5);
    const Color fg = !usable ? kTextDisabled : selected ? kOnAccent : kText;
    const double cx = x + w / 2, cy = y + h / 2;
    if (hit == Hit::Back || hit == Hit::Forward) {
        triangle(pen.cr, cx, cy, hit == Hit::Back, fg);
    } else if (hit == Hit::Reload) {
        reloadIcon(pen.cr, cx, cy, fg);
    } else {
        const double fitted = fitSize(pen, label, size, size * 0.65, w - 16, true);
        textCentered(pen, cx, centerBaseline(y, h, fitted), label, fitted, fg, true);
    }
    spots_.push_back(spot);
}

void BrowserPanel::renderBar(const BrowserBarState& state) {
    const Pen pen {cr_, &fonts_};
    spots_.clear();
    restoreUnder();  // the page under the last overlays
    cairo_save(cr_);
    cairo_scale(cr_, scale_, scale_);
    const double W = viewportW_ / scale_;
    // The bar's area (and the page area too when there is no page to keep)
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr_, 0, 0, W, kBarHeight);
    if (!hasPage_) cairo_rectangle(cr_, 0, kBarHeight, W, viewportH_ / scale_);
    cairo_fill(cr_);
    cairo_restore(cr_);
    // Rounded on top only: the page meets the bar with a straight edge
    cairo_save(cr_);
    cairo_rectangle(cr_, 0, 0, W, kBarHeight);
    cairo_clip(cr_);
    pen.color(kBg);
    pen.roundedRect(0, 0, W, kBarHeight + 30, 24);
    cairo_fill(cr_);
    cairo_restore(cr_);

    // ---- Row 1: the handle (title) and the panel's own buttons on the right ----
    // 닫기 at the far end and 끄기 (Chrome off; a second tap, 한 번 더, with a red ring) next to it, set apart from the
    // rest (not hit by mistake for 끌기)
    const double closeW = 120, quitW = 110, closeGap = 40, scrollW = 170, pinW = 130, stepW = 64, sizeW = 100;
    double x = W - kPad - closeW;
    button(pen, Hit::Close, 0, x, kRow1Y, closeW, kRow1H, tr(Tx::BrowserClose), false, true, 26);
    x -= kGap + quitW;
    button(pen, Hit::Quit, 0, x, kRow1Y, quitW, kRow1H, tr(state.quitArmed ? Tx::BrowserAgain : Tx::BrowserQuit), false,
           true, 24);
    if (state.quitArmed) strokeRounded(pen, x, kRow1Y, quitW, kRow1H, kRadius, kDanger, 3);
    x -= closeGap + scrollW;
    // What a drag on the page does now (a tap clicks either way): scrolls like a phone, or selects like a mouse
    button(pen, Hit::DragScroll, 0, x, kRow1Y, scrollW, kRow1H,
           tr(state.dragScroll ? Tx::BrowserDragScroll : Tx::BrowserDragSelect), false, true, 24);
    x -= kGap + pinW;
    // Pinned: fixed in the room; pressing again puts it back on its anchor (a wrist, or the head)
    button(pen, Hit::Pin, 0, x, kRow1Y, pinW, kRow1H, tr(state.pinned ? Tx::BrowserUnpin : Tx::BrowserPin), state.pinned,
           true, 24);
    x -= kGap + stepW;
    button(pen, Hit::SizeUp, 0, x, kRow1Y, stepW, kRow1H, "+", false, state.canGrow, 32);
    x -= sizeW;
    char value[32];
    std::snprintf(value, sizeof(value), "%.0f cm", state.widthM * 100);
    textCentered(pen, x + sizeW / 2, centerBaseline(kRow1Y, kRow1H, 24), value, 24, kText, true);
    x -= stepW;
    button(pen, Hit::SizeDown, 0, x, kRow1Y, stepW, kRow1H, "−", false, state.canShrink, 32);
    const double handleW = x - kGap;
    const Spot handle {Hit::Handle, 0, 0, 0, handleW, static_cast<double>(kHandleHeight), true};
    const bool handleActive = over(handle);
    if (handleActive) {
        pen.color(kControlHover);
        pen.roundedRect(4, 4, handleW - 4, kHandleHeight - 4, 20);
        cairo_fill(cr_);
    }
    drawGripDots(cr_, kPad + 14, kHandleHeight / 2.0, handleActive ? kAccent : kTextMuted);
    // The title, or (until the next navigation) a notice about the page: a page that kept crashing Chrome
    const bool notice = !state.status.notice.empty() && state.status.phase == BrowserSession::Phase::Ready;
    const std::string title = notice                        ? state.status.notice
                              : !state.status.title.empty() ? state.status.title
                                                            : std::string(tr(Tx::BrowserTitle));
    const double titleSize = notice ? 21 : 26;
    pen.text(kPad + 40, centerBaseline(0, kHandleHeight, titleSize),
             ellipsize(pen, title, titleSize, handleW - kPad - 52, !notice), titleSize, notice ? kWarn : kText, !notice);
    spots_.push_back(handle);

    // ---- Row 2: navigation, the address field, quick links, 즐겨찾기, the keyboard ----
    const bool ready = state.status.phase == BrowserSession::Phase::Ready;
    if (ready && !state.status.dialogType.empty()) {
        // The page waits for an answer (its script is stopped until then): the question, 확인 and 취소 in its place
        const double answerW = 130;
        x = W - kPad - answerW;
        button(pen, Hit::DialogDismiss, 0, x, kRow2Y, answerW, kRow2H, tr(Tx::BrowserCancel), false, true, 24);
        x -= kGap + answerW;
        button(pen, Hit::DialogAccept, 0, x, kRow2Y, answerW, kRow2H, tr(Tx::BrowserOk), true, true, 24);
        const double boxW = x - kGap - kPad;
        pen.color(kCard);
        pen.roundedRect(kPad, kRow2Y, boxW, kRow2H, kRadius);
        cairo_fill(cr_);
        strokeRounded(pen, kPad, kRow2Y, boxW, kRow2H, kRadius, kWarn, 2.5);
        pen.text(kPad + 20, centerBaseline(kRow2Y, kRow2H, 22),
                 ellipsize(pen, browserDialogText(state.status), 22, boxW - 40, false), 22, kText);
    } else {
        const double navW = 66, keyboardW = 118, translateW = 96, viewW = 96, favoritesW = 124;
        x = kPad;
        button(pen, Hit::Back, 0, x, kRow2Y, navW, kRow2H, "", false, ready && state.status.canBack);
        x += navW + kGap;
        button(pen, Hit::Forward, 0, x, kRow2Y, navW, kRow2H, "", false, ready && state.status.canForward);
        x += navW + kGap;
        // Reload also restarts a Chrome that is off or failed
        button(pen, Hit::Reload, 0, x, kRow2Y, navW, kRow2H, "", false, true);
        x += navW + kGap;
        // ☆: save this page to 즐겨찾기; ★: it is saved (pressing removes it)
        const bool canStar = ready && state.canStar;
        button(pen, Hit::Star, 0, x, kRow2Y, navW, kRow2H, "", false, canStar);
        starIcon(cr_, x + navW / 2, kRow2Y + kRow2H / 2, state.starred && canStar,
                 !canStar ? kTextDisabled : state.starred ? kAccent : kText);
        x += navW + kGap;
        const double right = W - kPad - keyboardW - kGap - favoritesW - kGap - viewW - kGap - translateW - kGap;
        const double addressW = right - x;
        {
            // The address field: the page's URL, or what is being typed into it (with the composing syllable)
            const Spot field {Hit::Address, 0, x, kRow2Y, addressW, kRow2H, true};
            // Editing: a ✕ at its end clears it (its spot first: it is inside the field's)
            const double clearS = kRow2H - 20, clearX = x + addressW - clearS - 12, clearY = kRow2Y + 10;
            const Spot clear {Hit::AddressClear, 0, clearX, clearY, clearS, clearS, true};
            if (state.addressEditing) spots_.push_back(clear);
            const bool active = over(field);
            pen.color(state.addressEditing ? kCard : (active ? kControlHover : kControl));
            pen.roundedRect(x, kRow2Y, addressW, kRow2H, kRow2H / 2);
            cairo_fill(cr_);
            strokeRounded(pen, x, kRow2Y, addressW, kRow2H, kRow2H / 2,
                          state.addressEditing || active ? kAccent : kBorder, state.addressEditing ? 3 : 1.5);
            const double tx = x + 24, room = addressW - 48;
            const double base = centerBaseline(kRow2Y, kRow2H, 22);
            if (state.addressEditing) {
                // Before the caret, the composing syllable, the caret, then the rest. What is before the caret is cut
                // from the front while the caret would be past the room (left of the ✕)
                const double editRoom = clearX - 10 - tx;
                const size_t caret = std::min(state.addressCaret, state.addressText.size());
                // At most the last 240 bytes before the caret are measured (a long URL fits nowhere near that many)
                std::string before = state.addressText.substr(caret > 240 ? caret - 240 : 0, std::min<size_t>(caret, 240));
                while (!before.empty() && (static_cast<unsigned char>(before[0]) & 0xC0) == 0x80) before.erase(0, 1);
                const std::string after = state.addressText.substr(caret, 240);
                while (!before.empty() && pen.measure(before + state.addressPreedit, 22, false) > editRoom - 10) {
                    before.erase(0, 1);
                    while (!before.empty() && (static_cast<unsigned char>(before[0]) & 0xC0) == 0x80) before.erase(0, 1);
                }
                double cx = tx;
                editing_ = true;
                editText_ = state.addressText;
                editCaret_ = caret;
                editShownFrom_ = caret - before.size();
                editX_ = tx;
                editRight_ = tx + editRoom;
                editAfterX_ = tx + pen.measure(before, 22, false) +
                              (state.addressPreedit.empty() ? 0 : pen.measure(state.addressPreedit, 22, true)) + 4;
                if (state.addressText.empty() && state.addressPreedit.empty()) {
                    pen.text(cx, base, tr(Tx::BrowserAddressHint), 22, kTextMuted);
                } else {
                    cx += pen.text(cx, base, before, 22, kText);
                    if (!state.addressPreedit.empty()) {
                        const double w = pen.text(cx, base, state.addressPreedit, 22, kAccentText, true);
                        pen.color(kAccent);
                        cairo_rectangle(cr_, cx, base + 5, w, 2.5);
                        cairo_fill(cr_);
                        cx += w;
                    }
                    const double restRoom = tx + editRoom - cx - 4;
                    if (!after.empty() && restRoom > 20) {
                        pen.text(cx + 4, base, ellipsize(pen, after, 22, restRoom, false), 22, kText);
                    }
                }
                pen.color(kAccent);
                cairo_rectangle(cr_, cx + 2, kRow2Y + 16, 2.5, kRow2H - 32);
                cairo_fill(cr_);
                const bool on = over(clear);
                pen.color(on ? kControlHover : kControl);
                pen.roundedRect(clearX, clearY, clearS, clearS, clearS / 2);
                cairo_fill(cr_);
                crossIcon(cr_, clearX + clearS / 2, clearY + clearS / 2, 9, on ? kDanger : kText);
            } else {
                editing_ = false;
                std::string url = state.status.url;
                for (const char* scheme : {"https://", "http://"}) {
                    if (url.rfind(scheme, 0) == 0) url = url.substr(std::strlen(scheme));
                }
                if (url.empty() || url == "about:blank") {
                    pen.text(tx, base, tr(Tx::BrowserAddressHint), 22, kTextMuted);
                } else {
                    pen.text(tx, base, ellipsize(pen, url, 22, room, false), 22, kText);
                }
            }
            spots_.push_back(field);
        }
        x = right + kGap;
        // 번역 (pink while on) and 보기 (its list: 버전, 확대, 비율)
        button(pen, Hit::Translate, 0, x, kRow2Y, translateW, kRow2H, tr(Tx::BrowserTranslate), state.translated, ready,
               22);
        x += translateW + kGap;
        button(pen, Hit::View, 0, x, kRow2Y, viewW, kRow2H, tr(Tx::BrowserView), state.viewOpen, true, 22);
        viewRight_ = x + viewW;
        x += viewW + kGap;
        button(pen, Hit::Favorites, 0, x, kRow2Y, favoritesW, kRow2H, tr(Tx::BrowserFavorites), state.favoritesOpen, true,
               22);
        favoritesRight_ = x + favoritesW;
        x += favoritesW + kGap;
        button(pen, Hit::Keyboard, 0, x, kRow2Y, keyboardW, kRow2H, tr(Tx::BrowserKeyboard), state.keyboardOpen, true,
               24);

        // Loading: an accent line along the bar's lower edge
        if (state.status.loading && ready) {
            pen.color(kAccent);
            cairo_rectangle(cr_, 0, kBarHeight - 5, W * 0.35, 4);
            cairo_fill(cr_);
        }
    }
    if (!hasPage_ || !ready) drawPageMessage(pen, state.status);
    cairo_restore(cr_);

    shown_ = state;
    saveUnder();
    renderOverlay();  // drawn (and measured) only here, when the bar's state is drawn
    compositeOverlay();
}

void BrowserPanel::drawPageMessage(const Pen& pen, const BrowserSession::Status& status) {
    const double W = viewportW_ / scale_, H = viewportH_ / scale_;
    pen.color(kCard);
    cairo_rectangle(cr_, 0, kBarHeight, W, H);
    cairo_fill(cr_);
    std::string text = browserStatusText(status);
    if (text.empty()) text = tr(Tx::BrowserLoading);
    const double cy = kBarHeight + H / 2.0;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    double y = cy - (lines.size() - 1) * 22;
    for (size_t i = 0; i < lines.size(); ++i) {
        const double size = i == 0 ? 30 : 22;
        const Color c = i == 0 ? kText : (status.phase == BrowserSession::Phase::Failed ? kDanger : kTextMuted);
        const std::string line = ellipsize(pen, lines[i], size, W - 80, i == 0);
        textCentered(pen, W / 2.0, y, line, size, c, i == 0);
        y += 44;
    }
}

void BrowserPanel::listBox(double& x, double& y, double& w, double& h) const {
    const double W = viewportW_ / scale_;
    const size_t rows = std::max<size_t>(1, shown_.favorites.size());
    x = std::clamp(favoritesRight_ - kListW, 0.0, std::max(0.0, W - kListW));
    y = kBarHeight + 6;
    w = kListW;
    h = 2 * kListPad + rows * kListRowH + (shown_.favoritesNote.empty() ? 0 : kListNoteH);
}

void BrowserPanel::viewBox(double& x, double& y, double& w, double& h) const {
    const double W = viewportW_ / scale_;
    x = std::clamp(viewRight_ - kViewW, 0.0, std::max(0.0, W - kViewW));
    y = kBarHeight + 6;
    w = kViewW;
    h = 2 * kViewPad + 3 * kViewRowH + 2 * 16 + kViewNoteH;
}

void BrowserPanel::menuBox(double& x, double& y, double& w, double& h) const {
    const double W = viewportW_ / scale_, bottom = kBarHeight + viewportH_ / scale_;
    const double mx = shown_.menuX / scale_, my = shown_.menuY / scale_;
    w = kMenuW;
    h = 2 * kMenuPad + shown_.menu.size() * kMenuRowH;
    // Left of the point when there is room (a page's own menu usually opens to its right), else right of it
    x = mx - 14 - kMenuW >= 0 ? mx - 14 - kMenuW : std::min(mx + 14, W - kMenuW);
    y = std::clamp(my - kMenuRowH / 2, kBarHeight + 4.0, std::max(kBarHeight + 4.0, bottom - 4 - h));
}

bool BrowserPanel::overlayBox(double& x, double& y, double& w, double& h) const {
    const double W = viewportW_ / scale_, bottom = kBarHeight + viewportH_ / scale_;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    const auto add = [&](double ax, double ay, double aw, double ah) {
        x0 = std::min(x0, ax);
        y0 = std::min(y0, ay);
        x1 = std::max(x1, ax + aw);
        y1 = std::max(y1, ay + ah);
    };
    double bx, by, bw, bh;
    if (shown_.favoritesOpen) {
        listBox(bx, by, bw, bh);
        add(bx, by, bw, bh);
    }
    if (!shown_.menu.empty()) {
        menuBox(bx, by, bw, bh);
        add(bx, by, bw, bh);
    }
    if (shown_.viewOpen) {
        viewBox(bx, by, bw, bh);
        add(bx, by, bw, bh);
    }
    if (shown_.pressRing) {
        const double rx = shown_.ringX / scale_, ry = shown_.ringY / scale_;
        add(rx - kRingR - 4, ry - kRingR - 4, 2 * kRingR + 8, 2 * kRingR + 8);
    }
    if (x1 < x0) return false;
    // Only over the page
    x0 = std::max(x0, 0.0);
    y0 = std::max(y0, static_cast<double>(kBarHeight));
    x1 = std::min(x1, W);
    y1 = std::min(y1, bottom);
    if (x1 <= x0 || y1 <= y0) return false;
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

bool BrowserPanel::overlayRect(int& x, int& y, int& w, int& h) const {
    double bx, by, bw, bh;
    if (!overlayBox(bx, by, bw, bh)) return false;
    // Texture px, a pixel wider on each side (antialiased edges), inside the page area
    x = std::max(0, static_cast<int>(std::floor(bx * scale_)) - 1);
    y = std::max(barH_, static_cast<int>(std::floor(by * scale_)) - 1);
    w = std::min(viewportW_, static_cast<int>(std::ceil((bx + bw) * scale_)) + 1) - x;
    h = std::min(height(), static_cast<int>(std::ceil((by + bh) * scale_)) + 1) - y;
    return w > 0 && h > 0;
}

void BrowserPanel::saveUnder() {
    if (!hasPage_ || !overlayRect(underX_, underY_, underW_, underH_)) {
        under_.clear();
        underW_ = underH_ = 0;
        return;
    }
    cairo_surface_flush(surface_);
    const unsigned char* data = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    const size_t row = static_cast<size_t>(underW_) * 4;
    under_.resize(row * underH_);
    for (int r = 0; r < underH_; ++r) {
        std::memcpy(&under_[r * row], data + static_cast<size_t>(underY_ + r) * stride + underX_ * 4, row);
    }
}

void BrowserPanel::restoreUnder() {
    if (underW_ <= 0 || underH_ <= 0 || under_.empty()) return;
    cairo_surface_flush(surface_);
    unsigned char* data = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    const size_t row = static_cast<size_t>(underW_) * 4;
    for (int r = 0; r < underH_; ++r) {
        std::memcpy(data + static_cast<size_t>(underY_ + r) * stride + underX_ * 4, &under_[r * row], row);
    }
    cairo_surface_mark_dirty_rectangle(surface_, underX_, underY_, underW_, underH_);
    under_.clear();
    underW_ = underH_ = 0;
}

void BrowserPanel::renderOverlay() {
    int x, y, w, h;
    if (!overlayRect(x, y, w, h)) {
        if (overlay_ != nullptr) cairo_surface_destroy(overlay_);
        overlay_ = nullptr;
        return;
    }
    if (overlay_ == nullptr || cairo_image_surface_get_width(overlay_) != w ||
        cairo_image_surface_get_height(overlay_) != h) {
        if (overlay_ != nullptr) cairo_surface_destroy(overlay_);
        overlay_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    }
    overlayX_ = x;
    overlayY_ = y;
    cairo_t* cr = cairo_create(overlay_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_get_font_options(cr_, options);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -x, -y);
    drawOverlays(Pen {cr, &fonts_});
    cairo_destroy(cr);
}

void BrowserPanel::compositeOverlay() {
    if (overlay_ == nullptr) return;
    cairo_surface_flush(overlay_);
    cairo_save(cr_);
    cairo_set_source_surface(cr_, overlay_, overlayX_, overlayY_);
    cairo_paint(cr_);
    cairo_restore(cr_);
}

void BrowserPanel::drawOverlays(const Pen& pen) {
    cairo_save(pen.cr);
    cairo_scale(pen.cr, scale_, scale_);
    const double W = viewportW_ / scale_, bottom = kBarHeight + viewportH_ / scale_;
    cairo_rectangle(pen.cr, 0, kBarHeight, W, bottom - kBarHeight);
    cairo_clip(pen.cr);
    const auto spot = [&](Hit hit, int arg, double x, double y, double w, double h) {
        const Spot s {hit, arg, x, y, w, h, true};
        spots_.push_back(s);
        return over(s);
    };
    const auto card = [&](double x, double y, double w, double h) {
        pen.color(kCard);
        pen.roundedRect(x, y, w, h, kRadius);
        cairo_fill(pen.cr);
        strokeRounded(pen, x, y, w, h, kRadius, kBorder, 2);
    };
    if (shown_.favoritesOpen) {
        // The 즐겨찾기 list under its button: a row opens the page, its ✕ removes it
        double x, y, w, h;
        listBox(x, y, w, h);
        const size_t n = shown_.favorites.size();
        card(x, y, w, h);
        double ry = y + kListPad;
        if (n == 0) {
            pen.text(x + 24, centerBaseline(ry, kListRowH, 21),
                     ellipsize(pen, tr(Tx::BrowserFavoritesEmpty), 21, kListW - 48, false), 21, kTextMuted);
            ry += kListRowH;
        }
        for (size_t i = 0; i < n; ++i) {
            const double rx = x + kListPad, rw = kListW - 2 * kListPad;
            const double crossS = kListRowH - 12, crossX = rx + rw - crossS - 4, crossY = ry + 6;
            const bool crossOn = spot(Hit::FavoriteRemove, static_cast<int>(i), crossX, crossY, crossS, crossS);
            const bool rowOn = spot(Hit::FavoriteOpen, static_cast<int>(i), rx, ry, crossX - rx - 4, kListRowH);
            if (rowOn) {
                pen.color(pressed_ ? kAccentPressed : kControlHover);
                pen.roundedRect(rx, ry, crossX - rx - 4, kListRowH, 10);
                cairo_fill(pen.cr);
            }
            const Bookmark& b = shown_.favorites[i];
            const std::string name = b.name.empty() ? b.url : b.name;
            pen.text(rx + 14, centerBaseline(ry, kListRowH, 22), ellipsize(pen, name, 22, crossX - rx - 36, false), 22,
                     rowOn && pressed_ ? kOnAccent : kText);
            pen.color(crossOn ? kControlHover : kControl);
            pen.roundedRect(crossX, crossY, crossS, crossS, 10);
            cairo_fill(pen.cr);
            crossIcon(pen.cr, crossX + crossS / 2, crossY + crossS / 2, 8, crossOn ? kDanger : kTextMuted);
            ry += kListRowH;
        }
        if (!shown_.favoritesNote.empty()) {
            pen.text(x + 24, centerBaseline(ry, kListNoteH, 19),
                     ellipsize(pen, shown_.favoritesNote, 19, kListW - 48, false), 19, kWarn);
        }
        spot(Hit::Popup, 0, x, y, kListW, h);  // the rest of the list: not the page under it
    }
    if (shown_.viewOpen) {
        // 보기: 버전 (데스크톱 | 모바일), 확대 (− % + 원래대로), 비율 (16:9 … 9:16) and what a 비율 change does
        double x, y, w, h;
        viewBox(x, y, w, h);
        card(x, y, w, h);
        const double lx = x + 24, cx = x + 140, cw = x + w - 22 - cx;
        const auto pill = [&](double py, const std::vector<std::string>& labels, Hit hit, int selected) {
            const double part = cw / labels.size();
            pen.color(kControl);
            pen.roundedRect(cx, py, cw, kViewRowH, kViewRowH / 2);
            cairo_fill(pen.cr);
            strokeRounded(pen, cx, py, cw, kViewRowH, kViewRowH / 2, kBorder, 1.5);
            for (size_t i = 0; i < labels.size(); ++i) {
                const double sx = cx + i * part;
                const bool on = spot(hit, static_cast<int>(i), sx, py, part, kViewRowH);
                const bool sel = static_cast<int>(i) == selected;
                if (sel || on) {
                    pen.color(sel ? (on ? kAccentPressed : kAccent) : kControlHover);
                    pen.roundedRect(sx + 4, py + 4, part - 8, kViewRowH - 8, (kViewRowH - 8) / 2);
                    cairo_fill(pen.cr);
                }
                textCentered(pen, sx + part / 2, centerBaseline(py, kViewRowH, 22), labels[i], 22,
                             sel ? kOnAccent : kText, sel);
            }
        };
        double ry = y + kViewPad;
        pen.text(lx, centerBaseline(ry, kViewRowH, 22), tr(Tx::BrowserViewSite), 22, kTextMuted);
        pill(ry, {tr(Tx::BrowserViewDesktop), tr(Tx::BrowserViewMobile)}, Hit::ViewMobile, shown_.mobile ? 1 : 0);
        ry += kViewRowH + 16;
        pen.text(lx, centerBaseline(ry, kViewRowH, 22), tr(Tx::BrowserViewZoom), 22, kTextMuted);
        const double stepW = 70;
        const auto small = [&](Hit hit, double bx, double bw, const char* label, double size) {
            const bool on = spot(hit, 0, bx, ry, bw, kViewRowH);
            pen.color(on ? (pressed_ ? kAccentPressed : kControlHover) : kControl);
            pen.roundedRect(bx, ry, bw, kViewRowH, kRadius);
            cairo_fill(pen.cr);
            strokeRounded(pen, bx, ry, bw, kViewRowH, kRadius, on ? kAccent : kBorder, on ? 2.5 : 1.5);
            textCentered(pen, bx + bw / 2, centerBaseline(ry, kViewRowH, size), label, size, kText, true);
        };
        small(Hit::ZoomOut, cx, stepW, "−", 30);
        textCentered(pen, cx + stepW + 70, centerBaseline(ry, kViewRowH, 26), std::to_string(shown_.zoomPct) + "%", 26,
                     kText, true);
        small(Hit::ZoomIn, cx + stepW + 140, stepW, "+", 30);
        small(Hit::ZoomReset, cx + 2 * stepW + 150, cw - (2 * stepW + 150), tr(Tx::BrowserViewZoomReset), 22);
        ry += kViewRowH + 16;
        pen.text(lx, centerBaseline(ry, kViewRowH, 22), tr(Tx::BrowserViewAspect), 22, kTextMuted);
        pill(ry, {kAspectNames[0], kAspectNames[1], kAspectNames[2], kAspectNames[3]}, Hit::Aspect, shown_.aspect);
        ry += kViewRowH;
        const std::string note = tr(Tx::BrowserViewAspectNote);
        const double noteSize = fitSize(pen, note, 18, 14, w - 48, false);
        pen.text(lx, centerBaseline(ry, kViewNoteH, noteSize), ellipsize(pen, note, noteSize, w - 48, false), noteSize,
                 kTextMuted);
        spot(Hit::Popup, 0, x, y, w, h);
    }
    if (!shown_.menu.empty()) {
        double x, y, w, h;
        menuBox(x, y, w, h);
        card(x, y, w, h);
        double ry = y + kMenuPad;
        for (const MenuItem item : shown_.menu) {
            const double rx = x + kMenuPad, rw = kMenuW - 2 * kMenuPad;
            const bool on = spot(Hit::MenuRow, static_cast<int>(item), rx, ry, rw, kMenuRowH);
            if (on) {
                pen.color(pressed_ ? kAccentPressed : kControlHover);
                pen.roundedRect(rx, ry, rw, kMenuRowH, 10);
                cairo_fill(pen.cr);
            }
            pen.text(rx + 18, centerBaseline(ry, kMenuRowH, 23), menuItemLabel(item), 23,
                     on && pressed_ ? kOnAccent : kText, true);
            ry += kMenuRowH;
        }
        spot(Hit::Popup, 0, x, y, kMenuW, h);
    }
    if (shown_.pressRing) {
        // Held long enough: a ring where the press is (let go: the menu; drag: select)
        const double rx = shown_.ringX / scale_, ry = shown_.ringY / scale_;
        cairo_new_path(pen.cr);
        cairo_arc(pen.cr, rx, ry, kRingR - 2, 0, 2 * M_PI);
        cairo_set_source_rgb(pen.cr, kOnAccent.r, kOnAccent.g, kOnAccent.b);
        cairo_set_line_width(pen.cr, 7);
        cairo_stroke(pen.cr);
        cairo_new_path(pen.cr);
        cairo_arc(pen.cr, rx, ry, kRingR - 2, 0, 2 * M_PI);
        cairo_set_source_rgb(pen.cr, kAccent.r, kAccent.g, kAccent.b);
        cairo_set_line_width(pen.cr, 4);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
}

void BrowserPanel::clearPage() {
    hasPage_ = false;
    resetPageRect();
}

void BrowserPanel::showFrame(const BgraFrame& frame) {
    if (frame.empty()) return;
    cairo_surface_flush(surface_);
    if (frame.width == viewportW_ && frame.height == viewportH_) {
        // The usual case: the page at the viewport's size, row by row into the page area
        unsigned char* data = cairo_image_surface_get_data(surface_);
        const int stride = cairo_image_surface_get_stride(surface_);
        const size_t row = static_cast<size_t>(frame.width) * 4;
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(data + static_cast<size_t>(barH_ + y) * stride, frame.pixels.data() + y * row, row);
        }
        cairo_surface_mark_dirty_rectangle(surface_, 0, barH_, viewportW_, viewportH_);
        resetPageRect();
        // A picture Chrome scaled down to the viewport (a page of the same shape, larger): presses in its CSS px
        if (frame.cssWidth >= 1 && frame.cssHeight >= 1) {
            pageCssW_ = static_cast<int>(std::lround(frame.cssWidth));
            pageCssH_ = static_cast<int>(std::lround(frame.cssHeight));
        }
    } else {
        // Another size (Chrome changed the page size, a resize on the way): scaled to fit with its shape kept, so it
        // never looks stretched, and the laser maps through where it is (viewMap) so a press lands on what it shows
        cairo_surface_t* image = cairo_image_surface_create_for_data(
            const_cast<unsigned char*>(frame.pixels.data()), CAIRO_FORMAT_ARGB32, frame.width, frame.height,
            frame.width * 4);
        const double fit = std::min(static_cast<double>(viewportW_) / frame.width,
                                    static_cast<double>(viewportH_) / frame.height);
        pageW_ = frame.width * fit;
        pageH_ = frame.height * fit;
        pageX_ = (viewportW_ - pageW_) / 2;
        pageY_ = barH_;
        // The CSS px the picture stands for: the screencast says (a picture Chrome scaled down), or its own size
        pageCssW_ = static_cast<int>(std::lround(frame.cssWidth >= 1 ? frame.cssWidth : frame.width));
        pageCssH_ = static_cast<int>(std::lround(frame.cssHeight >= 1 ? frame.cssHeight : frame.height));
        cairo_save(cr_);
        cairo_rectangle(cr_, 0, barH_, viewportW_, viewportH_);
        cairo_clip(cr_);
        cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr_, 0, 0, 0);
        cairo_paint(cr_);
        // Over the black: SOURCE would clear the area outside the picture to transparent (a hole in the panel)
        cairo_set_operator(cr_, CAIRO_OPERATOR_OVER);
        cairo_translate(cr_, pageX_, pageY_);
        cairo_scale(cr_, fit, fit);
        cairo_set_source_surface(cr_, image, 0, 0);
        // GOOD only when it shrinks a lot (BILINEAR would alias); otherwise the cheaper filter on the main thread
        cairo_pattern_set_filter(cairo_get_source(cr_), fit < 0.5 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
        cairo_paint(cr_);
        cairo_restore(cr_);
        cairo_surface_destroy(image);
    }
    hasPage_ = true;
    // The overlays stay on top of the new picture (what they cover is this frame now): composited as last drawn
    saveUnder();
    compositeOverlay();
}
